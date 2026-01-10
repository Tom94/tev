/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#pragma once

#define FMT_HEADER_ONLY 1
#include <fmt/core.h>

#include <nanogui/vector.h>

#include <tinylogger/tinylogger.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <filesystem>
#include <functional>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#    define NOMINMAX
#    include <Windows.h>
#    undef NOMINMAX
#    pragma warning(disable : 4127) // warning C4127: conditional expression is constant
#    pragma warning(disable : 4244) // warning C4244: conversion from X to Y, possible loss of data
#endif

// Define command key for windows/mac/linux
#ifdef __APPLE__
#    define SYSTEM_COMMAND_LEFT GLFW_KEY_LEFT_SUPER
#    define SYSTEM_COMMAND_RIGHT GLFW_KEY_RIGHT_SUPER
#else
#    define SYSTEM_COMMAND_LEFT GLFW_KEY_LEFT_CONTROL
#    define SYSTEM_COMMAND_RIGHT GLFW_KEY_RIGHT_CONTROL
#endif

#ifdef __GNUC__
#    define LIKELY(condition) __builtin_expect(static_cast<bool>(condition), 1)
#    define UNLIKELY(condition) __builtin_expect(static_cast<bool>(condition), 0)
#else
#    define LIKELY(condition) condition
#    define UNLIKELY(condition) condition
#endif

