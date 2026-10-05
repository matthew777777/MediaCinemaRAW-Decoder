// SPDX-License-Identifier: GPL-3.0-only
//
// Private container format constants and capacity backstops for the version
// 3 `.mcraw` reader. Item payload layouts are described in
// docs/CONTAINER.md; the per-kind dispatch lives in ContainerReader.cpp.
#pragma once

#include <cstddef>
#include <cstdint>

namespace mediacinemaraw {
namespace detail {

constexpr char kFileMagic[8] = {'M', 'O', 'T', 'I', 'O', 'N', ' ', 3};
constexpr uint32_t kFooterMagic = 0x8A905612;
constexpr uint32_t kMotionVersion = 1;
constexpr int kCompressionType7 = 7;

// One motion sample on the wire: int64 timestamp + 3x float32 axes +
// uint32 reserved, 24 bytes total.
constexpr size_t kMotionSampleWireSize = 24;
constexpr size_t kMotionHeaderSize = 8; // Version + sample count.

// Structural sizes: item headers, the footer payload (magic, frame count,
// frame-list offset), frame index entries (offset, timestamp), the audio
// index header (chunk count, start timestamp), and audio timestamps.
constexpr size_t kItemHeaderSize = 8;
constexpr size_t kFooterPayloadSize = 16;
constexpr size_t kFrameIndexEntrySize = 16;
constexpr size_t kAudioIndexHeaderSize = 16;
constexpr size_t kAudioTimestampSize = 8;

// Capacity backstops. Corrupt size fields must fail fast instead of driving
// huge allocations; legitimate clips stay far below every limit here.
constexpr size_t kMaxContainerMetadata = 8 * 1024 * 1024;
constexpr size_t kMaxFrameMetadata = 4 * 1024 * 1024;
constexpr size_t kMaxFramePayload = 256 * 1024 * 1024;
constexpr size_t kMaxAudioChunk = 64 * 1024 * 1024;
constexpr size_t kMaxMotionData = 256 * 1024 * 1024;
constexpr size_t kMaxScanItem = 1u << 30;
constexpr uint32_t kMaxFrameCount = 4000000;
constexpr int64_t kMaxAudioChunks = 1000000;
constexpr uint32_t kMaxMotionChunks = 100000;

} // namespace detail
} // namespace mediacinemaraw
