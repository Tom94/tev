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

nanogui::Matrix4f adobeToRec709Matrix();
nanogui::Matrix4f proPhotoToRec709Matrix();

std::array<nanogui::Vector2f, 4> rec709Chroma();
std::array<nanogui::Vector2f, 4> adobeChroma();
std::array<nanogui::Vector2f, 4> proPhotoChroma();

nanogui::Matrix3f adaptToXYZD50Bradford(const nanogui::Vector2f& xy);

nanogui::Matrix4f toMatrix4(const nanogui::Matrix3f& mat);

enum class EAlphaKind {
    // This refers to premultiplied alpha in nonlinear space, i.e. after a transfer function like gamma correction. This kind of
    // premultiplied alpha has generally little use, since one should not blend in non-linear space. But, regrettably, some image formats
    // represent premultiplied alpha this way. Our color management system (lcms2) for handling ICC color profiles unfortunately also
    // expects this kind of premultiplied alpha, so we have to support it.
    PremultipliedNonlinear,
    // This refers to premultiplied alpha in linear space, i.e. before a transfer function like gamma correction. This is the most useful
    // kind of premultiplied alpha.
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

// Partial implementation of https://www.itu.int/rec/T-REC-H.273-202407-I/en (no YCbCr conversion)
namespace ituth273 {
enum class EColorPrimaries : uint8_t {
    BT709 = 1,
    Unspecified = 2,
    BT470M = 4,
    BT470BG = 5,
    SMPTE170M = 6,
    SMPTE240M = 7,
    Film = 8,
    BT2020 = 9,
    SMPTE428 = 10,
    SMPTE431 = 11,
    SMPTE432 = 12,
    Weird = 22, // The spec says "No corresponding industry specification identified"
};

std::array<nanogui::Vector2f, 4> chroma(const EColorPrimaries primaries);

inline bool isTransferImplemented(const uint8_t transfer) {
    switch (transfer) {
        // BT.709
        case 1:
        case 6:
        case 14:
        case 15:
        // Variations of BT.709
        case 11: // Handles negative values by mirroring
        case 12: // Has special negative value handling
        // Simple gamma transfers
        case 4:
        case 5:
        // SMPTE ST 240
        case 7:
        // Linear
        case 8:
        // Logarithmic
        case 9:
        case 10:
        // sRGB
        case 13:
        // SMPTE ST 2084 (PQ)
        case 16:
        // SMPTE ST 428-1
        case 17:
        // HLG (hybrid log gamma)
        case 18: return true;
        default: return false;
    }
}

inline float bt709ToLinear(float val) {
    constexpr float beta = 0.018053968510807f;
    constexpr float alpha = 1.0f + 5.5f * beta;
    constexpr float thres = 4.5f * beta;
    return val <= thres ? (val / 4.5f) : std::pow((val + alpha - 1.0f) / alpha, 1.0f / 0.45f);
}

// From https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.1361-0-199802-W!!PDF-E.pdf, generalized to the more precise constants from the
// bt709ToLinear function as defined in https://www.itu.int/rec/T-REC-H.273-202407-I/en.
inline float bt1361ExtendedToLinear(float val) {
    constexpr float beta = 0.018053968510807f;
    constexpr float alpha = 1.0f + 5.5f * beta;
    constexpr float thres = 4.5f * beta;
    constexpr float negThres = -thres / 4.0f;

    if (val < negThres) {
        return -std::pow((-val * 4.0f + alpha - 1.0f) / alpha, 1.0f / 0.45f) / 4.0f;
    } else if (val <= thres) {
        return val / 4.5f;
    } else {
        return std::pow((val + alpha - 1.0f) / alpha, 1.0f / 0.45f);
    }
}

// From http://car.france3.mars.free.fr/HD/INA-%2026%20jan%2006/SMPTE%20normes%20et%20confs/s240m.pdf
inline float smpteSt240ToLinear(float val) { return val <= 0.0913f ? (val / 4.0f) : pow((val + 0.1115f) / 1.1115f, 1.0f / 0.45f); }

inline float pqToLinear(float val) {
    constexpr float c1 = 107.0f / 128.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;
    constexpr float invm1 = 8192.0f / 1305.0f;
    constexpr float invm2 = 32.0f / 2523.0f;

    const float tmp = std::pow(std::max(val, 0.0f), invm2);
    const float result_cdm2 = 10000.0f * std::pow(std::max(tmp - c1, 0.0f) / std::max(c2 - c3 * tmp, 1e-5f), invm1);
    return result_cdm2 / 80.0f; // Convert to linear sRGB units where SDR white (1.0) is 80 cd/m^2
}

inline float hlgToLinear(float val) {
    constexpr float a = 0.17883277f;
    constexpr float b = 0.28466892f;
    constexpr float c = 0.55991073f;
    const float result_cdm2 = 1000.0f * (val <= 0.5f ? (val * val / 3.0f) : ((std::exp((val - c) / a) + b) / 12.0f));
    return result_cdm2 / 80.0f; // Convert to linear sRGB units where SDR white (1.0) is 80 cd/m^2
}

inline float invTransfer(const uint8_t transfer, float val) {
    switch (transfer) {
        default: return val; // Not implemented
        case 1: // BT.709, BT.1361
        case 6: // BT.601, BT.1358, BT.1700, SMPTE ST 170
        case 14: // BT.2020 (10 bit)
        case 15: // BT.2020 (12 bit)
            return bt709ToLinear(val);
        case 11: // IEC 61966-2-4 (Handle negative values by mirroring)
            return std::copysign(bt709ToLinear(std::abs(val)), val);
        case 12: // BT 1361 extended color gamut system
            return bt1361ExtendedToLinear(val);
        case 4: // Gamma 2.2
            return std::pow(val, 2.2f);
        case 5: // Gamma 2.8
            return std::pow(val, 2.8f);
        case 7: // SMPTE ST 240
            return smpteSt240ToLinear(val);
        case 8: // Linear
            return val;
        case 9: // Logarithmic 100:1
            return val > 0.0f ? std::exp((val - 1.0f) * 2.0f * std::log(10.0f)) : 0.0f;
        case 10: // Logarithmic 100*srqt(10):1
            return val > 0.0f ? std::exp((val - 1.0f) * 2.5f * std::log(10.0f)) : 0.0f;
        case 13: // sRGB
            return toLinear(val);
        case 16: // SMPTE ST 2084 (PQ)
            return pqToLinear(val);
        case 17: // SMPTE ST 428-1
            return std::pow((52.37f / 48.0f) * val, 2.6f);
        case 18: // HLG (hybrid log gamma)
            return hlgToLinear(val);
    }

    return val;
}
} // namespace ituth273

} // namespace tev
