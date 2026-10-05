// SPDX-License-Identifier: GPL-3.0-only
//
// Independent MediaCinemaRAW v3 container reader: frame/audio/motion index
// discovery plus on-demand payload loading. Wire constants live in
// detail/ContainerFormat.h; the item-kind dispatch below is internal, since
// the encoder repo defines its own overlapping write-side constants.
#include <MediaCinemaRAW/ContainerReader.h>
#include <MediaCinemaRAW/Decoder.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "detail/ContainerFormat.h"
#include "detail/Endian.h"

namespace mediacinemaraw {
namespace {

// Item kinds on the wire. Type 7 is unused; 10/11 (OIS) are skipped, never
// surfaced; anything unrecognized ends the auxiliary scan the way the
// reference reader does.
enum ItemKind : uint32_t {
    kFooter = 0,
    kFrameIndex = 1,
    kFrameData = 2,
    kMetadata = 3,
    kAudioIndex = 4,
    kAudioData = 5,
    kAudioTimestamp = 6,
    kAudioF32 = 7,
    kGyroIndex = 8,
    kGyroData = 9,
    kOisIndex = 10,
    kOisData = 11,
    kAccelIndex = 12,
    kAccelData = 13,
};

struct IndexEntry {
    int64_t offset = 0;
    int64_t timestampNs = 0;
};

struct ItemHead {
    uint32_t kind = 0;
    uint32_t size = 0;
};

#if defined(_WIN32)
int SeekBy(FILE* handle, int64_t offset, int origin) {
    return _fseeki64(handle, offset, origin);
}
int64_t TellAt(FILE* handle) {
    return _ftelli64(handle);
}
#else
int SeekBy(FILE* handle, int64_t offset, int origin) {
    return fseeko(handle, static_cast<off_t>(offset), origin);
}
int64_t TellAt(FILE* handle) {
    return static_cast<int64_t>(ftello(handle));
}
#endif

// RAII file handle with checked positioning. Reads stream from disk, so
// large clips are never fully loaded; every seek and read is bounds
// checked against the file size discovered at open.
class File {
  public:
    explicit File(const std::string& path) {
        handle_ = std::fopen(path.c_str(), "rb");
        if (handle_ == nullptr)
            throw std::runtime_error("cannot open " + path);
    }
    ~File() {
        if (handle_ != nullptr)
            std::fclose(handle_);
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;

    int64_t Size() {
        if (SeekBy(handle_, 0, SEEK_END) != 0)
            throw std::runtime_error("size failed");
        return Tell();
    }
    int64_t Tell() {
        const int64_t pos = TellAt(handle_);
        if (pos < 0)
            throw std::runtime_error("tell failed");
        return pos;
    }
    void SeekTo(int64_t pos) {
        if (pos < 0 || SeekBy(handle_, pos, SEEK_SET) != 0)
            throw std::runtime_error("seek failed");
    }
    void Skip(int64_t delta) {
        if (delta < 0)
            throw std::runtime_error("negative skip");
        if (SeekBy(handle_, delta, SEEK_CUR) != 0)
            throw std::runtime_error("skip failed");
    }
    void Read(void* dst, size_t n) {
        if (n == 0)
            return;
        if (std::fread(dst, 1, n, handle_) != n)
            throw std::runtime_error("short read");
    }
    ItemHead ReadHeader() {
        uint8_t bytes[detail::kItemHeaderSize];
        Read(bytes, sizeof(bytes));
        return {detail::LoadU32LE(bytes), detail::LoadU32LE(bytes + 4)};
    }
    // Header peek for optional trailing items: false on a short read, with
    // the cursor left wherever the read ended.
    bool TryReadHeader(ItemHead& head) {
        uint8_t bytes[detail::kItemHeaderSize];
        if (std::fread(bytes, 1, sizeof(bytes), handle_) != sizeof(bytes))
            return false;
        head = {detail::LoadU32LE(bytes), detail::LoadU32LE(bytes + 4)};
        return true;
    }

  private:
    FILE* handle_ = nullptr;
};

// Minimal integer extractor for flat JSON: finds "key", then ':', then an
// optional sign and decimal digits. False when absent or malformed.
bool FindJsonInteger(const std::string& doc, const char* key, long long& value) {
    const std::string quoted = std::string("\"") + key + "\"";
    const size_t found = doc.find(quoted);
    if (found == std::string::npos)
        return false;
    const size_t colon = doc.find(':', found + quoted.size());
    if (colon == std::string::npos)
        return false;
    size_t i = colon + 1;
    while (i < doc.size() &&
           (doc[i] == ' ' || doc[i] == '\t' || doc[i] == '\n' || doc[i] == '\r' || doc[i] == '"'))
        ++i;
    bool negative = false;
    if (i < doc.size() && (doc[i] == '-' || doc[i] == '+')) {
        negative = doc[i] == '-';
        ++i;
    }
    if (i >= doc.size() || doc[i] < '0' || doc[i] > '9')
        return false;
    long long parsed = 0;
    while (i < doc.size() && doc[i] >= '0' && doc[i] <= '9')
        parsed = parsed * 10 + (doc[i++] - '0');
    value = negative ? -parsed : parsed;
    return true;
}

} // namespace

struct ContainerReader::Impl {
    explicit Impl(const std::string& path) : file(path) {
        fileSize = file.Size();
        file.SeekTo(0);
        const int64_t dataStart = ReadContainerHeader();
        const int64_t listHead = ReadFrameIndex(dataStart);
        ScanAuxiliaryIndexes(dataStart, listHead);
    }

