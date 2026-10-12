// SPDX-License-Identifier: GPL-3.0-only
//
// Frame-level lens shading metadata, compatible with MotionCam Pro files
// and MotionCam Tools: a per-frame gain grid correcting vignetting.
//
// Schema. Each frame JSON may carry three keys (verified against
// MotionCam Pro recordings and the MotionCam Tools renderer, which reads
// them and bakes the correction into its DNG output):
//
//   "lensShadingMapWidth": 17, "lensShadingMapHeight": 13,
//   "lensShadingMap": [[R...], [Gr...], [Gb...], [B...]]
//
// `lensShadingMap` holds exactly four nested planes, one per color
// channel in Android color order: 0 = R, 1 = Gr (green in R rows),
// 2 = Gb (green in B rows), 3 = B. Each plane holds exactly
// `Width * Height` row-major finite floats, each greater than zero.
// This is NOT CFA tile order: pixels map to planes through the
// `sensorArrangment` value (`"rggb"`, `"bggr"`, `"grbg"`, or `"gbrg"`).
// MotionCam Tools ignores a missing or malformed map (flat arrays,
// strings, and ragged planes all render as if absent — verified), and
// this parser likewise returns false, leaving its output untouched.
//
// Geometry. Grid point (c, r) sits at pixel-center coordinates
// `x = (c + 0.5) * width / columns`, `y = (r + 0.5) * height / rows`,
// where width/height are the decoded frame dimensions. The gain at a pixel
// is the bilinear interpolation of the four surrounding grid points,
// edge-clamped so border pixels reuse the outer samples. Grids that are
// one wide or one tall are constant along the degenerate axis.
//
// The map covers the decoded frame (post-crop, post-binning). When frames
// are cropped or 4x-downscaled, the producer must crop or resample the
// sensor shading map to the encoded window before embedding it (see the
// sibling encoder's `buildLensShadingJson()`).
//
// Correction. `applyLensShadingMap()` computes
// `out = black + (in - black) * gain`, rounded to nearest and clamped to
// [0, 65535]. Pass the sensor black level so gains scale the
// light-proportional signal; the upper clamp keeps the full uint16 range
// instead of clipping at the sensor white level.
//
// The shared section below (the struct, validation, channel mapping, and
// sampling) is duplicated in the sibling encoder repository, which must
// not depend on this one. Keep the two copies in sync.
#pragma once

#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace mediacinemaraw {

// Coarse per-channel gain grid covering one encoded frame: `columns` by
// `rows` grid points with four color-interleaved gains each, index
// `(r * columns + c) * 4 + ch`, channel order R, Gr, Gb, B.
struct LensShadingMap {
    int columns = 0;
    int rows = 0;
    std::vector<float> gains;
};

