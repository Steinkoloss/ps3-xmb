// SPDX-License-Identifier: GPL-2.0-or-later
// Cell's estimate table/algorithm from RPCS3 Emu/Cell/Common.h at
// 461db6fab39e8e4c07b18ce7bcfbfa9591f339b2 (RPCS3 contributors).
#pragma once
#include <bit>
#include <cstdint>
#include <cstring>

inline uint32_t cellRsqrtEstimate(uint32_t x, bool nj) {
    static constexpr uint32_t segments[32][3] = {
        {0x34fd00,0x568,0x00},{0x2f9700,0x4f3,0x26},{0x2aa500,0x48d,0xc8},{0x261800,0x435,0xc8},
        {0x21e400,0x3e7,0x26},{0x1dfe00,0x3a2,0x88},{0x1a5c00,0x365,0xc8},{0x16f800,0x32e,0x22},
        {0x13ca00,0x2fc,0x00},{0x10ce00,0x2d0,0x00},{0x0dfe00,0x2a8,0x00},{0x0b5700,0x283,0x26},
        {0x08d400,0x261,0xc8},{0x067300,0x243,0x26},{0x043100,0x226,0x22},{0x020b00,0x20b,0x26},
        {0x7ff400,0x7a4,0x00},{0x785200,0x700,0x00},{0x715400,0x670,0x00},{0x6ae400,0x5f2,0x88},
        {0x64f200,0x584,0x00},{0x5f6e00,0x524,0x00},{0x5a4c00,0x4cc,0x00},{0x558000,0x47e,0x22},
        {0x510200,0x43a,0x88},{0x4cca00,0x3fa,0x88},{0x48d000,0x3c2,0x88},{0x450e00,0x38e,0x22},
        {0x418200,0x35e,0x22},{0x3e2400,0x332,0x88},{0x3af200,0x30a,0x88},{0x37e800,0x2e6,0x22},
    };
    const auto sign = x & 0x80000000u;
    int32_t exponent = (x >> 23) & 255;
    auto mantissa = x & 0x7fffffu;
    if (exponent == 255) return mantissa ? x | 0x400000 : sign ? 0x7fc00000 : 0;
    if (exponent == 0 && (nj || mantissa == 0)) return sign | 0x7f800000;
    if (sign) return 0x7fc00000;
    if (exponent == 0) {
        const auto shift = std::countl_zero(mantissa) - 8;
        mantissa = (mantissa << shift) & 0x7fffff;
        exponent = 1 - static_cast<int32_t>(shift);
    }
    const auto& segment = segments[((exponent & 1) << 4) | (mantissa >> 19)];
    const auto key = (mantissa >> 9) & 0x3ff;
    return (((380 - exponent) >> 1) << 23) |
        (segment[0] - ((segment[1] * key) >> 2) - ((segment[2] >> (key & 7)) & 1));
}

inline void moyou_vrsqrtefp(void* destination, const void* source, uint32_t vscr) {
    uint8_t result[16];
    const auto* input = static_cast<const uint8_t*>(source);
    for (unsigned lane = 0; lane < 4; ++lane) {
        const auto* p = input + lane * 4;
        auto value = cellRsqrtEstimate(uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 |
            uint32_t(p[2]) << 8 | p[3], vscr & 0x10000);
        for (unsigned byte = 0; byte < 4; ++byte) result[lane * 4 + byte] = value >> (24 - byte * 8);
    }
    std::memcpy(destination, result, 16);
}
