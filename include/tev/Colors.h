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
                const auto start = 4 * xsimd::clip(floatToInt(linear * B(fcd.size() / 4)), vi{0}, vi(fcd.size() / 4 - 1));
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
    using xsimd::fma;
    using xsimd::max;

    const B tmp = fastPow(max(val, B{0.0f}), B{pq::invm2});
    const B num = max(tmp - pq::c1, B{0.0f});
    const B den = max(fma(tmp, B{-pq::c3}, B{pq::c2}), B(1e-5f));
    return (10000.0f / 203.0f) * fastPow(num / den, B{pq::invm1});
}

template <class B> B linearToPq(const B& val) {
    using xsimd::fma;
    using xsimd::max;

    const B p = fastPow(max(val * (203.0f / 10000.0f), B(0.0f)), B(pq::m1));
    const B num = fma(p, B{pq::c2}, B{pq::c1});
    const B den = fma(p, B{pq::c3}, B{1.0f});
    return fastPow(num / den, B(pq::m2));
}

// linear^(1/8) sampled at PQ code value i/1024, i = 0..1024.
// linear is normalized so 1.0 == 10000 nits.
// Reconstruct: y = lerp(table[i], table[i+1], t); y*=y; y*=y; y*=y;
// Max relative error vs exact ST 2084: 0.0068%% above 0.05 nits.
alignas(64) static const float kPqToLinearRoot8[1025] = {
    0.000000000e+00f, 8.927670873e-02f, 1.034226569e-01f, 1.127905356e-01f, 1.200279027e-01f, 1.260271477e-01f, 1.312064650e-01f,
    1.357980529e-01f, 1.399452315e-01f, 1.437431576e-01f, 1.472584848e-01f, 1.505398577e-01f, 1.536239502e-01f, 1.565391510e-01f,
    1.593079210e-01f, 1.619483619e-01f, 1.644752930e-01f, 1.669010126e-01f, 1.692358491e-01f, 1.714885669e-01f, 1.736666731e-01f,
    1.757766510e-01f, 1.778241415e-01f, 1.798140859e-01f, 1.817508390e-01f, 1.836382602e-01f, 1.854797875e-01f, 1.872784973e-01f,
    1.890371554e-01f, 1.907582572e-01f, 1.924440633e-01f, 1.940966284e-01f, 1.957178261e-01f, 1.973093699e-01f, 1.988728316e-01f,
    2.004096565e-01f, 2.019211771e-01f, 2.034086250e-01f, 2.048731404e-01f, 2.063157819e-01f, 2.077375337e-01f, 2.091393130e-01f,
    2.105219756e-01f, 2.118863219e-01f, 2.132331012e-01f, 2.145630164e-01f, 2.158767279e-01f, 2.171748565e-01f, 2.184579873e-01f,
    2.197266719e-01f, 2.209814312e-01f, 2.222227577e-01f, 2.234511175e-01f, 2.246669521e-01f, 2.258706806e-01f, 2.270627005e-01f,
    2.282433899e-01f, 2.294131082e-01f, 2.305721979e-01f, 2.317209852e-01f, 2.328597811e-01f, 2.339888828e-01f, 2.351085738e-01f,
    2.362191255e-01f, 2.373207973e-01f, 2.384138376e-01f, 2.394984845e-01f, 2.405749662e-01f, 2.416435015e-01f, 2.427043006e-01f,
    2.437575653e-01f, 2.448034896e-01f, 2.458422599e-01f, 2.468740558e-01f, 2.478990500e-01f, 2.489174090e-01f, 2.499292931e-01f,
    2.509348569e-01f, 2.519342497e-01f, 2.529276152e-01f, 2.539150926e-01f, 2.548968162e-01f, 2.558729156e-01f, 2.568435163e-01f,
    2.578087397e-01f, 2.587687032e-01f, 2.597235204e-01f, 2.606733014e-01f, 2.616181528e-01f, 2.625581780e-01f, 2.634934772e-01f,
    2.644241475e-01f, 2.653502831e-01f, 2.662719756e-01f, 2.671893137e-01f, 2.681023837e-01f, 2.690112694e-01f, 2.699160521e-01f,
    2.708168111e-01f, 2.717136232e-01f, 2.726065632e-01f, 2.734957041e-01f, 2.743811166e-01f, 2.752628697e-01f, 2.761410306e-01f,
    2.770156647e-01f, 2.778868357e-01f, 2.787546057e-01f, 2.796190351e-01f, 2.804801832e-01f, 2.813381072e-01f, 2.821928635e-01f,
    2.830445067e-01f, 2.838930903e-01f, 2.847386665e-01f, 2.855812861e-01f, 2.864209990e-01f, 2.872578536e-01f, 2.880918975e-01f,
    2.889231769e-01f, 2.897517373e-01f, 2.905776228e-01f, 2.914008769e-01f, 2.922215419e-01f, 2.930396591e-01f, 2.938552691e-01f,
    2.946684116e-01f, 2.954791254e-01f, 2.962874485e-01f, 2.970934179e-01f, 2.978970702e-01f, 2.986984411e-01f, 2.994975653e-01f,
    3.002944772e-01f, 3.010892103e-01f, 3.018817973e-01f, 3.026722706e-01f, 3.034606617e-01f, 3.042470014e-01f, 3.050313203e-01f,
    3.058136480e-01f, 3.065940137e-01f, 3.073724461e-01f, 3.081489732e-01f, 3.089236226e-01f, 3.096964215e-01f, 3.104673962e-01f,
    3.112365730e-01f, 3.120039775e-01f, 3.127696347e-01f, 3.135335693e-01f, 3.142958057e-01f, 3.150563675e-01f, 3.158152784e-01f,
    3.165725611e-01f, 3.173282384e-01f, 3.180823324e-01f, 3.188348651e-01f, 3.195858577e-01f, 3.203353315e-01f, 3.210833073e-01f,
    3.218298053e-01f, 3.225748457e-01f, 3.233184483e-01f, 3.240606323e-01f, 3.248014171e-01f, 3.255408213e-01f, 3.262788635e-01f,
    3.270155619e-01f, 3.277509343e-01f, 3.284849986e-01f, 3.292177719e-01f, 3.299492715e-01f, 3.306795141e-01f, 3.314085163e-01f,
    3.321362946e-01f, 3.328628649e-01f, 3.335882432e-01f, 3.343124450e-01f, 3.350354857e-01f, 3.357573804e-01f, 3.364781442e-01f,
    3.371977918e-01f, 3.379163375e-01f, 3.386337959e-01f, 3.393501808e-01f, 3.400655064e-01f, 3.407797861e-01f, 3.414930337e-01f,
    3.422052624e-01f, 3.429164852e-01f, 3.436267153e-01f, 3.443359654e-01f, 3.450442481e-01f, 3.457515759e-01f, 3.464579610e-01f,
    3.471634155e-01f, 3.478679515e-01f, 3.485715806e-01f, 3.492743146e-01f, 3.499761650e-01f, 3.506771430e-01f, 3.513772600e-01f,
    3.520765268e-01f, 3.527749546e-01f, 3.534725540e-01f, 3.541693357e-01f, 3.548653102e-01f, 3.555604878e-01f, 3.562548790e-01f,
    3.569484937e-01f, 3.576413419e-01f, 3.583334337e-01f, 3.590247787e-01f, 3.597153865e-01f, 3.604052668e-01f, 3.610944289e-01f,
    3.617828822e-01f, 3.624706358e-01f, 3.631576989e-01f, 3.638440804e-01f, 3.645297892e-01f, 3.652148342e-01f, 3.658992239e-01f,
    3.665829671e-01f, 3.672660721e-01f, 3.679485474e-01f, 3.686304013e-01f, 3.693116420e-01f, 3.699922776e-01f, 3.706723162e-01f,
    3.713517656e-01f, 3.720306338e-01f, 3.727089286e-01f, 3.733866575e-01f, 3.740638283e-01f, 3.747404485e-01f, 3.754165255e-01f,
    3.760920667e-01f, 3.767670794e-01f, 3.774415708e-01f, 3.781155480e-01f, 3.787890182e-01f, 3.794619884e-01f, 3.801344654e-01f,
    3.808064561e-01f, 3.814779674e-01f, 3.821490059e-01f, 3.828195783e-01f, 3.834896913e-01f, 3.841593513e-01f, 3.848285648e-01f,
    3.854973382e-01f, 3.861656779e-01f, 3.868335902e-01f, 3.875010811e-01f, 3.881681571e-01f, 3.888348240e-01f, 3.895010880e-01f,
    3.901669551e-01f, 3.908324313e-01f, 3.914975223e-01f, 3.921622340e-01f, 3.928265723e-01f, 3.934905427e-01f, 3.941541511e-01f,
    3.948174030e-01f, 3.954803040e-01f, 3.961428595e-01f, 3.968050752e-01f, 3.974669564e-01f, 3.981285085e-01f, 3.987897368e-01f,
    3.994506467e-01f, 4.001112432e-01f, 4.007715318e-01f, 4.014315174e-01f, 4.020912053e-01f, 4.027506004e-01f, 4.034097078e-01f,
    4.040685326e-01f, 4.047270795e-01f, 4.053853536e-01f, 4.060433597e-01f, 4.067011026e-01f, 4.073585871e-01f, 4.080158180e-01f,
    4.086727999e-01f, 4.093295376e-01f, 4.099860356e-01f, 4.106422985e-01f, 4.112983310e-01f, 4.119541376e-01f, 4.126097227e-01f,
    4.132650907e-01f, 4.139202462e-01f, 4.145751936e-01f, 4.152299371e-01f, 4.158844811e-01f, 4.165388298e-01f, 4.171929877e-01f,
    4.178469588e-01f, 4.185007474e-01f, 4.191543576e-01f, 4.198077937e-01f, 4.204610596e-01f, 4.211141596e-01f, 4.217670975e-01f,
    4.224198775e-01f, 4.230725036e-01f, 4.237249796e-01f, 4.243773096e-01f, 4.250294975e-01f, 4.256815472e-01f, 4.263334624e-01f,
    4.269852471e-01f, 4.276369050e-01f, 4.282884400e-01f, 4.289398558e-01f, 4.295911560e-01f, 4.302423445e-01f, 4.308934250e-01f,
    4.315444009e-01f, 4.321952761e-01f, 4.328460541e-01f, 4.334967385e-01f, 4.341473329e-01f, 4.347978408e-01f, 4.354482658e-01f,
    4.360986113e-01f, 4.367488808e-01f, 4.373990778e-01f, 4.380492057e-01f, 4.386992679e-01f, 4.393492678e-01f, 4.399992089e-01f,
    4.406490944e-01f, 4.412989277e-01f, 4.419487121e-01f, 4.425984509e-01f, 4.432481474e-01f, 4.438978048e-01f, 4.445474263e-01f,
    4.451970153e-01f, 4.458465749e-01f, 4.464961083e-01f, 4.471456186e-01f, 4.477951090e-01f, 4.484445826e-01f, 4.490940426e-01f,
    4.497434920e-01f, 4.503929340e-01f, 4.510423715e-01f, 4.516918077e-01f, 4.523412456e-01f, 4.529906881e-01f, 4.536401384e-01f,
    4.542895993e-01f, 4.549390739e-01f, 4.555885650e-01f, 4.562380757e-01f, 4.568876089e-01f, 4.575371674e-01f, 4.581867542e-01f,
    4.588363722e-01f, 4.594860241e-01f, 4.601357130e-01f, 4.607854415e-01f, 4.614352126e-01f, 4.620850291e-01f, 4.627348936e-01f,
    4.633848092e-01f, 4.640347784e-01f, 4.646848041e-01f, 4.653348890e-01f, 4.659850358e-01f, 4.666352474e-01f, 4.672855263e-01f,
    4.679358753e-01f, 4.685862970e-01f, 4.692367942e-01f, 4.698873695e-01f, 4.705380255e-01f, 4.711887649e-01f, 4.718395904e-01f,
    4.724905045e-01f, 4.731415098e-01f, 4.737926089e-01f, 4.744438045e-01f, 4.750950991e-01f, 4.757464952e-01f, 4.763979954e-01f,
    4.770496023e-01f, 4.777013184e-01f, 4.783531461e-01f, 4.790050881e-01f, 4.796571468e-01f, 4.803093247e-01f, 4.809616243e-01f,
    4.816140481e-01f, 4.822665985e-01f, 4.829192780e-01f, 4.835720890e-01f, 4.842250340e-01f, 4.848781154e-01f, 4.855313356e-01f,
    4.861846970e-01f, 4.868382021e-01f, 4.874918532e-01f, 4.881456527e-01f, 4.887996030e-01f, 4.894537064e-01f, 4.901079653e-01f,
    4.907623821e-01f, 4.914169591e-01f, 4.920716987e-01f, 4.927266031e-01f, 4.933816747e-01f, 4.940369158e-01f, 4.946923288e-01f,
    4.953479158e-01f, 4.960036793e-01f, 4.966596214e-01f, 4.973157446e-01f, 4.979720509e-01f, 4.986285428e-01f, 4.992852224e-01f,
    4.999420920e-01f, 5.005991538e-01f, 5.012564102e-01f, 5.019138632e-01f, 5.025715152e-01f, 5.032293684e-01f, 5.038874249e-01f,
    5.045456870e-01f, 5.052041568e-01f, 5.058628366e-01f, 5.065217286e-01f, 5.071808349e-01f, 5.078401577e-01f, 5.084996992e-01f,
    5.091594615e-01f, 5.098194468e-01f, 5.104796572e-01f, 5.111400950e-01f, 5.118007622e-01f, 5.124616609e-01f, 5.131227934e-01f,
    5.137841617e-01f, 5.144457679e-01f, 5.151076142e-01f, 5.157697027e-01f, 5.164320355e-01f, 5.170946146e-01f, 5.177574422e-01f,
    5.184205204e-01f, 5.190838513e-01f, 5.197474369e-01f, 5.204112793e-01f, 5.210753806e-01f, 5.217397428e-01f, 5.224043681e-01f,
    5.230692585e-01f, 5.237344160e-01f, 5.243998427e-01f, 5.250655407e-01f, 5.257315119e-01f, 5.263977585e-01f, 5.270642824e-01f,
    5.277310857e-01f, 5.283981704e-01f, 5.290655386e-01f, 5.297331922e-01f, 5.304011334e-01f, 5.310693640e-01f, 5.317378861e-01f,
    5.324067017e-01f, 5.330758129e-01f, 5.337452216e-01f, 5.344149298e-01f, 5.350849395e-01f, 5.357552526e-01f, 5.364258713e-01f,
    5.370967975e-01f, 5.377680330e-01f, 5.384395800e-01f, 5.391114405e-01f, 5.397836162e-01f, 5.404561093e-01f, 5.411289218e-01f,
    5.418020555e-01f, 5.424755124e-01f, 5.431492945e-01f, 5.438234037e-01f, 5.444978421e-01f, 5.451726114e-01f, 5.458477138e-01f,
    5.465231511e-01f, 5.471989253e-01f, 5.478750383e-01f, 5.485514921e-01f, 5.492282886e-01f, 5.499054297e-01f, 5.505829173e-01f,
    5.512607535e-01f, 5.519389400e-01f, 5.526174789e-01f, 5.532963721e-01f, 5.539756214e-01f, 5.546552289e-01f, 5.553351963e-01f,
    5.560155257e-01f, 5.566962190e-01f, 5.573772780e-01f, 5.580587047e-01f, 5.587405010e-01f, 5.594226687e-01f, 5.601052099e-01f,
    5.607881263e-01f, 5.614714200e-01f, 5.621550927e-01f, 5.628391464e-01f, 5.635235831e-01f, 5.642084045e-01f, 5.648936126e-01f,
    5.655792093e-01f, 5.662651965e-01f, 5.669515760e-01f, 5.676383498e-01f, 5.683255198e-01f, 5.690130878e-01f, 5.697010557e-01f,
    5.703894254e-01f, 5.710781988e-01f, 5.717673777e-01f, 5.724569641e-01f, 5.731469599e-01f, 5.738373669e-01f, 5.745281870e-01f,
    5.752194221e-01f, 5.759110740e-01f, 5.766031447e-01f, 5.772956360e-01f, 5.779885497e-01f, 5.786818879e-01f, 5.793756523e-01f,
    5.800698448e-01f, 5.807644673e-01f, 5.814595217e-01f, 5.821550098e-01f, 5.828509336e-01f, 5.835472948e-01f, 5.842440954e-01f,
    5.849413372e-01f, 5.856390221e-01f, 5.863371520e-01f, 5.870357288e-01f, 5.877347543e-01f, 5.884342304e-01f, 5.891341589e-01f,
    5.898345418e-01f, 5.905353808e-01f, 5.912366780e-01f, 5.919384351e-01f, 5.926406540e-01f, 5.933433366e-01f, 5.940464848e-01f,
    5.947501004e-01f, 5.954541853e-01f, 5.961587413e-01f, 5.968637705e-01f, 5.975692745e-01f, 5.982752553e-01f, 5.989817148e-01f,
    5.996886548e-01f, 6.003960773e-01f, 6.011039840e-01f, 6.018123769e-01f, 6.025212578e-01f, 6.032306286e-01f, 6.039404912e-01f,
    6.046508475e-01f, 6.053616992e-01f, 6.060730485e-01f, 6.067848970e-01f, 6.074972466e-01f, 6.082100993e-01f, 6.089234570e-01f,
    6.096373214e-01f, 6.103516946e-01f, 6.110665783e-01f, 6.117819745e-01f, 6.124978850e-01f, 6.132143117e-01f, 6.139312566e-01f,
    6.146487214e-01f, 6.153667082e-01f, 6.160852187e-01f, 6.168042548e-01f, 6.175238186e-01f, 6.182439117e-01f, 6.189645362e-01f,
    6.196856940e-01f, 6.204073868e-01f, 6.211296167e-01f, 6.218523856e-01f, 6.225756952e-01f, 6.232995475e-01f, 6.240239445e-01f,
    6.247488880e-01f, 6.254743800e-01f, 6.262004222e-01f, 6.269270167e-01f, 6.276541654e-01f, 6.283818701e-01f, 6.291101328e-01f,
    6.298389553e-01f, 6.305683397e-01f, 6.312982877e-01f, 6.320288014e-01f, 6.327598827e-01f, 6.334915334e-01f, 6.342237555e-01f,
    6.349565509e-01f, 6.356899215e-01f, 6.364238693e-01f, 6.371583962e-01f, 6.378935042e-01f, 6.386291950e-01f, 6.393654708e-01f,
    6.401023334e-01f, 6.408397848e-01f, 6.415778269e-01f, 6.423164616e-01f, 6.430556909e-01f, 6.437955168e-01f, 6.445359411e-01f,
    6.452769659e-01f, 6.460185930e-01f, 6.467608245e-01f, 6.475036623e-01f, 6.482471084e-01f, 6.489911646e-01f, 6.497358331e-01f,
    6.504811156e-01f, 6.512270143e-01f, 6.519735311e-01f, 6.527206679e-01f, 6.534684268e-01f, 6.542168096e-01f, 6.549658184e-01f,
    6.557154552e-01f, 6.564657219e-01f, 6.572166206e-01f, 6.579681532e-01f, 6.587203216e-01f, 6.594731280e-01f, 6.602265743e-01f,
    6.609806624e-01f, 6.617353945e-01f, 6.624907724e-01f, 6.632467983e-01f, 6.640034741e-01f, 6.647608018e-01f, 6.655187834e-01f,
    6.662774210e-01f, 6.670367166e-01f, 6.677966721e-01f, 6.685572897e-01f, 6.693185714e-01f, 6.700805191e-01f, 6.708431349e-01f,
    6.716064209e-01f, 6.723703791e-01f, 6.731350115e-01f, 6.739003202e-01f, 6.746663072e-01f, 6.754329746e-01f, 6.762003244e-01f,
    6.769683587e-01f, 6.777370796e-01f, 6.785064891e-01f, 6.792765892e-01f, 6.800473821e-01f, 6.808188699e-01f, 6.815910545e-01f,
    6.823639381e-01f, 6.831375228e-01f, 6.839118107e-01f, 6.846868038e-01f, 6.854625042e-01f, 6.862389140e-01f, 6.870160354e-01f,
    6.877938704e-01f, 6.885724212e-01f, 6.893516898e-01f, 6.901316784e-01f, 6.909123890e-01f, 6.916938239e-01f, 6.924759850e-01f,
    6.932588747e-01f, 6.940424949e-01f, 6.948268478e-01f, 6.956119356e-01f, 6.963977604e-01f, 6.971843244e-01f, 6.979716296e-01f,
    6.987596783e-01f, 6.995484726e-01f, 7.003380147e-01f, 7.011283067e-01f, 7.019193508e-01f, 7.027111492e-01f, 7.035037041e-01f,
    7.042970176e-01f, 7.050910919e-01f, 7.058859292e-01f, 7.066815317e-01f, 7.074779017e-01f, 7.082750412e-01f, 7.090729525e-01f,
    7.098716378e-01f, 7.106710994e-01f, 7.114713394e-01f, 7.122723601e-01f, 7.130741637e-01f, 7.138767524e-01f, 7.146801284e-01f,
    7.154842941e-01f, 7.162892516e-01f, 7.170950032e-01f, 7.179015511e-01f, 7.187088976e-01f, 7.195170450e-01f, 7.203259955e-01f,
    7.211357514e-01f, 7.219463149e-01f, 7.227576884e-01f, 7.235698742e-01f, 7.243828744e-01f, 7.251966915e-01f, 7.260113276e-01f,
    7.268267852e-01f, 7.276430665e-01f, 7.284601738e-01f, 7.292781095e-01f, 7.300968758e-01f, 7.309164751e-01f, 7.317369098e-01f,
    7.325581820e-01f, 7.333802943e-01f, 7.342032490e-01f, 7.350270483e-01f, 7.358516947e-01f, 7.366771904e-01f, 7.375035380e-01f,
    7.383307397e-01f, 7.391587979e-01f, 7.399877150e-01f, 7.408174934e-01f, 7.416481355e-01f, 7.424796436e-01f, 7.433120203e-01f,
    7.441452678e-01f, 7.449793886e-01f, 7.458143851e-01f, 7.466502597e-01f, 7.474870149e-01f, 7.483246531e-01f, 7.491631768e-01f,
    7.500025883e-01f, 7.508428901e-01f, 7.516840847e-01f, 7.525261746e-01f, 7.533691622e-01f, 7.542130499e-01f, 7.550578403e-01f,
    7.559035359e-01f, 7.567501391e-01f, 7.575976524e-01f, 7.584460783e-01f, 7.592954194e-01f, 7.601456781e-01f, 7.609968570e-01f,
    7.618489586e-01f, 7.627019854e-01f, 7.635559399e-01f, 7.644108248e-01f, 7.652666424e-01f, 7.661233955e-01f, 7.669810865e-01f,
    7.678397181e-01f, 7.686992927e-01f, 7.695598131e-01f, 7.704212816e-01f, 7.712837011e-01f, 7.721470739e-01f, 7.730114028e-01f,
    7.738766904e-01f, 7.747429392e-01f, 7.756101520e-01f, 7.764783312e-01f, 7.773474796e-01f, 7.782175998e-01f, 7.790886944e-01f,
    7.799607662e-01f, 7.808338177e-01f, 7.817078516e-01f, 7.825828707e-01f, 7.834588775e-01f, 7.843358748e-01f, 7.852138652e-01f,
    7.860928515e-01f, 7.869728364e-01f, 7.878538226e-01f, 7.887358127e-01f, 7.896188096e-01f, 7.905028160e-01f, 7.913878346e-01f,
    7.922738682e-01f, 7.931609195e-01f, 7.940489912e-01f, 7.949380862e-01f, 7.958282073e-01f, 7.967193571e-01f, 7.976115386e-01f,
    7.985047545e-01f, 7.993990075e-01f, 8.002943006e-01f, 8.011906366e-01f, 8.020880182e-01f, 8.029864483e-01f, 8.038859298e-01f,
    8.047864655e-01f, 8.056880582e-01f, 8.065907109e-01f, 8.074944263e-01f, 8.083992075e-01f, 8.093050572e-01f, 8.102119783e-01f,
    8.111199739e-01f, 8.120290467e-01f, 8.129391997e-01f, 8.138504359e-01f, 8.147627581e-01f, 8.156761693e-01f, 8.165906724e-01f,
    8.175062705e-01f, 8.184229665e-01f, 8.193407633e-01f, 8.202596639e-01f, 8.211796714e-01f, 8.221007887e-01f, 8.230230189e-01f,
    8.239463649e-01f, 8.248708298e-01f, 8.257964165e-01f, 8.267231283e-01f, 8.276509680e-01f, 8.285799388e-01f, 8.295100437e-01f,
    8.304412859e-01f, 8.313736682e-01f, 8.323071940e-01f, 8.332418663e-01f, 8.341776881e-01f, 8.351146626e-01f, 8.360527930e-01f,
    8.369920823e-01f, 8.379325337e-01f, 8.388741504e-01f, 8.398169355e-01f, 8.407608922e-01f, 8.417060237e-01f, 8.426523332e-01f,
    8.435998238e-01f, 8.445484988e-01f, 8.454983613e-01f, 8.464494147e-01f, 8.474016621e-01f, 8.483551068e-01f, 8.493097521e-01f,
    8.502656012e-01f, 8.512226573e-01f, 8.521809238e-01f, 8.531404040e-01f, 8.541011011e-01f, 8.550630185e-01f, 8.560261595e-01f,
    8.569905274e-01f, 8.579561256e-01f, 8.589229573e-01f, 8.598910261e-01f, 8.608603351e-01f, 8.618308879e-01f, 8.628026878e-01f,
    8.637757382e-01f, 8.647500425e-01f, 8.657256042e-01f, 8.667024266e-01f, 8.676805132e-01f, 8.686598675e-01f, 8.696404929e-01f,
    8.706223929e-01f, 8.716055709e-01f, 8.725900305e-01f, 8.735757752e-01f, 8.745628085e-01f, 8.755511338e-01f, 8.765407548e-01f,
    8.775316749e-01f, 8.785238978e-01f, 8.795174270e-01f, 8.805122661e-01f, 8.815084186e-01f, 8.825058882e-01f, 8.835046785e-01f,
    8.845047931e-01f, 8.855062356e-01f, 8.865090096e-01f, 8.875131189e-01f, 8.885185671e-01f, 8.895253579e-01f, 8.905334949e-01f,
    8.915429819e-01f, 8.925538225e-01f, 8.935660205e-01f, 8.945795797e-01f, 8.955945037e-01f, 8.966107964e-01f, 8.976284615e-01f,
    8.986475027e-01f, 8.996679239e-01f, 9.006897289e-01f, 9.017129215e-01f, 9.027375055e-01f, 9.037634848e-01f, 9.047908632e-01f,
    9.058196446e-01f, 9.068498329e-01f, 9.078814319e-01f, 9.089144456e-01f, 9.099488778e-01f, 9.109847325e-01f, 9.120220137e-01f,
    9.130607253e-01f, 9.141008712e-01f, 9.151424554e-01f, 9.161854820e-01f, 9.172299549e-01f, 9.182758781e-01f, 9.193232557e-01f,
    9.203720917e-01f, 9.214223902e-01f, 9.224741553e-01f, 9.235273909e-01f, 9.245821012e-01f, 9.256382904e-01f, 9.266959625e-01f,
    9.277551217e-01f, 9.288157721e-01f, 9.298779179e-01f, 9.309415633e-01f, 9.320067124e-01f, 9.330733694e-01f, 9.341415386e-01f,
    9.352112243e-01f, 9.362824305e-01f, 9.373551617e-01f, 9.384294220e-01f, 9.395052158e-01f, 9.405825474e-01f, 9.416614211e-01f,
    9.427418411e-01f, 9.438238119e-01f, 9.449073378e-01f, 9.459924232e-01f, 9.470790725e-01f, 9.481672900e-01f, 9.492570802e-01f,
    9.503484476e-01f, 9.514413965e-01f, 9.525359314e-01f, 9.536320568e-01f, 9.547297773e-01f, 9.558290972e-01f, 9.569300212e-01f,
    9.580325537e-01f, 9.591366993e-01f, 9.602424627e-01f, 9.613498483e-01f, 9.624588607e-01f, 9.635695046e-01f, 9.646817847e-01f,
    9.657957055e-01f, 9.669112717e-01f, 9.680284881e-01f, 9.691473592e-01f, 9.702678898e-01f, 9.713900847e-01f, 9.725139486e-01f,
    9.736394861e-01f, 9.747667022e-01f, 9.758956016e-01f, 9.770261891e-01f, 9.781584696e-01f, 9.792924478e-01f, 9.804281287e-01f,
    9.815655171e-01f, 9.827046179e-01f, 9.838454360e-01f, 9.849879764e-01f, 9.861322440e-01f, 9.872782437e-01f, 9.884259806e-01f,
    9.895754596e-01f, 9.907266857e-01f, 9.918796641e-01f, 9.930343996e-01f, 9.941908975e-01f, 9.953491628e-01f, 9.965092005e-01f,
    9.976710159e-01f, 9.988346140e-01f, 1.000000000e+00f,
};

