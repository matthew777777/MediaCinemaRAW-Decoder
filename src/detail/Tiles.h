// SPDX-License-Identifier: GPL-3.0-only
//
// Private payload decoding: header validation plus the 64x4 tile loop that
// turns packed per-channel deltas back into Bayer rows.
#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "BitUnpacking.h"
#include "Endian.h"

namespace mediacinemaraw {
namespace detail {

// Validated payload header: section offsets plus the derived tile grid.
struct PayloadLayout {
    int paddedWidth;   // Width rounded up to the 64-pixel tile stride.
    int height;        // Visible height; the payload stores no padding rows.
    size_t bitsOffset; // Start of the bit-width metadata stream.
    size_t refsOffset; // Start of the reference metadata stream.
    int tilesX;        // Padded width in 64-pixel tiles.
    int tilesY;        // Height in 4-row tiles.
    size_t channelCount; // tilesX * tilesY * 4 Bayer channels.
};

// Checks the caller geometry and the payload header. Throws
// std::invalid_argument for bad caller geometry (null payload, odd width,
// bad height) and std::runtime_error when the header disagrees with the
// visible dimensions or carries inconsistent section offsets.
inline PayloadLayout ParsePayloadLayout(const uint8_t* payload, size_t length, int width,
                                        int height) {
    if (payload == nullptr)
        throw std::invalid_argument("null payload");
    if (width <= 0 || height <= 0 || width > 65536 || height > 65536 || (width & 1) ||
        (height % 4 != 0))
        throw std::invalid_argument("invalid visible dimensions");
    if (length < 16)
        throw std::runtime_error("payload too short");

    const uint32_t storedPadded = LoadU32LE(payload);
    const uint32_t storedHeight = LoadU32LE(payload + 4);
    const uint32_t bitsOffset = LoadU32LE(payload + 8);
    const uint32_t refsOffset = LoadU32LE(payload + 12);

    const uint32_t wantPadded = static_cast<uint32_t>((width + 63) / 64 * kTileWidth);
    if (storedPadded != wantPadded)
        throw std::runtime_error("padded width mismatch");
    if (storedHeight != static_cast<uint32_t>(height))
        throw std::runtime_error("height mismatch");
    if (bitsOffset < 16 || refsOffset < bitsOffset || refsOffset > length || bitsOffset > length)
        throw std::runtime_error("invalid payload offsets");

    const int tilesX = (width + 63) >> 6; // == paddedWidth / 64.
    const int tilesY = height / kTileHeight;
    if (tilesX <= 0 || tilesY <= 0)
        throw std::runtime_error("bad tiling");
    const size_t channels = static_cast<size_t>(tilesX) * static_cast<size_t>(tilesY) * 4;
    return {static_cast<int>(storedPadded), height, bitsOffset, refsOffset, tilesX, tilesY, channels};
}

// Unpacks the tile payload in [16, bitsOffset) into row-major Bayer samples.
// `bitWidths`/`references` hold one entry per channel in (y, x, channel)
// order. Columns at x >= visibleWidth are padding and are discarded.
inline void DecodeTileRows(const uint8_t* payload, const PayloadLayout& layout,
                           const std::vector<uint16_t>& bitWidths,
                           const std::vector<uint16_t>& references, int visibleWidth,
                           std::vector<uint16_t>& out) {
    for (size_t i = 0; i < layout.channelCount; ++i)
        if (!IsSupportedBitWidth(bitWidths[i]))
            throw std::runtime_error("unsupported block width");

    out.assign(static_cast<size_t>(visibleWidth) * static_cast<size_t>(layout.height), 0);

    size_t pos = 16;
    uint16_t deltas[kChannelSamples];
    size_t channel = 0;
    for (int ty = 0; ty < layout.tilesY; ++ty) {
        for (int tx = 0; tx < layout.tilesX; ++tx) {
            const int tileX = tx * kTileWidth;
            const int tileY = ty * kTileHeight;
            for (int c = 0; c < 4; ++c, ++channel) {
                const int bits = bitWidths[channel];
                const uint16_t lo = references[channel];
                const size_t need = PackedBlockSize(bits);
                if (pos + need > layout.bitsOffset)
                    throw std::runtime_error("pixel data overrun");
                UnpackChannelDeltas(payload + pos, layout.bitsOffset - pos, deltas, bits);
                pos += need;
                CheckDeltaRange(deltas, bits);
                // Channel c covers the tile samples with x % 2 == c % 2 and
                // y % 2 == c / 2; sample i walks 32 column pairs, then the
                // next row pair.
                const int cx = c % 2;
                const int cy = c / 2;
                for (int i = 0; i < kChannelSamples; ++i) {
                    const int px = tileX + (i % 32) * 2 + cx;
                    if (px >= visibleWidth)
                        continue; // Padded columns.
                    const int py = tileY + (i / 32) * 2 + cy;
                    const uint32_t v = static_cast<uint32_t>(deltas[i]) + lo;
                    if (v > 0xFFFF)
                        throw std::runtime_error("pixel overflow");
                    out[static_cast<size_t>(py) * visibleWidth + px] = static_cast<uint16_t>(v);
                }
            }
        }
    }
    if (pos != layout.bitsOffset)
        throw std::runtime_error("pixel section mismatch");
}

} // namespace detail
} // namespace mediacinemaraw
