// SPDX-License-Identifier: GPL-3.0-only
//
// Independent MediaCinemaRAW frame decoder for compression type 7. The tile
// payload, the bit-width/reference metadata streams, and the Bayer
// reinterleaving live in detail/Tiles.h; this file only validates the
// geometry, bounds each section, and runs the stages in order.
#include <MediaCinemaRAW/Decoder.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "detail/Endian.h"
#include "detail/Tiles.h"

namespace mediacinemaraw {

bool queryPayloadDimensions(const uint8_t* payload, size_t length, int& paddedWidth,
                            int& height) noexcept {
    if (payload == nullptr || length < 16)
        return false;
    const uint32_t storedPadded = detail::LoadU32LE(payload);
    const uint32_t storedHeight = detail::LoadU32LE(payload + 4);
    if (storedPadded == 0 || storedPadded % 64 != 0 || storedPadded > 65536 * 2)
        return false;
    if (storedHeight == 0 || storedHeight % 4 != 0 || storedHeight > 65536)
        return false;
    paddedWidth = static_cast<int>(storedPadded);
    height = static_cast<int>(storedHeight);
    return true;
}

void decode(const uint8_t* payload, size_t length, int width, int height,
            std::vector<uint16_t>& out) {
    const detail::PayloadLayout layout = detail::ParsePayloadLayout(payload, length, width, height);

    // The width stream must occupy [bitsOffset, refsOffset) exactly; the
    // reference stream runs from refsOffset to the end of the payload.
    std::vector<uint16_t> bitWidths;
    const size_t bitsUsed = detail::DecodeMetadataList(payload + layout.bitsOffset,
                                                       layout.refsOffset - layout.bitsOffset,
                                                       layout.channelCount, bitWidths);
    if (layout.bitsOffset + bitsUsed != layout.refsOffset)
        throw std::runtime_error("metadata section mismatch");
    std::vector<uint16_t> references;
    detail::DecodeMetadataList(payload + layout.refsOffset, length - layout.refsOffset,
                               layout.channelCount, references);

    detail::DecodeTileRows(payload, layout, bitWidths, references, width, out);
}

} // namespace mediacinemaraw