namespace lensshading_detail {

// Grid backstop: 512 x 512 x 4 gains stay under typical frame metadata
// sizes while real sensor maps are an order of magnitude smaller.
constexpr int kMaxGridExtent = 512;

// Unchecked bilinear sample. The caller must have validated the map,
// geometry, and channel. Never throws.
inline float SampleGain(const LensShadingMap& map, int x, int y, int width, int height,
                        int channel) noexcept {
    const int columns = map.columns;
    const int rows = map.rows;
    float gx =
        (static_cast<float>(x) + 0.5f) * static_cast<float>(columns) / static_cast<float>(width) -
        0.5f;
    float gy =
        (static_cast<float>(y) + 0.5f) * static_cast<float>(rows) / static_cast<float>(height) -
        0.5f;
    if (gx < 0.0f)
        gx = 0.0f;
    if (gx > static_cast<float>(columns - 1))
        gx = static_cast<float>(columns - 1);
    if (gy < 0.0f)
        gy = 0.0f;
    if (gy > static_cast<float>(rows - 1))
        gy = static_cast<float>(rows - 1);
    // Truncation is floor for the clamped non-negative coordinates.
    int c0 = static_cast<int>(gx);
    if (c0 > columns - 1)
        c0 = columns - 1;
    int c1 = c0 + 1;
    if (c1 > columns - 1)
        c1 = columns - 1;
    int r0 = static_cast<int>(gy);
    if (r0 > rows - 1)
        r0 = rows - 1;
    int r1 = r0 + 1;
    if (r1 > rows - 1)
        r1 = rows - 1;
    const float fx = gx - static_cast<float>(c0);
    const float fy = gy - static_cast<float>(r0);
    const size_t stride = static_cast<size_t>(columns) * 4;
    const float* gains = map.gains.data();
    const float g00 = gains[static_cast<size_t>(r0) * stride + static_cast<size_t>(c0) * 4 +
                            static_cast<size_t>(channel)];
    const float g01 = gains[static_cast<size_t>(r0) * stride + static_cast<size_t>(c1) * 4 +
                            static_cast<size_t>(channel)];
    const float g10 = gains[static_cast<size_t>(r1) * stride + static_cast<size_t>(c0) * 4 +
                            static_cast<size_t>(channel)];
    const float g11 = gains[static_cast<size_t>(r1) * stride + static_cast<size_t>(c1) * 4 +
                            static_cast<size_t>(channel)];
    return (g00 * (1.0f - fx) + g01 * fx) * (1.0f - fy) + (g10 * (1.0f - fx) + g11 * fx) * fy;
}

inline size_t SkipWs(const std::string& text, size_t i, size_t end) {
    while (i < end && (text[i] == ' ' || text[i] == '\t' || text[i] == '\n' || text[i] == '\r'))
        ++i;
    return i;
}

// Finds `"key"` (quoted, so a longer key sharing the prefix cannot match)
// followed by a colon. Returns the position just past the colon, or npos.
// Earlier matches that open no value (the key text as a bare value) are
// skipped and the search continues.
inline size_t FindKeyColon(const std::string& text, const std::string& quoted, size_t start) {
    size_t search = start;
    for (;;) {
        const size_t found = text.find(quoted, search);
        if (found == std::string::npos)
            return std::string::npos;
        const size_t i = SkipWs(text, found + quoted.size(), text.size());
        if (i < text.size() && text[i] == ':')
            return i + 1;
        search = found + 1;
    }
}

// Document-level integer member: a decimal integer terminated by ',' or
// '}'. False when absent or malformed. Accumulation cannot overflow: the
// value saturates instead, and the extent check rejects it downstream.
inline bool FindDocInt(const std::string& text, const char* key, long long& value) {
    const std::string quoted = std::string("\"") + key + "\"";
    size_t search = 0;
    for (;;) {
        const size_t after = FindKeyColon(text, quoted, search);
        if (after == std::string::npos)
            return false;
        size_t i = SkipWs(text, after, text.size());
        bool negative = false;
        if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
            negative = text[i] == '-';
            ++i;
        }
        if (i >= text.size() || text[i] < '0' || text[i] > '9') {
            search = after;
            continue;
        }
        long long parsed = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            const int digit = text[i++] - '0';
            if (parsed > (0x7FFFFFFFFFFFFFFFLL - digit) / 10)
                parsed = 0x7FFFFFFFFFFFFFFFLL;
            else
                parsed = parsed * 10 + digit;
        }
        i = SkipWs(text, i, text.size());
        if (i >= text.size() || (text[i] != ',' && text[i] != '}')) {
            search = after;
            continue;
        }
        value = negative ? -parsed : parsed;
        return true;
    }
}

inline bool IsFloatChar(char c) {
    return (c >= '0' && c <= '9') || c == '.' || c == 'e' || c == 'E' || c == '+' || c == '-';
}

// Parses one JSON float token. The token must be fully consumed and finite;
// JSON's '.' is translated to the C numeric locale's decimal point first
// so parsing is locale-independent.
inline bool ParseFloatToken(const char* begin, const char* end, float& value) {
    if (begin >= end)
        return false;
    std::string token(begin, end);
    const char* separator = std::localeconv()->decimal_point;
    if (separator != nullptr && separator[0] != '\0' &&
        !(separator[0] == '.' && separator[1] == '\0')) {
        const size_t dot = token.find('.');
        if (dot != std::string::npos)
            token.replace(dot, 1, separator);
    }
    char* stop = nullptr;
    const float parsed = std::strtof(token.c_str(), &stop);
    if (stop == nullptr || stop != token.c_str() + token.size())
        return false;
    if (!std::isfinite(parsed))
        return false;
    value = parsed;
    return true;
}

