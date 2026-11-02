#include "OriginalScene.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QMatrix4x4>
#include <QTime>
#include <algorithm>
#include <cmath>
#include <cstdio>
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

namespace {
double presetHour(const QString& preset, double hour) {
    if (!std::isfinite(hour) || hour > 24) throw std::runtime_error("Invalid original background time");
    if (hour < 0 && preset == "clock") {
        const auto now = QTime::currentTime();
        return now.msecsSinceStartOfDay() / 3.6e6;
    }
    if (hour < 0) hour = preset == "day" ? 12 : preset == "yoake" ? 6 : preset == "higure" ? 18 : 0;
    return hour;
}
}

OriginalDisplay::OriginalDisplay(const QString& shaders, const QString& reference, const QString& preset,
                                 OriginalSimulation::Constants constants, unsigned month, double hour) :
    shaders_(shaders), reference_(reference), preset_(preset), constants_(std::move(constants)) {
    if (month < 1 || month > 12) throw std::runtime_error("Invalid original background month");
    month_ = month;
    hour_ = presetHour(preset,hour);
    line_ = originalMenu(reference,preset,"LINE1",hour_);
    background_ = originalMenu(reference,preset,"BACKGROUND",hour_);
    hdr_ = originalMenu(reference,preset,"HDR",hour_);
    build();
}

QJsonObject OriginalDisplay::backgroundTime() const {
    const auto day = float(hour_/24), spread = background_.at("DAYSPREAD");
    const auto smooth = [](float a, float b, float x) { x = std::clamp((x-a)/(b-a),0.0f,1.0f); return x*x*(3-2*x); };
    return {{"_DayTime",QJsonArray{((2*day*day*day-3*day*day+day)*spread-2*day*day*day+3*day*day)*2400}},
        {"_MonthTime",QJsonArray{0}},
        {"_NightTime",QJsonArray{(day < 0.5f ? day+0.5f : day-0.5f)*2400}},
        {"_NightDayBlend",QJsonArray{1+background_.at("NIGHTBLEND")*(smooth(4.0f/24,6.0f/24,day)*(1-smooth(18.0f/24,20.0f/24,day))-1)}}};
}

void OriginalDisplay::setTime(unsigned month, double hour) {
    if (month < 1 || month > 12) throw std::runtime_error("Invalid original background month");
    hour_ = presetHour(preset_,hour);
    if (month != month_) { month_ = month; build(); return; }
    const auto time = backgroundTime();
    for (auto i = time.begin(); i != time.end(); ++i) display_->uniform("background",i.key(),i.value().toArray());
}

void OriginalDisplay::build() {
    const auto& shaders = shaders_;
    const auto& line = line_;
    const auto& background = background_;
    const auto month = month_;
    const auto& weights = constants_.glare;
    auto bgUniforms = quadUniforms();
    const auto time = backgroundTime();
    for (auto i = time.begin(); i != time.end(); ++i) bgUniforms[i.key()] = i.value();
    bgUniforms["_NightBrightness"] = QJsonArray{background.at("NIGHTWHITBIAS")};
    bgUniforms["_Alpha"] = QJsonArray{1};
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
    // ParticlesLayer draws additively into the lit scene before glare (0x02060660).
    // The particle layer draws after the lines layer's glare and tone stage: it has its own
    // glare program (particles_second), and drawn earlier its glints would bloom twice.
    constexpr bool particlesLast = true;
    QString scene = "composition";
    QJsonArray particlePasses;
    for (const auto* program : {"particles_quads","particles_second"}) {
        QJsonObject particles{{"name",QString(program)},{"geometry","particles"},
            {"vertex",QString("lines/lib/particles/%1.vpo").arg(program)},
            {"fragment",QString("lines/lib/particles/%1.fpo").arg(program)},
            {"over",scene},{"blend","add"},{"uniforms",QJsonObject{}},
            {"particleBit",QString(program) == "particles_quads" ? 1 : 2},
            // 0x02060660: alpha test GREATER 0, additive ONE/ONE; 0x0205c254: no depth/cull/stencil.
            {"alphaTest","greater0"},
            {"textures",QJsonObject{{"0",QJsonObject{{"file","lines/particles/proc_iridescent.tga"}}}}}};
        particlePasses.append(particles);
    }
    if (!particlesLast) {
        for (auto value : particlePasses) {
            auto particles = value.toObject(); particles["over"] = scene;
            passes.append(particles); scene = particles["name"].toString();
        }
    }
    auto glareSource = pass("glare-source-pre","GlareSourcePre",quadUniforms(),{{"0",QJsonObject{{"pass",scene}}}});
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
    final["uniforms"] = finalUniforms; final["over"] = scene; final["blend"] = "add";
    if (particlesLast) {
        final["format"] = "rgba16f";
        passes.append(final);
        QString base = "display";
        for (auto value : particlePasses) {
            auto particles = value.toObject(); particles["over"] = base; particles["format"] = "rgba16f";
            passes.append(particles); base = particles["name"].toString();
        }
    } else
        passes.append(final);
    display_ = std::make_unique<Renderer>(shaders,graph(passes),reference_);
    const auto& textures = constants_.textures;
    display_->texture("stripes",QSize(textures[0].width,textures[0].height),textures[0].rgba.data());
    display_->texture("encode",QSize(textures[1].width,textures[1].height),textures[1].rgba.data());
}

