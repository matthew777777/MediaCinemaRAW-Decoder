// SPDX-License-Identifier: GPL-3.0-only
//
// Container-level lens shading metadata for `.mcraw` clips: a coarse
// per-channel gain grid that corrects vignetting and lens falloff.
//
// Schema. The container JSON may carry one `lensShadingMap` object with
// plain keys, so motioncam-decoder readers consume it through
// nlohmann::json with no new dependency:
//
//   "lensShadingMap": {"columns": 16, "rows": 12, "gains": [...]}
//
// `columns`/`rows` are positive grid extents (at most 512 each). `gains`
// holds exactly `columns * rows * 4` finite floats, each greater than zero,
// in row-major order with four channel-interleaved gains per grid point:
// index `(r * columns + c) * 4 + ch`, where `ch = (y % 2) * 2 + (x % 2)`
// is the CFA tile position of the pixel. For `sensorArrangment "rggb"` the
// channels are R, Gr, Gb, B in tile order. This matches the DNG GainMap
// opcode sample order (MapPointsV x MapPointsH x MapPlanes), so DNG
// exporters can copy `gains` into the opcode verbatim with map_planes 4,
// spacing (width / columns, height / rows), and origin
// (0.5 * width / columns, 0.5 * height / rows).
//
// Geometry. Grid point (c, r) sits at pixel-center coordinates
// `x = (c + 0.5) * width / columns`, `y = (r + 0.5) * height / rows`,
// where width/height are the encoded frame dimensions. The gain at a pixel
// is the bilinear interpolation of the four surrounding grid points,
// edge-clamped so border pixels reuse the outer samples. Grids that are
// one wide or one tall are constant along the degenerate axis.
//
// The map covers the encoded frame (post-crop, post-binning). When frames
// are cropped or 4x-downscaled, the producer must crop or resample the
// sensor shading map to the encoded window before embedding it (see the
// sibling encoder's `buildLensShadingJson()`); the decoder always applies
// the map over the decoded width x height.
//
// Correction. `applyLensShadingMap()` computes
// `out = black + (in - black) * gain`, rounded to nearest and clamped to
// [0, 65535]. Pass the sensor black level so gains scale the
// light-proportional signal; the upper clamp keeps the full uint16 range
// instead of clipping at the sensor white level.
//
// The shared section below (the struct, validation, and sampling) is
// duplicated in the sibling encoder repository, which must not depend on
// this one. Keep the two copies in sync.
#pragma once

#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <vector>