    // File header plus the container-level JSON. Returns the offset where
    // the frame/audio/motion items start.
    int64_t ReadContainerHeader() {
        uint8_t header[8];
        file.Read(header, sizeof(header));
        if (std::memcmp(header, detail::kFileMagic, 7) != 0)
            throw std::runtime_error("bad magic");
        if (header[7] != detail::kFileMagic[7])
            throw std::runtime_error("bad container version");

        const ItemHead first = file.ReadHeader();
        if (first.kind != kMetadata)
            throw std::runtime_error("missing container metadata");
        if (first.size > detail::kMaxContainerMetadata)
            throw std::runtime_error("metadata too large");
        if (file.Tell() + first.size > fileSize)
            throw std::runtime_error("truncated metadata");
        containerJson.assign(first.size, '\0');
        file.Read(containerJson.data(), first.size);
        return file.Tell();
    }

    // Footer plus the frame-offset list it points at. Returns the offset of
    // the frame-index item header, which bounds the auxiliary scan.
    int64_t ReadFrameIndex(int64_t dataStart) {
        // At minimum the file holds one item header ahead of the footer.
        const int64_t footerItem =
            static_cast<int64_t>(detail::kItemHeaderSize + detail::kFooterPayloadSize);
        const int64_t headerSize = static_cast<int64_t>(detail::kItemHeaderSize);
        if (fileSize < dataStart + headerSize + footerItem)
            throw std::runtime_error("file too short for index");

        file.SeekTo(fileSize - footerItem);
        const ItemHead foot = file.ReadHeader();
        if (foot.kind != kFooter || foot.size != detail::kFooterPayloadSize)
            throw std::runtime_error("missing footer index");
        uint8_t footer[detail::kFooterPayloadSize];
        file.Read(footer, sizeof(footer));
        if (detail::LoadU32LE(footer) != detail::kFooterMagic)
            throw std::runtime_error("bad footer magic");
        const uint32_t frameCount = detail::LoadU32LE(footer + 4);
        const int64_t listOffset = detail::LoadI64LE(footer + 8);
        if (frameCount > detail::kMaxFrameCount)
            throw std::runtime_error("implausible frame count");
        const uint64_t listBytes = static_cast<uint64_t>(frameCount) * detail::kFrameIndexEntrySize;
        if (listOffset < dataStart || listOffset < 0)
            throw std::runtime_error("bad frame list offset");
        if (listOffset + static_cast<int64_t>(listBytes) + footerItem != fileSize)
            throw std::runtime_error("frame list does not abut footer");
        const int64_t listHead = listOffset - headerSize;
        if (listHead < dataStart)
            throw std::runtime_error("bad frame index header");
        file.SeekTo(listHead);
        const ItemHead listItem = file.ReadHeader();
        if (listItem.kind != kFrameIndex || listItem.size != listBytes)
            throw std::runtime_error("bad frame index item");

        std::vector<IndexEntry> frames(frameCount);
        for (uint32_t i = 0; i < frameCount; ++i) {
            uint8_t entry[detail::kFrameIndexEntrySize];
            file.Read(entry, sizeof(entry));
            frames[i].offset = detail::LoadI64LE(entry);
            frames[i].timestampNs = detail::LoadI64LE(entry + 8);
            if (frames[i].offset < dataStart || frames[i].offset >= listHead)
                throw std::runtime_error("frame offset out of range");
        }
        std::sort(frames.begin(), frames.end(), [](const IndexEntry& a, const IndexEntry& b) {
            return a.timestampNs < b.timestampNs;
        });
        for (const auto& entry : frames) {
            if (byTime.count(entry.timestampNs) != 0)
                throw std::runtime_error("duplicate timestamp");
            byTime[entry.timestampNs] = entry.offset;
            ordered.push_back(entry.timestampNs);
        }
        return listHead;
    }

