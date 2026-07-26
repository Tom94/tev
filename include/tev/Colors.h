/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
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

#include <tev/Channel.h>
#include <tev/Common.h>
#include <tev/Simd.h>
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
nanogui::Matrix3f adaptWhiteBradford(nanogui::Vector2f srcWhite, nanogui::Vector2f dstWhite);

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

template <class B> B applyGamma(const B& val, const B& gamma) noexcept { return xsimd::copysign(fastPow(xsimd::abs(val), gamma), val); }

template <class B> nanogui::Array<B, 3> applyGamma(const nanogui::Array<B, 3>& val, const B& gamma) noexcept {
    return {applyGamma(val.x(), gamma), applyGamma(val.y(), gamma), applyGamma(val.z(), gamma)};
}

template <class B> nanogui::Array<B, 3> applyTonemap(const nanogui::Array<B, 3>& val, const float gamma, ETonemap tonemap) noexcept {
    switch (tonemap) {
        case ETonemap::Gamma: return applyGamma(val, B{1.0f / gamma});
        case ETonemap::FalseColor: {
            static constexpr auto falseColor = [](const B& linear) -> nanogui::Array<B, 3> {
                static const auto fcd = colormap::turbo();
                using vi = int_companion_t<B>;
                const auto start = 4 * xsimd::clip(float_to_int(linear * B(fcd.size() / 4)), vi{0}, vi(fcd.size() / 4 - 1));
                if constexpr (std::is_same_v<B, float>) {
                    return {fcd[start], fcd[start + 1], fcd[start + 2]};
                } else {
                    return {B::gather(fcd.data(), start), B::gather(fcd.data(), start + 1), B::gather(fcd.data(), start + 2)};
                }
            };

            return falseColor(fastLog2(mean(val) + 0.03125f) / 10 + 0.5f);
        }
        case ETonemap::PositiveNegative: {
            return {-2.0f * mean(min(val, nanogui::Array<B, 3>{B{0.0f}})), 2.0f * mean(max(val, nanogui::Array<B, 3>{B{0.0f}})), B{0.0f}};
        }
        default: return val; // Invalid tonemap selected, return input unchanged.
    }
}

inline float applyMetric(float image, float reference, EMetric metric) noexcept {
    float diff = image - reference;
    switch (metric) {
        case EMetric::Error: return diff;
        case EMetric::AbsoluteError: return std::abs(diff);
        case EMetric::SquaredError: return diff * diff;
        case EMetric::RelativeAbsoluteError: return std::abs(diff) / (reference + 0.01f);
        case EMetric::RelativeSquaredError: return diff * diff / (reference * reference + 0.01f);
        default: return diff; // Invalid metric selected, return error.
    }
}

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
    YCbCrLinear = 124,
    YCbCrSRGB = 125,
    LUT = 126,
    GenericGamma = 127,
};

std::string_view toString(const ETransfer transfer);

ETransfer fromWpTransfer(int wpTransfer);

namespace bt709 {
inline constexpr float beta = 0.018053968510807f;
inline constexpr float alpha = 1.0f + 5.5f * beta;
inline constexpr float thres = 4.5f * beta;
} // namespace bt709

template <class B> B bt709ToLinear(const B& val) {
    using xsimd::select;
    const B lo = val * (1.0f / 4.5f);
    const B hi = fastPow((val + (bt709::alpha - 1.0f)) * (1.0f / bt709::alpha), B(1.0f / 0.45f));
    return select(val <= B(bt709::thres), lo, hi);
}

template <class B> B linearToBt709(const B& val) {
    using xsimd::select;
    const B lo = val * 4.5f;
    const B hi = B(bt709::alpha) * fastPow(val, B(0.45f)) - (bt709::alpha - 1.0f);
    return select(val <= B(bt709::beta), lo, hi);
}

template <class B> B iec6196624ToLinear(const B& val) {
    using xsimd::abs;
    using xsimd::copysign;
    return copysign(bt709ToLinear(abs(val)), val);
}

template <class B> B linearToIec6196624(const B& val) {
    using xsimd::abs;
    using xsimd::copysign;
    return copysign(linearToBt709(abs(val)), val);
}

