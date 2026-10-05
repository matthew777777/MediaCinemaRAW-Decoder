// SPDX-License-Identifier: GPL-3.0-only
//
// Interop suite: encode with the external MediaCinemaRAW-Encoder checkout,
// decode with this repo. Built by tools/verify_interop.py (or the CMake
// MEDIACINEMARAW_ENCODER_DIR option), which provides both include paths;
// encoder sources are never vendored here.
#include <MediaCinemaRAW/ContainerReader.h>
#include <MediaCinemaRAW/ContainerWriter.h>
#include <MediaCinemaRAW/Decoder.h>
#include <MediaCinemaRAW/Encoder.h>

#include <cassert>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr char kScratchContainer[] = "interop.mcraw";

// Expected value of a 4x-downscaled sample: the rounded mean of four
// same-colour source samples around (x, y).
uint16_t ExpectedBinned(const std::vector<uint16_t>& src, int fullWidth, int cropTop, int x,
                        int y) {
    const int sx = x / 2 * 4 + x % 2;
    const int sy = cropTop + y / 2 * 4 + y % 2;
    const unsigned a = src[static_cast<size_t>(sy) * fullWidth + sx];
    const unsigned b = src[static_cast<size_t>(sy) * fullWidth + sx + 2];
    const unsigned c = src[static_cast<size_t>(sy + 2) * fullWidth + sx];
    const unsigned d = src[static_cast<size_t>(sy + 2) * fullWidth + sx + 2];
    return static_cast<uint16_t>((a + b + c + d + 2) / 4);
}

// One matrix cell: random source at the mode's value span, strided RAW16 or
// packed RAW10 plane, encode with crop, decode, compare pixel-for-pixel.
void RunPayloadCase(bool packed10, bool downscale, int width, int mode, std::mt19937& rng) {
    constexpr int kFullHeight = 24;
    constexpr int kCropTop = 2;
    constexpr int kCropHeight = 16;
    const size_t rowBytes =
        packed10 ? static_cast<size_t>(width) / 4 * 5 : static_cast<size_t>(width) * 2;
    const int stride = static_cast<int>(rowBytes) + 24;
    std::vector<uint16_t> src(static_cast<size_t>(width) * kFullHeight);
    const unsigned spans[] = {1, 2, 4, 8, 16, 32, 64, 256, 1024, 4096, 16384, 65536};
    for (auto& v : src) {
        v = packed10 ? static_cast<uint16_t>(rng() % std::min(1024u, spans[mode]))
                     : static_cast<uint16_t>(rng() % spans[mode]);
        if (mode == 0)
            v = packed10 ? 800 : 50000;
    }
    std::vector<uint8_t> raw(static_cast<size_t>(kFullHeight - 1) * stride + rowBytes, 0xAD);
    for (int y = 0; y < kFullHeight; ++y) {
        for (int x = 0; x < width; ++x) {
            const uint16_t v = src[static_cast<size_t>(y) * width + x];
            if (packed10) {
                raw[static_cast<size_t>(y) * stride + x / 4 * 5 + x % 4] =
                    static_cast<uint8_t>(v >> 2);
                if (x % 4 == 0)
                    raw[static_cast<size_t>(y) * stride + x / 4 * 5 + 4] = 0;
                raw[static_cast<size_t>(y) * stride + x / 4 * 5 + 4] |=
                    static_cast<uint8_t>((v & 3) << (2 * (x % 4)));
            } else {
                raw[static_cast<size_t>(y) * stride + x * 2] = static_cast<uint8_t>(v);
                raw[static_cast<size_t>(y) * stride + x * 2 + 1] = static_cast<uint8_t>(v >> 8);
            }
        }
    }
    std::vector<uint8_t> blob;
    mediacinemaraw::encode(raw.data(), raw.size(), width, kFullHeight, stride, packed10, kCropTop,
                           kCropHeight, downscale, blob);
    const int decodedWidth = downscale ? width / 2 : width;
    const int decodedHeight = downscale ? kCropHeight / 2 : kCropHeight;
    std::vector<uint16_t> got;
    mediacinemaraw::decode(blob.data(), blob.size(), decodedWidth, decodedHeight, got);
    assert(got.size() == static_cast<size_t>(decodedWidth) * decodedHeight);
    for (int y = 0; y < decodedHeight; ++y) {
        for (int x = 0; x < decodedWidth; ++x) {
            const uint16_t want = downscale ? ExpectedBinned(src, width, kCropTop, x, y)
                                            : src[static_cast<size_t>(kCropTop + y) * width + x];
            assert(got[static_cast<size_t>(y) * decodedWidth + x] == want);
        }
    }
}

