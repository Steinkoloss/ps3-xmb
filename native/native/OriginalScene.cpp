#include "OriginalScene.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QMatrix4x4>
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace {
QJsonArray identity() { return {1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}; }
QJsonObject quadUniforms() { return {{"_MVM",identity()},{"_Colour1",QJsonArray{1,1,1,1}},{"_Colour2",QJsonArray{1,1,1,1}}}; }
QJsonObject graph(QJsonArray passes) { return {{"schema","moyou.render-graph.v1"},{"passes",passes}}; }
QJsonObject pass(const QString& name, const QString& fragment, QJsonObject uniforms, QJsonObject textures) {
    return {{"name",name},{"vertex","lines/lib/moyou/background.vpo"},
        {"fragment","lines/lib/moyou/" + fragment + ".fpo"},{"uniforms",uniforms},{"textures",textures}};
}
}

std::map<QString,float> OriginalScene::menu(const QString& name) const {
    std::map<QString,float> values;
    QStringList files = {QDir(reference_).filePath("lines/" + name + ".mnu")};
    if (!preset_.isEmpty()) files.append(QDir(reference_).filePath("lines/override/" + preset_ + "/" + name + ".mnu"));
    for (const auto& path : files) {
        QFile file(path);
        if (!file.exists() && path != files.first()) continue;
        if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Missing original scene parameters: " + path.toStdString());
        for (const auto& row : file.readAll().split('\n')) {
            auto fields = QString::fromUtf8(row).trimmed().split(':');
            if (fields.size() != 3) continue;
            bool valid;
            const auto value = fields[2].toFloat(&valid);
            if (!valid || !std::isfinite(value)) throw std::runtime_error("Invalid original scene parameter");
            fields[0].remove(' ');
            values[fields[0]] = value;
        }
    }
    return values;
}