template <class B> B bt1361ExtendedToLinear(const B& val) {
    using xsimd::select;
    constexpr float negThres = -bt709::thres / 4.0f;

    const B neg = B(-1.0f / 4.0f) * fastPow((val * -4.0f + (bt709::alpha - 1.0f)) * (1.0f / bt709::alpha), B(1.0f / 0.45f));
    const B lin = val * (1.0f / 4.5f);
    const B pos = fastPow((val + (bt709::alpha - 1.0f)) * (1.0f / bt709::alpha), B(1.0f / 0.45f));

    // if (val < negThres) neg else if (val <= thres) lin else pos
    return select(val < B(negThres), neg, select(val <= B(bt709::thres), lin, pos));
}

template <class B> B linearToBt1361Extended(const B& val) {
    using xsimd::select;
    constexpr float negThres = -bt709::beta / 4.0f;

    const B neg = B(-1.0f / 4.0f) * (B(bt709::alpha) * fastPow(val * -4.0f, B(0.45f)) - (bt709::alpha - 1.0f));
    const B lin = val * 4.5f;
    const B pos = B(bt709::alpha) * fastPow(val, B(0.45f)) - (bt709::alpha - 1.0f);

    return select(val < B(negThres), neg, select(val <= B(bt709::beta), lin, pos));
}

template <class B> B gammaToLinear(const B& val, float gamma) {
    using xsimd::max;
    return fastPow(max(val, B(0.0f)), B(gamma));
}

template <class B> B linearToGamma(const B& val, float gamma) {
    using xsimd::max;
    return fastPow(max(val, B(0.0f)), B(1.0f / gamma));
}

template <class B> B log100ToLinear(const B& val) {
    using xsimd::exp;
    using xsimd::select;
    const B v = exp((val - 1.0f) * (2.0f * std::log(10.0f)));
    return select(val > B(0.0f), v, B(0.0f));
}

template <class B> B linearToLog100(const B& val) {
    using xsimd::log10;
    using xsimd::select;
    const B v = B(1.0f) + log10(val) * (1.0f / 2.0f);
    return select(val >= B(0.01f), v, B(0.0f));
}

template <class B> B log100Sqrt10ToLinear(const B& val) {
    using xsimd::exp;
    using xsimd::select;
    const B v = exp((val - 1.0f) * (2.5f * std::log(10.0f)));
    return select(val > B(0.0f), v, B(0.0f));
}

template <class B> B linearToLog100Sqrt10(const B& val) {
    using xsimd::log10;
    using xsimd::select;
    const B v = B(1.0f) + log10(val) * (1.0f / 2.5f);
    return select(val >= B(std::sqrt(10.0f) / 1000.0f), v, B(0.0f));
}

template <class B> B smpteSt240ToLinear(const B& val) {
    using xsimd::select;
    const B lo = val * (1.0f / 4.0f);
    const B hi = fastPow((val + 0.1115f) * (1.0f / 1.1115f), B(1.0f / 0.45f));
    return select(val <= B(0.0913f), lo, hi);
}

template <class B> B linearToSmpteSt240(const B& val) {
    using xsimd::select;
    const B lo = val * 4.0f;
    const B hi = B(1.1115f) * fastPow(val, B(0.45f)) - 0.1115f;
    return select(val <= B(0.022825f), lo, hi);
}

namespace srgb {
inline constexpr float a = 0.055f;
};

template <class B> B srgbToLinear(const B& val) {
    using xsimd::abs;
    using xsimd::select;
    const B a = abs(val);
    const B lo = val * (1.0f / 12.92f);
    const B hi = copysign(fastPow((a + srgb::a) * (1.0f / (1.0f + srgb::a)), B(2.4f)), val);
    return select(a <= B(0.04045f), lo, hi);
}

template <class B> B linearToSrgb(const B& val) {
    using xsimd::abs;
    using xsimd::select;
    const B a = abs(val);
    const B lo = val * 12.92f;
    const B hi = copysign((1.0f + srgb::a) * fastPow(a, B(1.0f / 2.4f)) - srgb::a, val);
    return select(a <= B(0.0031308f), lo, hi);
}

