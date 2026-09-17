#pragma once
// SPDX-License-Identifier: GPL-3.0-only
//
// Clean-room MediaCinemaRAW v3 container reader.
// Mirrors the encoder-side ContainerWriter API shape but does not share
// code with any third-party decoder. Only the C++ standard library is used;
// JSON metadata is returned as raw strings so no JSON library is required.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <MediaCinemaRAW/MotionSample.h>

namespace mediacinemaraw {

struct Frame {
    std::vector<uint16_t> pixels;
    int width = 0;
    int height = 0;
    std::string metadata;
};

struct AudioChunk {
    int64_t timestampNs = -1;
    std::vector<int16_t> samples;
};

class ContainerReader {
public:
    explicit ContainerReader(const std::string& path);
    ~ContainerReader();

    ContainerReader(const ContainerReader&) = delete;
    ContainerReader& operator=(const ContainerReader&) = delete;

    const std::string& containerMetadata() const noexcept;
    const std::vector<int64_t>& frameTimestamps() const noexcept;

    void loadFrame(int64_t timestamp, Frame& out) const;
    void loadFrameMetadata(int64_t timestamp, std::string& out) const;

    // Audio helpers parsed from container metadata extraData block.
    // Return 0 when the fields are absent.
    int audioSampleRateHz() const;
    int numAudioChannels() const;
    void loadAudio(std::vector<AudioChunk>& out) const;

    bool hasGyroData() const noexcept;
    void loadGyroData(std::vector<MotionSample>& out) const;

    bool hasAccelerometerData() const noexcept;
    void loadAccelerometerData(std::vector<MotionSample>& out) const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace mediacinemaraw
