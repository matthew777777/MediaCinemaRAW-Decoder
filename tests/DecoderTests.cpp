// SPDX-License-Identifier: GPL-3.0-only
//
// Self-contained decoder validation. Assert based with no test framework
// dependency; the CMake build compiles with -UNDEBUG so the checks stay live
// in Release builds. Fixtures are hand-built, except for the golden gradient
// payload, which was produced once by the sibling encoder and pins decoding
// of a real 10-bit payload byte-for-byte. The exhaustive
// bit-width/stride/crop matrix lives in InteropTests, which runs against an
// external encoder checkout.
#include <MediaCinemaRAW/ContainerReader.h>
#include <MediaCinemaRAW/Decoder.h>
#include <MediaCinemaRAW/LensShading.h>

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr char kScratchContainer[] = "test.mcraw";

// ---------------------------------------------------------------------------
// Little-endian fixture helpers.
// ---------------------------------------------------------------------------

void PushU32(std::vector<uint8_t>& out, uint32_t value) {
    for (int i = 0; i < 4; ++i)
        out.push_back(static_cast<uint8_t>(value >> (8 * i)));
}

uint64_t Fnv1aHash(const void* data, size_t size) {
    uint64_t hash = 1469598103934665603ULL;
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}

uint64_t Fnv1aHash(const std::vector<uint16_t>& pixels) {
    return Fnv1aHash(pixels.data(), pixels.size() * sizeof(uint16_t));
}

// ---------------------------------------------------------------------------
// Payload fixtures.
// ---------------------------------------------------------------------------

// All-zero 64x4 payload: no pixel bytes, both metadata lists trivial.
std::vector<uint8_t> ZeroPayload() {
    std::vector<uint8_t> payload;
    PushU32(payload, 64);
    PushU32(payload, 4);
    PushU32(payload, 16);
    PushU32(payload, 22);
    PushU32(payload, 64);
    payload.push_back(0x00);
    payload.push_back(0x00);
    PushU32(payload, 64);
    payload.push_back(0x00);
    payload.push_back(0x00);
    return payload;
}

// Constant-one 64x4 payload: trivial widths, references packed as width-1
// deltas of 1 over a zero ref.
std::vector<uint8_t> OnePayload() {
    std::vector<uint8_t> payload;
    PushU32(payload, 64);
    PushU32(payload, 4);
    PushU32(payload, 16);
    PushU32(payload, 22);
    PushU32(payload, 64);
    payload.push_back(0x00);
    payload.push_back(0x00);
    PushU32(payload, 64);
    payload.push_back(0x10);
    payload.push_back(0x00);
    payload.insert(payload.end(), {0x01, 0x01, 0x01, 0x01, 0x00, 0x00, 0x00, 0x00});
    return payload;
}

