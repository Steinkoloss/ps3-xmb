// Development: run the native core (rope, particles, surface, textures, exposure, glare) for
// a number of updates without Qt or a GPU and print a hash of every result, to compare
// builds across compilers and operating systems bit for bit.
//   core_check DATA_DIR [frames]      DATA_DIR as written by tools/import_data.py
#include "RopeSolver.h"
#include "SurfaceEvaluator.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>

namespace {
// lines/NAME.mnu, then the "day" preset's override (as OriginalSimulation reads them).
std::string linesDirectory;
std::map<std::string, float> menu(const std::string& name) {
    std::map<std::string, float> values;
    for (const auto& path : {linesDirectory + name + ".mnu", linesDirectory + "override/day/" + name + ".mnu"}) {
        std::ifstream file(path);
        if (!file) continue;
        for (std::string line; std::getline(file, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            const auto a = line.find(':'), b = line.find(':', a + 1);
            if (line.empty() || line[0] == '#' || a == std::string::npos || b == std::string::npos) continue;
            std::string key;
            for (char c : line.substr(0, a)) if (c != ' ') key += c;
            values[key] = std::stof(line.substr(b + 1));
        }
    }
    if (values.empty()) throw std::runtime_error("Missing menu " + name);
    return values;
}
std::map<std::string, float> particleMenu(const std::string& name) {
    std::map<std::string, float> values;
    for (auto [key, value] : menu(name)) {
        std::string k;
        for (char c : key) if (c != '_') k += char(std::tolower(static_cast<unsigned char>(c)));
        values[k] = value;
    }
    return values;
}
struct Hash {
    uint64_t h = 1469598103934665603ull;
    void add(const void* data, size_t size) {
        for (size_t i = 0; i < size; ++i) { h ^= static_cast<const uint8_t*>(data)[i]; h *= 1099511628211ull; }
    }
    template <class T> void all(const T& values) { add(values.data(), values.size() * sizeof(values[0])); }
};
}

int main(int argc, char** argv) try {
    if (argc < 2) { std::fprintf(stderr, "usage: core_check DATA_DIR [frames]\n"); return 2; }
    const std::string data = argv[1];
    linesDirectory = data + "/reference/lines/";
    const unsigned frames = argc > 2 ? unsigned(std::atoi(argv[2])) : 600;
    const auto line = menu("LINE1"), hdr = menu("HDR");
    RopeSolver rope(data + "/cpu/renderer-image.bin", data + "/cpu/math-image.bin");
    for (const char* key : {"DAMPING","LENGTH","TENSION","SPACING","THINNESS","BRIGHTNESS","MIPMAPBIAS",
             "FRESNEL","FALLOFF","TIMESTEP","PERTURBATION","ENDX","ENDY","ENDZ","FFDPARAM1"})
        rope.parameter(key, line.at(key));
    rope.reset(unsigned(line.at("STATE")));
    Hash setup;
    const auto model = rope.modelMatrix({line.at("POSX"),line.at("POSY"),line.at("POSZ"),1},
        {line.at("ANGX"),line.at("ANGY"),line.at("ANGZ"),0}, line.at("ANGROT"));
    setup.all(model);
    for (const auto& texture : rope.textures()) setup.all(texture.rgba);
    const auto exposure = rope.exposure(1.0f, 1.5f, 0.5f, 2.2f, 1.0f);
    for (const auto& table : exposure.rgba) setup.all(table);
    setup.all(exposure.noise.rgba);
    const auto glare = rope.glareWeights({hdr.at("GAUSSIANRADR"),hdr.at("GAUSSIANRADG"),hdr.at("GAUSSIANRADB"),0},
        hdr.at("GLARESUMPOW"), hdr.at("GLARELEVEL"));
    setup.add(&glare, sizeof glare);
    const auto system = rope.createParticles(2048);
    rope.particleParameters(system, particleMenu("PARTICLES"), particleMenu("PARTICLES_UI"),
        particleMenu("PARTICLES_SPE"));
    const auto layer = rope.createParticleLayer(system);
    SurfaceEvaluator surface("");
    std::array<SurfaceEvaluator::Vector, 128> controls;
    for (unsigned z = 0; z < 4; ++z) for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 8; ++x)
        controls[(z * 4 + y) * 8 + x] = {float(x) * 0.125f, float(y) * 0.25f, float(z) * 0.25f, 0};
    surface.lattice(controls, {line.at("FFDSCALE1X"),line.at("FFDSCALE1Y"),line.at("FFDSCALE1Z"),1},
        {line.at("FFDSCALE2X"),line.at("FFDSCALE2Y"),line.at("FFDSCALE2Z"),1},
        {line.at("FFDOFFSETX"),line.at("FFDOFFSETY"),line.at("FFDOFFSETZ"),1});
    std::printf("setup %016llx\n", static_cast<unsigned long long>(setup.h));
    Hash run;
    unsigned live = 0;
    for (unsigned frame = 1; frame <= frames; ++frame) {
        rope.advance(1.0 / 60.0);
        const auto nodes = rope.positions();
        run.all(nodes);
        SurfaceEvaluator::Grid grid;
        for (unsigned i = 0; i < grid.size(); ++i) grid[i] = nodes[i];
        const auto mesh = surface.subdivide(surface.project(grid, rope.multiply(rope.cameraView(), model)));
        run.all(mesh);
        live = rope.particleFrame(system, layer, mesh, 128, 128, model, 16.0f / 9.0f, float(frame) / 60.0f);
        run.all(rope.particleVertices(system));
        const auto uniforms = rope.particleUniforms(system);
        run.all(uniforms.camera);
        run.all(uniforms.lightPack);
        if (frame % 100 == 0 || frame == frames)
            std::printf("frame %u: %016llx (%u particles)\n", frame, static_cast<unsigned long long>(run.h), live);
    }
    return 0;
} catch (const std::exception& error) {
    std::fprintf(stderr, "core_check: %s\n", error.what());
    return 1;
}