namespace pq {
inline constexpr float c1 = 107.0f / 128.0f;
inline constexpr float c2 = 2413.0f / 128.0f;
inline constexpr float c3 = 2392.0f / 128.0f;
inline constexpr float m1 = 1305.0f / 8192.0f;
inline constexpr float m2 = 2523.0f / 32.0f;
inline constexpr float invm1 = 8192.0f / 1305.0f;
inline constexpr float invm2 = 32.0f / 2523.0f;
} // namespace pq

template <class B> B pqToLinear(const B& val) {
    using xsimd::max;
    const B tmp = fastPow(max(val, B(0.0f)), B(pq::invm2));
    const B num = max(tmp - pq::c1, B(0.0f));
    const B den = max(B(pq::c2) - B(pq::c3) * tmp, B(1e-5f));
    return B(10000.0f / 203.0f) * fastPow(num / den, B(pq::invm1));
}

template <class B> B linearToPq(B val) {
    using xsimd::max;
    val = val * (203.0f / 10000.0f);
    const B p = fastPow(max(val, B(0.0f)), B(pq::m1));
    const B num = B(pq::c1) + B(pq::c2) * p;
    return fastPow(num / (B(1.0f) + B(pq::c3) * p), B(pq::m2));
}

template <class B> B smpteSt428ToLinear(const B& val) { return fastPow(val, B(2.6f)) * (52.37f / 48.0f); }

template <class B> B linearToSmpteSt428(const B& val) { return fastPow(val * (48.0f / 52.37f), B(1.0f / 2.6f)); }

namespace hlg {
inline constexpr float Lw = 1000.0f;
inline constexpr float gain = Lw;
inline const float gamma = 1.2f + 0.42f * std::log10(Lw / 1000.0f);

inline constexpr float a = 0.17883277f;
inline constexpr float b = 0.28466892f;
inline constexpr float c = 0.55991073f;
} // namespace hlg

// HLG inverse OETF, per-lane (no channel coupling)
template <class B> B hlgInvOetf(const B& v) {
    using xsimd::select;
    const B lo = v * v * (1.0f / 3.0f);
    const B hi = (fastExp((v - hlg::c) * (1.0f / hlg::a)) + hlg::b) * (1.0f / 12.0f);
    return select(v <= B(0.5f), lo, hi);
}

template <class B> B hlgOetf(const B& v) {
    using xsimd::select;
    using xsimd::sqrt;
    const B lo = sqrt(v * 3.0f);
    const B hi = B(hlg::a) * fastLog(v * 12.0f - hlg::b) + hlg::c;
    return select(v <= B(1.0f / 12.0f), lo, hi);
}

// SoA HLG->linear: r,g,b are batches of the same set of pixels.
template <class B> nanogui::Array<B, 3> hlgToLinear(const nanogui::Array<B, 3>& rgb) {
    const B er = hlgInvOetf(rgb.x());
    const B eg = hlgInvOetf(rgb.y());
    const B eb = hlgInvOetf(rgb.z());

    const B lum = B(0.2627f) * er + B(0.6780f) * eg + B(0.0593f) * eb;
    const B scale = B(hlg::gain) * fastPow(lum, B(hlg::gamma - 1.0f)) * (1.0f / 203.0f);

    return {scale * er, scale * eg, scale * eb};
}

template <class B> nanogui::Array<B, 3> linearToHlg(const nanogui::Array<B, 3>& rgb) {
    // convert from linear units where SDR white is 1.0, then invOotf
    const B tr = rgb.x() * (203.0f / hlg::gain);
    const B tg = rgb.y() * (203.0f / hlg::gain);
    const B tb = rgb.z() * (203.0f / hlg::gain);

    const B lum = B(0.2627f) * tr + B(0.6780f) * tg + B(0.0593f) * tb;
    const B scale = fastPow(lum, B((1.0f - hlg::gamma) / hlg::gamma));

    return {hlgOetf(scale * tr), hlgOetf(scale * tg), hlgOetf(scale * tb)};
}

// R=G=B single-component HLG (matches original invTransferComponent<HLG>)
template <class B> B hlgToLinearComponent(const B& val) {
    const B e = hlgInvOetf(val);
    const B lum = e; // 0.2627+0.6780+0.0593 == 1
    return B(hlg::gain) * fastPow(lum, B(hlg::gamma - 1.0f)) * (1.0f / 203.0f) * e;
}