// Golden 64x4 gradient payload, encoded once by the sibling encoder from
// RAW16 pixels `(x * 17 + y * 31) & 4095`. Exercises 10-bit channels with
// non-trivial width/reference streams.
const uint8_t kGoldenGradient[] = {
    0x40, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00, 0x00, 0x10, 0x02, 0x00, 0x00,
    0x3e, 0x02, 0x00, 0x00, 0x00, 0x00, 0x22, 0x00, 0x44, 0x00, 0x66, 0x00,
    0x88, 0x00, 0xaa, 0x00, 0xcc, 0x00, 0xee, 0x00, 0x10, 0x01, 0x32, 0x01,
    0x54, 0x01, 0x76, 0x01, 0x98, 0x01, 0xba, 0x01, 0xdc, 0x01, 0xfe, 0x01,
    0x20, 0x02, 0x42, 0x02, 0x64, 0x02, 0x86, 0x02, 0xa8, 0x02, 0xca, 0x02,
    0xec, 0x02, 0x0e, 0x03, 0x30, 0x03, 0x52, 0x03, 0x74, 0x03, 0x96, 0x03,
    0xb8, 0x03, 0xda, 0x03, 0xfc, 0x03, 0x1e, 0x04, 0x3e, 0x00, 0x60, 0x00,
    0x82, 0x00, 0xa4, 0x00, 0xc6, 0x00, 0xe8, 0x00, 0x0a, 0x01, 0x2c, 0x01,
    0x4e, 0x01, 0x70, 0x01, 0x92, 0x01, 0xb4, 0x01, 0xd6, 0x01, 0xf8, 0x01,
    0x1a, 0x02, 0x3c, 0x02, 0x5e, 0x02, 0x80, 0x02, 0xa2, 0x02, 0xc4, 0x02,
    0xe6, 0x02, 0x08, 0x03, 0x2a, 0x03, 0x4c, 0x03, 0x6e, 0x03, 0x90, 0x03,
    0xb2, 0x03, 0xd4, 0x03, 0xf6, 0x03, 0x18, 0x04, 0x3a, 0x04, 0x5c, 0x04,
    0x00, 0x00, 0x22, 0x00, 0x44, 0x00, 0x66, 0x00, 0x88, 0x00, 0xaa, 0x00,
    0xcc, 0x00, 0xee, 0x00, 0x10, 0x01, 0x32, 0x01, 0x54, 0x01, 0x76, 0x01,
    0x98, 0x01, 0xba, 0x01, 0xdc, 0x01, 0xfe, 0x01, 0x20, 0x02, 0x42, 0x02,
    0x64, 0x02, 0x86, 0x02, 0xa8, 0x02, 0xca, 0x02, 0xec, 0x02, 0x0e, 0x03,
    0x30, 0x03, 0x52, 0x03, 0x74, 0x03, 0x96, 0x03, 0xb8, 0x03, 0xda, 0x03,
    0xfc, 0x03, 0x1e, 0x04, 0x3e, 0x00, 0x60, 0x00, 0x82, 0x00, 0xa4, 0x00,
    0xc6, 0x00, 0xe8, 0x00, 0x0a, 0x01, 0x2c, 0x01, 0x4e, 0x01, 0x70, 0x01,
    0x92, 0x01, 0xb4, 0x01, 0xd6, 0x01, 0xf8, 0x01, 0x1a, 0x02, 0x3c, 0x02,
    0x5e, 0x02, 0x80, 0x02, 0xa2, 0x02, 0xc4, 0x02, 0xe6, 0x02, 0x08, 0x03,
    0x2a, 0x03, 0x4c, 0x03, 0x6e, 0x03, 0x90, 0x03, 0xb2, 0x03, 0xd4, 0x03,
    0xf6, 0x03, 0x18, 0x04, 0x3a, 0x04, 0x5c, 0x04, 0x00, 0x00, 0x22, 0x00,
    0x44, 0x00, 0x66, 0x00, 0x88, 0x00, 0xaa, 0x00, 0xcc, 0x00, 0xee, 0x00,
    0x10, 0x01, 0x32, 0x01, 0x54, 0x01, 0x76, 0x01, 0x98, 0x01, 0xba, 0x01,
    0xdc, 0x01, 0xfe, 0x01, 0x20, 0x02, 0x42, 0x02, 0x64, 0x02, 0x86, 0x02,
    0xa8, 0x02, 0xca, 0x02, 0xec, 0x02, 0x0e, 0x03, 0x30, 0x03, 0x52, 0x03,
    0x74, 0x03, 0x96, 0x03, 0xb8, 0x03, 0xda, 0x03, 0xfc, 0x03, 0x1e, 0x04,
    0x3e, 0x00, 0x60, 0x00, 0x82, 0x00, 0xa4, 0x00, 0xc6, 0x00, 0xe8, 0x00,
    0x0a, 0x01, 0x2c, 0x01, 0x4e, 0x01, 0x70, 0x01, 0x92, 0x01, 0xb4, 0x01,
    0xd6, 0x01, 0xf8, 0x01, 0x1a, 0x02, 0x3c, 0x02, 0x5e, 0x02, 0x80, 0x02,
    0xa2, 0x02, 0xc4, 0x02, 0xe6, 0x02, 0x08, 0x03, 0x2a, 0x03, 0x4c, 0x03,
    0x6e, 0x03, 0x90, 0x03, 0xb2, 0x03, 0xd4, 0x03, 0xf6, 0x03, 0x18, 0x04,
    0x3a, 0x04, 0x5c, 0x04, 0x00, 0x00, 0x22, 0x00, 0x44, 0x00, 0x66, 0x00,
    0x88, 0x00, 0xaa, 0x00, 0xcc, 0x00, 0xee, 0x00, 0x10, 0x01, 0x32, 0x01,
    0x54, 0x01, 0x76, 0x01, 0x98, 0x01, 0xba, 0x01, 0xdc, 0x01, 0xfe, 0x01,
    0x20, 0x02, 0x42, 0x02, 0x64, 0x02, 0x86, 0x02, 0xa8, 0x02, 0xca, 0x02,
    0xec, 0x02, 0x0e, 0x03, 0x30, 0x03, 0x52, 0x03, 0x74, 0x03, 0x96, 0x03,
    0xb8, 0x03, 0xda, 0x03, 0xfc, 0x03, 0x1e, 0x04, 0x3e, 0x00, 0x60, 0x00,
    0x82, 0x00, 0xa4, 0x00, 0xc6, 0x00, 0xe8, 0x00, 0x0a, 0x01, 0x2c, 0x01,
    0x4e, 0x01, 0x70, 0x01, 0x92, 0x01, 0xb4, 0x01, 0xd6, 0x01, 0xf8, 0x01,
    0x1a, 0x02, 0x3c, 0x02, 0x5e, 0x02, 0x80, 0x02, 0xa2, 0x02, 0xc4, 0x02,
    0xe6, 0x02, 0x08, 0x03, 0x2a, 0x03, 0x4c, 0x03, 0x6e, 0x03, 0x90, 0x03,
    0xb2, 0x03, 0xd4, 0x03, 0xf6, 0x03, 0x18, 0x04, 0x3a, 0x04, 0x5c, 0x04,
    0x40, 0x00, 0x00, 0x00, 0x50, 0x00, 0x10, 0x10, 0x10, 0x10, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00,
    0x00, 0x00, 0x60, 0x00, 0x00, 0x11, 0x1f, 0x30, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00,
};
constexpr uint64_t kGoldenGradientHash = 0x58760a478fad6643ULL;

