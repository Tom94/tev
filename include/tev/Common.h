// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tinyformat.h>

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

struct NVGcontext;

TEV_NAMESPACE_BEGIN

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

bool endsWith(const std::string& str, const std::string& ending);

bool matches(std::string text, std::string filter);

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

std::string absolutePath(const std::string& path);

std::string homeDirectory();

void toggleConsole();

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