namespace mediacinemaraw {

// Coarse per-channel gain grid covering one encoded frame. See the schema
// above for the `gains` layout.
struct LensShadingMap {
    int columns = 0;
    int rows = 0;
    std::vector<float> gains;
};

namespace lensshading_detail {

// Grid backstop: 512 x 512 x 4 gains stay under typical container sizes
// while real sensor maps are an order of magnitude smaller.
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

// Matching '}' for the '{' at `open`; npos when unbalanced. String
// literals (with backslash escapes) are skipped so braces inside text
// don't count.
inline size_t MatchObjectEnd(const std::string& text, size_t open) {
    // size_t: the loop starts at the opening brace, so depth is at least 1
    // before any decrement, and no document can overflow the counter.
    size_t depth = 0;
    bool inString = false;
    bool escape = false;
    for (size_t i = open; i < text.size(); ++i) {
        const char c = text[i];
        if (inString) {
            if (escape)
                escape = false;
            else if (c == '\\')
                escape = true;
            else if (c == '"')
                inString = false;
            continue;
        }
        if (c == '"')
            inString = true;
        else if (c == '{')
            ++depth;
        else if (c == '}') {
            if (--depth == 0)
                return i;
        }
    }
    return std::string::npos;
}

// Scoped integer member: finds `"key"` in [begin, end), then ':' and a
// decimal integer terminated by ',', ']', or '}'. False when absent or
// malformed. Values are capped early; members only hold small extents.
inline bool FindMemberInt(const std::string& text, size_t begin, size_t end, const char* key,
                          long long& value) {
    const std::string quoted = std::string("\"") + key + "\"";
    const size_t found = text.find(quoted, begin);
    if (found == std::string::npos || found >= end)
        return false;
    size_t i = SkipWs(text, found + quoted.size(), end);
    if (i >= end || text[i] != ':')
        return false;
    i = SkipWs(text, i + 1, end);
    bool negative = false;
    if (i < end && (text[i] == '-' || text[i] == '+')) {
        negative = text[i] == '-';
        ++i;
    }
    if (i >= end || text[i] < '0' || text[i] > '9')
        return false;
    long long parsed = 0;
    while (i < end && text[i] >= '0' && text[i] <= '9') {
        parsed = parsed * 10 + (text[i++] - '0');
        if (parsed > 1000000000LL)
            return false;
    }
    // The terminator may be the object's own closing brace at `end`, so this
    // one skip is bounded by the document instead. It cannot run past `end`:
    // the brace there is not whitespace, so the skip always stops at or
    // before it.
    i = SkipWs(text, i, text.size());
    if (i >= text.size() || (text[i] != ',' && text[i] != ']' && text[i] != '}'))
        return false;
    value = negative ? -parsed : parsed;
    return true;
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

// Samples one channel gain at pixel (x, y) of a width x height frame.
// `channel` is the CFA tile position `(y % 2) * 2 + (x % 2)`; callers that
// already know the pixel's channel pass it directly. Validates the map and
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

// Parses the container-level `lensShadingMap` object out of `containerJson`
// (as returned by `ContainerReader::containerMetadata()`). Members may
// appear in any order and surrounding whitespace is allowed. Returns false
// when the key is absent or the object is malformed — wrong extents, a
// gain count other than `columns * rows * 4`, or a gain that is not finite
// and positive — leaving `out` untouched. A structurally valid occurrence
// decides the result, so a second `lensShadingMap` later in the document
// is never consulted; a key-shaped match inside a string literal is
// skipped and the search continues. No JSON library is used.
inline bool parseLensShadingMap(const std::string& containerJson, LensShadingMap& out) {
    const std::string& text = containerJson;
    const std::string key = "\"lensShadingMap\"";
    size_t search = 0;
    for (;;) {
        const size_t found = text.find(key, search);
        if (found == std::string::npos)
            return false;
        size_t i = lensshading_detail::SkipWs(text, found + key.size(), text.size());
        bool structural = false;
        size_t objectEnd = std::string::npos;
        if (i < text.size() && text[i] == ':') {
            i = lensshading_detail::SkipWs(text, i + 1, text.size());
            if (i < text.size() && text[i] == '{') {
                objectEnd = lensshading_detail::MatchObjectEnd(text, i);
                structural = objectEnd != std::string::npos;
            }
        }
        if (!structural) {
            // A key-shaped match that opens no object (a string literal, or
            // `null`); keep looking.
            search = found + 1;
            continue;
        }
        long long columns = 0, rows = 0;
        if (!lensshading_detail::FindMemberInt(text, i, objectEnd, "columns", columns) ||
            !lensshading_detail::FindMemberInt(text, i, objectEnd, "rows", rows))
            return false;
        if (columns < 1 || columns > lensshading_detail::kMaxGridExtent || rows < 1 ||
            rows > lensshading_detail::kMaxGridExtent)
            return false;
        const std::string gainsKey = "\"gains\"";
        const size_t gainsAt = text.find(gainsKey, i);
        if (gainsAt == std::string::npos || gainsAt >= objectEnd)
            return false;
        size_t j = lensshading_detail::SkipWs(text, gainsAt + gainsKey.size(), objectEnd);
        if (j >= objectEnd || text[j] != ':')
            return false;
        j = lensshading_detail::SkipWs(text, j + 1, objectEnd);
        if (j >= objectEnd || text[j] != '[')
            return false;
        ++j;
        const size_t expected = static_cast<size_t>(columns) * static_cast<size_t>(rows) * 4;
        std::vector<float> gains;
        gains.reserve(expected);
        bool expectValue = true;
        for (;;) {
            j = lensshading_detail::SkipWs(text, j, objectEnd);
            if (j >= objectEnd)
                return false;
            if (!expectValue && text[j] == ']') {
                ++j;
                break;
            }
            if (text[j] == ']' || text[j] == ',')
                return false; // Empty array, trailing comma, or bare comma.
            size_t tokenEnd = j;
            while (tokenEnd < objectEnd && lensshading_detail::IsFloatChar(text[tokenEnd]))
                ++tokenEnd;
            float gain = 0.0f;
            if (!lensshading_detail::ParseFloatToken(text.data() + j, text.data() + tokenEnd,
                                                     gain) ||
                gain <= 0.0f)
                return false;
            if (gains.size() >= expected)
                return false;
            gains.push_back(gain);
            expectValue = false;
            j = lensshading_detail::SkipWs(text, tokenEnd, objectEnd);
            if (j >= objectEnd)
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
        if (gains.size() != expected)
            return false;
        out.columns = static_cast<int>(columns);
        out.rows = static_cast<int>(rows);
        out.gains = std::move(gains);
        return true;
    }
}

// Applies lens shading correction in place: each pixel's channel gain is
// the bilinear sample for its CFA tile position `(y % 2) * 2 + (x % 2)`
// over the width x height frame, and the pixel becomes
// `blackLevel + (pixel - blackLevel) * gain`, rounded to nearest and
// clamped to [0, 65535]. Throws std::invalid_argument on bad geometry, a
// pixel count other than `width * height`, an invalid map (like
// validateLensShadingMap()), or a black level that is not finite and
// non-negative.
inline void applyLensShadingMap(std::vector<uint16_t>& pixels, int width, int height,
                                const LensShadingMap& map, float blackLevel = 0.0f) {
    if (width <= 0 || height <= 0)
        throw std::invalid_argument("lens shading dimensions");
    if (pixels.size() != static_cast<size_t>(width) * static_cast<size_t>(height))
        throw std::invalid_argument("lens shading pixel count");
    validateLensShadingMap(map);
    if (!std::isfinite(blackLevel) || blackLevel < 0.0f)
        throw std::invalid_argument("lens shading black level");
    const size_t stride = static_cast<size_t>(width);
    for (int y = 0; y < height; ++y) {
        for (int x = 0; x < width; ++x) {
            const int channel = (y & 1) * 2 + (x & 1);
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
