// SPDX-License-Identifier: GPL-3.0-only
//
// Interop suite: encode with the external MediaCinemaRAW-Encoder checkout,
// decode with this repo. Built by tools/verify_interop.py which provides
// both include paths; never vendors encoder sources.

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

uint16_t expectedBin(const std::vector<uint16_t>& src, int fullW, int top,
                     int x, int y) {
    const int sx = x / 2 * 4 + x % 2;
    const int sy = top + y / 2 * 4 + y % 2;
    const unsigned a = src[static_cast<size_t>(sy) * fullW + sx];
    const unsigned b = src[static_cast<size_t>(sy) * fullW + sx + 2];
    const unsigned c = src[static_cast<size_t>(sy + 2) * fullW + sx];
    const unsigned d = src[static_cast<size_t>(sy + 2) * fullW + sx + 2];
    return static_cast<uint16_t>((a + b + c + d + 2) / 4);
}

}  // namespace

int main() {
    std::mt19937 rng(9182);
    int done = 0;
    const int widths[] = {4, 64, 68, 128, 260};
    for (bool packed10 : {false, true}) {
        for (bool downscale : {false, true}) {
            for (int w : widths) {
                for (int mode = 0; mode < 12; ++mode) {
                    const int fullH = 24;
                    const int cropTop = 2;
                    const int cropH = 16;
                    const size_t rowBytes =
                        packed10 ? static_cast<size_t>(w) / 4 * 5
                                 : static_cast<size_t>(w) * 2;
                    const int stride = static_cast<int>(rowBytes) + 24;
                    std::vector<uint16_t> src(
                        static_cast<size_t>(w) * fullH);
                    const unsigned spans[] = {1,   2,   4,   8,   16,  32,
                                              64,  256, 1024, 4096, 16384, 65536};
                    for (auto& v : src) {
                        v = packed10
                                ? static_cast<uint16_t>(rng() %
                                                        std::min(1024u, spans[mode]))
                                : static_cast<uint16_t>(rng() % spans[mode]);
                        if (mode == 0) v = packed10 ? 800 : 50000;
                    }
                    std::vector<uint8_t> raw(
                        static_cast<size_t>(fullH - 1) * stride + rowBytes,
                        0xAD);
                    for (int y = 0; y < fullH; ++y) {
                        for (int x = 0; x < w; ++x) {
                            const uint16_t v =
                                src[static_cast<size_t>(y) * w + x];
                            if (packed10) {
                                raw[static_cast<size_t>(y) * stride + x / 4 * 5 +
                                    x % 4] = static_cast<uint8_t>(v >> 2);
                                if (x % 4 == 0)
                                    raw[static_cast<size_t>(y) * stride +
                                        x / 4 * 5 + 4] = 0;
                                raw[static_cast<size_t>(y) * stride + x / 4 * 5 +
                                    4] |= static_cast<uint8_t>((v & 3)
                                                               << (2 * (x % 4)));
                            } else {
                                raw[static_cast<size_t>(y) * stride + x * 2] =
                                    static_cast<uint8_t>(v);
                                raw[static_cast<size_t>(y) * stride + x * 2 +
                                    1] = static_cast<uint8_t>(v >> 8);
                            }
                        }
                    }
                    std::vector<uint8_t> blob;
                    mediacinemaraw::encode(raw.data(), raw.size(), w, fullH,
                                           stride, packed10, cropTop, cropH,
                                           downscale, blob);
                    const int dw = downscale ? w / 2 : w;
                    const int dh = downscale ? cropH / 2 : cropH;
                    std::vector<uint16_t> got;
                    mediacinemaraw::decode(blob.data(), blob.size(), dw, dh,
                                           got);
                    assert(got.size() ==
                           static_cast<size_t>(dw) * dh);
                    for (int y = 0; y < dh; ++y) {
                        for (int x = 0; x < dw; ++x) {
                            uint16_t want;
                            if (downscale)
                                want = expectedBin(src, w, cropTop, x, y);
                            else
                                want = src[static_cast<size_t>(cropTop + y) *
                                               w +
                                           x];
                            assert(got[static_cast<size_t>(y) * dw + x] ==
                                   want);
                        }
                    }
                    ++done;
                }
            }
        }
    }

    // Argument validation mirrors the encoder contract.
    {
        std::vector<uint8_t> raw(64 * 8 * 2, 255);
        for (int bad = 0; bad < 3; ++bad) {
            bool threw = false;
            try {
                std::vector<uint16_t> pixels;
                mediacinemaraw::decode(bad == 0 ? raw.data() : raw.data(),
                                       bad == 0 ? 10 : raw.size(),
                                       bad == 1 ? 65 : 64,
                                       bad == 2 ? 6 : 8, pixels);
            } catch (const std::exception&) {
                threw = true;
            }
            assert(threw);
        }
    }

    // Container round-trip: writer from encoder checkout, reader local.
    {
        const int w = 64, h = 8, stride = 128;
        std::vector<uint8_t> raw(static_cast<size_t>(h - 1) * stride + w * 2);
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                const uint16_t v =
                    static_cast<uint16_t>((x * 17 + y * 31) & 4095);
                raw[static_cast<size_t>(y) * stride + x * 2] =
                    static_cast<uint8_t>(v);
                raw[static_cast<size_t>(y) * stride + x * 2 + 1] =
                    static_cast<uint8_t>(v >> 8);
            }
        std::vector<uint8_t> blob;
        mediacinemaraw::encode(raw.data(), raw.size(), w, h, stride, false, 0,
                               h, false, blob);
        const std::string containerMeta =
            "{\"manufacturer\":\"Example\",\"model\":\"Camera 1\","
            "\"UniqueCameraModel\":\"Example Camera 1\","
            "\"extraData\":{\"audioSampleRate\":48000,\"audioChannels\":2}}";
        {
            mediacinemaraw::ContainerWriter writer("interop.mcraw",
                                                   containerMeta);
            writer.writeFrame(blob, 1000000000LL,
                              "{\"width\":64,\"height\":8,\"compressionType\":7}");
            writer.writeFrame(blob, 1033333333LL,
                              "{\"width\":64,\"height\":8,\"compressionType\":7}");
            const int16_t pcm[4] = {1, -2, 3, -4};
            writer.writeAudio(pcm, 4, 1001000000LL);
            mediacinemaraw::GyroSample gyro[1] = {{1002000000LL, 0.1f, -0.2f,
                                                   0.3f}};
            writer.writeGyro(gyro, 1);
            writer.close();
            assert(writer.frameCount() == 2);
        }
        mediacinemaraw::ContainerReader reader("interop.mcraw");
        assert(reader.frameTimestamps().size() == 2);
        assert(reader.audioSampleRateHz() == 48000);
        assert(reader.numAudioChannels() == 2);
        for (auto ts : reader.frameTimestamps()) {
            mediacinemaraw::Frame fr;
            reader.loadFrame(ts, fr);
            assert(fr.width == 64 && fr.height == 8);
            for (int y = 0; y < h; ++y)
                for (int x = 0; x < w; ++x)
                    assert(fr.pixels[static_cast<size_t>(y) * w + x] ==
                           static_cast<uint16_t>((x * 17 + y * 31) & 4095));
        }
        std::vector<mediacinemaraw::AudioChunk> chunks;
        reader.loadAudio(chunks);
        assert(chunks.size() == 1 && chunks[0].samples.size() == 4);
        assert(reader.hasGyroData());
        std::vector<mediacinemaraw::MotionSample> gyro;
        reader.loadGyroData(gyro);
        assert(gyro.size() == 1);
        std::remove("interop.mcraw");
    }

    std::cout << done << " encode/decode round-trips passed\n";
    return 0;
}