// ---------------------------------------------------------------------------
// Container fixture writer.
// ---------------------------------------------------------------------------

void WriteItem(std::ofstream& file, uint32_t type, uint32_t size) {
    uint8_t header[8];
    for (int i = 0; i < 4; ++i)
        header[i] = static_cast<uint8_t>(type >> (8 * i));
    for (int i = 0; i < 4; ++i)
        header[4 + i] = static_cast<uint8_t>(size >> (8 * i));
    file.write(reinterpret_cast<char*>(header), 8);
}

void WriteU32(std::ofstream& file, uint32_t value) {
    uint8_t bytes[4];
    for (int i = 0; i < 4; ++i)
        bytes[i] = static_cast<uint8_t>(value >> (8 * i));
    file.write(reinterpret_cast<char*>(bytes), 4);
}

void WriteI64(std::ofstream& file, int64_t value) {
    uint8_t bytes[8];
    uint64_t word = 0;
    std::memcpy(&word, &value, 8);
    for (int i = 0; i < 8; ++i)
        bytes[i] = static_cast<uint8_t>(word >> (8 * i));
    file.write(reinterpret_cast<char*>(bytes), 8);
}

void WriteFloat(std::ofstream& file, float value) {
    uint32_t word = 0;
    std::memcpy(&word, &value, 4);
    WriteU32(file, word);
}

int64_t TellOf(std::ofstream& file) {
    return static_cast<int64_t>(file.tellp());
}

constexpr char kContainerJson[] = "{\"manufacturer\":\"Example\",\"model\":\"Camera 1\","
                                  "\"UniqueCameraModel\":\"Example Camera 1\","
                                  "\"extraData\":{\"audioSampleRate\":48000,\"audioChannels\":2}}";
constexpr char kFrameJson[] = "{\"width\":64,\"height\":4,\"compressionType\":7}";
constexpr int64_t kFrameTs = 1000000000LL;
constexpr int64_t kAudioTs = 1001000000LL;
constexpr int64_t kGyroTs = 1002000000LL;
constexpr int64_t kAccelTs = 1003000000LL;

