// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <nanogui/vector.h>

#include <tinylogger/tinylogger.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <format>
#include <functional>
#include <string>
#include <sstream>
#include <vector>

#ifdef _WIN32
#   define NOMINMAX
#   include <Windows.h>
#   undef NOMINMAX
#   pragma warning(disable : 4127) // warning C4127: conditional expression is constant
#   pragma warning(disable : 4244) // warning C4244: conversion from X to Y, possible loss of data
#endif

// Define command key for windows/mac/linux
#ifdef __APPLE__
#define SYSTEM_COMMAND_LEFT GLFW_KEY_LEFT_SUPER
#define SYSTEM_COMMAND_RIGHT GLFW_KEY_RIGHT_SUPER
#else
#define SYSTEM_COMMAND_LEFT GLFW_KEY_LEFT_CONTROL
#define SYSTEM_COMMAND_RIGHT GLFW_KEY_RIGHT_CONTROL
#endif

// A macro is used such that external tools won't end up indenting entire files,
// resulting in wasted horizontal space.
#define TEV_NAMESPACE_BEGIN namespace tev {
#define TEV_NAMESPACE_END }

#ifdef __GNUC__
#   define LIKELY(condition) __builtin_expect(static_cast<bool>(condition), 1)
#   define UNLIKELY(condition) __builtin_expect(static_cast<bool>(condition), 0)
#else
#   define LIKELY(condition) condition
#   define UNLIKELY(condition) condition
#endif

