#include "OriginalSimulation.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QMatrix4x4>
#include <algorithm>
#include <cmath>
#include <future>
#include <cstdio>
#include <stdexcept>

PresetBlend presetBlend(double hour) {
    if (!std::isfinite(hour) || hour < 0 || hour > 24) throw std::runtime_error("Invalid original preset time");
    static const std::array<const char*,10> names = {"night","night","yoake","yoake","day","day","higure","higure","night","night"};
    static constexpr std::array<double,9> times = {1,5,7,11,13,17,19,23,24};
    const double t = hour;
    for (unsigned j = 0; j < times.size(); ++j) {
        if (t < times[j]) {
            const double start = j ? times[j - 1] : 0;
            const float w = float((t - start) / (times[j] - start));
            return {names[j],names[j + 1],w * w * (3 - 2 * w)};
        }
    }
    return {"night","night",0};
}

std::map<QString,float> originalMenu(const QString& reference, const QString& preset, const QString& name, double hour) {
    if (preset == "clock") {
        const auto blend = presetBlend(hour);
        auto from = originalMenu(reference,blend.from,name), to = originalMenu(reference,blend.to,name);
        for (auto& [key, value] : from) {
            auto found = to.find(key);
            if (found != to.end()) value += blend.weight * (found->second - value);
        }
        return from;
    }
    std::map<QString,float> values;
    QStringList files = {QDir(reference).filePath("lines/" + name + ".mnu")};
    if (!preset.isEmpty()) files.append(QDir(reference).filePath("lines/override/" + preset + "/" + name + ".mnu"));
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

OriginalSimulation::OriginalSimulation(const QString& shaders, const QString& reference, const QString& cpu,
                                       const QString& spline, const QString& preset, double hour) :
    reference_(reference), preset_(preset), menuHour_(hour) {
    const auto line = originalMenu(reference,preset,"LINE1",hour);
    hdr_ = originalMenu(reference,preset,"HDR",hour);
    rope_ = std::make_unique<RopeSolver>(QDir(cpu).filePath("renderer-image.bin").toStdString(),
        QDir(cpu).filePath("math-image.bin").toStdString());
    for (const auto* key : {"DAMPING","LENGTH","TENSION","SPACING","THINNESS","BRIGHTNESS","MIPMAPBIAS",
             "FRESNEL","FALLOFF","TIMESTEP","PERTURBATION","ENDX","ENDY","ENDZ","FFDPARAM1"})
        rope_->parameter(key, line.at(key));
    rope_->reset(unsigned(line.at("STATE")));
    model_ = rope_->modelMatrix({line.at("POSX"),line.at("POSY"),line.at("POSZ"),1},
        {line.at("ANGX"),line.at("ANGY"),line.at("ANGZ"),0},line.at("ANGROT"));
    constants_.textures = rope_->textures();
    for (unsigned i = 0; i < 3; ++i) {
        const auto axis = QString("XYZ").mid(i,1);
        scale1_[i] = line.at("FFDSCALE1" + axis);
        scale2_[i] = line.at("FFDSCALE2" + axis);
        offset_[i] = line.at("FFDOFFSET" + axis);
    }
    scale1_[3] = scale2_[3] = offset_[3] = 1;
    surface_ = std::make_unique<SurfaceEvaluator>(spline.toStdString());
    const QJsonObject uniforms{{"_MVM",QJsonArray{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1}},
        {"_Colour1",QJsonArray{1,1,1,1}},{"_Colour2",QJsonArray{1,1,1,1}},{"_Time",QJsonArray{0}}};
    const QJsonObject ffdPass{{"name","deformation"},{"vertex","lines/lib/moyou/background.vpo"},
        {"fragment",QString("lines/lib/moyou/ffd_shader%1.fpo").arg(int(line.at("FFDSHADER")))},
        {"uniforms",uniforms},{"textures",QJsonObject{{"0",QJsonObject{{"native","uvw"}}}}}};
    ffd_ = std::make_unique<Renderer>(shaders,
        QJsonObject{{"schema","moyou.render-graph.v1"},{"passes",QJsonArray{ffdPass}}},reference);
    std::array<SurfaceEvaluator::Vector,128> uvw;
    for (unsigned z = 0; z < 4; ++z) for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 8; ++x)
        uvw[(z * 4 + y) * 8 + x] = {float(x) * 0.125f,float(y) * 0.25f,float(z) * 0.25f,0};
    ffd_->texture("uvw",QSize(128,1),uvw.data(),true);
    deformation(0);
    const auto particleMenu = [&](const QString& name) {
        std::map<std::string,float> values;
        for (const auto& [key, value] : originalMenu(reference,preset,name,hour))
            values[QString(key).remove('_').toLower().toStdString()] = value;
        return values;
    };
    particles_ = rope_->createParticles(2048);
    rope_->particleParameters(particles_,particleMenu("PARTICLES"),particleMenu("PARTICLES_UI"),particleMenu("PARTICLES_SPE"));
    particleLayer_ = rope_->createParticleLayer(particles_);
    constants_.glare = rope_->glareWeights({hdr_.at("GAUSSIANRADR"),hdr_.at("GAUSSIANRADG"),hdr_.at("GAUSSIANRADB"),0},
        hdr_.at("GLARESUMPOW"),hdr_.at("GLARELEVEL"));
}