// R=G=B single-component HLG (inverse of hlgToLinearComponent, matches original linearToHlg with R=G=B)
template <class B> B linearToHlgComponent(const B& val) {
    const B tmp = val * (203.0f / hlg::gain); // linear units where SDR white is 1.0
    const B lum = tmp;                        // 0.2627 + 0.6780 + 0.0593 == 1
    const B e = fastPow(lum, B((1.0f - hlg::gamma) / hlg::gamma)) * tmp;
    return hlgOetf(e);
}

static constexpr float DEFAULT_YCBCR_OFFSETS[2] = {0.5f, 0.5f};
static constexpr float DEFAULT_YCBCR_COEFFS[4] = {1.402f, -0.344136f, -0.714136f, 1.772f};
static constexpr float DEFAULT_RGB_TO_YCBCR_COEFFS[3][3] = {
    {0.299f,     0.587f,     0.114f    },
    {-0.168736f, -0.331264f, 0.5f      },
    {0.5f,       -0.418688f, -0.081312f},
};

template <class B>
nanogui::Array<B, 3> yCbCrToRgb(
    const nanogui::Array<B, 3>& yCbCr, const float offsets[2] = DEFAULT_YCBCR_OFFSETS, const float coeffs[4] = DEFAULT_YCBCR_COEFFS
) {
    const B& y = yCbCr.x();
    const B& cb = yCbCr.y();
    const B& cr = yCbCr.z();

    const B cbOffset = cb - B(offsets[0]);
    const B crOffset = cr - B(offsets[1]);

    const auto r = y + B(coeffs[0]) * crOffset;
    const auto g = y + B(coeffs[1]) * cbOffset + B(coeffs[2]) * crOffset;
    const auto b = y + B(coeffs[3]) * cbOffset;

    return {r, g, b};
}

template <class B>
nanogui::Array<B, 3> rgbToYCbCr(
    const nanogui::Array<B, 3>& rgb, const float offsets[2] = DEFAULT_YCBCR_OFFSETS, const float coeffs[3][3] = DEFAULT_RGB_TO_YCBCR_COEFFS
) {
    const B y = B(coeffs[0][0]) * rgb.x() + B(coeffs[0][1]) * rgb.y() + B(coeffs[0][2]) * rgb.z();
    const B cb = B(offsets[0]) + B(coeffs[1][0]) * rgb.x() + B(coeffs[1][1]) * rgb.y() + B(coeffs[1][2]) * rgb.z();
    const B cr = B(offsets[1]) + B(coeffs[2][0]) * rgb.x() + B(coeffs[2][1]) * rgb.y() + B(coeffs[2][2]) * rgb.z();
    return {y, cb, cr};
}

static constexpr bool isTransferImplemented(const ETransfer transfer) {
    switch (transfer) {
        case ETransfer::BT709:
        case ETransfer::BT601:
        case ETransfer::BT202010bit:
        case ETransfer::BT202012bit:
        case ETransfer::IEC61966_2_4: // handles negative values by mirroring
        case ETransfer::BT1361Extended: // extended to negative values (weirdly)
        case ETransfer::Gamma22:
        case ETransfer::Gamma28:
        case ETransfer::SMPTE240:
        case ETransfer::Linear:
        case ETransfer::Log100:
        case ETransfer::Log100Sqrt10:
        case ETransfer::SRGB:
        case ETransfer::PQ:
        case ETransfer::SMPTE428:
        // YCbCr and HLG are implemented, but only incomplete when used component-wise. Only RGB overloads are whole
        case ETransfer::YCbCrLinear:
        case ETransfer::YCbCrSRGB:
        case ETransfer::HLG:
        case ETransfer::Unspecified: return true;
        // Require extra data
        case ETransfer::LUT:
        case ETransfer::GenericGamma: return false;
    }

    return false;
}

static constexpr bool isTransferRgb(const ETransfer transfer) {
    switch (transfer) {
        case ETransfer::HLG:
        case ETransfer::YCbCrLinear:
        case ETransfer::YCbCrSRGB: return true;
        default: return false;
    }
}

