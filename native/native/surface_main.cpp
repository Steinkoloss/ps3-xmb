#include "SurfaceEvaluator.h"
#include <QCoreApplication>
#include <QCommandLineParser>
#include <QFile>
#include <QTextStream>
#include <QtEndian>
#include <bit>
#include <stdexcept>

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("Original SPU surface checkpoint from a 19 x 19 clip-space control grid.");
    parser.addHelpOption();
    parser.addOptions({{"original","Verified original spline local store.","file"},
        {"input","361 x 4 float32 little-endian clip-space controls.","file"},
        {"output","128 x 128 x 8 float32 little-endian position/normal checkpoint.","file"}});
    parser.process(app);
    try {
        QFile input(parser.value("input"));
        if (!input.open(QIODevice::ReadOnly)) throw std::runtime_error("Cannot read original control grid");
        auto bytes = input.readAll();
        if (bytes.size() != 361 * 16) throw std::runtime_error("Wrong-sized original control grid");
        SurfaceEvaluator evaluator(parser.value("original").toStdString());
        SurfaceEvaluator::Grid grid;
        for (unsigned n = 0; n < 361; ++n) for (unsigned c = 0; c < 4; ++c)
            grid[n][c] = std::bit_cast<float>(qFromLittleEndian<uint32_t>(bytes.constData() + n * 16 + c * 4));
        auto mesh = evaluator.subdivide(grid);
        QByteArray outputBytes;
        for (const auto& vertex : mesh) for (float value : vertex) {
            auto bits = qToLittleEndian(std::bit_cast<uint32_t>(value));
            outputBytes.append(reinterpret_cast<const char*>(&bits), sizeof bits);
        }
        QFile output(parser.value("output"));
        if (!output.open(QIODevice::WriteOnly) || output.write(outputBytes) != outputBytes.size())
            throw std::runtime_error("Cannot write original surface checkpoint");
        QTextStream(stdout) << "Evaluated all 256 original spline patches\n";
        return 0;
    } catch (const std::exception& error) { QTextStream(stderr) << error.what() << '\n'; return 1; }
}