// Live preset blend: the menu values that differ between time-of-day presets are rope
// fields, FFD scales, the model rotation, HDR/glare and particle parameters.
void OriginalSimulation::applyMenus(double hour) {
    const auto line = originalMenu(reference_,preset_,"LINE1",hour);
    hdr_ = originalMenu(reference_,preset_,"HDR",hour);
    for (const auto* key : {"SPACING","THINNESS","BRIGHTNESS","MIPMAPBIAS","FRESNEL","FALLOFF","PERTURBATION","FFDPARAM1"})
        rope_->parameter(key, line.at(key));
    model_ = rope_->modelMatrix({line.at("POSX"),line.at("POSY"),line.at("POSZ"),1},
        {line.at("ANGX"),line.at("ANGY"),line.at("ANGZ"),0},line.at("ANGROT"));
    for (unsigned i = 0; i < 3; ++i) {
        const auto axis = QString("XYZ").mid(i,1);
        scale1_[i] = line.at("FFDSCALE1" + axis);
        scale2_[i] = line.at("FFDSCALE2" + axis);
        offset_[i] = line.at("FFDOFFSET" + axis);
    }
    const auto particleMenu = [&](const QString& name) {
        std::map<std::string,float> values;
        for (const auto& [key, value] : originalMenu(reference_,preset_,name,hour))
            values[QString(key).remove('_').toLower().toStdString()] = value;
        return values;
    };
    rope_->particleParameters(particles_,particleMenu("PARTICLES"),particleMenu("PARTICLES_UI"),particleMenu("PARTICLES_SPE"));
    constants_.glare = rope_->glareWeights({hdr_.at("GAUSSIANRADR"),hdr_.at("GAUSSIANRADG"),hdr_.at("GAUSSIANRADB"),0},
        hdr_.at("GLARESUMPOW"),hdr_.at("GLARELEVEL"));
    menuHour_ = hour;
    ++menus_;
}

void OriginalSimulation::deformation(float time) {
    requestDeformation(time);
    collectDeformation();
}

// The FFD lattice for the next update is read back asynchronously: requested at the end
// of one step, collected at the start of the next, so the GPU does not stall the CPU.
void OriginalSimulation::requestDeformation(float time) {
    ffd_->uniform("deformation","_Time",QJsonArray{time});
    ffd_->requestFloat(QSize(128,1));
    deformationPending_ = true;
}

void OriginalSimulation::collectDeformation() {
    if (!deformationPending_) return;
    const auto values = ffd_->collectFloat();
    for (unsigned n = 0; n < 128; ++n) for (unsigned c = 0; c < 4; ++c) lattice_[n][c] = values[n * 4 + c];
    deformationPending_ = false;
}

OriginalSimulation::~OriginalSimulation() {
    if (particleJob_.valid()) particleJob_.wait();
}

