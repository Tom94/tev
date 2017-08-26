// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#pragma once

#include "../include/Macros.h"

#include <algorithm>

// Taken from https://stackoverflow.com/a/26221725
template<typename ... Args>
std::string format(const std::string& format, Args ... args) {
    size_t size = snprintf(nullptr, 0, format.c_str(), args ...) + 1; // Extra space for '\0'
    std::unique_ptr<char[]> buf(new char[size]);
    snprintf(buf.get(), size, format.c_str(), args ...);
    return std::string(buf.get(), buf.get() + size - 1); // We don't want the '\0' inside
}

template <typename T>
constexpr T clamp(T value, T min, T max) {
    TEV_ASSERT(max >= min, "Minimum (%f) may not be larger than maximum (%f).", min, max);
    return std::max(std::min(value, max), min);
}
