// SPDX-License-Identifier: GPL-3.0-only
//
// Independent MediaCinemaRAW v3 container reader.
// Format knowledge comes from the encoder-side writer and public container
// facts (magic, version, item ids).

#include <MediaCinemaRAW/ContainerReader.h>
#include <MediaCinemaRAW/Decoder.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <utility>

#if defined(_WIN32)
#include <stdio.h>
#define MCRAW_SEEK _fseeki64
#define MCRAW_TELL _ftelli64
#else
#define MCRAW_SEEK fseeko
#define MCRAW_TELL ftello
#endif

namespace mediacinemaraw {
namespace {

constexpr uint8_t kMagic[7] = {'M', 'O', 'T', 'I', 'O', 'N', ' '};
constexpr uint8_t kVersion = 3;
constexpr uint32_t kFooterMagic = 0x8A905612;
constexpr int kType7 = 7;

enum ItemKind : uint32_t {
    kIndex = 0,
    kIndexData = 1,
    kFrame = 2,
    kMeta = 3,
    kAudioIndex = 4,
    kAudioData = 5,
    kAudioMeta = 6,
    kAudioF32 = 7,
    kGyroIndex = 8,
    kGyroData = 9,
    kOisIndex = 10,
    kOisData = 11,
    kAccelIndex = 12,
    kAccelData = 13,
};

struct RawOffset {
    int64_t off = 0;
    int64_t ts = 0;
};

uint32_t getU32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

int64_t getI64(const uint8_t* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<uint64_t>(p[i]) << (8 * i);
    int64_t out = 0;
    std::memcpy(&out, &v, 8);
    return out;
}

int64_t tellNow(FILE* f) {
    const int64_t p = static_cast<int64_t>(MCRAW_TELL(f));
    if (p < 0) throw std::runtime_error("tell failed");
    return p;
}

void seekTo(FILE* f, int64_t pos) {
    if (pos < 0 || MCRAW_SEEK(f, static_cast<off_t>(pos), SEEK_SET) != 0)
        throw std::runtime_error("seek failed");
}

void skipAhead(FILE* f, int64_t delta) {
    if (delta < 0) throw std::runtime_error("negative skip");
    if (MCRAW_SEEK(f, static_cast<off_t>(delta), SEEK_CUR) != 0)
        throw std::runtime_error("skip failed");
}

void readFully(FILE* f, void* dst, size_t n) {
    if (n == 0) return;
    if (std::fread(dst, 1, n, f) != n) throw std::runtime_error("short read");
}

struct ItemHead {
    uint32_t kind = 0;
    uint32_t size = 0;
};

ItemHead readHead(FILE* f) {
    uint8_t b[8];
    readFully(f, b, 8);
    return {getU32(b), getU32(b + 4)};
}

// Minimal integer extractor for flat JSON: finds "key" then ':' then an
// optional sign and decimal digits. Returns false when absent/malformed.
bool jsonInt(const std::string& doc, const char* key, long long& value) {
    const std::string quoted = std::string("\"") + key + "\"";
    const size_t k = doc.find(quoted);
    if (k == std::string::npos) return false;
    const size_t c = doc.find(':', k + quoted.size());
    if (c == std::string::npos) return false;
    size_t i = c + 1;
    while (i < doc.size() && (doc[i] == ' ' || doc[i] == '\t' ||
                              doc[i] == '\n' || doc[i] == '\r' ||
                              doc[i] == '"'))
        ++i;
    bool neg = false;
    if (i < doc.size() && (doc[i] == '-' || doc[i] == '+')) {
        neg = doc[i] == '-';
        ++i;
    }
    if (i >= doc.size() || doc[i] < '0' || doc[i] > '9') return false;
    long long v = 0;
    while (i < doc.size() && doc[i] >= '0' && doc[i] <= '9') {
        v = v * 10 + (doc[i] - '0');
        ++i;
    }
    value = neg ? -v : v;
    return true;
}

}  // namespace

struct ContainerReader::Impl {
    FILE* handle = nullptr;
    std::string containerJson;
    std::vector<int64_t> ordered;
    std::map<int64_t, int64_t> byTime;
    std::vector<RawOffset> audio;
    std::vector<RawOffset> gyro;
    std::vector<RawOffset> accel;
    int64_t fileSize = 0;