// In cycles
static constexpr size_t approxCost(const ETransfer transfer) {
    switch (transfer) {
        // Cheapest: linear. It's free.
        case ETransfer::Linear:
        case ETransfer::Unspecified: return 1;
        case ETransfer::YCbCrLinear: return 4;
        // Then: single fastExp / fastLog and some arithmetic.
        case ETransfer::Log100:
        case ETransfer::Log100Sqrt10: return 16;
        // Single fastPow
        case ETransfer::BT709:
        case ETransfer::BT601:
        case ETransfer::BT202010bit:
        case ETransfer::BT202012bit:
        case ETransfer::IEC61966_2_4: // handles negative values by mirroring
        case ETransfer::BT1361Extended: // extended to negative values (weirdly)
        case ETransfer::Gamma22:
        case ETransfer::Gamma28:
        case ETransfer::SMPTE240:
        case ETransfer::SRGB:
        case ETransfer::SMPTE428:
        case ETransfer::YCbCrSRGB:
        case ETransfer::GenericGamma:
        case ETransfer::LUT: return 32;
        // fastPow, fastExp/Log and some extra
        case ETransfer::HLG: return 64;
        // Two fastPow and some extra
        case ETransfer::PQ: return 96;
    }

    return 1;
}

// Default: linear passthrough
template <ETransfer E, class B> B invTransferComponentImpl(std::integral_constant<ETransfer, E>, const B& val) { return val; }

#define IT_SPEC(E, EXPR) \
    template <class B> B invTransferComponentImpl(std::integral_constant<ETransfer, ETransfer::E>, const B& val) { return EXPR; }

IT_SPEC(BT709, bt709ToLinear(val))
IT_SPEC(BT601, bt709ToLinear(val))
IT_SPEC(BT202010bit, bt709ToLinear(val))
IT_SPEC(BT202012bit, bt709ToLinear(val))
IT_SPEC(IEC61966_2_4, iec6196624ToLinear(val))
IT_SPEC(BT1361Extended, bt1361ExtendedToLinear(val))
IT_SPEC(Gamma22, gammaToLinear(val, 2.2f))
IT_SPEC(Gamma28, gammaToLinear(val, 2.8f))
IT_SPEC(SMPTE240, smpteSt240ToLinear(val))
IT_SPEC(Linear, val)
IT_SPEC(Log100, log100ToLinear(val))
IT_SPEC(Log100Sqrt10, log100Sqrt10ToLinear(val))
IT_SPEC(SRGB, srgbToLinear(val))
IT_SPEC(PQ, pqToLinear(val))
IT_SPEC(SMPTE428, smpteSt428ToLinear(val))
IT_SPEC(HLG, hlgToLinearComponent(val))
#undef IT_SPEC

template <ETransfer TRANSFER, class B> B invTransferComponent(const B& val) noexcept {
    return invTransferComponentImpl(std::integral_constant<ETransfer, TRANSFER>(), val);
}

template <class B> B invTransferComponent(ETransfer transfer, const B& val) noexcept {
    switch (transfer) {
        case ETransfer::BT709: return bt709ToLinear(val);
        case ETransfer::BT601: return bt709ToLinear(val);
        case ETransfer::BT202010bit: return bt709ToLinear(val);
        case ETransfer::BT202012bit: return bt709ToLinear(val);
        case ETransfer::IEC61966_2_4: return iec6196624ToLinear(val);
        case ETransfer::BT1361Extended: return bt1361ExtendedToLinear(val);
        case ETransfer::Gamma22: return gammaToLinear(val, 2.2f);
        case ETransfer::Gamma28: return gammaToLinear(val, 2.8f);
        case ETransfer::SMPTE240: return smpteSt240ToLinear(val);
        case ETransfer::Log100: return log100ToLinear(val);
        case ETransfer::Log100Sqrt10: return log100Sqrt10ToLinear(val);
        case ETransfer::SRGB: return srgbToLinear(val);
        case ETransfer::PQ: return pqToLinear(val);
        case ETransfer::SMPTE428: return smpteSt428ToLinear(val);
        case ETransfer::HLG: return hlgToLinearComponent(val);
        case ETransfer::YCbCrLinear: return val;
        case ETransfer::YCbCrSRGB: return srgbToLinear(val);
        default: return val; // Linear / Unspecified / LUT / GenericGamma / unimplemented
    }
}

