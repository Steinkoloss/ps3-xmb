// SPDX-License-Identifier: MIT
// SPU reciprocal estimate (frest) and its interpolation step (fi), lane rules after the scalar
// references in ps3recomp runtime/spu/spu_helpers.h at 4e8f3ca8 (MIT, Ned Heller / sp00nznet);
// the fraction table is the hardware's estimate table as measured there.
#pragma once
#include <bit>
#include <cstdint>

inline uint32_t spuReciprocalEstimate(uint32_t x) {   // frest
    static constexpr uint32_t fraction[32] = {
        0x7FFBE0, 0x7F87A6, 0x70EF72, 0x708B40, 0x638B12, 0x633AEA, 0x5792C4, 0x574AA0,
        0x4CCA7E, 0x4C9262, 0x430A44, 0x42D62A, 0x3A2E12, 0x39FDFA, 0x3215E4, 0x31F1D2,
        0x2AA9BE, 0x2A85AC, 0x23D59A, 0x23BD8E, 0x1D8576, 0x1D8576, 0x17AD5A, 0x17AD5A,
        0x124543, 0x124543, 0x0D392D, 0x0D392D, 0x08851A, 0x08851A, 0x041D07, 0x041D07};
    const uint32_t exponent = (x >> 23) & 0xff;
    const uint32_t result = exponent == 0 ? 255 : exponent >= 253 ? 0 : 253 - exponent;
    return (x & 0x80000000u) | result << 23 | (fraction[(x >> 18) & 0x1f] & 0x7fffff);
}

inline uint32_t spuInterpolate(uint32_t a, uint32_t estimate) {   // fi
    int exponent = int((estimate >> 23) & 0xff);
    if (!exponent) return 0;
    const uint64_t base = (estimate >> 10) & 0x1fff, step = estimate & 0x3ff, y = a & 0x7ffff;
    uint64_t m = ((uint64_t(1) << 13 | base) << 19) - step * y;
    if (m < (uint64_t(1) << 32)) {
        m <<= 1;
        if (--exponent == 0) return 0;
    }
    return (estimate & 0x80000000u) | uint32_t(exponent) << 23 | uint32_t((m >> 9) & 0x7fffff);
}

// 1/x as SPU code computes it with frest then fi.
inline float spuReciprocal(float x) {
    const auto bits = std::bit_cast<uint32_t>(x);
    return std::bit_cast<float>(spuInterpolate(bits, spuReciprocalEstimate(bits)));
}