OriginalScene::OriginalScene(const QString& shaders, const QString& reference, const QString& cpu,
                           const QString& spline, const QString& preset, unsigned month, double hour) : reference_(reference), preset_(preset) {
    if (month < 1 || month > 12 || !std::isfinite(hour) || hour > 24) throw std::runtime_error("Invalid original background date/time");
    if (hour < 0) hour = preset == "day" ? 12 : preset == "yoake" ? 6 : preset == "higure" ? 18 : 0;
    const auto line = menu("LINE1"), background = menu("BACKGROUND");
    hdr_ = menu("HDR");
    rope_ = std::make_unique<RopeSolver>(QDir(cpu).filePath("renderer-image.bin").toStdString(),
        QDir(cpu).filePath("math-image.bin").toStdString());
    for (const auto* key : {"DAMPING","LENGTH","TENSION","SPACING","THINNESS","BRIGHTNESS","MIPMAPBIAS",
             "FRESNEL","FALLOFF","TIMESTEP","PERTURBATION","ENDX","ENDY","ENDZ","FFDPARAM1"})
        rope_->parameter(key, line.at(key));
    rope_->reset(unsigned(line.at("STATE")));
    model_ = rope_->modelMatrix({line.at("POSX"),line.at("POSY"),line.at("POSZ"),1},
        {line.at("ANGX"),line.at("ANGY"),line.at("ANGZ"),0},line.at("ANGROT"));
    auto textures = rope_->textures();
    for (unsigned i = 0; i < 3; ++i) {
        const auto axis = QString("XYZ").mid(i,1);
        scale1_[i] = line.at("FFDSCALE1" + axis);
        scale2_[i] = line.at("FFDSCALE2" + axis);
        offset_[i] = line.at("FFDOFFSET" + axis);
    }
    scale1_[3] = scale2_[3] = offset_[3] = 1;
    surface_ = std::make_unique<SurfaceEvaluator>(spline.toStdString());
    auto ffdPass = pass("deformation", QString("ffd_shader%1").arg(int(line.at("FFDSHADER"))),
        quadUniforms(), {{"0",QJsonObject{{"native","uvw"}}}});
    auto ffdUniforms = ffdPass["uniforms"].toObject();
    ffdUniforms["_Time"] = QJsonArray{0};
    ffdPass["uniforms"] = ffdUniforms;
    ffd_ = std::make_unique<Renderer>(shaders, graph({ffdPass}), reference_);
    std::array<SurfaceEvaluator::Vector,128> uvw;
    for (unsigned z = 0; z < 4; ++z) for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 8; ++x)
        uvw[(z * 4 + y) * 8 + x] = {float(x) * 0.125f,float(y) * 0.25f,float(z) * 0.25f,0};
    ffd_->texture("uvw",QSize(128,1),uvw.data(),true);
    deformation(0);

    auto bgUniforms = quadUniforms();
    const auto day = float(hour/24), spread = background.at("DAYSPREAD");
    const auto smooth = [](float a, float b, float x) { x = std::clamp((x-a)/(b-a),0.0f,1.0f); return x*x*(3-2*x); };
    bgUniforms["_DayTime"] = QJsonArray{((2*day*day*day-3*day*day+day)*spread-2*day*day*day+3*day*day)*2400};
    bgUniforms["_MonthTime"] = QJsonArray{0};
    bgUniforms["_NightTime"] = QJsonArray{(day < 0.5f ? day+0.5f : day-0.5f)*2400};
    bgUniforms["_NightBrightness"] = QJsonArray{background.at("NIGHTWHITBIAS")};
    bgUniforms["_Alpha"] = QJsonArray{1};
    bgUniforms["_NightDayBlend"] = QJsonArray{1+background.at("NIGHTBLEND")*(smooth(4.0f/24,6.0f/24,day)*(1-smooth(18.0f/24,20.0f/24,day))-1)};
    const auto monthPath = [&](const QString& type, unsigned index) {
        return QJsonObject{{"file",QString("lines/textures/month_bg/%1/%2.dds").arg(type).arg(index,2,10,QChar('0'))}};
    };
    auto bg = pass("background","back_colours0",bgUniforms,{
        {"0",monthPath("rgb",month)},{"1",monthPath("rgb",month%12+1)},
        {"2",monthPath("night",month)},{"3",monthPath("night",month%12+1)}});
    bg["width"] = 32; bg["height"] = 32; bg["format"] = "rgba16f";
    QJsonObject sheet = {{"name","ribbon"},{"geometry","sheet"},
        {"vertex",QString("lines/lib/moyou/lines%1.vpo").arg(int(line.at("SHADER")))},
        {"fragment",QString("lines/lib/moyou/lines%1.fpo").arg(int(line.at("SHADER")))},
        {"uniforms",QJsonObject{{"_Spacing",QJsonArray{line.at("SPACING")}},{"_Thinness",QJsonArray{line.at("THINNESS")}},
            {"_Fresnel",QJsonArray{line.at("FRESNEL")}},{"_Brightness",QJsonArray{line.at("BRIGHTNESS")}},
            {"_MipmapBias",QJsonArray{line.at("MIPMAPBIAS")}}}},
        {"textures",QJsonObject{{"0",QJsonObject{{"native","stripes"}}},
            {"1",QJsonObject{{"file","lines/textures/TGA/freslut1.tga"}}},{"2",QJsonObject{{"native","encode"}}}}}};
    auto colours = quadUniforms();
    for (int i = 1; i <= 4; ++i) {
        const auto index = QString::number(i);
        colours["_Colour" + index] = QJsonArray{background.at(index + "RED"),background.at(index + "GREEN"),background.at(index + "BLUE"),1};
    }
    colours["_ScreenOffset"] = QJsonArray{0,0};
    colours["_ScreenSizeRecip"] = QJsonArray{1.0/1920,1.0/1080};
    auto composite = pass("composition","LinesController",colours,{
        {"0",QJsonObject{{"pass","ribbon"}}},{"1",QJsonObject{{"pass","background"}}},
        {"15",QJsonObject{{"native","exposure0"}}},{"14",QJsonObject{{"native","exposure1"}}},
        {"13",QJsonObject{{"native","noise"},{"repeat",true}}}});
    composite["vertex"] = "lines/lib/moyou/LinesController.vpo";
    QJsonArray passes{bg,sheet,composite};
    auto glareSource = pass("glare-source-pre","GlareSourcePre",quadUniforms(),{{"0",QJsonObject{{"pass","composition"}}}});
    glareSource["fragment"] = "lines/lib/glutils/GlareSourcePre.fpo";
    auto sourceUniforms = glareSource["uniforms"].toObject();
    sourceUniforms["_Exposure"] = QJsonArray{hdr_.at("EXPOSURE")};
    sourceUniforms["_WhiteSqrRcp"] = QJsonArray{1.0/(hdr_.at("WHITELEVEL")*hdr_.at("WHITELEVEL"))};
    glareSource["uniforms"] = sourceUniforms;
    glareSource["width"] = 128; glareSource["height"] = 64; glareSource["format"] = "rgba16f";
    passes.append(glareSource);
    auto source = pass("glare-source","GlareSource",quadUniforms(),{{"0",QJsonObject{{"pass","glare-source-pre"}}}});
    source["fragment"] = "lines/lib/glutils/GlareSource.fpo";
    source["width"] = source["height"] = 128; source["format"] = "rgba16f"; source["mipmaps"] = true;
    passes.append(source);
    const auto weights = rope_->glareWeights({hdr_.at("GAUSSIANRADR"),hdr_.at("GAUSSIANRADG"),hdr_.at("GAUSSIANRADB"),0},
        hdr_.at("GLARESUMPOW"),hdr_.at("GLARELEVEL"));
    for (unsigned level = 0; level < 6; ++level) {
        const auto horizontal = QString("glare-x-%1").arg(level), vertical = QString("glare-y-%1").arg(level);
        for (unsigned axis = 0; axis < 2; ++axis) {
            QJsonObject uniforms{{"_ModelviewProjection",identity()},
                {"_Offset",QJsonArray{axis == 0 ? double(1u << level)/128 : 0,axis == 1 ? double(1u << level)/128 : 0}}};
            for (unsigned tap = 0; tap < 8; ++tap) {
                const auto& w = weights.taps[tap];
                uniforms[QString("_Weights%1").arg(tap)] = QJsonArray{w[0],w[1],w[2],0};
            }
            QJsonObject texture{{"pass",axis == 0 ? QString("glare-source") : horizontal}};
            if (axis == 0) texture["level"] = int(level);
            QJsonObject blur{{"name",axis == 0 ? horizontal : vertical},
                {"vertex","lines/lib/glutils/Gaussian.vpo"},{"fragment","lines/lib/glutils/Gaussian.fpo"},
                {"uniforms",uniforms},{"textures",QJsonObject{{"0",texture}}},
                {"width",int(128u >> level)},{"height",int(128u >> level)},{"format","rgba16f"}};
            passes.append(blur);
        }
    }
    QString previous;
    for (int level = 5; level >= 0; --level) {
        const auto name = QString("glare-sum-%1").arg(level);
        auto sum = pass(name,"ApplyDisplay",quadUniforms(),{{"0",QJsonObject{{"pass",QString("glare-y-%1").arg(level)}}}});
        sum["fragment"] = "lines/lib/glutils/ApplyDisplay.fpo";
        auto uniforms = sum["uniforms"].toObject(); uniforms["_GlareWeight"] = QJsonArray{weights.levels[level]};
        sum["uniforms"] = uniforms; sum["width"] = sum["height"] = 128; sum["format"] = "rgba16f";
        if (!previous.isEmpty()) { sum["over"] = previous; sum["blend"] = "add"; }
        passes.append(sum); previous = name;
    }
    auto final = pass("display","ToneApplyDisplay",quadUniforms(),{
        {"1",QJsonObject{{"pass",previous}}},{"13",QJsonObject{{"native","noise"},{"repeat",true}}}});
    final["fragment"] = "lines/lib/glutils/ToneApplyDisplay.fpo";
    auto finalUniforms = final["uniforms"].toObject(); finalUniforms["_GlareWeight"] = QJsonArray{1};
    final["uniforms"] = finalUniforms; final["over"] = "composition"; final["blend"] = "add";
    passes.append(final);
    display_ = std::make_unique<Renderer>(shaders,graph(passes),reference_);
    display_->texture("stripes",QSize(textures[0].width,textures[0].height),textures[0].rgba.data());
    display_->texture("encode",QSize(textures[1].width,textures[1].height),textures[1].rgba.data());
}

