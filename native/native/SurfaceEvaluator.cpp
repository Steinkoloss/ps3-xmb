#include "SurfaceEvaluator.h"
#include "SpuRuntime.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace {
void checked(int success) { if (!success) throw std::runtime_error(moyou_spu_error()); }
void argument(unsigned reg, uint32_t value) {
    uint32_t words[] = {value,0,0,0};
    checked(moyou_spu_set_register(reg, words));
}
void vectorArgument(unsigned reg, const SurfaceEvaluator::Vector& vector) {
    uint32_t words[4];
    for (unsigned i = 0; i < 4; ++i) words[i] = std::bit_cast<uint32_t>(vector[i]);
    checked(moyou_spu_set_register(reg, words));
}
SurfaceEvaluator::Vector result() {
    uint32_t words[4];
    checked(moyou_spu_get_register(3, words));
    SurfaceEvaluator::Vector vector;
    for (unsigned i = 0; i < 4; ++i) vector[i] = std::bit_cast<float>(words[i]);
    return vector;
}
void put(uint32_t address, const SurfaceEvaluator::Vector& vector) {
    uint8_t bytes[16];
    for (unsigned i = 0; i < 4; ++i) {
        if (!std::isfinite(vector[i])) throw std::runtime_error("Non-finite original surface input");
        const auto word = std::bit_cast<uint32_t>(vector[i]);
        for (unsigned j = 0; j < 4; ++j) bytes[i * 4 + j] = word >> (24 - j * 8);
    }
    checked(moyou_spu_write(address, bytes, sizeof bytes));
}
float word(const uint8_t* bytes) {
    return std::bit_cast<float>(uint32_t(bytes[0]) << 24 | uint32_t(bytes[1]) << 16 |
        uint32_t(bytes[2]) << 8 | bytes[3]);
}
}

SurfaceEvaluator::SurfaceEvaluator(const std::string& original) {
    checked(moyou_spu_open(0, original.c_str()));
    argument(3, 1);
    argument(4, 0xffff);
    checked(moyou_spu_call(0x5fd8, 10000));
}

void SurfaceEvaluator::lattice(const std::array<Vector, 128>& controls, Vector scale1, Vector scale2, Vector offset) {
    for (unsigned z = 0; z < 7; ++z)
        for (unsigned y = 0; y < 7; ++y)
            for (unsigned x = 0; x < 11; ++x) {
                const auto a = std::clamp(int(x) - 1, 0, 7);
                const auto b = std::clamp(int(y) - 1, 0, 3);
                const auto c = std::clamp(int(z) - 1, 0, 3);
                // Original CPU VMX multiply-add precedes border replication.
                auto control = controls[(c * 4 + b) * 8 + a];
                for (unsigned i = 0; i < 4; ++i) control[i] = std::fma(control[i], scale1[i], offset[i]);
                put(0xb380 + ((z * 7 + y) * 11 + x) * 16, control);
            }
    put(0xd590, scale1);
    put(0xd5a0, scale2);
    put(0xd5b0, offset);
    latticeReady_ = true;
}

SurfaceEvaluator::Grid SurfaceEvaluator::project(const Grid& nodes, const Matrix& matrix) {
    if (!latticeReady_) throw std::runtime_error("The original surface requires its live FFD lattice");
    for (unsigned column = 0; column < 4; ++column) {
        Vector v;
        for (unsigned row = 0; row < 4; ++row) v[row] = matrix[column * 4 + row];
        put(0xb300 + column * 16, v);
    }
    Grid projected;
    for (unsigned node = 0; node < 361; ++node) {
        vectorArgument(3, nodes[node]);
        checked(moyou_spu_call(0x3c68, 20000));
        vectorArgument(4, result());
        argument(3, 0xb300);
        checked(moyou_spu_call(0x3bf0, 1000));
        projected[node] = result();
    }
    return projected;
}

SurfaceEvaluator::Mesh SurfaceEvaluator::subdivide(const Grid& grid) {
    for (unsigned node = 0; node < grid.size(); ++node) put(0x9c00 + node * 16, grid[node]);
    Mesh mesh(128 * 128);
    for (unsigned patch = 0; patch < 256; ++patch) {
        const uint8_t index[] = {0,0,0,static_cast<uint8_t>(patch)};
        checked(moyou_spu_write(0xe640, index, sizeof index));
        argument(3, 0x12000);
        checked(moyou_spu_call(0x45c0, 20000));
        uint8_t vertices[0x800];
        checked(moyou_spu_read(0x12000, vertices, sizeof vertices));
        for (unsigned y = 0; y < 8; ++y)
            for (unsigned x = 0; x < 8; ++x) {
                auto& vertex = mesh[((patch / 16) * 8 + y) * 128 + (patch % 16) * 8 + x];
                for (unsigned c = 0; c < 8; ++c) {
                    vertex[c] = word(vertices + (y * 8 + x) * 32 + c * 4);
                    if (!std::isfinite(vertex[c])) throw std::runtime_error("Non-finite original surface result");
                }
            }
    }
    return mesh;
}