#define TEV_ASSERT(cond, description, ...) \
    if (UNLIKELY(!(cond))) \
        throw std::runtime_error{std::format(description, ##__VA_ARGS__)};

#ifndef TEV_VERSION
#   define TEV_VERSION "undefined"
#endif

// Make std::filesystem::path formattable.
template <>
struct std::formatter<std::filesystem::path> : std::formatter<std::string_view> {
    auto format(const std::filesystem::path& path, std::format_context& ctx) {
        return std::format_to(ctx.out(), "{}", path.string());
    }
};

struct NVGcontext;

namespace nanogui {
    template <typename Value, size_t Size>
    Array<Value, Size> inverse(const Array<Value, Size> &a) {
        Array<Value, Size> result;
        for (size_t i = 0; i < Size; ++i)
            result.v[i] = 1.0f / a.v[i];
        return result;
    }

    template <typename Value, size_t Size>
    Value mean(const Array<Value, Size> &a) {
        Value result = 0;
        for (size_t i = 0; i < Size; ++i)
            result += a.v[i];
        return result / (Value)Size;
    }

    inline Matrix3f inverse(Matrix3f mat) {
        float d11 = mat.m[1][1] * mat.m[2][2] + mat.m[1][2] * -mat.m[2][1];
        float d12 = mat.m[1][0] * mat.m[2][2] + mat.m[1][2] * -mat.m[2][0];
        float d13 = mat.m[1][0] * mat.m[2][1] + mat.m[1][1] * -mat.m[2][0];

        float det = mat.m[0][0] * d11 - mat.m[0][1] * d12 + mat.m[0][2] * d13;

        if (std::abs(det) == 0.0f) {
            return Matrix3f{0.0f};
        }

        det = 1.0f / det;

        float d21 = mat.m[0][1] * mat.m[2][2] + mat.m[0][2] * -mat.m[2][1];
        float d22 = mat.m[0][0] * mat.m[2][2] + mat.m[0][2] * -mat.m[2][0];
        float d23 = mat.m[0][0] * mat.m[2][1] + mat.m[0][1] * -mat.m[2][0];

        float d31 = mat.m[0][1] * mat.m[1][2] - mat.m[0][2] * mat.m[1][1];
        float d32 = mat.m[0][0] * mat.m[1][2] - mat.m[0][2] * mat.m[1][0];
        float d33 = mat.m[0][0] * mat.m[1][1] - mat.m[0][1] * mat.m[1][0];

        mat.m[0][0] = +d11 * det;
        mat.m[0][1] = -d21 * det;
        mat.m[0][2] = +d31 * det;
        mat.m[1][0] = -d12 * det;
        mat.m[1][1] = +d22 * det;
        mat.m[1][2] = -d32 * det;
        mat.m[2][0] = +d13 * det;
        mat.m[2][1] = -d23 * det;
        mat.m[2][2] = +d33 * det;
        return mat;
    }

    inline Matrix2f extract2x2(const Matrix3f& mat) {
        Matrix2f result;
        result.m[0][0] = mat.m[0][0];
        result.m[0][1] = mat.m[0][1];
        result.m[1][0] = mat.m[1][0];
        result.m[1][1] = mat.m[1][1];
        return result;
    }

    inline float extractScale(const Matrix3f& mat) {
        float det = mat.m[0][0] * mat.m[1][1] - mat.m[0][1] * mat.m[1][0];
        return std::sqrt(det);
    }

    template <typename Value, size_t Size>
    Array<Value, Size> operator*(const Matrix<Value, Size>& m, const Array<Value, Size>& v) {
        Array<Value, Size> result;
        for (size_t i = 0; i < Size; ++i) {
            Value accum = 0;
            for (size_t k = 0; k < Size; ++k)
                accum += m.m[k][i] * v.v[k];

            result.v[i] = accum;
        }
        return result;
    }

    template <typename Value, size_t Size>
    Array<Value, Size-1> operator*(const Matrix<Value, Size>& m, const Array<Value, Size-1>& v) {
        Array<Value, Size-1> result;
        Value w = 0;
        for (size_t i = 0; i < Size; ++i) {
            Value accum = 0;
            for (size_t k = 0; k < Size; ++k)
                accum += m.m[k][i] * (k == Size - 1 ? 1 : v.v[k]);

            if (i == Size-1) {
                w = accum;
            } else {
                result.v[i] = accum;
            }
        }
        return result / w;
    }

    template <typename Value, size_t Size>
    bool operator==(const Matrix<Value, Size>& a, const Matrix<Value, Size>& b) {
        for (size_t m = 0; m < Size; ++m) {
            for (size_t n = 0; n < Size; ++n) {
                if (a.m[m][n] != b.m[m][n]) {
                    return false;
                }
            }
        }
        return true;
    }

    template <typename Value, size_t Size>
    bool operator!=(const Matrix<Value, Size>& a, const Matrix<Value, Size>& b) {
        return !(a == b);
    }
}

TEV_NAMESPACE_BEGIN

namespace fs = std::filesystem;

inline uint32_t swapBytes(uint32_t value) {
#ifdef _WIN32
    return _byteswap_ulong(value);
#else
    return __builtin_bswap32(value);
#endif
}

inline float swapBytes(float value) {
    float result;
    auto valueChars = reinterpret_cast<char*>(&value);
    auto resultChars = reinterpret_cast<char*>(&result);

    resultChars[0] = valueChars[3];
    resultChars[1] = valueChars[2];
    resultChars[2] = valueChars[1];
    resultChars[3] = valueChars[0];

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

std::string ensureUtf8(const std::string& str);
std::string utf16to8(const std::wstring& utf16);
fs::path toPath(const std::string& utf8);
std::string toString(const fs::path& path);

// Taken from https://en.wikibooks.org/wiki/Algorithm_Implementation/Strings/Levenshtein_distance#C++
template<typename T>
typename T::size_type levenshteinDistance(const T& source, const T& target) {
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

template <typename F>
void forEachFileInDir(bool recursive, const fs::path& path, F&& callback) {
    // Ignore errors in case a directory no longer exists.
    // Simply don't invoke the loop body in that case.
    std::error_code ec;
    if (recursive) {
        for (auto const& entry : fs::recursive_directory_iterator{path, ec}) {
            callback(entry);
        }
    } else {
        for (auto const& entry : fs::directory_iterator{path, ec}) {
            callback(entry);
        }
    }
}

template <typename T>
class ScopeGuard {
public:
    ScopeGuard(const T& callback) : mCallback{callback} {}
    ScopeGuard(T&& callback) : mCallback{std::move(callback)} {}
    ScopeGuard(const ScopeGuard<T>& other) = delete;
    ScopeGuard& operator=(const ScopeGuard<T>& other) = delete;
    ScopeGuard(ScopeGuard<T>&& other) { *this = std::move(other); }
    ScopeGuard& operator=(ScopeGuard<T>&& other) { mCallback = std::move(other.mCallback); other.mCallback = {}; }
    ~ScopeGuard() { mCallback(); }
private:
    T mCallback;
};

template <typename T>
T round(T value, T decimals) {
    auto precision = std::pow(static_cast<T>(10), decimals);
    return std::round(value * precision) / precision;
}

template <typename T>
std::string join(const T& components, const std::string& delim) {
    std::ostringstream s;
    for (const auto& component : components) {
        if (&components[0] != &component) {
            s << delim;
        }
        s << component;
    }

    return s.str();
}

std::vector<std::string> split(std::string text, const std::string& delim);

std::string toLower(std::string str);
std::string toUpper(std::string str);

bool matchesFuzzy(std::string text, std::string filter, size_t* matchedPartId = nullptr);
bool matchesRegex(std::string text, std::string filter);
inline bool matchesFuzzyOrRegex(const std::string& text, const std::string& filter, bool isRegex) {
    return isRegex ? matchesRegex(text, filter) : matchesFuzzy(text, filter);
}

void drawTextWithShadow(NVGcontext* ctx, float x, float y, std::string text, float shadowAlpha = 1.0f);

inline float toSRGB(float linear, float gamma = 2.4f) {
    static const float a = 0.055f;
    if (linear <= 0.0031308f) {
        return 12.92f * linear;
    } else {
        return (1 + a) * pow(linear, 1 / gamma) - a;
    }
}

inline float toLinear(float sRGB, float gamma = 2.4f) {
    static const float a = 0.055f;
    if (sRGB <= 0.04045f) {
        return sRGB / 12.92f;
    } else {
        return pow((sRGB + a) / (1 + a), gamma);
    }
}

int lastError();
int lastSocketError();
std::string errorString(int errorId);

fs::path homeDirectory();

void toggleConsole();

bool shuttingDown();
void setShuttingDown();

enum ETonemap : int {
    SRGB = 0,
    Gamma,
    FalseColor,
    PositiveNegative,

    // This enum value should never be used directly.
    // It facilitates looping over all members of this enum.
    NumTonemaps,
};

ETonemap toTonemap(std::string name);

enum EMetric : int {
    Error = 0,
    AbsoluteError,
    SquaredError,
    RelativeAbsoluteError,
    RelativeSquaredError,

    // This enum value should never be used directly.
    // It facilitates looping over all members of this enum.
    NumMetrics,
};

EMetric toMetric(std::string name);

enum EDirection {
    Forward,
    Backward,
};

// Implemented in main.cpp
void scheduleToMainThread(const std::function<void()>& fun);
void redrawWindow();

static const nanogui::Color IMAGE_COLOR = {0.35f, 0.35f, 0.8f, 1.0f};
static const nanogui::Color REFERENCE_COLOR = {0.7f, 0.4f, 0.4f, 1.0f};

TEV_NAMESPACE_END