// Parses one nested gain plane: '[' followed by exactly `plane` floats and
// ']'. Advances `pos` past the closing bracket. False on any deviation,
// including ragged counts and trailing commas.
inline bool ParseGainPlane(const std::string& text, size_t& pos, float* channelGains,
                           size_t plane) {
    size_t j = SkipWs(text, pos, text.size());
    if (j >= text.size() || text[j] != '[')
        return false;
    ++j;
    bool expectValue = true;
    size_t count = 0;
    for (;;) {
        j = SkipWs(text, j, text.size());
        if (j >= text.size())
            return false;
        if (!expectValue && text[j] == ']') {
            ++j;
            break;
        }
        if (text[j] == ']' || text[j] == ',')
            return false; // Empty plane, trailing comma, or bare comma.
        size_t tokenEnd = j;
        while (tokenEnd < text.size() && IsFloatChar(text[tokenEnd]))
            ++tokenEnd;
        float gain = 0.0f;
        if (!ParseFloatToken(text.data() + j, text.data() + tokenEnd, gain) || gain <= 0.0f)
            return false;
        if (count >= plane)
            return false;
        channelGains[count * 4] = gain;
        ++count;
        expectValue = false;
        j = SkipWs(text, tokenEnd, text.size());
        if (j >= text.size())
            return false;
        if (text[j] == ',') {
            ++j;
            expectValue = true;
            continue;
        }
        if (text[j] == ']') {
            ++j;
            break;
        }
        return false;
    }
    if (count != plane)
        return false;
    pos = j;
    return true;
}

} // namespace lensshading_detail

// Validates a map against the schema above. Throws std::invalid_argument
// on out-of-range extents, a gain-count mismatch, or a gain that is not
// finite and positive.
inline void validateLensShadingMap(const LensShadingMap& map) {
    if (map.columns < 1 || map.columns > lensshading_detail::kMaxGridExtent)
        throw std::invalid_argument("lens shading columns out of range");
    if (map.rows < 1 || map.rows > lensshading_detail::kMaxGridExtent)
        throw std::invalid_argument("lens shading rows out of range");
    const size_t expected = static_cast<size_t>(map.columns) * static_cast<size_t>(map.rows) * 4;
    if (map.gains.size() != expected)
        throw std::invalid_argument("lens shading gain count mismatch");
    for (float gain : map.gains) {
        if (!std::isfinite(gain))
            throw std::invalid_argument("lens shading gain not finite");
        if (gain <= 0.0f)
            throw std::invalid_argument("lens shading gain not positive");
    }
}

// Maps pixel (x, y) to its color channel index (0 = R, 1 = Gr, 2 = Gb,
// 3 = B) through the 2x2 CFA tile named by `sensorArrangement`, one of
// `"rggb"`, `"bggr"`, `"grbg"`, `"gbrg"` (lowercase, as MotionCam writes
// it). A green pixel in an R row is Gr, in a B row Gb. Only the pixel
// parity matters. Throws std::invalid_argument on any other arrangement.
inline int lensShadingChannelIndex(const std::string& sensorArrangement, int x, int y) {
    if (sensorArrangement != "rggb" && sensorArrangement != "bggr" && sensorArrangement != "grbg" &&
        sensorArrangement != "gbrg")
        throw std::invalid_argument("lens shading sensor arrangement");
    const int tx = ((x % 2) + 2) % 2;
    const int ty = ((y % 2) + 2) % 2;
    const char color = sensorArrangement[size_t(ty) * 2 + size_t(tx)];
    if (color == 'r')
        return 0;
    if (color == 'b')
        return 3;
    const bool rowHasRed =
        sensorArrangement[size_t(ty) * 2] == 'r' || sensorArrangement[size_t(ty) * 2 + 1] == 'r';
    return rowHasRed ? 1 : 2;
}

// Samples one color-channel gain at pixel (x, y) of a width x height
// frame; `channel` is 0 = R, 1 = Gr, 2 = Gb, 3 = B. Validates the map and
// throws std::invalid_argument like validateLensShadingMap(), plus on bad
// geometry, out-of-range coordinates, or a channel outside [0, 3].
inline float lensShadingGainAt(const LensShadingMap& map, int x, int y, int width, int height,
                               int channel) {
    validateLensShadingMap(map);
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("lens shading dimensions");
    if (x < 0 || x >= width || y < 0 || y >= height)
        throw std::invalid_argument("lens shading coordinates");
    if (channel < 0 || channel > 3)
        throw std::invalid_argument("lens shading channel");
    return lensshading_detail::SampleGain(map, x, y, width, height, channel);
}

