// SPDX-License-Identifier: GPL-3.0-only
//
// Lossless decoder for MediaCinemaRAW compression type 7 frame payloads.
//
// Decodes one payload into row-major uint16 Bayer samples with no demosaic,
// black-level subtraction, scaling, or orientation applied. See
// docs/FORMAT.md for the wire layout.
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace mediacinemaraw {

// Decodes one type 7 frame payload.
//
// `width`/`height` are the visible dimensions from the frame JSON metadata.
// The payload header stores the padded width `(width + 63) / 64 * 64` and
// the same height; padding columns are discarded. `out` is resized to
// `width * height` samples in Bayer order.
//
// The visible dimensions must satisfy: even width, height a multiple of
// four. Throws std::invalid_argument on malformed caller geometry (null
// payload, odd width, bad height) and std::runtime_error on corrupt content
// (truncated payload, padded-width or height mismatch, bad section offsets,
// unsupported block width).
void decode(const uint8_t* payload, size_t length, int width, int height,
            std::vector<uint16_t>& out);

// Convenience overload for byte vectors. Equivalent to the pointer form.
inline void decode(const std::vector<uint8_t>& payload, int width, int height,
                   std::vector<uint16_t>& out) {
    decode(payload.data(), payload.size(), width, height, out);
}

// Inspects a payload header without decoding pixels. Returns false when the
// header is missing or implausible; otherwise fills the padded width and
// height stored in the payload. Never throws.
bool queryPayloadDimensions(const uint8_t* payload, size_t length, int& paddedWidth,
                            int& height) noexcept;

} // namespace mediacinemaraw
