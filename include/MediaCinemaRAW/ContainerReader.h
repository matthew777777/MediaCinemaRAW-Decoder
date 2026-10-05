// SPDX-License-Identifier: GPL-3.0-only
//
// Version 3 `.mcraw` container reader: frames plus JSON metadata, timestamped
// PCM16 audio, and gyro/accelerometer motion samples.
//
// Only the C++ standard library is used; JSON metadata is returned as raw
// strings so no JSON library is required. See docs/CONTAINER.md for the wire
// layout.
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <MediaCinemaRAW/MotionSample.h>

namespace mediacinemaraw {

// One decoded frame: row-major uint16 Bayer samples (`width * height`), the
// visible dimensions, and the frame-level JSON metadata.
struct Frame {
    std::vector<uint16_t> pixels;
    int width = 0;
    int height = 0;
    std::string metadata;
};

// One PCM16 audio chunk with its nanosecond timestamp (`-1` when an old
// chunk has no timestamp item).
struct AudioChunk {
    int64_t timestampNs = -1;
    std::vector<int16_t> samples;
};

class ContainerReader {
  public:
    // Opens `path` and discovers the frame, audio, and motion indexes.
    // Throws std::runtime_error when the file cannot be opened or parsed
    // (bad magic or version, corrupt offsets, duplicate timestamps).
    explicit ContainerReader(const std::string& path);
    ~ContainerReader();

    ContainerReader(const ContainerReader&) = delete;
    ContainerReader& operator=(const ContainerReader&) = delete;

    // Container-level JSON metadata. Valid for the life of the reader.
    const std::string& containerMetadata() const noexcept;
    // Frame timestamps in increasing order. Valid for the life of the reader.
    const std::vector<int64_t>& frameTimestamps() const noexcept;

    // Decodes the frame at `timestamp` into `out`, filling pixels, visible
    // dimensions, and metadata. The frame JSON must carry `width`, `height`,
    // and `compressionType` (only type 7 is supported). Throws
    // std::runtime_error when the frame is missing or corrupt.
    void loadFrame(int64_t timestamp, Frame& out) const;
    // Loads only the frame JSON, without decoding pixels. Throws like
    // loadFrame() when the frame is missing or corrupt.
    void loadFrameMetadata(int64_t timestamp, std::string& out) const;

    // Audio helpers parsed from the container metadata extraData block.
    // Return 0 when `audioSampleRate` / `audioChannels` are absent.
    int audioSampleRateHz() const;
    int numAudioChannels() const;
    // Loads every audio chunk in file order. Throws std::runtime_error on
    // corrupt audio items (odd chunk size, bad offsets).
    void loadAudio(std::vector<AudioChunk>& out) const;

    // False when the file holds no gyro data at all.
    bool hasGyroData() const noexcept;
    // Appends every gyro sample in file order. Throws std::runtime_error on
    // corrupt gyro items (bad version, size mismatch).
    void loadGyroData(std::vector<MotionSample>& out) const;

    // False when the file holds no accelerometer data at all.
    bool hasAccelerometerData() const noexcept;
    // Appends every accelerometer sample in file order. Throws like
    // loadGyroData() on corrupt items.
    void loadAccelerometerData(std::vector<MotionSample>& out) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace mediacinemaraw