    ~Impl() {
        if (handle) std::fclose(handle);
    }
};

ContainerReader::ContainerReader(const std::string& path)
    : impl_(new Impl()) {
    Impl& st = *impl_;
    st.handle = std::fopen(path.c_str(), "rb");
    if (!st.handle) throw std::runtime_error("cannot open " + path);
    FILE* f = st.handle;
    try {
        if (MCRAW_SEEK(f, 0, SEEK_END) != 0) throw std::runtime_error("size failed");
        st.fileSize = tellNow(f);
        seekTo(f, 0);

        uint8_t hdr[8];
        readFully(f, hdr, 8);
        if (std::memcmp(hdr, kMagic, 7) != 0) throw std::runtime_error("bad magic");
        if (hdr[7] != kVersion) throw std::runtime_error("bad container version");

        const ItemHead first = readHead(f);
        if (first.kind != kMeta) throw std::runtime_error("missing container metadata");
        if (first.size > 8 * 1024 * 1024) throw std::runtime_error("metadata too large");
        if (tellNow(f) + first.size > st.fileSize) throw std::runtime_error("truncated metadata");
        st.containerJson.assign(first.size, '\0');
        readFully(f, st.containerJson.data(), first.size);

        const int64_t dataStart = tellNow(f);
        if (st.fileSize < dataStart + 24 + 8)
            throw std::runtime_error("file too short for index");

        // Footer: [item(0,16)][magic u32][count u32][indexOff i64].
        seekTo(f, st.fileSize - 24);
        const ItemHead foot = readHead(f);
        if (foot.kind != kIndex || foot.size != 16)
            throw std::runtime_error("missing footer index");
        uint8_t fpay[16];
        readFully(f, fpay, 16);
        if (getU32(fpay) != kFooterMagic) throw std::runtime_error("bad footer magic");
        const uint32_t nFrames = getU32(fpay + 4);
        const int64_t listOff = getI64(fpay + 8);
        if (nFrames > 4000000) throw std::runtime_error("implausible frame count");
        const uint64_t listBytes = static_cast<uint64_t>(nFrames) * 16;
        if (listOff < dataStart || listOff < 0)
            throw std::runtime_error("bad frame list offset");
        if (listOff + static_cast<int64_t>(listBytes) + 24 != st.fileSize)
            throw std::runtime_error("frame list does not abut footer");
        const int64_t listHead = listOff - 8;
        if (listHead < dataStart) throw std::runtime_error("bad frame index header");
        seekTo(f, listHead);
        const ItemHead listItem = readHead(f);
        if (listItem.kind != kIndexData ||
            listItem.size != listBytes)
            throw std::runtime_error("bad frame index item");

        std::vector<RawOffset> frames(nFrames);
        for (uint32_t i = 0; i < nFrames; ++i) {
            uint8_t e[16];
            readFully(f, e, 16);
            frames[i].off = getI64(e);
            frames[i].ts = getI64(e + 8);
            if (frames[i].off < dataStart || frames[i].off >= listHead)
                throw std::runtime_error("frame offset out of range");
        }
        std::sort(frames.begin(), frames.end(),
                  [](const RawOffset& a, const RawOffset& b) {
                      return a.ts < b.ts;
                  });
        for (const auto& e : frames) {
            if (st.byTime.count(e.ts)) throw std::runtime_error("duplicate timestamp");
            st.byTime[e.ts] = e.off;
            st.ordered.push_back(e.ts);
        }

        // Scan the pre-index region for audio/motion indexes.
        seekTo(f, dataStart);
        const int64_t scanEnd = listHead;
        while (tellNow(f) + 8 <= scanEnd) {
            const int64_t itemPos = tellNow(f);
            uint8_t hb[8];
            if (std::fread(hb, 1, 8, f) != 8) break;
            const uint32_t kind = getU32(hb);
            const uint32_t size = getU32(hb + 4);
            const int64_t remain = scanEnd - (itemPos + 8);
            if (size > remain || size > (1u << 30))
                throw std::runtime_error("item size out of range");
            switch (kind) {
                case kFrame:
                case kMeta:
                case kAudioData:
                case kAudioMeta:
                case kAudioF32:
                case kGyroData:
                case kOisData:
                case kAccelData:
                    skipAhead(f, size);
                    break;
                case kAudioIndex: {
                    if (size < 16) throw std::runtime_error("bad audio index");
                    uint8_t pre[16];
                    readFully(f, pre, 16);
                    const int64_t n = getI64(pre);
                    // pre[8..16] is start timestamp ms, informational.
                    if (n < 0 || n > 1000000) throw std::runtime_error("bad audio count");
                    if (16 + static_cast<uint64_t>(n) * 16 != size)
                        throw std::runtime_error("audio index size mismatch");
                    st.audio.resize(static_cast<size_t>(n));
                    for (int64_t i = 0; i < n; ++i) {
                        uint8_t e[16];
                        readFully(f, e, 16);
                        st.audio[static_cast<size_t>(i)] = {getI64(e),
                                                            getI64(e + 8)};
                    }
                    break;
                }
                case kGyroIndex:
                case kAccelIndex: {
                    if (size < 8) throw std::runtime_error("bad motion index");
                    uint8_t pre[8];
                    readFully(f, pre, 8);
                    if (getU32(pre) != 1) throw std::runtime_error("bad motion index version");
                    const uint32_t n = getU32(pre + 4);
                    if (n > 100000) throw std::runtime_error("too many motion chunks");
                    if (8 + static_cast<uint64_t>(n) * 16 != size)
                        throw std::runtime_error("motion index size mismatch");
                    std::vector<RawOffset> tmp(n);
                    for (uint32_t i = 0; i < n; ++i) {
                        uint8_t e[16];
                        readFully(f, e, 16);
                        tmp[i] = {getI64(e), getI64(e + 8)};
                    }
                    if (kind == kGyroIndex)
                        st.gyro = std::move(tmp);
                    else
                        st.accel = std::move(tmp);
                    break;
                }
                case kOisIndex:
                    skipAhead(f, size);
                    break;
                default:
                    // Unknown or footer area: stop like the reference reader.
                    MCRAW_SEEK(f, itemPos, SEEK_SET);
                    goto scan_done;
            }
        }
    scan_done:;
    } catch (...) {
        // Impl destructor closes the file.
        throw;
    }
}

ContainerReader::~ContainerReader() = default;

const std::string& ContainerReader::containerMetadata() const noexcept {
    return impl_->containerJson;
}

const std::vector<int64_t>& ContainerReader::frameTimestamps() const noexcept {
    return impl_->ordered;
}

void ContainerReader::loadFrameMetadata(int64_t timestamp,
                                        std::string& out) const {
    FILE* f = impl_->handle;
    const auto it = impl_->byTime.find(timestamp);
    if (it == impl_->byTime.end()) throw std::runtime_error("frame not found");
    seekTo(f, it->second);
    const ItemHead buf = readHead(f);
    if (buf.kind != kFrame) throw std::runtime_error("expected frame item");
    if (tellNow(f) + buf.size > impl_->fileSize)
        throw std::runtime_error("truncated frame");
    skipAhead(f, buf.size);
    const ItemHead meta = readHead(f);
    if (meta.kind != kMeta) throw std::runtime_error("expected frame metadata");
    if (meta.size > 4 * 1024 * 1024) throw std::runtime_error("frame metadata too large");
    out.assign(meta.size, '\0');
    readFully(f, out.data(), meta.size);
}

void ContainerReader::loadFrame(int64_t timestamp, Frame& out) const {
    FILE* f = impl_->handle;
    const auto it = impl_->byTime.find(timestamp);
    if (it == impl_->byTime.end()) throw std::runtime_error("frame not found");
    seekTo(f, it->second);
    const ItemHead buf = readHead(f);
    if (buf.kind != kFrame) throw std::runtime_error("expected frame item");
    if (buf.size > 256 * 1024 * 1024) throw std::runtime_error("frame too large");
    std::vector<uint8_t> payload(buf.size);
    readFully(f, payload.data(), payload.size());
    const ItemHead meta = readHead(f);
    if (meta.kind != kMeta) throw std::runtime_error("expected frame metadata");
    if (meta.size > 4 * 1024 * 1024) throw std::runtime_error("frame metadata too large");
    out.metadata.assign(meta.size, '\0');
    readFully(f, out.metadata.data(), meta.size);

    long long w = 0, h = 0, ct = -1;
    if (!jsonInt(out.metadata, "width", w) ||
        !jsonInt(out.metadata, "height", h) ||
        !jsonInt(out.metadata, "compressionType", ct))
        throw std::runtime_error("frame metadata lacks width/height/compressionType");
    if (ct != kType7) throw std::runtime_error("unsupported compression type (only 7)");
    if (w <= 0 || w > 65536 || h <= 0 || h > 65536)
        throw std::runtime_error("bad frame dimensions");
    out.width = static_cast<int>(w);
    out.height = static_cast<int>(h);
    decode(payload.data(), payload.size(), out.width, out.height, out.pixels);
}

int ContainerReader::audioSampleRateHz() const {
    long long v = 0;
    return jsonInt(impl_->containerJson, "audioSampleRate", v) && v > 0 &&
                   v < 1000000
               ? static_cast<int>(v)
               : 0;
}

int ContainerReader::numAudioChannels() const {
    long long v = 0;
    return jsonInt(impl_->containerJson, "audioChannels", v) && v > 0 && v < 64
               ? static_cast<int>(v)
               : 0;
}

void ContainerReader::loadAudio(std::vector<AudioChunk>& out) const {
    FILE* f = impl_->handle;
    out.clear();
    out.reserve(impl_->audio.size());
    for (const auto& e : impl_->audio) {
        seekTo(f, e.off);
        const ItemHead head = readHead(f);
        if (head.kind != kAudioData) throw std::runtime_error("expected audio data");
        if (head.size % 2 != 0) throw std::runtime_error("odd audio size");
        if (head.size > 64 * 1024 * 1024) throw std::runtime_error("audio chunk too large");
        AudioChunk chunk;
        chunk.timestampNs = -1;
        chunk.samples.resize(head.size / 2);
        if (!chunk.samples.empty()) {
            readFully(f, chunk.samples.data(), head.size);
#if __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            for (auto& s : chunk.samples) {
                uint16_t u = 0;
                std::memcpy(&u, &s, 2);
                u = static_cast<uint16_t>((u >> 8) | (u << 8));
                std::memcpy(&s, &u, 2);
            }
#endif
        }
        // Optional trailing timestamp item; rewind if it is something else.
        uint8_t hb[8];
        if (std::fread(hb, 1, 8, f) == 8) {
            if (getU32(hb) == kAudioMeta && getU32(hb + 4) == 8) {
                uint8_t tb[8];
                readFully(f, tb, 8);
                chunk.timestampNs = getI64(tb);
            } else {
                if (MCRAW_SEEK(f, -8, SEEK_CUR) != 0)
                    throw std::runtime_error("rewind failed");
            }
        }
        out.push_back(std::move(chunk));
    }
}