// Hand-built container: one frame, one audio chunk with timestamp, one gyro
// and one accelerometer sample. An OIS index/data pair sits between the
// motion payloads and the audio index so the suite pins OIS skipping.
void WriteRoundTripContainer(const char* path) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    assert(static_cast<bool>(file));
    const char header[8] = {'M', 'O', 'T', 'I', 'O', 'N', ' ', 3};
    file.write(header, 8);
    WriteItem(file, 3, static_cast<uint32_t>(std::strlen(kContainerJson)));
    file.write(kContainerJson, static_cast<std::streamsize>(std::strlen(kContainerJson)));

    const std::vector<uint8_t> payload = ZeroPayload();
    const int64_t frameOff = TellOf(file);
    WriteItem(file, 2, static_cast<uint32_t>(payload.size()));
    file.write(reinterpret_cast<const char*>(payload.data()),
               static_cast<std::streamsize>(payload.size()));
    WriteItem(file, 3, static_cast<uint32_t>(std::strlen(kFrameJson)));
    file.write(kFrameJson, static_cast<std::streamsize>(std::strlen(kFrameJson)));

    const int64_t audioOff = TellOf(file);
    const int16_t pcm[2] = {100, -200};
    WriteItem(file, 5, 4);
    file.write(reinterpret_cast<const char*>(pcm), 4);
    WriteItem(file, 6, 8);
    WriteI64(file, kAudioTs);

    const int64_t gyroOff = TellOf(file);
    WriteItem(file, 9, 8 + 24);
    WriteU32(file, 1);
    WriteU32(file, 1);
    WriteI64(file, kGyroTs);
    WriteFloat(file, 0.1f);
    WriteFloat(file, -0.2f);
    WriteFloat(file, 0.3f);
    WriteU32(file, 0);

    const int64_t accelOff = TellOf(file);
    WriteItem(file, 13, 8 + 24);
    WriteU32(file, 1);
    WriteU32(file, 1);
    WriteI64(file, kAccelTs);
    WriteFloat(file, 0.0f);
    WriteFloat(file, 9.81f);
    WriteFloat(file, 0.0f);
    WriteU32(file, 0);

    WriteItem(file, 10, 8);
    WriteU32(file, 1);
    WriteU32(file, 0);
    WriteItem(file, 11, 4);
    WriteU32(file, 0);

    WriteItem(file, 4, 16 + 16);
    WriteI64(file, 1);
    WriteI64(file, kAudioTs / 1000000);
    WriteI64(file, audioOff);
    WriteI64(file, kAudioTs);

    WriteItem(file, 8, 8 + 16);
    WriteU32(file, 1);
    WriteU32(file, 1);
    WriteI64(file, gyroOff);
    WriteI64(file, kGyroTs);

    WriteItem(file, 12, 8 + 16);
    WriteU32(file, 1);
    WriteU32(file, 1);
    WriteI64(file, accelOff);
    WriteI64(file, kAccelTs);

    WriteItem(file, 1, 16);
    const int64_t listOff = TellOf(file);
    WriteI64(file, frameOff);
    WriteI64(file, kFrameTs);

    WriteItem(file, 0, 16);
    WriteU32(file, 0x8A905612);
    WriteU32(file, 1);
    WriteI64(file, listOff);
    file.flush();
    assert(static_cast<bool>(file));
}

// ---------------------------------------------------------------------------
// Decoder suites.
// ---------------------------------------------------------------------------

void TestDecodeTrivialFixtures() {
    std::vector<uint16_t> pixels;
    mediacinemaraw::decode(ZeroPayload(), 64, 4, pixels);
    assert(pixels.size() == 256);
    for (auto v : pixels)
        assert(v == 0);

    mediacinemaraw::decode(OnePayload(), 64, 4, pixels);
    assert(pixels.size() == 256);
    for (auto v : pixels)
        assert(v == 1);
}

void TestGoldenGradientPayload() {
    std::vector<uint16_t> pixels;
    mediacinemaraw::decode(kGoldenGradient, sizeof(kGoldenGradient), 64, 4, pixels);
    assert(pixels.size() == 256);
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 64; ++x)
            assert(pixels[y * 64 + x] == static_cast<uint16_t>((x * 17 + y * 31) & 4095));
    assert(Fnv1aHash(pixels) == kGoldenGradientHash);
}

void TestQueryPayloadDimensions() {
    const std::vector<uint8_t> payload = ZeroPayload();
    int paddedWidth = 0, height = 0;
    assert(mediacinemaraw::queryPayloadDimensions(payload.data(), payload.size(), paddedWidth,
                                                 height));
    assert(paddedWidth == 64 && height == 4);
    assert(!mediacinemaraw::queryPayloadDimensions(nullptr, 0, paddedWidth, height));
    assert(!mediacinemaraw::queryPayloadDimensions(payload.data(), 3, paddedWidth, height));
}

