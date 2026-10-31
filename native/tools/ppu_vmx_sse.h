/* ppu_vmx_sse.h -- SSE/AVX2 versions of the hot VMX helpers for lifted PPU code.
 *
 * lift_rope.py appends this to the staged ps3recomp ppu_vmx.h and renames the scalar
 * definitions of the same instructions to *_ref. Semantics are those of the _ref
 * helpers bit for bit (registers stay big-endian byte arrays; VSCR[NJ] flushes
 * denormal inputs and results; NaN rules as documented in ppu_vmx.h). Requires
 * x86-64-v3 (AVX2, FMA).
 */
#ifndef PPU_VMX_SSE_H
#define PPU_VMX_SSE_H

#include <immintrin.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VMX_SSE static inline __attribute__((always_inline))

/* Big-endian 16-byte register <-> four native 32-bit lanes (element i in lane i). */
VMX_SSE __m128i vmx_sse_load(const void* p) {
    const __m128i swap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    return _mm_shuffle_epi8(_mm_loadu_si128((const __m128i*)p), swap);
}
VMX_SSE void vmx_sse_store(void* p, __m128i v) {
    const __m128i swap = _mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
    _mm_storeu_si128((__m128i*)p, _mm_shuffle_epi8(v, swap));
}
/* VSCR[NJ]: a denormal (zero exponent) becomes a zero of the same sign. */
VMX_SSE __m128i vmx_sse_flush(__m128i x, uint32_t vscr) {
    if (!(vscr & VMX_NJ)) return x;
    const __m128i exponent = _mm_and_si128(x, _mm_set1_epi32(0x7F800000));
    const __m128i zero = _mm_cmpeq_epi32(exponent, _mm_setzero_si128());
    return _mm_blendv_epi8(x, _mm_and_si128(x, _mm_set1_epi32((int)0x80000000u)), zero);
}
VMX_SSE __m128i vmx_sse_isnan(__m128i x) {
    return _mm_cmpgt_epi32(_mm_and_si128(x, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F800000));
}
/* vmx_fix: any NaN result becomes the default QNaN. */
VMX_SSE __m128i vmx_sse_fix(__m128i r) {
    return _mm_blendv_epi8(r, _mm_set1_epi32(0x7FC00000), vmx_sse_isnan(r));
}
/* vmx_vnan2: A's NaN (quieted), else B's NaN (quieted), else vmx_fix(r). */
VMX_SSE __m128i vmx_sse_nan2(__m128i r, __m128i a, __m128i b) {
    const __m128i quiet = _mm_set1_epi32(0x7FC00000);
    r = vmx_sse_fix(r);
    r = _mm_blendv_epi8(r, _mm_or_si128(b, quiet), vmx_sse_isnan(b));
    return _mm_blendv_epi8(r, _mm_or_si128(a, quiet), vmx_sse_isnan(a));
}

VMX_SSE void vmx_vmaddfp(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t vscr) {
    const __m128 a = _mm_castsi128_ps(vmx_sse_flush(vmx_sse_load(a_), vscr));
    const __m128 b = _mm_castsi128_ps(vmx_sse_flush(vmx_sse_load(b_), vscr));
    const __m128 c = _mm_castsi128_ps(vmx_sse_flush(vmx_sse_load(c_), vscr));
    const __m128i r = vmx_sse_fix(_mm_castps_si128(_mm_fmadd_ps(a, c, b)));
    vmx_sse_store(d_, vmx_sse_flush(r, vscr));
}
/* -(a*c - b): negate the fused a*c - b so a zero difference keeps the reference sign. */
VMX_SSE void vmx_vnmsubfp(VMX_D, VMX_A, VMX_B, VMX_C, uint32_t vscr) {
    const __m128 a = _mm_castsi128_ps(vmx_sse_flush(vmx_sse_load(a_), vscr));
    const __m128 b = _mm_castsi128_ps(vmx_sse_flush(vmx_sse_load(b_), vscr));
    const __m128 c = _mm_castsi128_ps(vmx_sse_flush(vmx_sse_load(c_), vscr));
    const __m128i difference = _mm_castps_si128(_mm_fmsub_ps(a, c, b));
    const __m128i r = vmx_sse_fix(_mm_xor_si128(difference, _mm_set1_epi32((int)0x80000000u)));
    vmx_sse_store(d_, vmx_sse_flush(r, vscr));
}
VMX_SSE void vmx_vaddfp(VMX_D, VMX_A, VMX_B, uint32_t vscr) {
    const __m128i a = vmx_sse_flush(vmx_sse_load(a_), vscr), b = vmx_sse_flush(vmx_sse_load(b_), vscr);
    const __m128i r = _mm_castps_si128(_mm_add_ps(_mm_castsi128_ps(a), _mm_castsi128_ps(b)));
    vmx_sse_store(d_, vmx_sse_flush(vmx_sse_nan2(r, a, b), vscr));
}
VMX_SSE void vmx_vsubfp(VMX_D, VMX_A, VMX_B, uint32_t vscr) {
    const __m128i a = vmx_sse_flush(vmx_sse_load(a_), vscr), b = vmx_sse_flush(vmx_sse_load(b_), vscr);
    const __m128i r = _mm_castps_si128(_mm_sub_ps(_mm_castsi128_ps(a), _mm_castsi128_ps(b)));
    vmx_sse_store(d_, vmx_sse_flush(vmx_sse_nan2(r, a, b), vscr));
}
VMX_SSE void vmx_vslw(VMX_D, VMX_A, VMX_B) {
    const __m128i count = _mm_and_si128(vmx_sse_load(b_), _mm_set1_epi32(31));
    vmx_sse_store(d_, _mm_sllv_epi32(vmx_sse_load(a_), count));
}
/* vperm on big-endian byte arrays: d[i] = (a || b)[c[i] & 31]. */
VMX_SSE void vmx_vperm(void* d_, const void* a_, const void* b_, const void* c_) {
    const __m128i a = _mm_loadu_si128((const __m128i*)a_), b = _mm_loadu_si128((const __m128i*)b_);
    const __m128i c = _mm_loadu_si128((const __m128i*)c_);
    const __m128i index = _mm_and_si128(c, _mm_set1_epi8(15));
    const __m128i fromB = _mm_cmpeq_epi8(_mm_and_si128(c, _mm_set1_epi8(16)), _mm_set1_epi8(16));
    _mm_storeu_si128((__m128i*)d_, _mm_blendv_epi8(_mm_shuffle_epi8(a, index), _mm_shuffle_epi8(b, index), fromB));
}

#ifdef __cplusplus
}
#endif
#endif /* PPU_VMX_SSE_H */
