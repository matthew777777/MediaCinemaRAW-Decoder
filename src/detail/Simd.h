// SPDX-License-Identifier: GPL-3.0-only
//
// Private SIMD helpers for the unpacking paths. NEON on ARM, SSE2 on x86,
// portable scalar fallback elsewhere. All loads/stores are unaligned; the
// 16-bit literal path keeps scalar little-endian loads so it stays correct
// on big-endian hosts too.
#pragma once

#include <cstdint>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#define MCRAW_USE_NEON 1
#elif defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define MCRAW_USE_SSE2 1
#endif

namespace mediacinemaraw {
namespace detail {

// Zero-extends `count` bytes to 16-bit lanes. Used by the 8-bit blocks and
// the low-byte halves of the 10-bit blocks.
inline void WidenU8ToU16(const uint8_t* src, uint16_t* dst, int count) {
#if defined(MCRAW_USE_NEON)
    int i = 0;
    for (; i + 8 <= count; i += 8)
        vst1q_u16(dst + i, vmovl_u8(vld1_u8(src + i)));
    for (; i < count; ++i)
        dst[i] = src[i];
#elif defined(MCRAW_USE_SSE2)
    int i = 0;
    for (; i + 8 <= count; i += 8) {
        __m128i low = _mm_loadl_epi64(reinterpret_cast<const __m128i*>(src + i));
        _mm_storeu_si128(reinterpret_cast<__m128i*>(dst + i),
                         _mm_unpacklo_epi8(low, _mm_setzero_si128()));
    }
    for (; i < count; ++i)
        dst[i] = src[i];
#else
    for (int i = 0; i < count; ++i)
        dst[i] = src[i];
#endif
}

} // namespace detail
} // namespace mediacinemaraw