void TestDecodeRejectsBadInput() {
    bool truncated = false;
    try {
        std::vector<uint16_t> pixels;
        std::vector<uint8_t> payload = ZeroPayload();
        payload.resize(10);
        mediacinemaraw::decode(payload, 64, 4, pixels);
    } catch (const std::runtime_error&) {
        truncated = true;
    }
    assert(truncated);

    bool widthMismatch = false;
    try {
        std::vector<uint16_t> pixels;
        mediacinemaraw::decode(ZeroPayload(), 128, 4, pixels);
    } catch (const std::runtime_error&) {
        widthMismatch = true;
    }
    assert(widthMismatch);

    bool nullPayload = false;
    try {
        std::vector<uint16_t> pixels;
        mediacinemaraw::decode(nullptr, 0, 64, 4, pixels);
    } catch (const std::invalid_argument&) {
        nullPayload = true;
    }
    assert(nullPayload);
}

// ---------------------------------------------------------------------------
// Container suites.
// ---------------------------------------------------------------------------

void TestContainerRoundTrip() {
    WriteRoundTripContainer(kScratchContainer);
    mediacinemaraw::ContainerReader reader(kScratchContainer);
    assert(reader.containerMetadata().find("Example Camera 1") != std::string::npos);
    assert(reader.frameTimestamps().size() == 1);
    assert(reader.frameTimestamps()[0] == kFrameTs);

    mediacinemaraw::Frame frame;
    reader.loadFrame(kFrameTs, frame);
    assert(frame.width == 64 && frame.height == 4);
    assert(frame.pixels.size() == 256);
    for (auto v : frame.pixels)
        assert(v == 0);
    assert(frame.metadata.find("\"compressionType\":7") != std::string::npos);

    std::string meta;
    reader.loadFrameMetadata(kFrameTs, meta);
    assert(meta == kFrameJson);

    assert(reader.audioSampleRateHz() == 48000);
    assert(reader.numAudioChannels() == 2);
    std::vector<mediacinemaraw::AudioChunk> audio;
    reader.loadAudio(audio);
    assert(audio.size() == 1);
    assert(audio[0].timestampNs == kAudioTs);
    assert(audio[0].samples.size() == 2);
    assert(audio[0].samples[0] == 100 && audio[0].samples[1] == -200);

    assert(reader.hasGyroData());
    std::vector<mediacinemaraw::MotionSample> gyro;
    reader.loadGyroData(gyro);
    assert(gyro.size() == 1);
    assert(gyro[0].timestampNs == kGyroTs);
    assert(std::fabs(gyro[0].x - 0.1f) < 1e-6f);

    assert(reader.hasAccelerometerData());
    std::vector<mediacinemaraw::MotionSample> accel;
    reader.loadAccelerometerData(accel);
    assert(accel.size() == 1);
    assert(accel[0].timestampNs == kAccelTs);
    assert(std::fabs(accel[0].y - 9.81f) < 1e-4f);
}

void TestContainerRejectsUnknownFrame() {
    mediacinemaraw::ContainerReader reader(kScratchContainer);
    bool threw = false;
    try {
        mediacinemaraw::Frame frame;
        reader.loadFrame(12345, frame);
    } catch (const std::runtime_error&) {
        threw = true;
    }
    assert(threw);
}

// ---------------------------------------------------------------------------
// Lens shading suites.
// ---------------------------------------------------------------------------

