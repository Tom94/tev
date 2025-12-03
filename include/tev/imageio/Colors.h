/*
 * tev -- the EDR viewer
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
#include <optional>

namespace tev {

enum class ERenderingIntent {
    Perceptual = 0,
    RelativeColorimetric = 1,
    Saturation = 2,
    AbsoluteColorimetric = 3,
};

std::string_view toString(ERenderingIntent intent);

std::array<nanogui::Vector2f, 4> zeroChroma();

nanogui::Matrix3f xyzToChromaMatrix(const std::array<nanogui::Vector2f, 4>& chroma);
nanogui::Matrix3f adaptWhiteBradford(const nanogui::Vector2f& srcWhite, const nanogui::Vector2f& dstWhite);

nanogui::Matrix3f convertColorspaceMatrix(
    const std::array<nanogui::Vector2f, 4>& srcChroma, const std::array<nanogui::Vector2f, 4>& dstChroma, ERenderingIntent intent
);

nanogui::Vector2f whiteD50();
nanogui::Vector2f whiteD55();
nanogui::Vector2f whiteD65();
nanogui::Vector2f whiteD75();
nanogui::Vector2f whiteD93();

nanogui::Vector2f whiteA();
nanogui::Vector2f whiteB();
nanogui::Vector2f whiteC();

nanogui::Vector2f whiteCenter();
nanogui::Vector2f whiteDci();

std::array<nanogui::Vector2f, 4> rec709Chroma();
std::array<nanogui::Vector2f, 4> adobeChroma();
std::array<nanogui::Vector2f, 4> proPhotoChroma();
std::array<nanogui::Vector2f, 4> displayP3Chroma();
std::array<nanogui::Vector2f, 4> dciP3Chroma();
std::array<nanogui::Vector2f, 4> bt2020Chroma();
std::array<nanogui::Vector2f, 4> bt2100Chroma();

enum EExifLightSource : uint16_t {
    Unknown = 0,
    Daylight = 1,
    Fluorescent = 2,
    TungstenIncandescent = 3,
    Flash = 4,
    FineWeather = 9,
    Cloudy = 10,
    Shade = 11,
    DaylightFluorescent = 12,
    DayWhiteFluorescent = 13,
    CoolWhiteFluorescent = 14,
    WhiteFluorescent = 15,
    WarmWhiteFluorescent = 16,
    StandardLightA = 17,
    StandardLightB = 18,
    StandardLightC = 19,
    D55 = 20,
    D65 = 21,
    D75 = 22,
    D50 = 23,
    ISOStudioTungsten = 24,
    Other = 255,
};

std::string_view toString(EExifLightSource lightSource);
nanogui::Vector2f xy(EExifLightSource lightSource);

std::array<nanogui::Vector2f, 4> chromaFromWpPrimaries(int wpPrimaries);
std::string_view wpPrimariesToString(int wpPrimaties);

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
    BT2020 = 9, // Same as BT2100
    SMPTE428 = 10,
    SMPTE431 = 11,
    SMPTE432 = 12,
    Weird = 22, // The spec says "No corresponding industry specification identified"
};

std::string_view toString(const EColorPrimaries primaries);
std::array<nanogui::Vector2f, 4> chroma(const EColorPrimaries primaries);

EColorPrimaries fromWpPrimaries(int wpPrimaries);

enum class ETransferCharacteristics : uint8_t {
    BT709 = 1, // Also BT1361
    Unspecified = 2,
    BT470M = 4,
    BT470BG = 5,
    BT601 = 6, // Also BT1358, BT1700, SMPTE ST 170
    SMPTE240 = 7,
    Linear = 8,
    Log100 = 9,
    Log100Sqrt10 = 10,
    IEC61966_2_4 = 11,
    BT1361Extended = 12,
    SRGB = 13,
    BT202010bit = 14,
    BT202012bit = 15,
    PQ = 16, // Perceptual Quantizer, SMPTE ST 2084
    SMPTE428 = 17,
    HLG = 18, // Hybrid Log-Gamma
};

std::string_view toString(const ETransferCharacteristics transfer);
bool isTransferImplemented(const ETransferCharacteristics transfer);

ETransferCharacteristics fromWpTransfer(int wpTransfer);

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

    float result;
    if (val < negThres) {
        result = -std::pow((-val * 4.0f + alpha - 1.0f) / alpha, 1.0f / 0.45f) / 4.0f;
    } else if (val <= thres) {
        result = val / 4.5f;
    } else {
        result = std::pow((val + alpha - 1.0f) / alpha, 1.0f / 0.45f);
    }

    return result;
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
    return 10000.0f / 203.0f * std::pow(std::max(tmp - c1, 0.0f) / std::max(c2 - c3 * tmp, 1e-5f), invm1);
}

inline float smpteSt428ToLinear(float val) { return std::pow(val, 2.6f) * (52.37f / 48.0f); }

inline nanogui::Vector3f hlgToLinear(const nanogui::Vector3f& val) {
    const auto invOetf = [](const float val) {
        constexpr float a = 0.17883277f;
        constexpr float b = 0.28466892f;
        constexpr float c = 0.55991073f;
        return val <= 0.5f ? (val * val / 3.0f) : ((std::exp((val - c) / a) + b) / 12.0f);
    };

    const auto ootf = [](const nanogui::Vector3f& val) {
        // TODO: make these params configurable
        constexpr float Lw = 1000.0; // display peak brightness in cd/m² (nits)
        constexpr float gain = Lw; // can technically be adjusted, but usually set to Lw
        const float gamma = 1.2f + 0.42f * std::log10(Lw / 1000.0f);

        // NOTE: HLG (BT.2100) mandates the use of Rec. 2020 primaries, so the following equation should always be valid.
        const float lum = 0.2627f * val.x() + 0.6780f * val.y() + 0.0593f * val.z();
        return gain * pow(lum, gamma - 1.0f) * val;
    };

    return ootf({invOetf(val.x()), invOetf(val.y()), invOetf(val.z())}) / 203.0f; // Convert to linear sRGB units where SDR white is 1.0
}

inline float invTransferComponent(const ETransferCharacteristics transfer, float val) noexcept {
    switch (transfer) {
        case ETransferCharacteristics::BT709:
        case ETransferCharacteristics::BT601:
        case ETransferCharacteristics::BT202010bit:
        case ETransferCharacteristics::BT202012bit: return bt709ToLinear(val);
        case ETransferCharacteristics::IEC61966_2_4: // handles negative values by mirroring
            return std::copysign(bt709ToLinear(std::abs(val)), val);
        case ETransferCharacteristics::BT1361Extended: // extended to negative values (weirdly)
            return bt1361ExtendedToLinear(val);
        case ETransferCharacteristics::BT470M: return std::pow(std::max(val, 0.0f), 2.2f);
        case ETransferCharacteristics::BT470BG: return std::pow(std::max(val, 0.0f), 2.8f);
        case ETransferCharacteristics::SMPTE240: return smpteSt240ToLinear(val);
        case ETransferCharacteristics::Linear: return val;
        case ETransferCharacteristics::Log100: return val > 0.0f ? std::exp((val - 1.0f) * 2.0f * std::log(10.0f)) : 0.0f;
        case ETransferCharacteristics::Log100Sqrt10: return val > 0.0f ? std::exp((val - 1.0f) * 2.5f * std::log(10.0f)) : 0.0f;
        case ETransferCharacteristics::SRGB: return toLinear(val);
        case ETransferCharacteristics::PQ: return pqToLinear(val);
        case ETransferCharacteristics::SMPTE428: return smpteSt428ToLinear(val);
        case ETransferCharacteristics::HLG: return val; // Should be handled by invTransfer below
        case ETransferCharacteristics::Unspecified: return val; // Default to linear if unspecified
    }

    // Other transfer functions are not implemented. Default to linear.
    return val;
}

inline nanogui::Vector3f invTransfer(const ETransferCharacteristics transfer, const nanogui::Vector3f& val) noexcept {
    if (transfer == ETransferCharacteristics::HLG) {
        return hlgToLinear(val);
    } else {
        return {
            invTransferComponent(transfer, val.x()),
            invTransferComponent(transfer, val.y()),
            invTransferComponent(transfer, val.z()),
        };
    }
}

inline float bestGuessReferenceWhiteLevel(const ETransferCharacteristics transfer) {
    switch (transfer) {
        case ETransferCharacteristics::PQ:
        case ETransferCharacteristics::HLG: return 203.0f;

        case ETransferCharacteristics::BT709: // 100 nits by convention, see e.g.
                                              // https://partnerhelp.netflixstudios.com/hc/en-us/articles/360000591787-Color-Critical-Display-Calibration-Guidelines
        case ETransferCharacteristics::BT601: // same as BT709 in practice
        case ETransferCharacteristics::BT1361Extended: // Extends BT709 and inherits conventions.
        case ETransferCharacteristics::IEC61966_2_4: // xvYCC proposed by sony. Extends BT709 and inherits conventions.
        case ETransferCharacteristics::BT202010bit: // SMPTE ST 2080-1 specifies 100 nits for SDR white
        case ETransferCharacteristics::BT202012bit: return 100.0f;

        default: return 80.0f;
    }
}
} // namespace ituth273

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

class ColorProfile {
public:
    ColorProfile(void* profile) : mProfile{profile} {}
    ~ColorProfile();

    ColorProfile(const ColorProfile&) = delete;
    ColorProfile& operator=(const ColorProfile&) = delete;

    ColorProfile(ColorProfile&& other) noexcept { std::swap(mProfile, other.mProfile); }

    ColorProfile& operator=(ColorProfile&& other) noexcept {
        std::swap(mProfile, other.mProfile);
        return *this;
    }

    struct CICP {
        ituth273::EColorPrimaries primaries;
        ituth273::ETransferCharacteristics transfer;
        uint8_t matrixCoeffs;
        uint8_t videoFullRangeFlag;
    };

    std::optional<CICP> cicp() const;
    ERenderingIntent renderingIntent() const;

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
    int numChannelsOut,
    int priority
);

struct LimitedRange {
    float scale = 1.0f; // Scale factor for limited range to full range conversion
    float offset = 0.0f; // Offset for limited range to full range conversion

    static constexpr LimitedRange full() { return {1.0f, 0.0f}; }
};

LimitedRange limitedRangeForBitsPerSample(int bitsPerSample);

} // namespace tev