void OriginalScene::deformation(float time) {
    ffd_->uniform("deformation","_Time",QJsonArray{time});
    auto values = ffd_->captureFloat(QSize(128,1));
    for (unsigned n = 0; n < 128; ++n) for (unsigned c = 0; c < 4; ++c) lattice_[n][c] = values[n * 4 + c];
}

void OriginalScene::advance(double seconds, QSize size) {
    rope_->advance(seconds);
    surface_->lattice(lattice_,scale1_,scale2_,offset_);
    QMatrix4x4 projection;
    projection.perspective(53.0f,float(size.width()) / size.height(),0.1f,1000.0f);
    RopeSolver::Matrix p;
    std::copy_n(projection.constData(),16,p.begin());
    auto matrix = rope_->multiply(p,model_);
    auto clip = surface_->project(rope_->positions(),matrix);
    auto evaluated = surface_->subdivide(clip);
    vertices_.resize(128 * 128 * 9);
    for (unsigned y = 0; y < 128; ++y) for (unsigned x = 0; x < 128; ++x) {
        auto index = y * 128 + x;
        for (unsigned c = 0; c < 7; ++c) vertices_[index * 9 + c] = evaluated[index][c];
        const auto u = float(x) / 127.0f, v = float(y) / 127.0f;
        vertices_[index * 9 + 7] = v;
        vertices_[index * 9 + 8] = std::min(1.0f,(0.5f-std::abs(0.5f-u))*10.0f) *
            std::min(1.0f,(0.5f-std::abs(0.5f-v))*10.0f);
    }
    display_->mesh(vertices_);
    auto exposure = rope_->exposure(hdr_.at("EXPOSURE"),hdr_.at("WHITELEVEL"),hdr_.at("GLARETHRESH"),1,1.0f/255);
    for (unsigned i = 0; i < 2; ++i) display_->texture(QString("exposure%1").arg(i),QSize(128,128),exposure.rgba[i].data(),true,true);
    display_->texture("noise",QSize(32,32),exposure.noise.rgba.data(),false,true);
    display_->uniform("composition","_ScreenSizeRecip",QJsonArray{1.0/size.width(),1.0/size.height()});
    deformation(rope_->time() * 10.0f);
}

void OriginalScene::render(GLuint target, QSize size) { display_->render(target,size); }
QImage OriginalScene::capture(QSize size) { return display_->capture(size).convertToFormat(QImage::Format_RGB32); }