void TestLensShadingParse() {
    mediacinemaraw::LensShadingMap map;
    // Absent key: false, `out` untouched.
    map.columns = 7;
    map.rows = 7;
    map.gains = {1.0f, 2.0f, 3.0f};
    assert(!mediacinemaraw::parseLensShadingMap("{\"model\":\"X\"}", map));
    assert(map.columns == 7 && map.rows == 7 && map.gains.size() == 3);

    // Golden document in the encoder builder's exact output shape.
    const std::string doc = "{\"model\":\"X\",\"lensShadingMap\":{\"columns\":2,\"rows\":1,"
                            "\"gains\":[1,1.5,2,2.5,0.5,1.25,3,4]},\"extraData\":{}}";
    assert(mediacinemaraw::parseLensShadingMap(doc, map));
    assert(map.columns == 2 && map.rows == 1);
    assert((map.gains == std::vector<float>{1.0f, 1.5f, 2.0f, 2.5f, 0.5f, 1.25f, 3.0f, 4.0f}));

    // Members in any order, with whitespace and exponent notation.
    const std::string shuffled = "{ \"lensShadingMap\" : { \"rows\" : 1 , \"gains\" : "
                                 "[ 1e0 , 1.5 , 2 , 2.5 , .5 , 1.25 , 3 , 4 ] , "
                                 "\"columns\" : 2 } }";
    assert(mediacinemaraw::parseLensShadingMap(shuffled, map));
    assert(map.columns == 2 && map.rows == 1);
    assert(map.gains[4] == 0.5f);

    // The encoder prints gains with %.9g; 0.1f prints as 0.100000001 and
    // must parse back to the identical float.
    const std::string tenths = "{\"lensShadingMap\":{\"columns\":1,\"rows\":1,"
                               "\"gains\":[0.100000001,1,1,1]}}";
    assert(mediacinemaraw::parseLensShadingMap(tenths, map));
    assert(map.gains[0] == 0.1f);

    // A key-shaped match inside a string literal is skipped: the decoy
    // holds the exact `"lensShadingMap":null` text but opens no object.
    const std::string decoy = "{\"note\":\"the \\\"lensShadingMap\\\":null key\","
                              "\"lensShadingMap\":{\"columns\":1,\"rows\":1,"
                              "\"gains\":[1,1,1,1]}}";
    assert(mediacinemaraw::parseLensShadingMap(decoy, map));
    assert(map.columns == 1 && map.rows == 1);

    // Every malformed shape fails without touching `out`.
    const char* badDocs[] = {
        "{\"lensShadingMap\":null}",
        "{\"lensShadingMap\":{\"columns\":2,\"rows\":1}}",          // No gains.
        "{\"lensShadingMap\":{\"columns\":2,\"gains\":[1,1,1,1]}}", // No rows.
        "{\"lensShadingMap\":{\"columns\":0,\"rows\":1,\"gains\":[1,1,1,1]}}",
        "{\"lensShadingMap\":{\"columns\":513,\"rows\":1,\"gains\":[1,1,1,1]}}",
        "{\"lensShadingMap\":{\"columns\":2,\"rows\":1,\"gains\":[1,1,1]}}",       // Short.
        "{\"lensShadingMap\":{\"columns\":1,\"rows\":1,\"gains\":[1,1,1,1,1]}}",   // Long.
        "{\"lensShadingMap\":{\"columns\":1,\"rows\":1,\"gains\":[1,1,1,0]}}",     // Zero.
        "{\"lensShadingMap\":{\"columns\":1,\"rows\":1,\"gains\":[1,1,1,-2]}}",    // Negative.
        "{\"lensShadingMap\":{\"columns\":1,\"rows\":1,\"gains\":[1,1,1,1e999]}}", // Infinite.
        "{\"lensShadingMap\":{\"columns\":1,\"rows\":1,\"gains\":[1,1,1,]}}",    // Trailing comma.
        "{\"lensShadingMap\":{\"columns\":1,\"rows\":1,\"gains\":[]}}",          // Empty.
        "{\"lensShadingMap\":{\"columns\":1,\"rows\":1,\"gains\":1.5}}",         // Not an array.
        "{\"lensShadingMap\":{\"columns\":1.5,\"rows\":1,\"gains\":[1,1,1,1]}}", // Float extent.
        "{\"lensShadingMap\":{\"columns\":2,\"rows\":1,\"gains\":[1,1,1",        // Truncated.
    };
    for (const char* bad : badDocs) {
        map.columns = 7;
        map.rows = 7;
        map.gains = {1.0f, 2.0f, 3.0f};
        assert(!mediacinemaraw::parseLensShadingMap(bad, map));
        assert(map.columns == 7 && map.rows == 7 && map.gains.size() == 3);
    }
}

