// SPDX-License-Identifier: GPL-3.0-only
//
// Private little-endian loads. The wire format is little-endian throughout,
// so every multi-byte field is read through these helpers instead of direct
// dereference; unaligned and big-endian hosts decode identically.
#pragma once

#include <cstdint>
#include <cstring>

namespace mediacinemaraw {
namespace detail {

inline uint16_t LoadU16LE(const uint8_t* p) {
    return static_cast<uint16_t>(static_cast<uint16_t>(p[0]) |
                                 (static_cast<uint16_t>(p[1]) << 8));
}

inline uint32_t LoadU32LE(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

inline int64_t LoadI64LE(const uint8_t* p) {
    uint64_t value = 0;
    for (int i = 0; i < 8; ++i)
        value |= static_cast<uint64_t>(p[i]) << (8 * i);
    int64_t out = 0;
    std::memcpy(&out, &value, 8);
    return out;
}

} // namespace detail
} // namespace mediacinemaraw
