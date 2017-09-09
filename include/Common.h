// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tinyformat.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <sstream>
#include <vector>

#ifdef _WIN32
#pragma warning(disable : 4127) // warning C4127: conditional expression is constant
#pragma warning(disable : 4244) // warning C4244: conversion from X to Y, possible loss of data
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

#define TEV_ASSERT(cond, description, ...) if (UNLIKELY(!(cond))) std::cerr << tfm::format(description, ##__VA_ARGS__) << std::endl;

TEV_NAMESPACE_BEGIN

template<typename T, typename... Args>
std::unique_ptr<T> make_unique(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

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

bool matches(std::string text, std::string filter);

enum ETonemap : int {
    SRGB = 0,
    Gamma,
    FalseColor,
    PositiveNegative,

    // This enum value should never be used directly.
    // It facilitates looping over all members of this enum.
    AmountTonemaps,
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
    AmountMetrics,
};

EMetric toMetric(std::string name);

enum EDirection {
    Forward,
    Backward,
};

void toggleConsole();

TEV_NAMESPACE_END
