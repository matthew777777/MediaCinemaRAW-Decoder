// SPDX-License-Identifier: GPL-3.0-only
//
// Clean-room inverse of the MediaCinemaRAW type-7 packing.
// The layout is documented here from encoder-observed behavior; no
// third-party decoder source was copied.

#include <MediaCinemaRAW/Decoder.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace mediacinemaraw {
namespace {

uint32_t loadU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

bool isSupportedWidth(int bits) {
    switch (bits) {
        case 0:
        case 1:
        case 2:
        case 3:
        case 4:
        case 5:
        case 6:
        case 8:
        case 10:
        case 16:
            return true;
        default:
            return false;
    }
}

size_t packedSize(int bits) {
    if (bits == 0) return 0;
    if (bits == 16) return 128;
    return static_cast<size_t>(bits) * 8;
}

int nibbleToBits(int nibble) {
    if (nibble == 15) return 16;
    return nibble;
}

void fail(const char* msg) { throw std::runtime_error(msg); }

// Unpack 64 delta values. Returns bytes consumed.
size_t unpackBlock(const uint8_t* src, size_t avail, uint16_t* dst, int bits) {
    const size_t need = packedSize(bits);
    if (avail < need) fail("truncated bitstream block");
    if (bits == 0) {
        for (int i = 0; i < 64; ++i) dst[i] = 0;
        return 0;
    }
    if (bits == 16) {
#if defined(__ARM_NEON)
        // Fresh fast path: pairwise little-endian load via scalar is fine,
        // keep NEON out of this path for clarity on unaligned inputs.
#endif
        for (int i = 0; i < 64; ++i)
            dst[i] = static_cast<uint16_t>(src[2 * i] | (src[2 * i + 1] << 8));
        return 128;
    }
    if (bits == 8) {
#if defined(__ARM_NEON)
        for (int i = 0; i < 64; i += 8) {
            uint8x8_t v = vld1_u8(src + i);
            vst1q_u16(dst + i, vmovl_u8(v));
        }
#else
        for (int i = 0; i < 64; ++i) dst[i] = src[i];
#endif
        return 64;
    }
    if (bits == 3) {
        for (int lane = 0; lane < 8; ++lane) {
            const uint8_t b0 = src[lane];
            const uint8_t b1 = src[8 + lane];
            const uint8_t b2 = src[16 + lane];
            const uint16_t v0 = b0 & 7;
            const uint16_t v1 = (b0 >> 3) & 7;
            const uint16_t v2lo = (b0 >> 6) & 3;
            const uint16_t v3 = b1 & 7;
            const uint16_t v4 = (b1 >> 3) & 7;
            const uint16_t v5lo = (b1 >> 6) & 3;
            const uint16_t v6 = b2 & 7;
            const uint16_t v7 = (b2 >> 3) & 7;
            const uint16_t v2 = v2lo | (((b2 >> 6) & 1) << 2);
            const uint16_t v5 = v5lo | (((b2 >> 7) & 1) << 2);
            dst[lane] = v0;
            dst[8 + lane] = v1;
            dst[16 + lane] = v2;
            dst[24 + lane] = v3;
            dst[32 + lane] = v4;
            dst[40 + lane] = v5;
            dst[48 + lane] = v6;
            dst[56 + lane] = v7;
        }
        return 24;
    }
    if (bits == 5) {
        for (int lane = 0; lane < 8; ++lane) {
            const uint8_t b0 = src[lane];
            const uint8_t b1 = src[8 + lane];
            const uint8_t b2 = src[16 + lane];
            const uint8_t b3 = src[24 + lane];
            const uint8_t b4 = src[32 + lane];
            const uint16_t v0 = b0 & 31;
            const uint16_t v1 = b1 & 31;
            const uint16_t v2 = b2 & 31;
            const uint16_t v3 = b3 & 31;
            const uint16_t v4 = b4 & 31;
            const uint16_t v5 = ((b0 >> 5) & 7) | (((b3 >> 5) & 3) << 3);
            const uint16_t v6 = ((b1 >> 5) & 7) | (((b4 >> 5) & 3) << 3);
            const uint16_t v7 = ((b2 >> 5) & 7) | (((b3 >> 7) & 1) << 3) |
                                (((b4 >> 7) & 1) << 4);
            dst[lane] = v0;
            dst[8 + lane] = v1;
            dst[16 + lane] = v2;
            dst[24 + lane] = v3;
            dst[32 + lane] = v4;
            dst[40 + lane] = v5;
            dst[48 + lane] = v6;
            dst[56 + lane] = v7;
        }
        return 40;
    }
    if (bits == 6) {
        for (int lane = 0; lane < 8; ++lane) {
            uint8_t b[6];
            for (int g = 0; g < 6; ++g) b[g] = src[g * 8 + lane];
            for (int g = 0; g < 6; ++g)
                dst[g * 8 + lane] = static_cast<uint16_t>(b[g] & 63);
            const uint16_t hi0 = static_cast<uint16_t>(((b[0] >> 6) & 3) |
                                                       (((b[1] >> 6) & 3) << 2) |
                                                       (((b[2] >> 6) & 3) << 4));
            const uint16_t hi1 = static_cast<uint16_t>(((b[3] >> 6) & 3) |
                                                       (((b[4] >> 6) & 3) << 2) |
                                                       (((b[5] >> 6) & 3) << 4));
            dst[48 + lane] = hi0;
            dst[56 + lane] = hi1;
        }
        return 48;
    }
    if (bits == 10) {
        for (int half = 0; half < 2; ++half) {
            const uint8_t* low = src + half * 40;
            const uint8_t* high = src + half * 40 + 32;
            uint16_t* out = dst + half * 32;
#if defined(__ARM_NEON)
            for (int i = 0; i < 32; i += 8)
                vst1q_u16(out + i, vmovl_u8(vld1_u8(low + i)));
#else
            for (int i = 0; i < 32; ++i) out[i] = low[i];
#endif
            for (int lane = 0; lane < 8; ++lane) {
                const uint8_t h = high[lane];
                for (int g = 0; g < 4; ++g) {
                    const uint16_t top =
                        static_cast<uint16_t>((h >> (g * 2)) & 3);
                    out[g * 8 + lane] |= static_cast<uint16_t>(top << 8);
                }
            }
        }
        return 80;
    }
    // Generic 1/2/4-bit lane-interleaved packing.
    const int groups = 8 / bits;
    const uint16_t mask = static_cast<uint16_t>((1u << bits) - 1);
    size_t pos = 0;
    for (int base = 0; base < 64; base += groups * 8) {
        for (int lane = 0; lane < 8; ++lane) {
            const uint8_t b = src[pos++];
            for (int g = 0; g < groups; ++g)
                dst[base + g * 8 + lane] =
                    static_cast<uint16_t>((b >> (g * bits)) & mask);
        }
    }
    return pos;
}

// Decode one metadata list (bits or refs). `wanted` is the number of
// entries the frame needs; the stream count is padded up to a multiple
// of 64 with zeros.
size_t decodeList(const uint8_t* src, size_t avail, size_t wanted,
                  std::vector<uint16_t>& out) {
    if (avail < 4) fail("truncated metadata count");
    const uint32_t stored = loadU32(src);
    if (stored % 64 != 0) fail("invalid metadata count");
    if (stored < wanted) fail("metadata too short");
    if (stored > 16 * 1024 * 1024) fail("metadata implausibly large");
    out.assign(stored, 0);
    size_t pos = 4;
    uint16_t tmp[64];
    for (uint32_t base = 0; base < stored; base += 64) {
        if (avail < pos + 2) fail("truncated metadata header");
        const int nibble = (src[pos] >> 4) & 15;
        const int bits = nibbleToBits(nibble);
        if (!isSupportedWidth(bits)) fail("unsupported metadata width");
        const uint16_t ref = static_cast<uint16_t>(((src[pos] & 15) << 8) |
                                                   src[pos + 1]);
        pos += 2;
        const size_t need = packedSize(bits);
        if (avail < pos + need) fail("truncated metadata block");
        unpackBlock(src + pos, avail - pos, tmp, bits);
        pos += need;
        for (int i = 0; i < 64; ++i) {
            const uint32_t v = static_cast<uint32_t>(tmp[i]) + ref;
            if (v > 0xFFFF) fail("metadata value overflow");
            out[base + i] = static_cast<uint16_t>(v);
            if (bits != 16 && tmp[i] >= (bits == 0 ? 1u : (1u << bits)))
                fail("metadata delta out of range");
        }
        if (ref > 4095) fail("metadata reference out of range");
    }
    // Padding entries must be zero (encoder pads with zeros).
    for (size_t i = wanted; i < out.size(); ++i)
        if (out[i] != 0) fail("nonzero metadata padding");
    out.resize(wanted);
    return pos;
}

}  // namespace