    // Linear scan of the pre-index region for the audio and motion indexes.
    // Payload items are skipped by size; an unknown kind ends the scan.
    void ScanAuxiliaryIndexes(int64_t dataStart, int64_t scanEnd) {
        const int64_t headerSize = static_cast<int64_t>(detail::kItemHeaderSize);
        file.SeekTo(dataStart);
        while (file.Tell() + headerSize <= scanEnd) {
            const int64_t itemPos = file.Tell();
            ItemHead head;
            if (!file.TryReadHeader(head))
                break;
            const int64_t remain = scanEnd - (itemPos + headerSize);
            if (head.size > remain || head.size > detail::kMaxScanItem)
                throw std::runtime_error("item size out of range");
            switch (head.kind) {
                case kFrameData:
                case kMetadata:
                case kAudioData:
                case kAudioTimestamp:
                case kAudioF32:
                case kGyroData:
                case kOisData:
                case kAccelData:
                case kOisIndex:
                    file.Skip(head.size);
                    break;
                case kAudioIndex:
                    ReadAudioIndex(head.size);
                    break;
                case kGyroIndex:
                    ReadMotionIndex(head.size, gyro);
                    break;
                case kAccelIndex:
                    ReadMotionIndex(head.size, accel);
                    break;
                default:
                    // Unknown or footer area: stop like the reference reader.
                    file.SeekTo(itemPos);
                    return;
            }
        }
    }

    void ReadAudioIndex(uint32_t size) {
        if (size < detail::kAudioIndexHeaderSize)
            throw std::runtime_error("bad audio index");
        uint8_t prefix[detail::kAudioIndexHeaderSize];
        file.Read(prefix, sizeof(prefix));
        const int64_t count = detail::LoadI64LE(prefix);
        // prefix[8..16] is the start timestamp in ms, informational only.
        if (count < 0 || count > detail::kMaxAudioChunks)
            throw std::runtime_error("bad audio count");
        if (detail::kAudioIndexHeaderSize + static_cast<uint64_t>(count) * detail::kFrameIndexEntrySize !=
            size)
            throw std::runtime_error("audio index size mismatch");
        audio.resize(static_cast<size_t>(count));
        for (int64_t i = 0; i < count; ++i) {
            uint8_t entry[detail::kFrameIndexEntrySize];
            file.Read(entry, sizeof(entry));
            audio[static_cast<size_t>(i)] = {detail::LoadI64LE(entry),
                                             detail::LoadI64LE(entry + 8)};
        }
    }

