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

nanogui::Matrix4f chromaToRec709Matrix(const std::array<nanogui::Vector2f, 4>& chroma);
nanogui::Matrix4f xyzToChromaMatrix(const std::array<nanogui::Vector2f, 4>& chroma);

nanogui::Matrix4f xyzToRec709Matrix();
nanogui::Matrix3f adaptToXYZD50Bradford(const nanogui::Vector2f& xy);

nanogui::Matrix4f toMatrix4(const nanogui::Matrix3f& mat);

enum class EAlphaKind {
    // This refers to premultiplied alpha in nonlinear space, i.e. after a transfer function like gamma correction. This kind of
    // premultiplied alpha has generally little use, since one should not blend in non-linear space. But, regrettably, some image formats
    // represent premultiplied alpha this way.
    PremultipliedNonlinear,
    // This refers to premultiplied alpha in linear space, i.e. before a transfer function like gamma correction. This is the most common
    // and useful kind of premultiplied alpha and the one color management systems (like lcms2) expect.
    Premultiplied,
    Straight,
    None,
};

enum class EPixelFormat {
    U8,
    U16,
    F16,
    F32,
};

class ColorProfile {
public:
    ColorProfile(void* profile) : mProfile{profile} {}
    ~ColorProfile();

    static ColorProfile fromIcc(const uint8_t* iccProfile, size_t iccProfileSize);
    static ColorProfile srgb();
    static ColorProfile linearSrgb();

    void* get() const { return mProfile; }

    bool isValid() const { return mProfile; }

private:
    void* mProfile = nullptr;
};

// Converts colors from an ICC profile to linear sRGB Rec.709 w/ premultiplied alpha.
//
// Note that, because we this function converts potentially larger color gamuts to sRGB, output channels may have values larger than 1 or
// smaller than 0, even if the input was within [0, 1]. This is by design, and, on macOS, the OS translates these out-of-bounds colors
// correctly to the gamut of the display. Other operating systems, like Windows and Linux don't do this -- it's a TODO for tev to explicitly
// hook into these OSs' color management systems to ensure that out-of-bounds colors are displayed correctly.
Task<void> toLinearSrgbPremul(
    const ColorProfile& profile,
    const nanogui::Vector2i& size,
    int numColorChannels,
    EAlphaKind alphaKind,
    EPixelFormat pixelFormat,
    uint8_t* __restrict src,
    float* __restrict rgbaDst,
    int priority
);

} // namespace tev