int TestPayloadRoundTrips() {
    std::mt19937 rng(9182);
    int done = 0;
    const int widths[] = {4, 64, 68, 128, 260};
    for (bool packed10 : {false, true}) {
        for (bool downscale : {false, true}) {
            for (int width : widths) {
                for (int mode = 0; mode < 12; ++mode) {
                    RunPayloadCase(packed10, downscale, width, mode, rng);
                    ++done;
                }
            }
        }
    }
    return done;
}

// Argument validation mirrors the encoder contract.
void TestArgumentValidation() {
    std::vector<uint8_t> raw(64 * 8 * 2, 255);
    for (int bad = 0; bad < 3; ++bad) {
        bool threw = false;
        try {
            std::vector<uint16_t> pixels;
            mediacinemaraw::decode(raw.data(), bad == 0 ? 10 : raw.size(), bad == 1 ? 65 : 64,
                                   bad == 2 ? 6 : 8, pixels);
        } catch (const std::exception&) {
            threw = true;
        }
        assert(threw);
    }
}

// Container round-trip: writer from the encoder checkout, reader local.
void TestContainerRoundTrip() {
    constexpr int kWidth = 64, kHeight = 8, kStride = 128;
    std::vector<uint8_t> raw(static_cast<size_t>(kHeight - 1) * kStride + kWidth * 2);
    for (int y = 0; y < kHeight; ++y)
        for (int x = 0; x < kWidth; ++x) {
            const uint16_t v = static_cast<uint16_t>((x * 17 + y * 31) & 4095);
            raw[static_cast<size_t>(y) * kStride + x * 2] = static_cast<uint8_t>(v);
            raw[static_cast<size_t>(y) * kStride + x * 2 + 1] = static_cast<uint8_t>(v >> 8);
        }
    std::vector<uint8_t> blob;
    mediacinemaraw::encode(raw.data(), raw.size(), kWidth, kHeight, kStride, false, 0, kHeight,
                           false, blob);
    const std::string containerMeta = "{\"manufacturer\":\"Example\",\"model\":\"Camera 1\","
                                      "\"UniqueCameraModel\":\"Example Camera 1\","
                                      "\"extraData\":{\"audioSampleRate\":48000,\"audioChannels\":2}}";
    {
        mediacinemaraw::ContainerWriter writer(kScratchContainer, containerMeta);
        writer.writeFrame(blob, 1000000000LL,
                          "{\"width\":64,\"height\":8,\"compressionType\":7}");
        writer.writeFrame(blob, 1033333333LL,
                          "{\"width\":64,\"height\":8,\"compressionType\":7}");
        const int16_t pcm[4] = {1, -2, 3, -4};
        writer.writeAudio(pcm, 4, 1001000000LL);
        mediacinemaraw::GyroSample gyro[1] = {{1002000000LL, 0.1f, -0.2f, 0.3f}};
        writer.writeGyro(gyro, 1);
        writer.close();
        assert(writer.frameCount() == 2);
    }
    mediacinemaraw::ContainerReader reader(kScratchContainer);
    assert(reader.frameTimestamps().size() == 2);
    assert(reader.audioSampleRateHz() == 48000);
    assert(reader.numAudioChannels() == 2);
    for (auto ts : reader.frameTimestamps()) {
        mediacinemaraw::Frame frame;
        reader.loadFrame(ts, frame);
        assert(frame.width == 64 && frame.height == 8);
        for (int y = 0; y < kHeight; ++y)
            for (int x = 0; x < kWidth; ++x)
                assert(frame.pixels[static_cast<size_t>(y) * kWidth + x] ==
                       static_cast<uint16_t>((x * 17 + y * 31) & 4095));
    }
    std::vector<mediacinemaraw::AudioChunk> chunks;
    reader.loadAudio(chunks);
    assert(chunks.size() == 1 && chunks[0].samples.size() == 4);
    assert(reader.hasGyroData());
    std::vector<mediacinemaraw::MotionSample> gyro;
    reader.loadGyroData(gyro);
    assert(gyro.size() == 1);
    std::remove(kScratchContainer);
}

} // namespace

int main() {
    // Drop leftovers from an aborted run so the container suite starts clean.
    std::remove(kScratchContainer);
    const int done = TestPayloadRoundTrips();
    std::puts("payload round-trips passed");
    TestArgumentValidation();
    std::puts("argument validation passed");
    TestContainerRoundTrip();
    std::puts("container round-trip passed");
    std::cout << done << " encode/decode round-trips passed\n";
    return 0;
}
