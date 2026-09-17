// SPDX-License-Identifier: GPL-3.0-only
//
// Self-contained decoder validation. Hand-built fixtures only;
// the exhaustive bit-width/stride/crop matrix lives in InteropTests
// which runs against an external encoder checkout.

#include <MediaCinemaRAW/ContainerReader.h>
#include <MediaCinemaRAW/Decoder.h>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void pushU32(std::vector<uint8_t>& out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out.push_back(static_cast<uint8_t>(v >> (8 * i)));
}

// All-zero 64x4 payload: no pixel bytes, both metadata lists trivial.
std::vector<uint8_t> zeroPayload() {
    std::vector<uint8_t> p;
    pushU32(p, 64);
    pushU32(p, 4);
    pushU32(p, 16);
    pushU32(p, 22);
    pushU32(p, 64);
    p.push_back(0x00);
    p.push_back(0x00);
    pushU32(p, 64);
    p.push_back(0x00);
    p.push_back(0x00);
    return p;
}

// Constant-one 64x4 payload (see header comment for packing).
std::vector<uint8_t> onePayload() {
    std::vector<uint8_t> p;
    pushU32(p, 64);
    pushU32(p, 4);
    pushU32(p, 16);
    pushU32(p, 22);
    pushU32(p, 64);
    p.push_back(0x00);
    p.push_back(0x00);
    pushU32(p, 64);
    p.push_back(0x10);
    p.push_back(0x00);
    p.insert(p.end(), {0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00});
    return p;
}

void writeItem(std::ofstream& f, uint32_t type, uint32_t size) {
    uint8_t h[8];
    for (int i = 0; i < 4; ++i) h[i] = static_cast<uint8_t>(type >> (8 * i));
    for (int i = 0; i < 4; ++i) h[4 + i] = static_cast<uint8_t>(size >> (8 * i));
    f.write(reinterpret_cast<char*>(h), 8);
}

void writeU32(std::ofstream& f, uint32_t v) {
    uint8_t b[4];
    for (int i = 0; i < 4; ++i) b[i] = static_cast<uint8_t>(v >> (8 * i));
    f.write(reinterpret_cast<char*>(b), 4);
}

void writeI64(std::ofstream& f, int64_t v) {
    uint8_t b[8];
    uint64_t u = 0;
    std::memcpy(&u, &v, 8);
    for (int i = 0; i < 8; ++i) b[i] = static_cast<uint8_t>(u >> (8 * i));
    f.write(reinterpret_cast<char*>(b), 8);
}

void writeFloat(std::ofstream& f, float v) {
    uint32_t u = 0;
    std::memcpy(&u, &v, 4);
    writeU32(f, u);
}

int64_t tellOf(std::ofstream& f) {
    return static_cast<int64_t>(f.tellp());
}

}  // namespace

