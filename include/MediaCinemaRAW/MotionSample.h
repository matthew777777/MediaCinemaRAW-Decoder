// SPDX-License-Identifier: GPL-3.0-only
//
// Motion-sample layout for MediaCinemaRAW containers: 24 bytes on the wire
// (int64 timestamp, three float axes, one uint32 reserved word), matching
// what ContainerReader memcpys from gyro/accelerometer data items.
#pragma once

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace mediacinemaraw {

struct MotionSample {
    int64_t timestampNs = 0;
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
    uint32_t reserved = 0;
};

static_assert(sizeof(MotionSample) == 24, "MotionSample must be 24 bytes");
static_assert(offsetof(MotionSample, timestampNs) == 0, "timestamp offset");
static_assert(offsetof(MotionSample, x) == 8, "x offset");
static_assert(offsetof(MotionSample, y) == 12, "y offset");
static_assert(offsetof(MotionSample, z) == 16, "z offset");
static_assert(offsetof(MotionSample, reserved) == 20, "reserved offset");
static_assert(std::is_standard_layout_v<MotionSample>, "standard layout");
static_assert(std::is_trivially_copyable_v<MotionSample>, "trivially copyable");

} // namespace mediacinemaraw