template <ETransfer E, class B>
nanogui::Array<B, 3> invTransferRgbImpl(std::integral_constant<ETransfer, E>, const nanogui::Array<B, 3>& rgb) {
    return {
        invTransferComponent<E>(rgb.v[0]),
        invTransferComponent<E>(rgb.v[1]),
        invTransferComponent<E>(rgb.v[2]),
    };
}

template <class B>
nanogui::Array<B, 3> invTransferRgbImpl(std::integral_constant<ETransfer, ETransfer::HLG>, const nanogui::Array<B, 3>& rgb) {
    return hlgToLinear(rgb);
}

template <class B>
nanogui::Array<B, 3> invTransferRgbImpl(std::integral_constant<ETransfer, ETransfer::YCbCrLinear>, const nanogui::Array<B, 3>& rgb) {
    return yCbCrToRgb(rgb);
}

template <class B>
nanogui::Array<B, 3> invTransferRgbImpl(std::integral_constant<ETransfer, ETransfer::YCbCrSRGB>, const nanogui::Array<B, 3>& rgb) {
    return invTransferRgbImpl(std::integral_constant<ETransfer, ETransfer::SRGB>(), yCbCrToRgb(rgb));
}

template <ETransfer TRANSFER, class B> nanogui::Array<B, 3> invTransferRgb(const nanogui::Array<B, 3>& rgb) noexcept {
    return invTransferRgbImpl(std::integral_constant<ETransfer, TRANSFER>(), rgb);
}

template <class B> nanogui::Array<B, 3> invTransferRgb(const ETransfer transfer, const nanogui::Array<B, 3>& rgb) noexcept {
    switch (transfer) {
        case ETransfer::BT709: return invTransferRgb<ETransfer::BT709>(rgb);
        case ETransfer::BT601: return invTransferRgb<ETransfer::BT601>(rgb);
        case ETransfer::BT202010bit: return invTransferRgb<ETransfer::BT202010bit>(rgb);
        case ETransfer::BT202012bit: return invTransferRgb<ETransfer::BT202012bit>(rgb);
        case ETransfer::IEC61966_2_4: return invTransferRgb<ETransfer::IEC61966_2_4>(rgb);
        case ETransfer::BT1361Extended: return invTransferRgb<ETransfer::BT1361Extended>(rgb);
        case ETransfer::Gamma22: return invTransferRgb<ETransfer::Gamma22>(rgb);
        case ETransfer::Gamma28: return invTransferRgb<ETransfer::Gamma28>(rgb);
        case ETransfer::SMPTE240: return invTransferRgb<ETransfer::SMPTE240>(rgb);
        case ETransfer::Linear: return invTransferRgb<ETransfer::Linear>(rgb);
        case ETransfer::Log100: return invTransferRgb<ETransfer::Log100>(rgb);
        case ETransfer::Log100Sqrt10: return invTransferRgb<ETransfer::Log100Sqrt10>(rgb);
        case ETransfer::SRGB: return invTransferRgb<ETransfer::SRGB>(rgb);
        case ETransfer::PQ: return invTransferRgb<ETransfer::PQ>(rgb);
        case ETransfer::SMPTE428: return invTransferRgb<ETransfer::SMPTE428>(rgb);
        case ETransfer::HLG: return invTransferRgb<ETransfer::HLG>(rgb);
        case ETransfer::Unspecified: return invTransferRgb<ETransfer::Unspecified>(rgb);
        case ETransfer::YCbCrLinear: return invTransferRgb<ETransfer::YCbCrLinear>(rgb);
        case ETransfer::YCbCrSRGB: return invTransferRgb<ETransfer::YCbCrSRGB>(rgb);
        case ETransfer::LUT: return invTransferRgb<ETransfer::LUT>(rgb);
        case ETransfer::GenericGamma: return invTransferRgb<ETransfer::GenericGamma>(rgb);
        default: return rgb;
    }
}

// Default: linear passthrough
template <ETransfer E, class B> B transferComponentImpl(std::integral_constant<ETransfer, E>, const B& val) { return val; }

#define IT_SPEC(E, EXPR) \
    template <class B> B transferComponentImpl(std::integral_constant<ETransfer, ETransfer::E>, const B& val) { return EXPR; }