// Preset "clock": the simulation re-blended its menus; take the same blend for the
// display uniforms (every blended value is a uniform, so no rebuild).
void OriginalDisplay::applyMenus(const OriginalSimulation::Frame& frame) {
    menus_ = frame.menus;
    if (preset_ != "clock" || frame.menuHour < 0) return;
    line_ = originalMenu(reference_,preset_,"LINE1",frame.menuHour);
    background_ = originalMenu(reference_,preset_,"BACKGROUND",frame.menuHour);
    hdr_ = originalMenu(reference_,preset_,"HDR",frame.menuHour);
    constants_.glare = frame.glare;
    for (const auto& [uniform, key] : std::initializer_list<std::pair<const char*, const char*>>{{"_Spacing","SPACING"},
            {"_Thinness","THINNESS"},{"_Fresnel","FRESNEL"},{"_Brightness","BRIGHTNESS"},{"_MipmapBias","MIPMAPBIAS"}})
        display_->uniform("ribbon",uniform,QJsonArray{line_.at(key)});
    display_->uniform("glare-source-pre","_Exposure",QJsonArray{hdr_.at("EXPOSURE")});
    display_->uniform("glare-source-pre","_WhiteSqrRcp",QJsonArray{1.0/(hdr_.at("WHITELEVEL")*hdr_.at("WHITELEVEL"))});
    for (unsigned level = 0; level < 6; ++level) {
        for (const auto& pass : {QString("glare-x-%1").arg(level),QString("glare-y-%1").arg(level)})
            for (unsigned tap = 0; tap < 8; ++tap) {
                const auto& w = frame.glare.taps[tap];
                display_->uniform(pass,QString("_Weights%1").arg(tap),QJsonArray{w[0],w[1],w[2],0});
            }
        display_->uniform(QString("glare-sum-%1").arg(level),"_GlareWeight",QJsonArray{frame.glare.levels[level]});
    }
}