// Parses the frame-level `lensShadingMapWidth` / `lensShadingMapHeight` /
// `lensShadingMap` triplet out of `frameJson` (as returned by
// `ContainerReader::loadFrameMetadata()`). Members may appear in any order
// and surrounding whitespace is allowed; the map must be exactly four
// nested planes of `Width * Height` floats each, all finite and positive.
// Returns false when any key is absent or malformed, leaving `out`
// untouched. A value of the wrong JSON type is skipped like an absent key
// and the search continues, mirroring MotionCam Tools, which renders such
// files as if the map were absent. No JSON library is used.
inline bool parseLensShadingMap(const std::string& frameJson, LensShadingMap& out) {
    const std::string& text = frameJson;
    long long columns = 0, rows = 0;
    if (!lensshading_detail::FindDocInt(text, "lensShadingMapWidth", columns) ||
        !lensshading_detail::FindDocInt(text, "lensShadingMapHeight", rows))
        return false;
    if (columns < 1 || columns > lensshading_detail::kMaxGridExtent || rows < 1 ||
        rows > lensshading_detail::kMaxGridExtent)
        return false;
    const size_t plane = static_cast<size_t>(columns) * static_cast<size_t>(rows);
    const std::string key = "\"lensShadingMap\"";
    size_t search = 0;
    for (;;) {
        const size_t after = lensshading_detail::FindKeyColon(text, key, search);
        if (after == std::string::npos)
            return false;
        size_t j = lensshading_detail::SkipWs(text, after, text.size());
        if (j >= text.size() || text[j] != '[') {
            // A key-shaped match holding no array (a string, a number, or
            // `null`); keep looking.
            search = after;
            continue;
        }
        ++j;
        // The value opened an array, so this occurrence decides the result:
        // anything malformed from here on fails instead of searching on.
        std::vector<float> gains(plane * 4);
        bool ok = true;
        for (int ch = 0; ch < 4 && ok; ++ch) {
            if (!lensshading_detail::ParseGainPlane(text, j, gains.data() + ch, plane)) {
                ok = false;
                break;
            }
            j = lensshading_detail::SkipWs(text, j, text.size());
            if (j >= text.size()) {
                ok = false;
                break;
            }
            if (ch < 3 && text[j] != ',') {
                ok = false;
                break;
            }
            if (ch == 3 && text[j] != ']') {
                ok = false;
                break;
            }
            ++j;
        }
        if (!ok)
            return false;
        out.columns = static_cast<int>(columns);
        out.rows = static_cast<int>(rows);
        out.gains = std::move(gains);
        return true;
    }
}

// Applies lens shading correction in place: each pixel's color channel
// comes from `sensorArrangement` (see lensShadingChannelIndex()), its gain
// is the bilinear sample for that channel over the width x height frame,
// and the pixel becomes `blackLevel + (pixel - blackLevel) * gain`,
// rounded to nearest and clamped to [0, 65535]. Throws
// std::invalid_argument on bad geometry, a pixel count other than
// `width * height`, an invalid map (like validateLensShadingMap()), an
// unknown sensor arrangement, or a black level that is not finite and
// non-negative.
inline void applyLensShadingMap(std::vector<uint16_t>& pixels, int width, int height,
                                const LensShadingMap& map, const std::string& sensorArrangement,
                                float blackLevel = 0.0f) {
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("lens shading dimensions");
    if (pixels.size() != static_cast<size_t>(width) * static_cast<size_t>(height))
        throw std::invalid_argument("lens shading pixel count");
    validateLensShadingMap(map);
    // Validates the arrangement once; the loop below reuses the tile.
    int tile[2][2];
    for (int ty = 0; ty < 2; ++ty)
        for (int tx = 0; tx < 2; ++tx)
            tile[ty][tx] = lensShadingChannelIndex(sensorArrangement, tx, ty);
    if (!std::isfinite(blackLevel) || blackLevel < 0.0f)
        throw std::invalid_argument("lens shading black level");
    const size_t stride = static_cast<size_t>(width);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int channel = tile[y & 1][x & 1];
            const float gain = lensshading_detail::SampleGain(map, x, y, width, height, channel);
            const float raw = static_cast<float>(
                pixels[static_cast<size_t>(y) * stride + static_cast<size_t>(x)]);
            // Clamped before rounding: an absurd gain overflows the product
            // to infinity, and lround() on infinity is undefined.
            const float corrected = blackLevel + (raw - blackLevel) * gain;
            long rounded;
            if (corrected <= 0.0f)
                rounded = 0;
            else if (corrected >= 65535.0f)
                rounded = 65535;
            else
                rounded = std::lround(corrected);
            pixels[static_cast<size_t>(y) * stride + static_cast<size_t>(x)] =
                static_cast<uint16_t>(rounded);
        }
    }
}

} // namespace mediacinemaraw