bool queryPayloadDimensions(const uint8_t* payload, size_t length,
                            int& paddedWidth, int& height) noexcept {
    if (!payload || length < 16) return false;
    const uint32_t ew = loadU32(payload);
    const uint32_t h = loadU32(payload + 4);
    if (ew == 0 || ew % 64 != 0 || ew > 65536 * 2) return false;
    if (h == 0 || h % 4 != 0 || h > 65536) return false;
    paddedWidth = static_cast<int>(ew);
    height = static_cast<int>(h);
    return true;
}

void decode(const uint8_t* payload, size_t length, int width, int height,
            std::vector<uint16_t>& out) {
    if (!payload) throw std::invalid_argument("null payload");
    if (width <= 0 || height <= 0 || width > 65536 || height > 65536 ||
        (width & 1) || (height % 4 != 0))
        throw std::invalid_argument("invalid visible dimensions");
    if (length < 16) throw std::runtime_error("payload too short");

    const uint32_t ew = loadU32(payload);
    const uint32_t hh = loadU32(payload + 4);
    const uint32_t offBits = loadU32(payload + 8);
    const uint32_t offRefs = loadU32(payload + 12);

    const uint32_t wantPad =
        static_cast<uint32_t>((width + 63) / 64 * 64);
    if (ew != wantPad) throw std::runtime_error("padded width mismatch");
    if (hh != static_cast<uint32_t>(height))
        throw std::runtime_error("height mismatch");
    if (offBits < 16 || offRefs < offBits || offRefs > length ||
        offBits > length)
        throw std::runtime_error("invalid payload offsets");

    const int tilesX = (width + 63) >> 6;  // == ew / 64
    const int tilesY = height / 4;
    if (tilesX <= 0 || tilesY <= 0) throw std::runtime_error("bad tiling");
    const size_t blocks =
        static_cast<size_t>(tilesX) * static_cast<size_t>(tilesY) * 4;

    std::vector<uint16_t> bitWidths;
    std::vector<uint16_t> references;
    const size_t bitsUsed =
        decodeList(payload + offBits, length - offBits, blocks, bitWidths);
    (void)bitsUsed;
    // decodeList consumes to end of payload for the second list; validate
    // that the first list ended exactly at the refs section.
    // Re-derive by decoding refs from offRefs and checking pixel data end.
    std::vector<uint16_t> refsCheck;
    decodeList(payload + offRefs, length - offRefs, blocks, refsCheck);
    references.swap(refsCheck);

    // The bits list must occupy [offBits, offRefs).
    {
        // Re-encode-free length check: decode again with a bounded view.
        std::vector<uint16_t> tmp;
        const size_t consumed =
            decodeList(payload + offBits, offRefs - offBits, blocks, tmp);
        if (offBits + consumed != offRefs)
            throw std::runtime_error("metadata section mismatch");
        bitWidths.swap(tmp);
    }

    for (size_t i = 0; i < blocks; ++i)
        if (!isSupportedWidth(bitWidths[i]))
            throw std::runtime_error("unsupported block width");

    out.assign(static_cast<size_t>(width) * static_cast<size_t>(height), 0);

    size_t pos = 16;
    uint16_t deltas[64];
    size_t block = 0;
    for (int ty = 0; ty < tilesY; ++ty) {
        for (int tx = 0; tx < tilesX; ++tx) {
            const int tileX = tx * 64;
            const int tileY = ty * 4;
            for (int c = 0; c < 4; ++c, ++block) {
                const int bits = bitWidths[block];
                const uint16_t lo = references[block];
                const size_t need = packedSize(bits);
                if (pos + need > offBits)
                    throw std::runtime_error("pixel data overrun");
                unpackBlock(payload + pos, offBits - pos, deltas, bits);
                pos += need;
#if defined(__ARM_NEON)
                if (bits != 0 && bits != 16) {
                    // Validate range in a vectorised pass (scalar fallback
                    // below does the same element-wise).
                    const uint32_t limit =
                        bits >= 16 ? 0x10000u : (1u << bits);
                    for (int i = 0; i < 64; ++i)
                        if (deltas[i] >= limit)
                            throw std::runtime_error("delta out of range");
                }
#else
                if (bits != 0 && bits != 16) {
                    const uint32_t limit = (1u << bits);
                    for (int i = 0; i < 64; ++i)
                        if (deltas[i] >= limit)
                            throw std::runtime_error("delta out of range");
                }
#endif
                const int cx = c % 2;
                const int cy = c / 2;
                for (int i = 0; i < 64; ++i) {
                    const int px = tileX + (i % 32) * 2 + cx;
                    if (px >= width) continue;  // padded columns
                    const int py = tileY + (i / 32) * 2 + cy;
                    const uint32_t v =
                        static_cast<uint32_t>(deltas[i]) + lo;
                    if (v > 0xFFFF)
                        throw std::runtime_error("pixel overflow");
                    out[static_cast<size_t>(py) * width + px] =
                        static_cast<uint16_t>(v);
                }
            }
        }
    }
    if (pos != offBits) throw std::runtime_error("pixel section mismatch");
}

}  // namespace mediacinemaraw
