// SPDX-License-Identifier: GPL-3.0-only
//
// Private inverse of the type 7 delta packing: per-channel delta unpacking
// plus the trailing bits/references metadata streams. Channels hold 64
// samples each, stored as eight interleaved lanes rather than a conventional
// contiguous bitstream; the unpackers below mirror that layout exactly.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "Endian.h"
#include "Simd.h"

namespace mediacinemaraw {
namespace detail {

// Payload geometry shared by the tile loop and the metadata streams.
constexpr int kTileWidth = 64;
constexpr int kTileHeight = 4;
constexpr int kChannelSamples = 64;
constexpr int kLaneCount = 8;
constexpr int kMetadataGroupSize = 64;
// The header's four-bit width field has no room for 16, so the 16-bit
// literal mode is spelled 15 on the wire.
constexpr int kLiteralBitWidth = 16;
constexpr int kLiteralWidthNibble = 15;

inline bool IsSupportedBitWidth(int bits) {
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

inline size_t PackedBlockSize(int bits) {
    if (bits == 0)
        return 0;
    if (bits == kLiteralBitWidth)
        return 128;
    return static_cast<size_t>(bits) * 8;
}

inline int WidthFromNibble(int nibble) {
    if (nibble == kLiteralWidthNibble)
        return kLiteralBitWidth;
    return nibble;
}

// Unpacks 64 delta values into `out`. Returns the bytes consumed.
inline size_t UnpackChannelDeltas(const uint8_t* src, size_t available, uint16_t* out, int bits) {
    const size_t need = PackedBlockSize(bits);
    if (available < need)
        throw std::runtime_error("truncated bitstream block");
    if (bits == 0) {
        for (int i = 0; i < kChannelSamples; ++i)
            out[i] = 0;
        return 0;
    }
    if (bits == kLiteralBitWidth) {
        for (int i = 0; i < kChannelSamples; ++i)
            out[i] = LoadU16LE(src + 2 * i);
        return 128;
    }
    if (bits == 8) {
        WidenU8ToU16(src, out, kChannelSamples);
        return 64;
    }
    if (bits == 3) {
        for (int lane = 0; lane < kLaneCount; ++lane) {
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
            out[lane] = v0;
            out[8 + lane] = v1;
            out[16 + lane] = v2;
            out[24 + lane] = v3;
            out[32 + lane] = v4;
            out[40 + lane] = v5;
            out[48 + lane] = v6;
            out[56 + lane] = v7;
        }
        return 24;
    }
    if (bits == 5) {
        for (int lane = 0; lane < kLaneCount; ++lane) {
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
            const uint16_t v7 =
                ((b2 >> 5) & 7) | (((b3 >> 7) & 1) << 3) | (((b4 >> 7) & 1) << 4);
            out[lane] = v0;
            out[8 + lane] = v1;
            out[16 + lane] = v2;
            out[24 + lane] = v3;
            out[32 + lane] = v4;
            out[40 + lane] = v5;
            out[48 + lane] = v6;
            out[56 + lane] = v7;
        }
        return 40;
    }
    if (bits == 6) {
        for (int lane = 0; lane < kLaneCount; ++lane) {
            uint8_t b[6];
            for (int g = 0; g < 6; ++g)
                b[g] = src[g * 8 + lane];
            for (int g = 0; g < 6; ++g)
                out[g * 8 + lane] = static_cast<uint16_t>(b[g] & 63);
            const uint16_t hi0 = static_cast<uint16_t>(((b[0] >> 6) & 3) | (((b[1] >> 6) & 3) << 2) |
                                                       (((b[2] >> 6) & 3) << 4));
            const uint16_t hi1 = static_cast<uint16_t>(((b[3] >> 6) & 3) | (((b[4] >> 6) & 3) << 2) |
                                                       (((b[5] >> 6) & 3) << 4));
            out[48 + lane] = hi0;
            out[56 + lane] = hi1;
        }
        return 48;
    }
    if (bits == 10) {
        for (int half = 0; half < 2; ++half) {
            const uint8_t* low = src + half * 40;
            const uint8_t* high = src + half * 40 + 32;
            uint16_t* halfOut = out + half * 32;
            WidenU8ToU16(low, halfOut, 32);
            for (int lane = 0; lane < kLaneCount; ++lane) {
                const uint8_t h = high[lane];
                for (int g = 0; g < 4; ++g) {
                    const uint16_t top = static_cast<uint16_t>((h >> (g * 2)) & 3);
                    halfOut[g * 8 + lane] |= static_cast<uint16_t>(top << 8);
                }
            }
        }
        return 80;
    }
    // Generic 1/2/4-bit lane-interleaved packing.
    const int groups = 8 / bits;
    const uint16_t mask = static_cast<uint16_t>((1u << bits) - 1);
    size_t pos = 0;
    for (int base = 0; base < kChannelSamples; base += groups * 8) {
        for (int lane = 0; lane < kLaneCount; ++lane) {
            const uint8_t b = src[pos++];
            for (int g = 0; g < groups; ++g)
                out[base + g * 8 + lane] = static_cast<uint16_t>((b >> (g * bits)) & mask);
        }
    }
    return pos;
}

// Range check for unpacked pixel deltas. Widths 0 and 16 carry no live range
// (nothing stored, full literals); every other width must fit its bit mask.
inline void CheckDeltaRange(const uint16_t* deltas, int bits) {
    if (bits == 0 || bits == kLiteralBitWidth)
        return;
    const uint32_t limit = 1u << bits;
    for (int i = 0; i < kChannelSamples; ++i)
        if (deltas[i] >= limit)
            throw std::runtime_error("delta out of range");
}

// Decodes one metadata list (bit widths or references). `wanted` is the
// number of entries the frame needs; the stream count is padded up to a
// multiple of 64 with zeros. Returns the bytes consumed.
inline size_t DecodeMetadataList(const uint8_t* src, size_t available, size_t wanted,
                                 std::vector<uint16_t>& out) {
    if (available < 4)
        throw std::runtime_error("truncated metadata count");
    const uint32_t stored = LoadU32LE(src);
    if (stored % kMetadataGroupSize != 0)
        throw std::runtime_error("invalid metadata count");
    if (stored < wanted)
        throw std::runtime_error("metadata too short");
    if (stored > 16 * 1024 * 1024)
        throw std::runtime_error("metadata implausibly large");
    out.assign(stored, 0);
    size_t pos = 4;
    uint16_t tmp[kChannelSamples];
    for (uint32_t base = 0; base < stored; base += kMetadataGroupSize) {
        if (available < pos + 2)
            throw std::runtime_error("truncated metadata header");
        const int bits = WidthFromNibble((src[pos] >> 4) & 15);
        if (!IsSupportedBitWidth(bits))
            throw std::runtime_error("unsupported metadata width");
        const uint16_t ref = static_cast<uint16_t>(((src[pos] & 15) << 8) | src[pos + 1]);
        pos += 2;
        const size_t need = PackedBlockSize(bits);
        if (available < pos + need)
            throw std::runtime_error("truncated metadata block");
        UnpackChannelDeltas(src + pos, available - pos, tmp, bits);
        pos += need;
        for (int i = 0; i < kChannelSamples; ++i) {
            const uint32_t v = static_cast<uint32_t>(tmp[i]) + ref;
            if (v > 0xFFFF)
                throw std::runtime_error("metadata value overflow");
            out[base + i] = static_cast<uint16_t>(v);
            if (bits != kLiteralBitWidth && tmp[i] >= (bits == 0 ? 1u : (1u << bits)))
                throw std::runtime_error("metadata delta out of range");
        }
        if (ref > 4095)
            throw std::runtime_error("metadata reference out of range");
    }
    // Padding entries must be zero (the encoder pads with zeros).
    for (size_t i = wanted; i < out.size(); ++i)
        if (out[i] != 0)
            throw std::runtime_error("nonzero metadata padding");
    out.resize(wanted);
    return pos;
}

} // namespace detail
} // namespace mediacinemaraw