template <class B> B pqToLinearLut(const B& n) {
    using xsimd::clip;
    using xsimd::fma;
    using xsimd::min;

    using vi = int_companion_t<B>;

    const auto x = clip(n, B{0.0f}, B{1.0f}) * 1024.0f;
    const auto i = min(floatToInt(x), vi{1023});
    const auto t = x - intToFloat(i);

    const auto a0 = gather<B>(kPqToLinearRoot8, i);
    const auto a1 = gather<B>(kPqToLinearRoot8, i + 1);
    B y = fma(t, a1 - a0, a0);

    y = y * y;
    y = y * y;
    y = y * y;
    return y * (10000.0f / 203.0f);
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
    using xsimd::fma;
    using xsimd::select;

    const B lo = v * v * (1.0f / 3.0f);
    const B hi = fma(fastExp((v - hlg::c) * (1.0f / hlg::a)), B{1.0f / 12.0f}, B{hlg::b / 12.0f});
    return select(v <= B(0.5f), lo, hi);
}

template <class B> B hlgOetf(const B& v) {
    using xsimd::fma;
    using xsimd::select;
    using xsimd::sqrt;

    const B lo = sqrt(v * 3.0f);
    const B hi = fma(fastLog(fma(v, B{12.0f}, B{-hlg::b})), B{hlg::a}, B{hlg::c});
    return select(v <= B(1.0f / 12.0f), lo, hi);
}

