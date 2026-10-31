#include "OriginalSimulation.h"
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QMatrix4x4>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <stdexcept>

std::map<QString,float> originalMenu(const QString& reference, const QString& preset, const QString& name) {
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
                                       const QString& spline, const QString& preset) {
    const auto line = originalMenu(reference,preset,"LINE1");
    hdr_ = originalMenu(reference,preset,"HDR");
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
    std::map<std::string,float> particleMenu;
    for (const auto& [key, value] : originalMenu(reference,preset,"PARTICLES"))
        particleMenu[QString(key).remove('_').toLower().toStdString()] = value;
    particles_ = rope_->createParticles(2048);
    rope_->particleParameters(particles_,particleMenu);
    particleLayer_ = rope_->createParticleLayer(particles_);
    constants_.glare = rope_->glareWeights({hdr_.at("GAUSSIANRADR"),hdr_.at("GAUSSIANRADG"),hdr_.at("GAUSSIANRADB"),0},
        hdr_.at("GLARESUMPOW"),hdr_.at("GLARELEVEL"));
}

void OriginalSimulation::deformation(float time) {
    ffd_->uniform("deformation","_Time",QJsonArray{time});
    auto values = ffd_->captureFloat(QSize(128,1));
    for (unsigned n = 0; n < 128; ++n) for (unsigned c = 0; c < 4; ++c) lattice_[n][c] = values[n * 4 + c];
}

OriginalSimulation::Frame OriginalSimulation::step(double seconds, QSize size) {
    rope_->advance(seconds);
    surface_->lattice(lattice_,scale1_,scale2_,offset_);
    QMatrix4x4 projection;
    projection.perspective(53.0f,float(size.width()) / size.height(),0.1f,1000.0f);
    RopeSolver::Matrix p;
    std::copy_n(projection.constData(),16,p.begin());
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
    // Particles emit from the world-space ribbon: inverse projection of the clip-space mesh.
    RopeSolver::Matrix columns;
    std::copy_n(projection.inverted().constData(),16,columns.begin());
    rope_->emitParticles(particleLayer_,evaluated,128,128,columns);
    rope_->stepParticles(particles_,float(seconds));
    frame.particles = rope_->particleVertices(particles_);
    frame.particleUniforms = rope_->particleUniforms(particles_,0);
    std::copy_n(projection.constData(),16,frame.projection.begin());
    auto exposure = rope_->exposure(hdr_.at("EXPOSURE"),hdr_.at("WHITELEVEL"),hdr_.at("GLARETHRESH"),1,1.0f/255);
    frame.exposure = std::move(exposure.rgba);
    frame.noise = std::move(exposure.noise);
    deformation(rope_->time() * 10.0f);
    return frame;
}
