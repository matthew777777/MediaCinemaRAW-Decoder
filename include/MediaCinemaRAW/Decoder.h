#pragma once
// SPDX-License-Identifier: GPL-3.0-only
//
// Independent MediaCinemaRAW frame decoder for compression type 7.
// Written independently; it inverts the documented packing described in
// MediaCinemaRAW-Encoder.

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mediacinemaraw {

// Decode one type-7 frame payload into row-major uint16 samples.
//
// `width`/`height` are the visible dimensions (from the frame JSON
// metadata). The payload header stores the padded width
// `padded = (width + 63) / 64 * 64` and the same height; padding
// columns are discarded. Throws std::invalid_argument on malformed
// geometry and std::runtime_error on truncated payloads.
void decode(const uint8_t* payload, size_t length, int width, int height,
            std::vector<uint16_t>& out);

// Convenience overload for byte vectors.
inline void decode(const std::vector<uint8_t>& payload, int width, int height,
                   std::vector<uint16_t>& out) {
    decode(payload.data(), payload.size(), width, height, out);
}

// Inspect a payload header without decoding pixels.
// Returns false when the header is too short; otherwise fills the
// padded width and height stored in the payload.
bool queryPayloadDimensions(const uint8_t* payload, size_t length,
                            int& paddedWidth, int& height) noexcept;

}  // namespace mediacinemaraw
