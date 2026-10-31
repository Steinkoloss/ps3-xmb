#include "RopeSolver.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QTextStream>
#include <QtEndian>
#include <bit>
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
        {"output","361 x 4 float32 little-endian render-grid checkpoint.","file"}});
    parser.process(app);
    try {
        bool valid;
        const auto frames = parser.value("frames").toUInt(&valid);
        if (!valid || frames > 36000) throw std::runtime_error("Invalid bounded frame count");
        RopeSolver rope(parser.value("renderer").toStdString(), parser.value("math").toStdString());
        std::set<QString> required = {"DAMPING","LENGTH","TENSION","TIMESTEP","PERTURBATION",
            "ENDX","ENDY","ENDZ","FFDPARAM1"};
        const std::set<QString> supported = {"DAMPING","LENGTH","TENSION","SPACING","THINNESS",
            "BRIGHTNESS","MIPMAPBIAS","FRESNEL","FALLOFF","TIMESTEP","PERTURBATION","ENDX",
            "ENDY","ENDZ","FFDPARAM1"};
        unsigned state = 3;
        for (const auto& path : parser.values("settings")) {
            QFile file(path);
            if (!file.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read original menu parameters");
            for (const auto& line : file.readAll().split('\n')) {
                const auto fields = QString::fromUtf8(line).trimmed().split(':');
                if (fields.size() != 3) continue;
                auto key = fields[0]; key.remove(' ');
                if (key == "STATE") { state = fields[2].toUInt(&valid); if (!valid) state = 3; }
                if (!supported.contains(key)) continue;
                const auto value = fields[2].toFloat(&valid);
                if (!valid || fields[1] != "float") throw std::runtime_error("Invalid original float parameter");
                rope.parameter(key.toStdString(), value);
                required.erase(key);
            }
        }
        if (!required.empty()) throw std::runtime_error("The original menu is missing required simulation parameters");
        rope.reset(state);
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
