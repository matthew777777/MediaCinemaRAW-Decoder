// SPDX-License-Identifier: GPL-3.0-only
//
// Minimal .mcraw inspection tool with no third-party dependencies.
// Dumps the first frame to PGM and the audio to WAV for quick checks.

#include <MediaCinemaRAW/ContainerReader.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

void writePgm(const std::string& path, const mediacinemaraw::Frame& frame) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open " + path);
    out << "P5\n" << frame.width << ' ' << frame.height << "\n65535\n";
    for (auto v : frame.pixels) {
        const uint8_t hi = static_cast<uint8_t>(v >> 8);
        const uint8_t lo = static_cast<uint8_t>(v & 0xFF);
        out.put(static_cast<char>(hi));
        out.put(static_cast<char>(lo));
    }
    if (!out) throw std::runtime_error("pgm write failed");
}

void writeWav(const std::string& path, int rate, int channels,
              const std::vector<mediacinemaraw::AudioChunk>& chunks) {
    size_t frames = 0;
    for (const auto& c : chunks) frames += c.samples.size() / channels;
    const uint32_t dataBytes = static_cast<uint32_t>(frames * channels * 2);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot open " + path);
    auto u32 = [&](uint32_t v) {
        for (int i = 0; i < 4; ++i)
            out.put(static_cast<char>(v >> (8 * i)));
    };
    auto u16 = [&](uint16_t v) {
        out.put(static_cast<char>(v & 0xFF));
        out.put(static_cast<char>(v >> 8));
    };
    out.write("RIFF", 4);
    u32(36 + dataBytes);
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    u32(16);
    u16(1);
    u16(static_cast<uint16_t>(channels));
    u32(static_cast<uint32_t>(rate));
    u32(static_cast<uint32_t>(rate * channels * 2));
    u16(static_cast<uint16_t>(channels * 2));
    u16(16);
    out.write("data", 4);
    u32(dataBytes);
    if (channels == 1) {
        for (const auto& c : chunks)
            for (auto s : c.samples) u16(static_cast<uint16_t>(s));
    } else {
        // Interleave per chunk assuming each chunk is already interleaved.
        for (const auto& c : chunks)
            for (auto s : c.samples) u16(static_cast<uint16_t>(s));
    }
    if (!out) throw std::runtime_error("wav write failed");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: mcraw_dump <input.mcraw> [--pgm out.pgm] "
                     "[--wav out.wav]\n";
        return 2;
    }
    try {
        mediacinemaraw::ContainerReader reader(argv[1]);
        std::cout << "container: " << reader.containerMetadata().size()
                  << " bytes of JSON\n";
        std::cout << "frames: " << reader.frameTimestamps().size() << "\n";
        std::cout << "audio rate/ch: " << reader.audioSampleRateHz() << "/"
                  << reader.numAudioChannels() << "\n";
        std::cout << "gyro: " << (reader.hasGyroData() ? "yes" : "no")
                  << " accel: "
                  << (reader.hasAccelerometerData() ? "yes" : "no") << "\n";

        std::string pgm, wav;
        for (int i = 2; i + 1 < argc; i += 2) {
            const std::string k = argv[i];
            if (k == "--pgm")
                pgm = argv[i + 1];
            else if (k == "--wav")
                wav = argv[i + 1];
            else {
                std::cerr << "unknown option " << k << "\n";
                return 2;
            }
        }

        if (!reader.frameTimestamps().empty() && !pgm.empty()) {
            mediacinemaraw::Frame first;
            reader.loadFrame(reader.frameTimestamps().front(), first);
            std::cout << "first frame " << first.width << "x"
                      << first.height << " -> " << pgm << "\n";
            writePgm(pgm, first);
        }
        if (!wav.empty()) {
            std::vector<mediacinemaraw::AudioChunk> chunks;
            reader.loadAudio(chunks);
            const int rate = reader.audioSampleRateHz() > 0
                                 ? reader.audioSampleRateHz()
                                 : 48000;
            const int ch =
                reader.numAudioChannels() > 0 ? reader.numAudioChannels() : 1;
            std::cout << "audio chunks " << chunks.size() << " -> " << wav
                      << "\n";
            writeWav(wav, rate, ch, chunks);
        }
    } catch (const std::exception& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