int main() {
    // --- Frame-level fixtures ---
    {
        std::vector<uint16_t> out;
        mediacinemaraw::decode(zeroPayload(), 64, 4, out);
        assert(out.size() == 256);
        for (auto v : out) assert(v == 0);
    }
    {
        std::vector<uint16_t> out;
        mediacinemaraw::decode(onePayload(), 64, 4, out);
        assert(out.size() == 256);
        for (auto v : out) assert(v == 1);
    }
    {
        int pw = 0, ph = 0;
        assert(mediacinemaraw::queryPayloadDimensions(zeroPayload().data(),
                                                      zeroPayload().size(), pw,
                                                      ph));
        assert(pw == 64 && ph == 4);
        assert(!mediacinemaraw::queryPayloadDimensions(nullptr, 0, pw, ph));
        assert(!mediacinemaraw::queryPayloadDimensions(zeroPayload().data(), 3,
                                                      pw, ph));
    }
    {
        bool threw = false;
        try {
            std::vector<uint16_t> out;
            auto p = zeroPayload();
            p.resize(10);
            mediacinemaraw::decode(p, 64, 4, out);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
    }
    {
        bool threw = false;
        try {
            std::vector<uint16_t> out;
            mediacinemaraw::decode(zeroPayload(), 128, 4, out);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
    }
    {
        bool threw = false;
        try {
            std::vector<uint16_t> out;
            mediacinemaraw::decode(nullptr, 0, 64, 4, out);
        } catch (const std::invalid_argument&) {
            threw = true;
        }
        assert(threw);
    }

    // --- Container round-trip with a hand-built file ---
    const std::string containerJson =
        "{\"manufacturer\":\"Example\",\"model\":\"Camera 1\","
        "\"UniqueCameraModel\":\"Example Camera 1\","
        "\"extraData\":{\"audioSampleRate\":48000,\"audioChannels\":2}}";
    const std::string frameJson =
        "{\"width\":64,\"height\":4,\"compressionType\":7}";
    const auto payload = zeroPayload();
    const int64_t frameTs = 1000000000LL;
    const int64_t audioTs = 1001000000LL;
    const int64_t gyroTs = 1002000000LL;
    const int64_t accelTs = 1003000000LL;

    {
        std::ofstream f("test.mcraw", std::ios::binary | std::ios::trunc);
        assert(static_cast<bool>(f));
        const char hdr[8] = {'M', 'O', 'T', 'I', 'O', 'N', ' ', 3};
        f.write(hdr, 8);
        writeItem(f, 3, static_cast<uint32_t>(containerJson.size()));
        f.write(containerJson.data(),
                static_cast<std::streamsize>(containerJson.size()));

        const int64_t frameOff = tellOf(f);
        writeItem(f, 2, static_cast<uint32_t>(payload.size()));
        f.write(reinterpret_cast<const char*>(payload.data()),
                static_cast<std::streamsize>(payload.size()));
        writeItem(f, 3, static_cast<uint32_t>(frameJson.size()));
        f.write(frameJson.data(),
                static_cast<std::streamsize>(frameJson.size()));

        const int64_t audioOff = tellOf(f);
        const int16_t pcm[2] = {100, -200};
        writeItem(f, 5, 4);
        f.write(reinterpret_cast<const char*>(pcm), 4);
        writeItem(f, 6, 8);
        writeI64(f, audioTs);

        const int64_t gyroOff = tellOf(f);
        writeItem(f, 9, 8 + 24);
        writeU32(f, 1);
        writeU32(f, 1);
        writeI64(f, gyroTs);
        writeFloat(f, 0.1f);
        writeFloat(f, -0.2f);
        writeFloat(f, 0.3f);
        writeU32(f, 0);

        const int64_t accelOff = tellOf(f);
        writeItem(f, 13, 8 + 24);
        writeU32(f, 1);
        writeU32(f, 1);
        writeI64(f, accelTs);
        writeFloat(f, 0.0f);
        writeFloat(f, 9.81f);
        writeFloat(f, 0.0f);
        writeU32(f, 0);

        // OIS pair must be skipped without breaking accel discovery.
        writeItem(f, 10, 8);
        writeU32(f, 1);
        writeU32(f, 0);
        writeItem(f, 11, 4);
        writeU32(f, 0);

        writeItem(f, 4, 16 + 16);
        writeI64(f, 1);
        writeI64(f, audioTs / 1000000);
        writeI64(f, audioOff);
        writeI64(f, audioTs);

        writeItem(f, 8, 8 + 16);
        writeU32(f, 1);
        writeU32(f, 1);
        writeI64(f, gyroOff);
        writeI64(f, gyroTs);

        writeItem(f, 12, 8 + 16);
        writeU32(f, 1);
        writeU32(f, 1);
        writeI64(f, accelOff);
        writeI64(f, accelTs);

        writeItem(f, 1, 16);
        const int64_t listOff = tellOf(f);
        writeI64(f, frameOff);
        writeI64(f, frameTs);

        writeItem(f, 0, 16);
        writeU32(f, 0x8A905612);
        writeU32(f, 1);
        writeI64(f, listOff);
        f.flush();
        assert(static_cast<bool>(f));
    }
    {
        mediacinemaraw::ContainerReader reader("test.mcraw");
        assert(reader.containerMetadata().find("Example Camera 1") !=
               std::string::npos);
        assert(reader.frameTimestamps().size() == 1);
        assert(reader.frameTimestamps()[0] == frameTs);

        mediacinemaraw::Frame frame;
        reader.loadFrame(frameTs, frame);
        assert(frame.width == 64 && frame.height == 4);
        assert(frame.pixels.size() == 256);
        for (auto v : frame.pixels) assert(v == 0);
        assert(frame.metadata.find("\"compressionType\":7") !=
               std::string::npos);

        std::string meta;
        reader.loadFrameMetadata(frameTs, meta);
        assert(meta == frameJson);

        assert(reader.audioSampleRateHz() == 48000);
        assert(reader.numAudioChannels() == 2);
        std::vector<mediacinemaraw::AudioChunk> audio;
        reader.loadAudio(audio);
        assert(audio.size() == 1);
        assert(audio[0].timestampNs == audioTs);
        assert(audio[0].samples.size() == 2);
        assert(audio[0].samples[0] == 100 && audio[0].samples[1] == -200);

        assert(reader.hasGyroData());
        std::vector<mediacinemaraw::MotionSample> gyro;
        reader.loadGyroData(gyro);
        assert(gyro.size() == 1);
        assert(gyro[0].timestampNs == gyroTs);
        assert(std::fabs(gyro[0].x - 0.1f) < 1e-6f);

        assert(reader.hasAccelerometerData());
        std::vector<mediacinemaraw::MotionSample> accel;
        reader.loadAccelerometerData(accel);
        assert(accel.size() == 1);
        assert(accel[0].timestampNs == accelTs);
        assert(std::fabs(accel[0].y - 9.81f) < 1e-4f);

        bool threw = false;
        try {
            mediacinemaraw::Frame bad;
            reader.loadFrame(12345, bad);
        } catch (const std::runtime_error&) {
            threw = true;
        }
        assert(threw);
    }
    std::remove("test.mcraw");

    std::cout << "decoder self-tests passed\n";
    return 0;
}
