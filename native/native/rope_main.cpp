#include "RopeSolver.h"
#include <cstdio>
#include <cstdlib>
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QDir>
#include <QImage>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QTextStream>
#include <QtEndian>
#include <bit>
#include <map>
#include <set>
#include <stdexcept>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("Original CPU wave simulation checkpoint; GPU FFD remains a separate rendering stage.");
    parser.addHelpOption();
    parser.addOptions({{"renderer","Verified relocated renderer image.","file"},
        {"math","Verified original VSH math image.","file"},
        {"settings","Original LINE1.mnu and optional override, in order.","file"},
        {"frames","Number of original 60 Hz frame updates.","count","1"},
        {"assets","Export original procedural LUTs and model matrix.","directory"},
        {"output","361 x 4 float32 little-endian render-grid checkpoint.","file"},
        {"particles","Development: construct the original particle system and report.","capacity"},
        {"noise-check","Compare native exposure noise with the original for N frames.","frames"},
        {"sincos-check","Compare native _FSin with the math module for N inputs.","count"}});
    parser.process(app);
    try {
        bool valid;
        const auto frames = parser.value("frames").toUInt(&valid);
        if (!valid || frames > 36000) throw std::runtime_error("Invalid bounded frame count");
        RopeSolver rope(parser.value("renderer").toStdString(), parser.value("math").toStdString());
        if (parser.isSet("noise-check")) {
            rope.exposure(0.6f, 1.2f, 0.4f, 1, 1.0f / 255);
            if (!rope.checkExposureNoise(parser.value("noise-check").toUInt())) throw std::runtime_error("Native exposure noise differs");
            std::printf("exposure noise identical\n");
            return 0;
        }
        if (parser.isSet("sincos-check")) {
            const auto mismatches = rope.checkSinCos(parser.value("sincos-check").toUInt());
            std::printf("_FSin mismatches: %u\n", mismatches);
            return mismatches ? 1 : 0;
        }
        if (parser.isSet("particles")) {
            const auto system = rope.createParticles(parser.value("particles").toUInt());
            const auto layer = rope.createParticleLayer(system);
            std::printf("particle system at %08x, layer at %08x\n", system, layer);
            return 0;
        }
        std::set<QString> required = {"DAMPING","LENGTH","TENSION","TIMESTEP","PERTURBATION",
            "ENDX","ENDY","ENDZ","FFDPARAM1"};
        const std::set<QString> supported = {"DAMPING","LENGTH","TENSION","SPACING","THINNESS",
            "BRIGHTNESS","MIPMAPBIAS","FRESNEL","FALLOFF","TIMESTEP","PERTURBATION","ENDX",
            "ENDY","ENDZ","FFDPARAM1"};
        unsigned state = 3;
        std::map<QString, float> parameters;
        for (const auto& path : parser.values("settings")) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read original menu parameters");
            for (const auto& line : file.readAll().split('\n')) {
                const auto fields = QString::fromUtf8(line).trimmed().split(':');
                if (fields.size() != 3) continue;
                auto key = fields[0]; key.remove(' ');
                if (key == "STATE") { state = fields[2].toUInt(&valid); if (!valid) state = 3; }
                if (fields[1] == "float") parameters[key] = fields[2].toFloat();
                if (!supported.contains(key)) continue;
                const auto value = fields[2].toFloat(&valid);
                if (!valid || fields[1] != "float") throw std::runtime_error("Invalid original float parameter");
                rope.parameter(key.toStdString(), value);
                required.erase(key);
            }
        }
        if (!required.empty()) throw std::runtime_error("The original menu is missing required simulation parameters");
        rope.reset(state);
        if (parser.isSet("assets")) {
            QDir directory(parser.value("assets"));
            if (!directory.mkpath(".")) throw std::runtime_error("Cannot create original asset output");
            auto textures = rope.textures();
            for (unsigned i = 0; i < textures.size(); ++i) {
                const auto& t = textures[i];
                QImage image(t.rgba.data(), int(t.width), int(t.height), int(t.width * 4), QImage::Format_RGBA8888);
                if (!image.save(directory.filePath(i == 0 ? "stripes.png" : "encode.png")))
                    throw std::runtime_error("Cannot save original procedural texture");
            }
            auto matrix = rope.modelMatrix({parameters.at("POSX"),parameters.at("POSY"),parameters.at("POSZ"),1},
                {parameters.at("ANGX"),parameters.at("ANGY"),parameters.at("ANGZ"),0}, parameters.at("ANGROT"));
            QJsonArray values;
            for (auto value : matrix) values.append(value);
            QFile metadata(directory.filePath("model.json"));
            if (!metadata.open(QIODevice::WriteOnly) ||
                metadata.write(QJsonDocument(QJsonObject{{"model",values}}).toJson()) < 0)
                throw std::runtime_error("Cannot save original model matrix");
        }
        for (unsigned frame = 0; frame < frames; ++frame) rope.advance(1.0 / 60.0);
        auto nodes = rope.positions();
        QByteArray bytes;
        for (const auto& node : nodes) for (float value : node) {
            auto bits = qToLittleEndian(std::bit_cast<uint32_t>(value));
            bytes.append(reinterpret_cast<const char*>(&bits), sizeof bits);
        }
        QFile output(parser.value("output"));
        if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size())
            throw std::runtime_error("Cannot write original wave checkpoint");
        QTextStream(stdout) << "Original wave frame " << frames << ", simulation time " << rope.time()
            << ", random state " << rope.randomState() << '\n';
        return 0;
    } catch (const std::exception& error) {
        QTextStream(stderr) << error.what() << '\n';
        return 1;
    }
}