bool ContainerReader::hasGyroData() const noexcept {
    return !impl_->gyro.empty();
}

void ContainerReader::loadGyroData(std::vector<MotionSample>& out) const {
    FILE* f = impl_->handle;
    for (const auto& e : impl_->gyro) {
        seekTo(f, e.off);
        const ItemHead head = readHead(f);
        if (head.kind != kGyroData || head.size < 8)
            throw std::runtime_error("expected gyro data");
        if (head.size > 256 * 1024 * 1024) throw std::runtime_error("gyro chunk too large");
        uint8_t pre[8];
        readFully(f, pre, 8);
        if (getU32(pre) != 1) throw std::runtime_error("bad gyro version");
        const uint32_t n = getU32(pre + 4);
        if (n == 0 || 8 + static_cast<uint64_t>(n) * 24 != head.size)
            throw std::runtime_error("gyro size mismatch");
        if (n > out.max_size() - out.size()) throw std::runtime_error("too many gyro samples");
        const size_t base = out.size();
        out.resize(base + n);
        readFully(f, out.data() + base, static_cast<size_t>(n) * 24);
    }
}

bool ContainerReader::hasAccelerometerData() const noexcept {
    return !impl_->accel.empty();
}

void ContainerReader::loadAccelerometerData(
    std::vector<MotionSample>& out) const {
    FILE* f = impl_->handle;
    for (const auto& e : impl_->accel) {
        seekTo(f, e.off);
        const ItemHead head = readHead(f);
        if (head.kind != kAccelData || head.size < 8)
            throw std::runtime_error("expected accelerometer data");
        if (head.size > 256 * 1024 * 1024)
            throw std::runtime_error("accelerometer chunk too large");
        uint8_t pre[8];
        readFully(f, pre, 8);
        if (getU32(pre) != 1)
            throw std::runtime_error("bad accelerometer version");
        const uint32_t n = getU32(pre + 4);
        if (n == 0 || 8 + static_cast<uint64_t>(n) * 24 != head.size)
            throw std::runtime_error("accelerometer size mismatch");
        if (n > out.max_size() - out.size())
            throw std::runtime_error("too many accelerometer samples");
        const size_t base = out.size();
        out.resize(base + n);
        readFully(f, out.data() + base, static_cast<size_t>(n) * 24);
    }
}

}  // namespace mediacinemaraw