// SoA HLG->linear: r,g,b are batches of the same set of pixels.
template <class B> nanogui::Array<B, 3> hlgToLinear(const nanogui::Array<B, 3>& rgb) {
    const B er = hlgInvOetf(rgb.x());
    const B eg = hlgInvOetf(rgb.y());
    const B eb = hlgInvOetf(rgb.z());

    const B lum = B{0.2627f} * er + B{0.6780f} * eg + B{0.0593f} * eb;
    const B scale = fastPow(lum, B{hlg::gamma - 1.0f}) * (hlg::gain / 203.0f);

    return {scale * er, scale * eg, scale * eb};
}

template <class B> nanogui::Array<B, 3> linearToHlg(const nanogui::Array<B, 3>& rgb) {
    // convert from linear units where SDR white is 1.0, then invOotf
    const B tr = rgb.x() * (203.0f / hlg::gain);
    const B tg = rgb.y() * (203.0f / hlg::gain);
    const B tb = rgb.z() * (203.0f / hlg::gain);

    const B lum = B(0.2627f) * tr + B(0.6780f) * tg + B(0.0593f) * tb;
    const B scale = fastPow(lum, B{(1.0f - hlg::gamma) / hlg::gamma});

    return {hlgOetf(scale * tr), hlgOetf(scale * tg), hlgOetf(scale * tb)};
}

// R=G=B single-component HLG (matches original invTransferComponent<HLG>)
template <class B> B hlgToLinearComponent(const B& val) {
    const B e = hlgInvOetf(val);
    const B lum = e; // 0.2627+0.6780+0.0593 == 1
    return fastPow(lum, B(hlg::gamma - 1.0f)) * e * (hlg::gain / 203.0f);
}

// R=G=B single-component HLG (inverse of hlgToLinearComponent, matches original linearToHlg with R=G=B)
template <class B> B linearToHlgComponent(const B& val) {
    const B tmp = val * (203.0f / hlg::gain); // linear units where SDR white is 1.0
    const B lum = tmp;                        // 0.2627 + 0.6780 + 0.0593 == 1
    const B e = fastPow(lum, B{(1.0f - hlg::gamma) / hlg::gamma}) * tmp;
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
IT_SPEC(PQ, pqToLinearLut(val))
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
        case ETransfer::PQ: return pqToLinearLut(val);
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
// smaller than 0, even if the input was within [0, 1]. This is by design such that larger-than-sRGB colors are not clipped and display
// correctly.
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