void OriginalDisplay::show(const OriginalSimulation::Frame& frame, QSize size) {
    if (frame.menus != menus_) applyMenus(frame);
    // A screen showing part of a wider canvas: geometry rasterizes through an offset viewport,
    // the background gradient samples the same sub-rectangle.
    const auto& k = corners_;
    const auto near = [](double a, double b) { return std::abs(a - b) < 1e-9; };
    const bool upright = near(k[0].y(),k[1].y()) && near(k[0].x(),k[3].x()) && k[1].x() > k[0].x() && k[3].y() > k[0].y();
    QRectF viewport;
    if (upright) {
        viewport = QRectF(k[0],k[2]);
        display_->mapping({},{});
    } else {
        double left = 1e300, top = 1e300, right = -1e300, bottom = -1e300;
        for (const auto& corner : k) {
            left = std::min(left,corner.x()); right = std::max(right,corner.x());
            top = std::min(top,corner.y()); bottom = std::max(bottom,corner.y());
        }
        viewport = QRectF(left,top,right - left,bottom - top);
        // Keep the screen's pixel density: its top edge, in canvas units, spans size.width().
        const double edge = std::hypot((k[1].x() - k[0].x()) * aspect_,k[1].y() - k[0].y());
        const double perUnit = size.width() / edge;
        const QSize source(std::max(1,int(std::lround(viewport.width() * aspect_ * perUnit))),
                           std::max(1,int(std::lround(viewport.height() * perUnit))));
        std::array<QPointF,4> mapped;
        for (unsigned i = 0; i < 4; ++i)
            mapped[i] = QPointF((k[i].x() - left) / viewport.width(),(k[i].y() - top) / viewport.height());
        display_->mapping(source,mapped);
        size = source;
    }
    const bool cropped = viewport != QRectF(0,0,1,1);
    display_->geometryViewport(viewport);
    display_->quadRect("background",cropped ? viewport : QRectF());
    display_->mesh(frame.vertices);
    for (unsigned i = 0; i < 2; ++i) display_->texture(QString("exposure%1").arg(i),QSize(128,128),frame.exposure[i].data(),true,true);
    display_->texture("noise",QSize(32,32),frame.noise.rgba.data(),false,true);
    display_->uniform("composition","_ScreenSizeRecip",QJsonArray{1.0/size.width(),1.0/size.height()});
    display_->particles(frame.particles);
    if (frame.particles.empty()) return;
    const auto& u = frame.particleUniforms;
    display_->particleMask(glints_ ? u.passes : u.passes & ~2u);
    const auto array = [](const auto& values, int count) { QJsonArray out; for (int i = 0; i < count; ++i) out.append(values[i]); return out; };
    // RSX matrix parameters are rows; vectormath memory is column-major.
    const auto rows = [](const RopeSolver::Matrix& columns) {
        QJsonArray out; for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) out.append(columns[c * 4 + r]); return out; };
    for (const auto* program : {"particles_quads","particles_second"}) {
        const bool second = QString(program) == "particles_second";
        display_->uniform(program,"_ModelviewProjection",rows(frame.projection));
        display_->uniform(program,"_Modelview",rows(frame.view));
        display_->uniform(program,"_LightPack",rows(u.lightPack));
        for (const auto& [name, count] : std::initializer_list<std::pair<const char*, int>>{{"_ParticleSize",3},
                {"_Transparency",2},{"_Focus",4},{"_FocusCurves",3},{"_FrontFacingQuaternion",4},
                {"_LifeBoundsMin",3},{"_LifeBoundsMax",3},{"_Darkness",2}})
            display_->uniform(program,name,array(u.vertex.at(name),count));
        // Same-named parameters exist in both stages; set the vertex value, then the fragment one.
        const auto* both = second ? "_Glare" : "_NearControl";
        display_->uniform(program,both,array(u.vertex.at(both),4));
        display_->fragmentUniform(program,both,array(u.fragment.at(both),4));
        const auto light = rows(u.fragmentLightPack);
        for (int r = 0; r < 4; ++r)
            display_->uniform(program,QString("_LightPack[%1]").arg(r),QJsonArray{light[r*4],light[r*4+1],light[r*4+2],light[r*4+3]});
        display_->uniform(program,"_Color",array(u.fragment.at("_Color"),3));
        display_->uniform(program,"_IridescentExp",array(u.fragment.at("_IridescentExp"),1));
        display_->uniform(program,"_Gamma",array(u.fragment.at("_Gamma"),1));
    }
}

QImage OriginalDisplay::capture(QSize size) { return display_->capture(size).convertToFormat(QImage::Format_RGB32); }

OriginalScene::OriginalScene(const QString& shaders, const QString& reference, const QString& cpu,
                             const QString& spline, const QString& preset, unsigned month, double hour) :
    simulation_(shaders,reference,cpu,spline,preset,presetHour(preset,hour)),
    display_(shaders,reference,preset,simulation_.constants(),month,hour) {}

void OriginalScene::advance(double seconds, QSize size) {
    advance(seconds,size,QRect(QPoint(),size));
}

void OriginalScene::advance(double seconds, QSize canvas, QRect viewport) {
    frame_ = simulation_.step(seconds,canvas);
    display_.setViewport(QRectF(double(viewport.x()) / canvas.width(),double(viewport.y()) / canvas.height(),
        double(viewport.width()) / canvas.width(),double(viewport.height()) / canvas.height()));
    display_.show(frame_,viewport.size());
}

void OriginalScene::advance(double seconds, QSize canvas, const std::array<QPointF,4>& corners, QSize screen) {
    frame_ = simulation_.step(seconds,canvas);
    display_.setScreen(corners,double(canvas.width()) / canvas.height());
    display_.show(frame_,screen);
}
