// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD-style license contained in the LICENSE.txt file.

#pragma once

#include "../include/Macros.h"

#include <algorithm>

template <typename T>
constexpr T clamp(T value, T min, T max) {
    TVIEW_ASSERT(max >= min, "Minimum may not be larger than maximum.");
    return std::max(std::min(value, max), min);
}