    void ReadMotionIndex(uint32_t size, std::vector<IndexEntry>& index) {
        if (size < detail::kMotionHeaderSize)
            throw std::runtime_error("bad motion index");
        uint8_t prefix[detail::kMotionHeaderSize];
        file.Read(prefix, sizeof(prefix));
        if (detail::LoadU32LE(prefix) != detail::kMotionVersion)
            throw std::runtime_error("bad motion index version");
        const uint32_t count = detail::LoadU32LE(prefix + 4);
        if (count > detail::kMaxMotionChunks)
            throw std::runtime_error("too many motion chunks");
        if (detail::kMotionHeaderSize + static_cast<uint64_t>(count) * detail::kFrameIndexEntrySize !=
            size)
            throw std::runtime_error("motion index size mismatch");
        index.resize(count);
        for (uint32_t i = 0; i < count; ++i) {
            uint8_t entry[detail::kFrameIndexEntrySize];
            file.Read(entry, sizeof(entry));
            index[i] = {detail::LoadI64LE(entry), detail::LoadI64LE(entry + 8)};
        }
    }

    int64_t FrameOffset(int64_t timestamp) const {
        const auto it = byTime.find(timestamp);
        if (it == byTime.end())
            throw std::runtime_error("frame not found");
        return it->second;
    }

    // Shared gyro/accelerometer loader; `name` selects the error strings.
    void LoadMotionSamples(const std::vector<IndexEntry>& index, uint32_t dataKind, const char* name,
                           std::vector<MotionSample>& out) const {
        for (const auto& entry : index) {
            file.SeekTo(entry.offset);
            const ItemHead head = file.ReadHeader();
            if (head.kind != dataKind || head.size < detail::kMotionHeaderSize)
                throw std::runtime_error(std::string("expected ") + name + " data");
            if (head.size > detail::kMaxMotionData)
                throw std::runtime_error(std::string(name) + " chunk too large");
            uint8_t prefix[detail::kMotionHeaderSize];
            file.Read(prefix, sizeof(prefix));
            if (detail::LoadU32LE(prefix) != detail::kMotionVersion)
                throw std::runtime_error(std::string("bad ") + name + " version");
            const uint32_t count = detail::LoadU32LE(prefix + 4);
            if (count == 0 ||
                detail::kMotionHeaderSize + static_cast<uint64_t>(count) * detail::kMotionSampleWireSize !=
                    head.size)
                throw std::runtime_error(std::string(name) + " size mismatch");
            if (count > out.max_size() - out.size())
                throw std::runtime_error(std::string("too many ") + name + " samples");
            const size_t base = out.size();
            out.resize(base + count);
            file.Read(out.data() + base, static_cast<size_t>(count) * detail::kMotionSampleWireSize);
        }
    }

