// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tinyformat.h>
#include <tinylogger/tinylogger.h>

#include <algorithm>
#include <cmath>
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

// Needs to be included _after_ Windows.h to ensure NOMINMAX has an effect
#include <filesystem/path.h>

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
        throw std::runtime_error{tfm::format(description, ##__VA_ARGS__)};

#ifndef TEV_VERSION
#   define TEV_VERSION "undefined"
#endif

struct NVGcontext;

TEV_NAMESPACE_BEGIN

class ThreadPool;
extern ThreadPool* gThreadPool;

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

inline bool isSystemLittleEndian() {
    uint16_t beef = 0xbeef;
    return *reinterpret_cast<const uint8_t*>(&beef) == 0xef;
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
std::wstring utf8to16(const std::string& utf8);
std::string utf16to8(const std::wstring& utf16);

#ifdef _WIN32
inline std::wstring nativeString(const filesystem::path& path) {
    return path.wstr();
}
#else
inline std::string nativeString(const filesystem::path& path) {
    return path.str();
}
#endif

class ScopeGuard {
public:
    ScopeGuard(const std::function<void(void)>& callback) : mCallback{callback} {}
    ~ScopeGuard() { mCallback(); }
private:
    std::function<void(void)> mCallback;
};

template <typename T>
T clamp(T value, T min, T max) {
    TEV_ASSERT(max >= min, "Minimum (%f) may not be larger than maximum (%f).", min, max);
    return std::max(std::min(value, max), min);
}

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

filesystem::path homeDirectory();

void toggleConsole();

// Implemented in main.cpp
void scheduleToMainThread(const std::function<void()>& fun);

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

TEV_NAMESPACE_END
