// SPDX-License-Identifier: GPL-2.0-or-later
// Big-endian guest memory accessors (defined in RopeSolver.cpp). Builds with the private
// original translations take these declarations from the translation's header instead.
#pragma once
#include <cstdint>

extern "C" {
uint8_t vm_read8(uint64_t address);
uint16_t vm_read16(uint64_t address);
uint32_t vm_read32(uint64_t address);
uint64_t vm_read64(uint64_t address);
void vm_write8(uint64_t address, uint8_t value);
void vm_write16(uint64_t address, uint16_t value);
void vm_write32(uint64_t address, uint32_t value);
void vm_write64(uint64_t address, uint64_t value);
}

// The relocated custom_render_plugin image (tools/renderer_image.py), firmware 4.93.
constexpr unsigned renderer_image_size = 666192;
constexpr uint8_t renderer_image_digest[32] = {
    0x31,0xb3,0xc6,0xfe,0x7b,0x31,0x5f,0x94,0xec,0xd7,0x92,0x70,0x28,0xc5,0xe1,0xba,
    0xc5,0xe6,0xd1,0xc5,0x99,0xb2,0xcd,0x8a,0x5a,0x62,0x66,0x0e,0xe1,0x3e,0x38,0xe4};