#define TEV_ASSERT(cond, description, ...) \
    if (UNLIKELY(!(cond)))                 \
        throw std::runtime_error { fmt::format(description, ##__VA_ARGS__) }

#ifndef TEV_VERSION
#    define TEV_VERSION "undefined"
#endif

// Make std::filesystem::path formattable.
template <> struct fmt::formatter<std::filesystem::path> : fmt::formatter<std::string_view> {
    template <typename FormatContext> auto format(const std::filesystem::path& path, FormatContext& ctx) const {
        return formatter<std::string_view>::format(path.string(), ctx);
    }
};

template <typename T, size_t N_DIMS> struct fmt::formatter<std::array<T, N_DIMS>> {
    template <class ParseContext> constexpr ParseContext::iterator parse(ParseContext& ctx) { return ctx.begin(); }
    template <class FmtContext> FmtContext::iterator format(const std::array<T, N_DIMS>& v, FmtContext& ctx) const {
        auto&& out = ctx.out();

        fmt::format_to(out, "[");
        for (size_t i = 0; i < N_DIMS; ++i) {
            if (i != 0) {
                fmt::format_to(out, ", ");
            }

            fmt::format_to(out, "{}", v[i]);
        }

        return fmt::format_to(out, "]");
    }
};

template <typename T, size_t N_DIMS> struct fmt::formatter<nanogui::Array<T, N_DIMS>> {
    template <class ParseContext> constexpr ParseContext::iterator parse(ParseContext& ctx) { return ctx.begin(); }
    template <class FmtContext> FmtContext::iterator format(const nanogui::Array<T, N_DIMS>& v, FmtContext& ctx) const {
        auto&& out = ctx.out();

        fmt::format_to(out, "[");
        for (size_t i = 0; i < N_DIMS; ++i) {
            if (i != 0) {
                fmt::format_to(out, ", ");
            }

            fmt::format_to(out, "{}", v[i]);
        }

        return fmt::format_to(out, "]");
    }
};

template <typename T, size_t N_DIMS> struct fmt::formatter<nanogui::Matrix<T, N_DIMS>> {
    template <class ParseContext> constexpr ParseContext::iterator parse(ParseContext& ctx) { return ctx.begin(); }
    template <class FmtContext> FmtContext::iterator format(const nanogui::Matrix<T, N_DIMS>& m, FmtContext& ctx) const {
        auto&& out = ctx.out();

        fmt::format_to(out, "[");
        for (size_t i = 0; i < N_DIMS; ++i) {
            if (i != 0) {
                fmt::format_to(out, ", ");
            }

            fmt::format_to(out, "[");
            for (size_t j = 0; j < N_DIMS; ++j) {
                if (j != 0) {
                    fmt::format_to(out, ", ");
                }

                fmt::format_to(out, "{}", m.m[j][i]);
            }

            fmt::format_to(out, "]");
        }

        return fmt::format_to(out, "]");
    }
};

inline nanogui::Matrix2f extract2x2(const nanogui::Matrix3f& mat) {
    nanogui::Matrix2f result;
    result.m[0][0] = mat.m[0][0];
    result.m[0][1] = mat.m[0][1];
    result.m[1][0] = mat.m[1][0];
    result.m[1][1] = mat.m[1][1];

    return result;
}

inline float extractScale(const nanogui::Matrix3f& mat) {
    float det = mat.m[0][0] * mat.m[1][1] - mat.m[0][1] * mat.m[1][0];
    return std::sqrt(det);
}

template <size_t N_DIMS> nanogui::Array<float, N_DIMS> abs(const nanogui::Array<float, N_DIMS>& v) {
    nanogui::Array<float, N_DIMS> result;
    for (size_t i = 0; i < N_DIMS; ++i) {
        result[i] = std::abs(v[i]);
    }

    return result;
}

template <size_t N_DIMS> nanogui::Array<float, N_DIMS> exp(const nanogui::Array<float, N_DIMS>& v) {
    nanogui::Array<float, N_DIMS> result;
    for (size_t i = 0; i < N_DIMS; ++i) {
        result[i] = std::exp(v[i]);
    }

    return result;
}

template <size_t N_DIMS> nanogui::Array<float, N_DIMS> log(const nanogui::Array<float, N_DIMS>& v) {
    nanogui::Array<float, N_DIMS> result;
    for (size_t i = 0; i < N_DIMS; ++i) {
        result[i] = std::log(v[i]);
    }

    return result;
}

template <size_t N_DIMS> nanogui::Array<float, N_DIMS> max(const nanogui::Array<float, N_DIMS>& a, const nanogui::Array<float, N_DIMS>& b) {
    nanogui::Array<float, N_DIMS> result;
    for (size_t i = 0; i < N_DIMS; ++i) {
        result[i] = std::max(a[i], b[i]);
    }

    return result;
}

template <size_t N_DIMS> nanogui::Array<float, N_DIMS> min(const nanogui::Array<float, N_DIMS>& a, const nanogui::Array<float, N_DIMS>& b) {
    nanogui::Array<float, N_DIMS> result;
    for (size_t i = 0; i < N_DIMS; ++i) {
        result[i] = std::min(a[i], b[i]);
    }

    return result;
}

template <size_t N_DIMS> nanogui::Array<float, N_DIMS> pow(const nanogui::Array<float, N_DIMS>& v, float exponent) {
    nanogui::Array<float, N_DIMS> result;
    for (size_t i = 0; i < N_DIMS; ++i) {
        result[i] = std::pow(v[i], exponent);
    }

    return result;
}

struct NVGcontext;

namespace tev {

namespace fs = std::filesystem;

inline uint16_t swapBytes(uint16_t value) {
#ifdef _WIN32
    return _byteswap_ushort(value);
#else
    return __builtin_bswap16(value);
#endif
}

inline uint32_t swapBytes(uint32_t value) {
#ifdef _WIN32
    return _byteswap_ulong(value);
#else
    return __builtin_bswap32(value);
#endif
}

inline uint64_t swapBytes(uint64_t value) {
#ifdef _WIN32
    return _byteswap_uint64(value);
#else
    return __builtin_bswap64(value);
#endif
}

template <typename T> T swapBytes(T value) {
    T result;
    auto valueChars = reinterpret_cast<char*>(&value);
    auto resultChars = reinterpret_cast<char*>(&result);

    for (size_t i = 0; i < sizeof(T); ++i) {
        resultChars[i] = valueChars[sizeof(T) - 1 - i];
    }

    return result;
}

inline int codePointLength(char first) {
    if ((first & 0xf8) == 0xf0) {
        return 4;
    } else if ((first & 0xf0) == 0xe0) {
        return 3;
    } else if ((first & 0xe0) == 0xc0) {
        return 2;
    } else {
        return 1;
    }
}

std::string ensureUtf8(std::string_view str);
std::string utf16to8(std::wstring_view utf16);
fs::path toPath(std::string_view utf8);
std::string toString(const fs::path& path);
std::string toDisplayString(const fs::path& path);

bool naturalCompare(std::string_view a, std::string_view b);

template <typename T> void removeDuplicates(std::vector<T>& vec) {
    std::unordered_set<T> tmp;
    size_t idx = 0;
    for (const auto& v : vec) {
        if (tmp.contains(v)) {
            continue;
        }

        tmp.insert(v);
        vec[idx++] = v;
    }

    vec.resize(idx);
}

// Taken from https://en.wikibooks.org/wiki/Algorithm_Implementation/Strings/Levenshtein_distance#C++
template <typename T> typename T::size_type levenshteinDistance(const T& source, const T& target) {
    if (source.size() > target.size()) {
        return levenshteinDistance(target, source);
    }

    using TSizeType = typename T::size_type;
    const TSizeType minSize = source.size(), max_size = target.size();
    std::vector<TSizeType> levDist(minSize + 1);

    for (TSizeType i = 0; i <= minSize; ++i) {
        levDist[i] = i;
    }

    for (TSizeType j = 1; j <= max_size; ++j) {
        TSizeType previousDiagonal = levDist[0], previousDiagonalSave;
        ++levDist[0];

        for (TSizeType i = 1; i <= minSize; ++i) {
            previousDiagonalSave = levDist[i];
            if (source[i - 1] == target[j - 1]) {
                levDist[i] = previousDiagonal;
            } else {
                levDist[i] = std::min(std::min(levDist[i - 1], levDist[i]), previousDiagonal) + 1;
            }

            previousDiagonal = previousDiagonalSave;
        }
    }

    return levDist[minSize];
}

template <typename F> void forEachFileInDir(bool recursive, const fs::path& path, F&& callback) {
    // Ignore errors in case a directory no longer exists. Simply don't invoke the loop body in that case.
    std::error_code ec;
    if (recursive) {
        for (const auto& entry : fs::recursive_directory_iterator{path, ec}) {
            callback(entry);
        }
    } else {
        for (const auto& entry : fs::directory_iterator{path, ec}) {
            callback(entry);
        }
    }
}

template <typename T> class ScopeGuard {
public:
    ScopeGuard(const T& callback) : mCallback{callback} {}
    ScopeGuard(T&& callback) : mCallback{std::move(callback)} {}
    ScopeGuard(const ScopeGuard<T>& other) = delete;
    ScopeGuard& operator=(const ScopeGuard<T>& other) = delete;
    ScopeGuard(ScopeGuard<T>&& other) { *this = std::move(other); }
    ScopeGuard& operator=(ScopeGuard<T>&& other) {
        mCallback = std::move(other.mCallback);
        other.mCallback = {};
        return *this;
    }

    ~ScopeGuard() {
        if (mArmed) {
            mCallback();
        }
    }

    void disarm() { mArmed = false; }

private:
    T mCallback;
    bool mArmed = true;
};

template <typename T> class HeapArray {
public:
    HeapArray() : mBuf{nullptr}, mSize{0} {}
    HeapArray(size_t size) : mBuf{std::make_unique<T[]>(size)}, mSize{size} {}
    HeapArray(HeapArray&& other) = default;
    HeapArray& operator=(HeapArray&& other) = default;

    operator bool() const { return mBuf != nullptr; }
    T& operator[](size_t idx) { return mBuf[idx]; }
    const T& operator[](size_t idx) const { return mBuf[idx]; }

    T* data() { return mBuf.get(); }
    const T* data() const { return mBuf.get(); }

    size_t size() const { return mSize; }

    operator std::span<const T>() const { return std::span<const T>{mBuf.get(), mSize}; }
    operator std::span<T>() { return std::span<T>{mBuf.get(), mSize}; }

private:
    std::unique_ptr<T[]> mBuf;
    size_t mSize;
};

template <typename T> T round(T value, T decimals) {
    auto precision = std::pow(static_cast<T>(10), decimals);
    return std::round(value * precision) / precision;
}

template <typename T> T nextPot(T value) {
    if (value == 0) {
        return 1;
    }

    T pot = 1;
    while (pot < value) {
        pot <<= 1;
    }

    return pot;
}

inline bool isPot(size_t value) {
    if (value == 0) {
        return false;
    }

    return (value & (value - 1)) == 0;
}

template <typename Int> Int divRoundUp(Int value, Int divisor) { return (value + divisor - 1) / divisor; }

template <typename Int> Int nextMultiple(Int value, Int multiple) { return divRoundUp(value, multiple) * multiple; }

template <typename T> std::string join(const T& components, std::string_view delim) {
    std::ostringstream s;
    for (const auto& component : components) {
        if (&components[0] != &component) {
            s << delim;
        }

        s << component;
    }

    return s.str();
}

// If `inclusive` is true, trailing delimiters are included in the resulting parts.
std::vector<std::string_view> split(std::string_view text, std::string_view delim, bool inclusive = false);
std::vector<std::string_view> splitWhitespace(std::string_view text, bool inclusive = false);

std::string toLower(std::string_view str);
std::string toUpper(std::string_view str);

std::string_view trimLeft(std::string_view s);
std::string_view trimRight(std::string_view s);
std::string_view trim(std::string_view s);

bool matchesFuzzy(std::string_view text, std::string_view filter, size_t* matchedPartId = nullptr);
bool matchesRegex(std::string_view text, std::string_view filter);
inline bool matchesFuzzyOrRegex(std::string_view text, std::string_view filter, bool isRegex) {
    return isRegex ? matchesRegex(text, filter) : matchesFuzzy(text, filter);
}

void drawTextWithShadow(NVGcontext* ctx, float x, float y, std::string_view text, float shadowAlpha = 1.0f);

int maxTextureSize();

inline float toSRGB(float val, float gamma = 2.4f) {
    static constexpr float a = 0.055f;
    static constexpr float threshold = 0.0031308f;

    const float absVal = std::abs(val);
    if (absVal <= threshold) {
        return 12.92f * val;
    } else {
        return std::copysign((1.0f + a) * std::pow(absVal, 1.0f / gamma) - a, val);
    }
}

inline float toLinear(float val, float gamma = 2.4f) {
    static constexpr float a = 0.055f;
    static constexpr float threshold = 0.04045f;

    const float absVal = std::abs(val);
    if (absVal <= threshold) {
        return val / 12.92f;
    } else {
        return std::copysign(std::pow((absVal + a) / (1.0f + a), gamma), val);
    }
}

int lastError();
int lastSocketError();
std::string errorString(int errorId);

fs::path homeDirectory();
fs::path runtimeDirectory();

void toggleConsole();

bool shuttingDown();
void setShuttingDown();

struct FlatpakInfo {
    std::string flatpakId = "";
    std::unordered_map<std::string_view, std::unordered_map<std::string_view, std::string>> metadata;

    bool hasNetworkAccess() const {
        const auto it = metadata.find("Context");
        if (it == metadata.end()) {
            return false;
        }

        const auto accessIt = it->second.find("shared");
        if (accessIt == it->second.end()) {
            return false;
        }

        const auto parts = split(accessIt->second, ";");
        return std::find(parts.begin(), parts.end(), "network") != parts.end();
    }
};

const std::optional<FlatpakInfo>& flatpakInfo();

enum EInterpolationMode : int {
    Nearest = 0,
    Bilinear,
    Trilinear,

    // This enum value should never be used directly. It facilitates looping over all members of this enum.
    NumInterpolationModes,
};

EInterpolationMode toInterpolationMode(std::string_view name);
std::string toString(EInterpolationMode mode);

enum class ETonemap : int {
    None = 0,
    SRGB = 0,
    Gamma,
    FalseColor,
    PositiveNegative,

    // This enum value should never be used directly. It facilitates looping over all members of this enum.
    Count,
};

ETonemap toTonemap(std::string_view name);

enum class EMetric : int {
    Error = 0,
    AbsoluteError,
    SquaredError,
    RelativeAbsoluteError,
    RelativeSquaredError,

    // This enum value should never be used directly. It facilitates looping over all members of this enum.
    Count,
};

EMetric toMetric(std::string_view name);

enum EDirection {
    Forward,
    Backward,
};

enum EOrientation : int {
    None = 0,
    TopLeft = 1,
    TopRight = 2,
    BottomRight = 3,
    BottomLeft = 4,
    LeftTop = 5,
    RightTop = 6,
    RightBottom = 7,
    LeftBottom = 8,
};

inline nanogui::Vector2i applyOrientation(EOrientation orientation, const nanogui::Vector2i& pos, const nanogui::Vector2i& size) {
    switch (orientation) {
        case None: return pos;
        case TopLeft: return pos;
        case TopRight: return {size.x() - pos.x() - 1, pos.y()};
        case BottomRight: return {size.x() - pos.x() - 1, size.y() - pos.y() - 1};
        case BottomLeft: return {pos.x(), size.y() - pos.y() - 1};
        case LeftTop: return {pos.y(), pos.x()};
        case RightTop: return {pos.y(), size.x() - pos.x() - 1};
        case RightBottom: return {size.y() - pos.y() - 1, size.x() - pos.x() - 1};
        case LeftBottom: return {size.y() - pos.y() - 1, pos.x()};
    }

    return pos;
}

enum class EPixelFormat {
    U8,
    U16,
    F16,
    F32,
};

std::string toString(EPixelFormat format);

inline size_t nBytes(EPixelFormat format) {
    switch (format) {
        case EPixelFormat::U8: return 1;
        case EPixelFormat::U16: return 2;
        case EPixelFormat::F16: return 2;
        case EPixelFormat::F32: return 4;
    }

    return 0;
}

inline size_t nBits(EPixelFormat format) { return nBytes(format) * 8; }

// Implemented in main.cpp
void scheduleToMainThread(const std::function<void()>& fun);
void redrawWindow();

static const nanogui::Color IMAGE_COLOR = {0.35f, 0.35f, 0.8f, 1.0f};
static const nanogui::Color REFERENCE_COLOR = {0.7f, 0.4f, 0.4f, 1.0f};
static const nanogui::Color CROP_COLOR = {0.2f, 0.5f, 0.2f, 1.0f};

// Exceptions
class ImageLoadError : public std::runtime_error {
public:
    ImageLoadError(const std::string& message) : std::runtime_error{message} {}
};

class ImageModifyError : public std::runtime_error {
public:
    ImageModifyError(const std::string& message) : std::runtime_error{message} {}
};

class ImageSaveError : public std::runtime_error {
public:
    ImageSaveError(const std::string& message) : std::runtime_error{message} {}
};

} // namespace tev
