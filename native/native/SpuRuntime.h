#pragma once
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// 0 = spline, 1 = particles. State is per thread: open on one thread, publish its
// prepared context, attach other threads to a copy of it.
int moyou_spu_open(unsigned program, const char* image);
int moyou_spu_publish(void);
int moyou_spu_attach(void);
int moyou_spu_call(uint32_t entry, uint64_t budget);
int moyou_spu_write(uint32_t address, const void* bytes, size_t size);
int moyou_spu_read(uint32_t address, void* bytes, size_t size);
int moyou_spu_set_register(unsigned reg, const uint32_t words[4]);
int moyou_spu_get_register(unsigned reg, uint32_t words[4]);
const char* moyou_spu_error(void);
uint64_t moyou_spu_instructions(void);
#ifdef __cplusplus
}
#endif