IT_SPEC(BT709, linearToBt709(val))
IT_SPEC(BT601, linearToBt709(val))
IT_SPEC(BT202010bit, linearToBt709(val))
IT_SPEC(BT202012bit, linearToBt709(val))
IT_SPEC(IEC61966_2_4, linearToIec6196624(val))
IT_SPEC(BT1361Extended, linearToBt1361Extended(val))
IT_SPEC(Gamma22, linearToGamma(val, 2.2f))
IT_SPEC(Gamma28, linearToGamma(val, 2.8f))
IT_SPEC(SMPTE240, linearToSmpteSt240(val))
IT_SPEC(Linear, val)
IT_SPEC(Log100, linearToLog100(val))
IT_SPEC(Log100Sqrt10, linearToLog100Sqrt10(val))
IT_SPEC(SRGB, linearToSrgb(val))
IT_SPEC(PQ, linearToPq(val))
IT_SPEC(SMPTE428, linearToSmpteSt428(val))
IT_SPEC(HLG, linearToHlgComponent(val))
#undef IT_SPEC

template <ETransfer TRANSFER, class B> B transferComponent(const B& val) noexcept {
    return transferComponentImpl(std::integral_constant<ETransfer, TRANSFER>(), val);
}

template <class B> B transferComponent(const ETransfer transfer, const B& val) noexcept {
    switch (transfer) {
        case ETransfer::BT709:
        case ETransfer::BT601:
        case ETransfer::BT202010bit:
        case ETransfer::BT202012bit: return linearToBt709(val);
        case ETransfer::IEC61966_2_4: return linearToIec6196624(val);
        case ETransfer::BT1361Extended: return linearToBt1361Extended(val);
        case ETransfer::Gamma22: return linearToGamma(val, 2.2f);
        case ETransfer::Gamma28: return linearToGamma(val, 2.8f);
        case ETransfer::SMPTE240: return linearToSmpteSt240(val);
        case ETransfer::Linear: return val;
        case ETransfer::Log100: return linearToLog100(val);
        case ETransfer::Log100Sqrt10: return linearToLog100Sqrt10(val);
        case ETransfer::SRGB: return linearToSrgb(val);
        case ETransfer::PQ: return linearToPq(val);
        case ETransfer::SMPTE428: return linearToSmpteSt428(val);
        case ETransfer::HLG: return linearToHlgComponent(val);
        case ETransfer::YCbCrLinear: return val;
        case ETransfer::YCbCrSRGB: return linearToSrgb(val);
        default: return val; // Linear / Unspecified / LUT / GenericGamma / unimplemented
    }
}

template <ETransfer E, class B>
nanogui::Array<B, 3> transferRgbImpl(std::integral_constant<ETransfer, E>, const nanogui::Array<B, 3>& rgb) {
    return {
        transferComponent<E>(rgb.v[0]),
        transferComponent<E>(rgb.v[1]),
        transferComponent<E>(rgb.v[2]),
    };
}

template <class B>
nanogui::Array<B, 3> transferRgbImpl(std::integral_constant<ETransfer, ETransfer::HLG>, const nanogui::Array<B, 3>& rgb) {
    return linearToHlg(rgb);
}

template <class B>
nanogui::Array<B, 3> transferRgbImpl(std::integral_constant<ETransfer, ETransfer::YCbCrLinear>, const nanogui::Array<B, 3>& rgb) {
    return rgbToYCbCr(rgb);
}

template <class B>
nanogui::Array<B, 3> transferRgbImpl(std::integral_constant<ETransfer, ETransfer::YCbCrSRGB>, const nanogui::Array<B, 3>& rgb) {
    return rgbToYCbCr(transferRgbImpl(std::integral_constant<ETransfer, ETransfer::SRGB>(), rgb));
}

template <ETransfer TRANSFER, class B> nanogui::Array<B, 3> transferRgb(const nanogui::Array<B, 3>& rgb) noexcept {
    return transferRgbImpl(std::integral_constant<ETransfer, TRANSFER>(), rgb);
}

