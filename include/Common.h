// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#pragma once

#include <tinyformat.h>

#include <algorithm>

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

template <typename T>
constexpr T clamp(T value, T min, T max) {
    TEV_ASSERT(max >= min, "Minimum (%f) may not be larger than maximum (%f).", min, max);
    return std::max(std::min(value, max), min);
}

template <typename T>
constexpr T round(T value, T decimals) {
    return std::round(value * std::pow(static_cast<T>(10), decimals)) / std::pow(static_cast<T>(10), decimals);
}

TEV_NAMESPACE_END