void TestLensShadingApply() {
    // Uniform 2x gain, zero black: values double.
    mediacinemaraw::LensShadingMap flat{2, 2, std::vector<float>(16, 2.0f)};
    std::vector<uint16_t> pixels = {100, 200, 300, 400};
    mediacinemaraw::applyLensShadingMap(pixels, 2, 2, flat);
    assert((pixels == std::vector<uint16_t>{200, 400, 600, 800}));

    // Black-level formula: out = 64 + (in - 64) * 2; below-black clamps
    // at zero instead of wrapping.
    pixels = {64, 100, 0, 200};
    mediacinemaraw::applyLensShadingMap(pixels, 2, 2, flat, 64.0f);
    assert((pixels == std::vector<uint16_t>{64, 136, 0, 336}));

    // Highlights clamp at the uint16 ceiling, not the sensor white level.
    mediacinemaraw::LensShadingMap one{1, 1, {2.0f, 2.0f, 2.0f, 2.0f}};
    pixels = {40000};
    mediacinemaraw::applyLensShadingMap(pixels, 1, 1, one);
    assert(pixels[0] == 65535);

    // Channel selection follows the CFA tile: (y % 2) * 2 + (x % 2).
    mediacinemaraw::LensShadingMap channels{1, 1, {1.0f, 2.0f, 3.0f, 4.0f}};
    pixels = {100, 100, 100, 100};
    mediacinemaraw::applyLensShadingMap(pixels, 2, 2, channels);
    assert((pixels == std::vector<uint16_t>{100, 200, 300, 400}));

    // Bilinear ramp: 4x1 frame over a 2x1 grid with channel gains 1 and 3.
    // Outer columns clamp to the edge samples; x = 1, 2 interpolate.
    mediacinemaraw::LensShadingMap ramp{2, 1, {1.0f, 1.0f, 1.0f, 1.0f, 3.0f, 3.0f, 3.0f, 3.0f}};
    pixels = {100, 100, 100, 100};
    mediacinemaraw::applyLensShadingMap(pixels, 4, 1, ramp);
    assert((pixels == std::vector<uint16_t>{100, 150, 250, 300}));
    assert(mediacinemaraw::lensShadingGainAt(ramp, 0, 0, 4, 1, 1) == 1.0f);
    assert(mediacinemaraw::lensShadingGainAt(ramp, 3, 0, 4, 1, 1) == 3.0f);
    assert(std::fabs(mediacinemaraw::lensShadingGainAt(ramp, 1, 0, 4, 1, 1) - 1.5f) < 1e-6f);

    // Absurd gains overflow the product to infinity; clamping in the float
    // domain saturates instead of feeding infinity to lround(), which
    // would be undefined.
    mediacinemaraw::LensShadingMap huge{1, 1, {1e30f, 1e30f, 1e30f, 1e30f}};
    pixels = {100, 0, 65535, 64};
    mediacinemaraw::applyLensShadingMap(pixels, 2, 2, huge, 64.0f);
    assert((pixels == std::vector<uint16_t>{65535, 0, 65535, 64}));

    // Guards: pixel-count mismatch, invalid map, bad black level.
    bool threw = false;
    try {
        pixels = {1, 2, 3};
        mediacinemaraw::applyLensShadingMap(pixels, 2, 2, flat);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
    threw = false;
    try {
        pixels = {1, 2, 3, 4};
        mediacinemaraw::LensShadingMap shortGains{1, 1, {1.0f}};
        mediacinemaraw::applyLensShadingMap(pixels, 2, 2, shortGains);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
    threw = false;
    try {
        pixels = {1, 2, 3, 4};
        mediacinemaraw::applyLensShadingMap(pixels, 2, 2, flat, -1.0f);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    assert(threw);
}

} // namespace

int main() {
    // Drop leftovers from an aborted run so every suite starts clean.
    std::remove(kScratchContainer);
    TestDecodeTrivialFixtures();
    std::puts("trivial fixtures passed");
    TestGoldenGradientPayload();
    std::puts("golden gradient payload passed");
    TestQueryPayloadDimensions();
    std::puts("payload dimension query passed");
    TestDecodeRejectsBadInput();
    std::puts("bad-input rejection passed");
    TestContainerRoundTrip();
    std::puts("container round-trip passed");
    TestContainerRejectsUnknownFrame();
    std::puts("unknown-frame rejection passed");
    TestLensShadingParse();
    std::puts("lens shading parse passed");
    TestLensShadingApply();
    std::puts("lens shading apply passed");
    std::remove(kScratchContainer);
    std::cout << "decoder self-tests passed\n";
    return 0;
}
