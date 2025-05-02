/*
 * tev -- the EXR viewer
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

#include <tev/Common.h>
#include <tev/Task.h>

#include <nanogui/vector.h>

#include <array>

namespace tev {

nanogui::Matrix4f convertChromaToRec709(std::array<nanogui::Vector2f, 4> chroma);

enum class EAlphaKind { Premultiplied, PremultipliedLinear, Straight, None };

// Converts colors from an ICC profile to linear sRGB Rec.709 w/ *straight* alpha. We would have preferred to convert in premultiplied
// space, but the underlying color management library (lcms2) does not support this. Hence, callers of this function are responsible to
// remultiply alpha if desired.
//
// Note that, because we this function converts potentially larger color gamuts to sRGB, output channels may have values larger than 1 or
// smaller than 0, even if the input was within [0, 1]. This is by design, and underlying rendering APIs, like OpenGL and Metal will
// correctly display such colors on HDR displays.
Task<void> convertIccToRec709(
    const std::vector<uint8_t>& iccProfile,
    const nanogui::Vector2i& size,
    int numColorChannels,
    EAlphaKind alphaKind,
    float* __restrict src,
    float* __restrict rgbaDst,
    int priority
);

} // namespace tev