OriginalSimulation::Frame OriginalSimulation::step(double seconds, QSize size) {
    rope_->advance(seconds);
    collectDeformation();
    surface_->lattice(lattice_,scale1_,scale2_,offset_);
    QMatrix4x4 projection;
    projection.perspective(53.0f,float(size.width()) / size.height(),0.1f,1000.0f);
    // Both the lines layer (0x02027d64/0x02027dcc) and the particle layer render under the
    // original camera (0x02024a68: FOV 0.925 rad, eye (0,0,2), target (0,0,-7)), so the ribbon
    // is P·V·M. Its on-screen span over the capture (median 65 px vs 67 px at 960x540;
    // 78 px without the view) agrees.
    const auto view = rope_->cameraView();
    const QMatrix4x4 ribbonProjection = projection * QMatrix4x4(view.data()).transposed();
    projection *= QMatrix4x4(view.data()).transposed();
    RopeSolver::Matrix p;
    std::copy_n(ribbonProjection.constData(),16,p.begin());
    auto matrix = rope_->multiply(p,model_);
    auto clip = surface_->project(rope_->positions(),matrix);
    auto evaluated = surface_->subdivide(clip);
    Frame frame;
    auto& vertices = frame.vertices;
    vertices.resize(128 * 128 * 9);
    for (unsigned y = 0; y < 128; ++y) for (unsigned x = 0; x < 128; ++x) {
        auto index = y * 128 + x;
        for (unsigned c = 0; c < 7; ++c) vertices[index * 9 + c] = evaluated[index][c];
        const auto u = float(x) / 127.0f, v = float(y) / 127.0f;
        vertices[index * 9 + 7] = v;
        vertices[index * 9 + 8] = std::min(1.0f,(0.5f-std::abs(0.5f-u))*10.0f) *
            std::min(1.0f,(0.5f-std::abs(0.5f-v))*10.0f);
    }
    // Particles emit from the world-space ribbon: inverse camera projection of the clip-space mesh.
    RopeSolver::Matrix columns;
    std::copy_n(projection.inverted().constData(),16,columns.begin());
    // Particles run beside the next rope/surface update; a frame carries the particles
    // of the previous update (the firmware double-buffers particle vertices too).
    Particles previous;
    if (particleJob_.valid()) previous = particleJob_.get();
    // Re-blend between particle jobs (they share the particle system); a minute of clock
    // time moves the eased weight by under 1%.
    if (preset_ == "clock" && pendingHour_ >= 0 && std::abs(pendingHour_ - menuHour_) >= 1.0 / 60) applyMenus(pendingHour_);
    const float aspect = float(size.width()) / size.height();
    particleJob_ = std::async(std::launch::async,[this,grid = std::move(evaluated),columns,seconds,aspect] {
        rope_->particleFrame(particles_,particleLayer_,grid,128,128,columns,aspect,float(seconds));
        // Development: MOYOU_PARTICLE_DUMP=<path> writes the system and layer for comparison
        // with a running console/emulator (overwritten every frame).
        if (const char* dump = std::getenv("MOYOU_PARTICLE_DUMP")) {
            QFile file(dump);
            if (file.open(QIODevice::WriteOnly)) {
                const auto put = [&](uint32_t address, uint32_t size) {
                    const auto bytes = rope_->guestBytes(address,size);
                    file.write(reinterpret_cast<const char*>(&address),4);
                    file.write(reinterpret_cast<const char*>(&size),4);
                    file.write(reinterpret_cast<const char*>(bytes.data()),bytes.size());
                };
                put(particles_,0x1800);
                put(particleLayer_,0x200);
                put(rope_->guestWord(particles_ + 0x60),0x30 * 0x800);
            }
        }
        return Particles{rope_->particleVertices(particles_),rope_->particleUniforms(particles_)};
    });
    frame.particles = std::move(previous.vertices);
    frame.particleUniforms = std::move(previous.uniforms);
    std::copy_n(projection.constData(),16,frame.projection.begin());
    frame.view = view;
    frame.menus = menus_;
    frame.menuHour = menuHour_;
    frame.glare = constants_.glare;
    auto exposure = rope_->exposure(hdr_.at("EXPOSURE"),hdr_.at("WHITELEVEL"),hdr_.at("GLARETHRESH"),1,1.0f/255);
    frame.exposure = std::move(exposure.rgba);
    frame.noise = std::move(exposure.noise);
    requestDeformation(rope_->time() * 10.0f);
    return frame;
}