    mutable File file;
    std::string containerJson;
    std::vector<int64_t> ordered;
    std::map<int64_t, int64_t> byTime;
    std::vector<IndexEntry> audio;
    std::vector<IndexEntry> gyro;
    std::vector<IndexEntry> accel;
    int64_t fileSize = 0;
};

ContainerReader::ContainerReader(const std::string& path) : impl_(new Impl(path)) {}

ContainerReader::~ContainerReader() = default;

const std::string& ContainerReader::containerMetadata() const noexcept {
    return impl_->containerJson;
}

const std::vector<int64_t>& ContainerReader::frameTimestamps() const noexcept {
    return impl_->ordered;
}

void ContainerReader::loadFrameMetadata(int64_t timestamp, std::string& out) const {
    Impl& state = *impl_;
    state.file.SeekTo(state.FrameOffset(timestamp));
    const ItemHead frame = state.file.ReadHeader();
    if (frame.kind != kFrameData)
        throw std::runtime_error("expected frame item");
    if (state.file.Tell() + frame.size > state.fileSize)
        throw std::runtime_error("truncated frame");
    state.file.Skip(frame.size);
    const ItemHead meta = state.file.ReadHeader();
    if (meta.kind != kMetadata)
        throw std::runtime_error("expected frame metadata");
    if (meta.size > detail::kMaxFrameMetadata)
        throw std::runtime_error("frame metadata too large");
    out.assign(meta.size, '\0');
    state.file.Read(out.data(), meta.size);
}

void ContainerReader::loadFrame(int64_t timestamp, Frame& out) const {
    Impl& state = *impl_;
    state.file.SeekTo(state.FrameOffset(timestamp));
    const ItemHead frame = state.file.ReadHeader();
    if (frame.kind != kFrameData)
        throw std::runtime_error("expected frame item");
    if (frame.size > detail::kMaxFramePayload)
        throw std::runtime_error("frame too large");
    std::vector<uint8_t> payload(frame.size);
    state.file.Read(payload.data(), payload.size());
    const ItemHead meta = state.file.ReadHeader();
    if (meta.kind != kMetadata)
        throw std::runtime_error("expected frame metadata");
    if (meta.size > detail::kMaxFrameMetadata)
        throw std::runtime_error("frame metadata too large");
    out.metadata.assign(meta.size, '\0');
    state.file.Read(out.metadata.data(), meta.size);

    long long width = 0, height = 0, compression = -1;
    if (!FindJsonInteger(out.metadata, "width", width) ||
        !FindJsonInteger(out.metadata, "height", height) ||
        !FindJsonInteger(out.metadata, "compressionType", compression))
        throw std::runtime_error("frame metadata lacks width/height/compressionType");
    if (compression != detail::kCompressionType7)
        throw std::runtime_error("unsupported compression type (only 7)");
    if (width <= 0 || width > 65536 || height <= 0 || height > 65536)
        throw std::runtime_error("bad frame dimensions");
    out.width = static_cast<int>(width);
    out.height = static_cast<int>(height);
    decode(payload.data(), payload.size(), out.width, out.height, out.pixels);
}

int ContainerReader::audioSampleRateHz() const {
    long long value = 0;
    return FindJsonInteger(impl_->containerJson, "audioSampleRate", value) && value > 0 &&
                   value < 1000000
               ? static_cast<int>(value)
               : 0;
}

int ContainerReader::numAudioChannels() const {
    long long value = 0;
    return FindJsonInteger(impl_->containerJson, "audioChannels", value) && value > 0 && value < 64
               ? static_cast<int>(value)
               : 0;
}

void ContainerReader::loadAudio(std::vector<AudioChunk>& out) const {
    Impl& state = *impl_;
    out.clear();
    out.reserve(state.audio.size());
    for (const auto& entry : state.audio) {
        state.file.SeekTo(entry.offset);
        const ItemHead head = state.file.ReadHeader();
        if (head.kind != kAudioData)
            throw std::runtime_error("expected audio data");
        if (head.size % 2 != 0)
            throw std::runtime_error("odd audio size");
        if (head.size > detail::kMaxAudioChunk)
            throw std::runtime_error("audio chunk too large");
        AudioChunk chunk;
        chunk.timestampNs = -1;
        chunk.samples.resize(head.size / 2);
        if (!chunk.samples.empty()) {
            state.file.Read(chunk.samples.data(), head.size);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
            for (auto& sample : chunk.samples) {
                uint16_t word = 0;
                std::memcpy(&word, &sample, 2);
                word = static_cast<uint16_t>((word >> 8) | (word << 8));
                std::memcpy(&sample, &word, 2);
            }
#endif
        }
        // Optional trailing timestamp item; rewind when it is something else.
        ItemHead next;
        if (state.file.TryReadHeader(next)) {
            if (next.kind == kAudioTimestamp && next.size == detail::kAudioTimestampSize) {
                uint8_t stamp[detail::kAudioTimestampSize];
                state.file.Read(stamp, sizeof(stamp));
                chunk.timestampNs = detail::LoadI64LE(stamp);
            } else {
                state.file.SeekTo(state.file.Tell() - detail::kItemHeaderSize);
            }
        }
        out.push_back(std::move(chunk));
    }
}

bool ContainerReader::hasGyroData() const noexcept {
    return !impl_->gyro.empty();
}

void ContainerReader::loadGyroData(std::vector<MotionSample>& out) const {
    impl_->LoadMotionSamples(impl_->gyro, kGyroData, "gyro", out);
}

bool ContainerReader::hasAccelerometerData() const noexcept {
    return !impl_->accel.empty();
}

void ContainerReader::loadAccelerometerData(std::vector<MotionSample>& out) const {
    impl_->LoadMotionSamples(impl_->accel, kAccelData, "accelerometer", out);
}

} // namespace mediacinemaraw
