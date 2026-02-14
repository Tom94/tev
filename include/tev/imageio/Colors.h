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

// R, G, B, W
using chroma_t = std::array<nanogui::Vector2f, 4>;

enum class ERenderingIntent {
    Perceptual = 0,
    RelativeColorimetric = 1,
    Saturation = 2,
    AbsoluteColorimetric = 3,
};

std::string_view toString(ERenderingIntent intent);

chroma_t zeroChroma();

nanogui::Matrix3f xyzToChromaMatrix(const chroma_t& chroma);
nanogui::Matrix3f adaptWhiteBradford(const nanogui::Vector2f& srcWhite, const nanogui::Vector2f& dstWhite);

nanogui::Matrix3f convertColorspaceMatrix(
    const chroma_t& srcChroma, const chroma_t& dstChroma, ERenderingIntent intent, std::optional<nanogui::Vector2f> adoptedNeutral = std::nullopt
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

chroma_t rec709Chroma();
chroma_t adobeChroma();
chroma_t proPhotoChroma();
chroma_t displayP3Chroma();
chroma_t dciP3Chroma();
chroma_t bt2020Chroma();
chroma_t bt2100Chroma();

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

enum class EWpPrimaries : int {
    SRGB = 1, // BT709
    PALM = 2, // BT470
    PAL = 3, // BT601
    NTSC = 4, // BT601
    Film = 5,
    BT2020 = 6,
    CIE1931XYZ = 7, // SMPTE428
    DCIP3 = 8, // SMPTE431
    DisplayP3 = 9, // SMPTE432
    AdobeRGB = 10, // ISO 12640-4
    //
    ProPhotoRGB = 127, // Not actually in the spec, but useful for tev to have
};

chroma_t chroma(EWpPrimaries wpPrimaries);
std::string_view toString(EWpPrimaries wpPrimaties);

// Partial implementation of https://www.itu.int/rec/T-REC-H.273-202407-I/en (no YCbCr conversion)
namespace ituth273 {
enum class EColorPrimaries : uint8_t {
    BT709 = 1,
    Unspecified = 2,
    BT470M = 4,
    BT470BG = 5, // BT601 pal
    SMPTE170M = 6, // BT601 ntsc
    SMPTE240M = 7, // functionally same as SMPTE170M
    Film = 8,
    BT2020 = 9, // Same as BT2100
    SMPTE428 = 10,
    SMPTE431 = 11,
    SMPTE432 = 12,
    Weird = 22, // The spec says "No corresponding industry specification identified"
};

std::string_view toString(const EColorPrimaries primaries);
chroma_t chroma(const EColorPrimaries primaries);

EColorPrimaries fromWpPrimaries(EWpPrimaries wpPrimaries);

enum class ETransfer : uint8_t {
    BT709 = 1, // Also BT1361
    Unspecified = 2,
    Gamma22 = 4,
    Gamma28 = 5,
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
    // Not actually in the spec, but useful for tev to have
    LUT = 126,
    GenericGamma = 127,
};

std::string_view toString(const ETransfer transfer);
bool isTransferImplemented(const ETransfer transfer);

ETransfer fromWpTransfer(int wpTransfer);

namespace bt709 {
static constexpr float beta = 0.018053968510807f;
static constexpr float alpha = 1.0f + 5.5f * beta;
static constexpr float thres = 4.5f * beta;
} // namespace bt709

inline float bt709ToLinear(float val) {
    return val <= bt709::thres ? (val / 4.5f) : std::pow((val + bt709::alpha - 1.0f) / bt709::alpha, 1.0f / 0.45f);
}

inline float linearToBt709(float val) {
    return val <= bt709::beta ? (val * 4.5f) : (bt709::alpha * std::pow(val, 0.45f) - (bt709::alpha - 1.0f));
}

// From https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.1361-0-199802-W!!PDF-E.pdf, generalized to the more precise constants from the
// bt709ToLinear function as defined in https://www.itu.int/rec/T-REC-H.273-202407-I/en.
inline float bt1361ExtendedToLinear(float val) {
    constexpr float negThres = -bt709::thres / 4.0f;

    float result;
    if (val < negThres) {
        result = (-1.0f / 4.0f) * std::pow((-4.0f * val + bt709::alpha - 1.0f) / bt709::alpha, 1.0f / 0.45f);
    } else if (val <= bt709::thres) {
        result = val / 4.5f;
    } else {
        result = std::pow((val + bt709::alpha - 1.0f) / bt709::alpha, 1.0f / 0.45f);
    }

    return result;
}

inline float linearToBt1361Extended(float val) {
    constexpr float negThres = -bt709::beta / 4.0f;

    float result;
    if (val < negThres) {
        result = (-1.0f / 4.0f) * (bt709::alpha * std::pow(-4.0f * val, 0.45f) - (bt709::alpha - 1.0f));
    } else if (val <= bt709::beta) {
        result = val * 4.5f;
    } else {
        result = bt709::alpha * std::pow(val, 0.45f) - (bt709::alpha - 1.0f);
    }

    return result;
}

// From http://car.france3.mars.free.fr/HD/INA-%2026%20jan%2006/SMPTE%20normes%20et%20confs/s240m.pdf
inline float smpteSt240ToLinear(float val) { return val <= 0.0913f ? (val / 4.0f) : pow((val + 0.1115f) / 1.1115f, 1.0f / 0.45f); }
inline float linearToSmpteSt240(float val) { return val <= 0.022825f ? (val * 4.0f) : (1.1115f * pow(val, 0.45f) - 0.1115f); }

namespace pq {
static constexpr float c1 = 107.0f / 128.0f;
static constexpr float c2 = 2413.0f / 128.0f;
static constexpr float c3 = 2392.0f / 128.0f;
static constexpr float m1 = 1305.0f / 8192.0f;
static constexpr float m2 = 2523.0f / 32.0f;
static constexpr float invm1 = 8192.0f / 1305.0f;
static constexpr float invm2 = 32.0f / 2523.0f;
} // namespace pq

inline float pqToLinear(float val) {
    const float tmp = std::pow(std::max(val, 0.0f), pq::invm2);
    return 10000.0f / 203.0f * std::pow(std::max(tmp - pq::c1, 0.0f) / std::max(pq::c2 - pq::c3 * tmp, 1e-5f), pq::invm1);
}

inline float linearToPq(float val) {
    val = val * 203.0f / 10000.0f;
    const float p = std::pow(std::max(val, 0.0f), pq::m1);

    const float num = pq::c1 + pq::c2 * p;
    return std::pow(num / (1.0f + pq::c3 * p), pq::m2);
}

inline float smpteSt428ToLinear(float val) { return std::pow(val, 2.6f) * (52.37f / 48.0f); }
inline float linearToSmpteSt428(float val) { return std::pow(val * (48.0f / 52.37f), 1.0f / 2.6f); }

namespace hlg {
// TODO: make these params configurable at runtime
static constexpr float Lw = 1000.0f; // display peak brightness in cd/m² (nits)
static constexpr float gain = Lw; // can technically be adjusted, but usually set to Lw
static const float gamma = 1.2f + 0.42f * std::log10(Lw / 1000.0f);

static constexpr float a = 0.17883277f;
static constexpr float b = 0.28466892f;
static constexpr float c = 0.55991073f;
} // namespace hlg

inline nanogui::Vector3f hlgToLinear(const nanogui::Vector3f& val) {
    const auto invOetf = [](const float val) {
        return val <= 0.5f ? (val * val / 3.0f) : ((std::exp((val - hlg::c) / hlg::a) + hlg::b) / 12.0f);
    };

    const auto ootf = [](const nanogui::Vector3f& val) {
        // NOTE: HLG (BT.2100) mandates the use of Rec. 2020 primaries, so the following equation should always be valid.
        const float lum = 0.2627f * val.x() + 0.6780f * val.y() + 0.0593f * val.z();
        return hlg::gain * pow(lum, hlg::gamma - 1.0f) * val;
    };

    return ootf({invOetf(val.x()), invOetf(val.y()), invOetf(val.z())}) / 203.0f; // Convert to linear units where SDR white is 1.0
}

inline nanogui::Vector3f linearToHlg(const nanogui::Vector3f& val) {
    const auto oetf = [](const float val) {
        return val <= 1.0f / 12.0f ? std::sqrt(3.0f * val) : (hlg::a * std::log(12.0f * val - hlg::b) + hlg::c);
    };

    const auto invOotf = [](const nanogui::Vector3f& val) {
        const auto tmp = val / hlg::gain;

        // NOTE: HLG (BT.2100) mandates the use of Rec. 2020 primaries, so the following equation should always be valid.
        const float lum = 0.2627f * tmp.x() + 0.6780f * tmp.y() + 0.0593f * tmp.z();
        return pow(lum, (1.0f - hlg::gamma) / hlg::gamma) * tmp;
    };

    const auto tmp = invOotf(val * 203.0f); // Convert from linear units where SDR white is 1.0;
    return {oetf(tmp.x()), oetf(tmp.y()), oetf(tmp.z())};
}

inline float invTransferComponent(const ETransfer transfer, float val) noexcept {
    switch (transfer) {
        case ETransfer::BT709:
        case ETransfer::BT601:
        case ETransfer::BT202010bit:
        case ETransfer::BT202012bit: return bt709ToLinear(val);
        case ETransfer::IEC61966_2_4: // handles negative values by mirroring
            return std::copysign(bt709ToLinear(std::abs(val)), val);
        case ETransfer::BT1361Extended: // extended to negative values (weirdly)
            return bt1361ExtendedToLinear(val);
        case ETransfer::Gamma22: return std::pow(std::max(val, 0.0f), 2.2f);
        case ETransfer::Gamma28: return std::pow(std::max(val, 0.0f), 2.8f);
        case ETransfer::SMPTE240: return smpteSt240ToLinear(val);
        case ETransfer::Linear: return val;
        case ETransfer::Log100: return val > 0.0f ? std::exp((val - 1.0f) * 2.0f * std::log(10.0f)) : 0.0f;
        case ETransfer::Log100Sqrt10: return val > 0.0f ? std::exp((val - 1.0f) * 2.5f * std::log(10.0f)) : 0.0f;
        case ETransfer::SRGB: return toLinear(val);
        case ETransfer::PQ: return pqToLinear(val);
        case ETransfer::SMPTE428: return smpteSt428ToLinear(val);
        case ETransfer::HLG: return hlgToLinear({val, val, val}).x(); // Treat single component as R=G=B
        case ETransfer::Unspecified: return val; // Default to linear if unspecified
        default: return val; // Other transfer functions are not implemented. Default to linear.
    }
}

inline nanogui::Vector3f invTransfer(const ETransfer transfer, const nanogui::Vector3f& val) noexcept {
    if (transfer == ETransfer::HLG) {
        return hlgToLinear(val);
    } else {
        return {
            invTransferComponent(transfer, val.x()),
            invTransferComponent(transfer, val.y()),
            invTransferComponent(transfer, val.z()),
        };
    }
}

inline float transferComponent(const ETransfer transfer, float val) noexcept {
    switch (transfer) {
        case ETransfer::BT709:
        case ETransfer::BT601:
        case ETransfer::BT202010bit:
        case ETransfer::BT202012bit: return linearToBt709(val);
        case ETransfer::IEC61966_2_4: // handles negative values by mirroring
            return std::copysign(linearToBt709(std::abs(val)), val);
        case ETransfer::BT1361Extended: // extended to negative values (weirdly)
            return linearToBt1361Extended(val);
        case ETransfer::Gamma22: return std::pow(std::max(val, 0.0f), 1.0f / 2.2f);
        case ETransfer::Gamma28: return std::pow(std::max(val, 0.0f), 1.0f / 2.8f);
        case ETransfer::SMPTE240: return linearToSmpteSt240(val);
        case ETransfer::Linear: return val;
        case ETransfer::Log100: return val >= 0.01f ? 1.0f + std::log10(val) / 2.0f : 0.0f;
        case ETransfer::Log100Sqrt10: return val >= std::sqrt(10.0f) / 1000.0f ? 1.0f + std::log10(val) / 2.5f : 0.0f;
        case ETransfer::SRGB: return toSRGB(val);
        case ETransfer::PQ: return linearToPq(val);
        case ETransfer::SMPTE428: return linearToSmpteSt428(val);
        case ETransfer::HLG: return linearToHlg({val, val, val}).x(); // Treat single component as R=G=B
        case ETransfer::Unspecified: return val; // Default to linear if unspecified
        default: return val; // Other transfer functions are not implemented. Default to linear.
    }
}

inline nanogui::Vector3f transfer(const ETransfer transfer, const nanogui::Vector3f& val) noexcept {
    if (transfer == ETransfer::HLG) {
        return linearToHlg(val);
    } else {
        return {
            transferComponent(transfer, val.x()),
            transferComponent(transfer, val.y()),
            transferComponent(transfer, val.z()),
        };
    }
}

inline float bestGuessReferenceWhiteLevel(const ETransfer transfer) {
    switch (transfer) {
        case ETransfer::PQ:
        case ETransfer::HLG: return 203.0f;

        case ETransfer::BT709: // 100 nits by convention, see e.g.
                               // https://partnerhelp.netflixstudios.com/hc/en-us/articles/360000591787-Color-Critical-Display-Calibration-Guidelines
        case ETransfer::BT601: // same as BT709 in practice
        case ETransfer::BT1361Extended: // Extends BT709 and inherits conventions.
        case ETransfer::IEC61966_2_4: // xvYCC proposed by sony. Extends BT709 and inherits conventions.
        case ETransfer::BT202010bit: // SMPTE ST 2080-1 specifies 100 nits for SDR white
        case ETransfer::BT202012bit: return 100.0f;

        default: return 80.0f;
    }
}
} // namespace ituth273

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
        ituth273::ETransfer transfer;
        uint8_t matrixCoeffs;
        uint8_t videoFullRangeFlag;
    };

    std::optional<CICP> cicp() const;
    ERenderingIntent renderingIntent() const;

    std::optional<chroma_t> chroma() const;

    static ColorProfile fromIcc(std::span<const uint8_t> iccData);
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
    std::optional<ERenderingIntent> intentOverride,
    int priority
);

struct LimitedRange {
    float scale = 1.0f; // Scale factor for limited range to full range conversion
    float offset = 0.0f; // Offset for limited range to full range conversion

    static constexpr LimitedRange full() { return {1.0f, 0.0f}; }

    bool operator==(const LimitedRange& other) const { return scale == other.scale && offset == other.offset; }
};

LimitedRange limitedRangeForBitsPerSample(int bitsPerSample);

} // namespace tev