template <class B> nanogui::Array<B, 3> transferRgb(const ETransfer transfer, const nanogui::Array<B, 3>& rgb) noexcept {
    switch (transfer) {
        case ETransfer::BT709: return transferRgb<ETransfer::BT709>(rgb);
        case ETransfer::BT601: return transferRgb<ETransfer::BT601>(rgb);
        case ETransfer::BT202010bit: return transferRgb<ETransfer::BT202010bit>(rgb);
        case ETransfer::BT202012bit: return transferRgb<ETransfer::BT202012bit>(rgb);
        case ETransfer::IEC61966_2_4: return transferRgb<ETransfer::IEC61966_2_4>(rgb);
        case ETransfer::BT1361Extended: return transferRgb<ETransfer::BT1361Extended>(rgb);
        case ETransfer::Gamma22: return transferRgb<ETransfer::Gamma22>(rgb);
        case ETransfer::Gamma28: return transferRgb<ETransfer::Gamma28>(rgb);
        case ETransfer::SMPTE240: return transferRgb<ETransfer::SMPTE240>(rgb);
        case ETransfer::Linear: return transferRgb<ETransfer::Linear>(rgb);
        case ETransfer::Log100: return transferRgb<ETransfer::Log100>(rgb);
        case ETransfer::Log100Sqrt10: return transferRgb<ETransfer::Log100Sqrt10>(rgb);
        case ETransfer::SRGB: return transferRgb<ETransfer::SRGB>(rgb);
        case ETransfer::PQ: return transferRgb<ETransfer::PQ>(rgb);
        case ETransfer::SMPTE428: return transferRgb<ETransfer::SMPTE428>(rgb);
        case ETransfer::HLG: return transferRgb<ETransfer::HLG>(rgb);
        case ETransfer::Unspecified: return transferRgb<ETransfer::Unspecified>(rgb);
        case ETransfer::YCbCrLinear: return transferRgb<ETransfer::YCbCrLinear>(rgb);
        case ETransfer::YCbCrSRGB: return transferRgb<ETransfer::YCbCrSRGB>(rgb);
        case ETransfer::LUT: return transferRgb<ETransfer::LUT>(rgb);
        case ETransfer::GenericGamma: return transferRgb<ETransfer::GenericGamma>(rgb);
        default: return rgb;
    }
}

static constexpr float bestGuessReferenceWhiteLevel(const ETransfer transfer) {
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

    std::string_view description() const { return mDescription; }

private:
    void* mProfile = nullptr;
    std::string mDescription = "";
};

// Converts colors from an ICC profile to linear sRGB Rec.709 w/ premultiplied alpha.
//
// Note that, because we this function converts potentially larger color gamuts to sRGB, output channels may have values larger than 1 or
// smaller than 0, even if the input was within [0, 1]. This is by design, and, on macOS, the OS translates these out-of-bounds colors
// correctly to the gamut of the display. Other operating systems, like Windows and Linux don't do this -- it's a TODO for tev to explicitly
// hook into these OSs' color management systems to ensure that out-of-bounds colors are displayed correctly.
template <typename T>
Task<void> toLinearSrgbPremul(
    const ColorProfile& profile,
    EAlphaKind alphaKind,
    const MultiChannelView<const T>& src,
    const MultiChannelView<float>& rgbaDst,
    std::optional<ERenderingIntent> intentOverride,
    int priority,
    bool invertCmyk = false
);

template <typename T>
Task<void> toLinearSrgbPremul(
    const ColorProfile& profile,
    EAlphaKind alphaKind,
    const MultiChannelView<T>& src,
    const MultiChannelView<float>& rgbaDst,
    std::optional<ERenderingIntent> intentOverride,
    int priority,
    bool invertCmyk = false
) {
    co_await toLinearSrgbPremul(profile, alphaKind, MultiChannelView<const T>{src}, rgbaDst, intentOverride, priority, invertCmyk);
}

struct LimitedRange {
    float scale = 1.0f; // Scale factor for limited range to full range conversion
    float offset = 0.0f; // Offset for limited range to full range conversion

    static constexpr LimitedRange full() { return {1.0f, 0.0f}; }

    bool operator==(const LimitedRange& other) const { return scale == other.scale && offset == other.offset; }
};

LimitedRange limitedRangeForBitsPerSample(int bitsPerSample, bool cbcr = false);

} // namespace tev
