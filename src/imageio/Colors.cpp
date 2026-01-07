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

#include <tev/Common.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/Colors.h>

#include <nanogui/vector.h>

#include <lcms2.h>
#include <lcms2_fast_float.h>

#include <algorithm>

using namespace std;
using namespace nanogui;

namespace tev {

string_view toString(ERenderingIntent intent) {
    switch (intent) {
        case ERenderingIntent::Perceptual: return "perceptual";
        case ERenderingIntent::RelativeColorimetric: return "relative_colorimetric";
        case ERenderingIntent::Saturation: return "saturation";
        case ERenderingIntent::AbsoluteColorimetric: return "absolute_colorimetric";
        default: throw invalid_argument{"Unknown rendering intent."};
    }
}

array<Vector2f, 4> zeroChroma() {
    array<Vector2f, 4> chroma;
    fill(chroma.begin(), chroma.end(), Vector2f{0.0f});
    return chroma;
}

// This function takes matrix params in row-major order and converts them to column-major Matrix3f
Matrix3f toMatrix3(const float data[3][3]) {
    Matrix3f result;
    for (int m = 0; m < 3; ++m) {
        for (int n = 0; n < 3; ++n) {
            result.m[m][n] = data[n][m];
        }
    }

    return result;
}

// This routine was copied from OpenEXR's ImfChromaticities.cpp in accordance
// with its BSD-3-Clause license. See the header of that file for details.
// https://github.com/AcademySoftwareFoundation/openexr/blob/main/src/lib/OpenEXR/ImfChromaticities.cpp
Matrix3f rgbToXyz(const std::array<Vector2f, 4>& chroma, float Y) {
    //
    // For an explanation of how the color conversion matrix is derived,
    // see Roy Hall, "Illumination and Color in Computer Generated Imagery",
    // Springer-Verlag, 1989, chapter 3, "Perceptual Response"; and
    // Charles A. Poynton, "A Technical Introduction to Digital Video",
    // John Wiley & Sons, 1996, chapter 7, "Color science for video".
    //

    //
    // X and Z values of RGB value (1, 1, 1), or "white"
    //

    const Vector2f& red = chroma[0];
    const Vector2f& green = chroma[1];
    const Vector2f& blue = chroma[2];
    const Vector2f& white = chroma[3];

    // prevent a division that rounds to zero
    if (std::abs(white.y()) <= 1.f && std::abs(white.x() * Y) >= std::abs(white.y()) * std::numeric_limits<float>::max()) {
        throw std::invalid_argument("Bad chromaticities: white.y cannot be zero");
    }

    float X = white.x() * Y / white.y();
    float Z = (1 - white.x() - white.y()) * Y / white.y();

    //
    // Scale factors for matrix rows, compute numerators and common denominator
    //

    float d = red.x() * (blue.y() - green.y()) + blue.x() * (green.y() - red.y()) + green.x() * (red.y() - blue.y());

    float SrN =
        (X * (blue.y() - green.y()) - green.x() * (Y * (blue.y() - 1) + blue.y() * (X + Z)) +
         blue.x() * (Y * (green.y() - 1) + green.y() * (X + Z)));

    float SgN =
        (X * (red.y() - blue.y()) + red.x() * (Y * (blue.y() - 1) + blue.y() * (X + Z)) - blue.x() * (Y * (red.y() - 1) + red.y() * (X + Z)));

    float SbN =
        (X * (green.y() - red.y()) - red.x() * (Y * (green.y() - 1) + green.y() * (X + Z)) +
         green.x() * (Y * (red.y() - 1) + red.y() * (X + Z)));

    if (std::abs(d) < 1.f &&
        (std::abs(SrN) >= std::abs(d) * std::numeric_limits<float>::max() || std::abs(SgN) >= std::abs(d) * std::numeric_limits<float>::max() ||
         std::abs(SbN) >= std::abs(d) * std::numeric_limits<float>::max())) {
        // cannot generate matrix if all RGB primaries have the same y value
        // or if they all have the an x value of zero
        // in both cases, the primaries are colinear, which makes them unusable
        throw std::invalid_argument("Bad chromaticities: RGBtoXYZ matrix is degenerate");
    }

    float Sr = SrN / d;
    float Sg = SgN / d;
    float Sb = SbN / d;

    //
    // Assemble the matrix
    //

    Matrix3f M;

    M.m[0][0] = Sr * red.x();
    M.m[0][1] = Sr * red.y();
    M.m[0][2] = Sr * (1 - red.x() - red.y());

    M.m[1][0] = Sg * green.x();
    M.m[1][1] = Sg * green.y();
    M.m[1][2] = Sg * (1 - green.x() - green.y());

    M.m[2][0] = Sb * blue.x();
    M.m[2][1] = Sb * blue.y();
    M.m[2][2] = Sb * (1 - blue.x() - blue.y());

    return M;
}

Matrix3f xyzToRgb(const std::array<Vector2f, 4>& chroma, float Y) { return inverse(rgbToXyz(chroma, Y)); }

Matrix3f xyzToChromaMatrix(const std::array<Vector2f, 4>& chroma) { return xyzToRgb(chroma, 1); }

// Adapted from LittleCMS's AdaptToXYZD50 function
Matrix3f adaptWhiteBradford(const Vector2f& srcWhite, const Vector2f& dstWhite) {
    if (srcWhite == dstWhite) {
        return Matrix3f{1.0f};
    }

    const float br[3][3] = {
        {0.8951f,  0.2664f,  -0.1614f},
        {-0.7502f, 1.7135f,  0.0367f },
        {0.0389f,  -0.0685f, 1.0296f },
    };
    const auto kBradford = toMatrix3(br);
    const float brInv[3][3] = {
        {0.9869929f,  -0.1470543f, 0.1599627f},
        {0.4323053f,  0.5183603f,  0.0492912f},
        {-0.0085287f, 0.0400428f,  0.9684867f},
    };
    const auto kBradfordInv = toMatrix3(brInv);

    const auto xyToXYZ = [](const Vector2f& c) { return Vector3f{c.x() / c.y(), 1.0f, (1.0f - c.x() - c.y()) / c.y()}; };

    const auto lmsSrc = kBradford * xyToXYZ(srcWhite);
    const auto lmsDst = kBradford * xyToXYZ(dstWhite);

    const float a[3][3] = {
        {lmsDst[0] / lmsSrc[0], 0,                     0                    },
        {0,                     lmsDst[1] / lmsSrc[1], 0                    },
        {0,                     0,                     lmsDst[2] / lmsSrc[2]},
    };
    const auto aMat = toMatrix3(a);

    const auto b = aMat * kBradford;
    return kBradfordInv * b;
}

Matrix3f convertColorspaceMatrix(const std::array<Vector2f, 4>& srcChroma, const std::array<Vector2f, 4>& dstChroma, ERenderingIntent intent) {
    if (srcChroma == dstChroma) {
        return Matrix3f{1.0f};
    }

    switch (intent) {
        case ERenderingIntent::Saturation:
            tlog::warning() << "Saturation rendering intent is not supported; falling back to Perceptual.";
            [[fallthrough]];
        case ERenderingIntent::Perceptual:
        case ERenderingIntent::RelativeColorimetric:
            return xyzToRgb(dstChroma, 1) * adaptWhiteBradford(srcChroma[3], dstChroma[3]) * rgbToXyz(srcChroma, 1);
        case ERenderingIntent::AbsoluteColorimetric: return xyzToRgb(dstChroma, 1) * rgbToXyz(srcChroma, 1);
        default: throw std::invalid_argument("Invalid rendering intent");
    }
}

Matrix3f chromaToRec709Matrix(const std::array<Vector2f, 4>& chroma, ERenderingIntent intent) {
    return convertColorspaceMatrix(chroma, rec709Chroma(), intent);
}

Vector2f whiteD50() { return {0.34567f, 0.35850f}; }
Vector2f whiteD55() { return {0.33242f, 0.34743f}; }
Vector2f whiteD65() { return {0.31271f, 0.32902f}; }
Vector2f whiteD75() { return {0.29902f, 0.31485f}; }
Vector2f whiteD93() { return {0.28315f, 0.29711f}; }

Vector2f whiteA() { return {0.44758f, 0.40745f}; }
Vector2f whiteB() { return {0.34842f, 0.35161f}; }
Vector2f whiteC() { return {0.31006f, 0.31616f}; }

Vector2f whiteCenter() { return {0.333333f, 0.333333f}; }
Vector2f whiteDci() { return {0.314f, 0.351f}; }

array<Vector2f, 4> rec709Chroma() {
    return {
        {
         {0.6400f, 0.3300f},
         {0.3000f, 0.6000f},
         {0.1500f, 0.0600f},
         whiteD65(),
         }
    };
}

array<Vector2f, 4> adobeChroma() {
    return {
        {
         {0.6400f, 0.3300f},
         {0.2100f, 0.7100f},
         {0.1500f, 0.0600f},
         whiteD65(),
         }
    };
}

array<Vector2f, 4> proPhotoChroma() {
    return {
        {
         {0.734699f, 0.265301f},
         {0.159597f, 0.840403f},
         {0.036598f, 0.000105f},
         whiteD50(),
         }
    };
}

array<Vector2f, 4> displayP3Chroma() {
    return {
        {
         {0.6800f, 0.3200f},
         {0.2650f, 0.6900f},
         {0.1500f, 0.0600f},
         whiteD65(),
         }
    };
}

array<Vector2f, 4> dciP3Chroma() {
    return {
        {
         {0.6800f, 0.3200f},
         {0.2650f, 0.6900f},
         {0.1500f, 0.0600f},
         whiteDci(),
         }
    };
}

array<Vector2f, 4> bt2020Chroma() {
    return {
        {
         {0.7080f, 0.2920f},
         {0.1700f, 0.7970f},
         {0.1310f, 0.0460f},
         whiteD65(),
         }
    };
}

array<Vector2f, 4> bt2100Chroma() {
    return bt2020Chroma(); // BT.2100 uses the same primaries as BT.2020
}

string_view toString(EExifLightSource lightSource) {
    switch (lightSource) {
        case EExifLightSource::Unknown: return "unknown";
        case EExifLightSource::Daylight: return "daylight";
        case EExifLightSource::Fluorescent: return "fluorescent";
        case EExifLightSource::TungstenIncandescent: return "tungsten_incandescent";
        case EExifLightSource::Flash: return "flash";
        case EExifLightSource::FineWeather: return "fine_weather";
        case EExifLightSource::Cloudy: return "cloudy";
        case EExifLightSource::Shade: return "shade";
        case EExifLightSource::DaylightFluorescent: return "daylight_fluorescent";
        case EExifLightSource::DayWhiteFluorescent: return "day_white_fluorescent";
        case EExifLightSource::CoolWhiteFluorescent: return "cool_white_fluorescent";
        case EExifLightSource::WhiteFluorescent: return "white_fluorescent";
        case EExifLightSource::WarmWhiteFluorescent: return "warm_white_fluorescent";
        case EExifLightSource::StandardLightA: return "standard_a";
        case EExifLightSource::StandardLightB: return "standard_b";
        case EExifLightSource::StandardLightC: return "standard_c";
        case EExifLightSource::D55: return "D55";
        case EExifLightSource::D65: return "D65";
        case EExifLightSource::D75: return "D75";
        case EExifLightSource::D50: return "D50";
        case EExifLightSource::ISOStudioTungsten: return "iso_studio_tungsten";
        case EExifLightSource::Other: return "other";
    }

    throw invalid_argument{"Unknown EXIF light source."};
}

nanogui::Vector2f xy(EExifLightSource lightSource) {
    switch (lightSource) {
        case EExifLightSource::Unknown: return {0.0f, 0.0f};
        case EExifLightSource::Daylight: return {0.31292f, 0.32933f};
        case EExifLightSource::Fluorescent: return {0.37417f, 0.37281f};
        case EExifLightSource::TungstenIncandescent: return {0.44758f, 0.40745f};
        case EExifLightSource::Flash: return {0.0f, 0.0f};
        case EExifLightSource::FineWeather: return {0.0f, 0.0f};
        case EExifLightSource::Cloudy: return {0.0f, 0.0f};
        case EExifLightSource::Shade: return {0.0f, 0.0f};
        case EExifLightSource::DaylightFluorescent: return {0.31310f, 0.33710f};
        case EExifLightSource::DayWhiteFluorescent: return {0.31379f, 0.34531f};
        case EExifLightSource::CoolWhiteFluorescent: return {0.37208f, 0.37529f};
        case EExifLightSource::WhiteFluorescent: return {0.40910f, 0.39430f};
        case EExifLightSource::WarmWhiteFluorescent: return {0.44018f, 0.40329f};
        case EExifLightSource::StandardLightA: return whiteA();
        case EExifLightSource::StandardLightB: return whiteB();
        case EExifLightSource::StandardLightC: return whiteC();
        case EExifLightSource::D55: return whiteD55();
        case EExifLightSource::D65: return whiteD65();
        case EExifLightSource::D75: return whiteD75();
        case EExifLightSource::D50: return whiteD50();
        case EExifLightSource::ISOStudioTungsten: return {0.430944089109761f, 0.403585442674295f};
        case EExifLightSource::Other: return {0.0f, 0.0f};
        default: throw invalid_argument{"Invalid EExifLightSource value."};
    }
}

array<Vector2f, 4> chromaFromWpPrimaries(int wpPrimaries) {
    if (wpPrimaries == 10) {
        // Special case for Adobe RGB (1998) primaries, which is not in the H.273 spec
        return adobeChroma();
    }

    return ituth273::chroma(ituth273::fromWpPrimaries(wpPrimaries));
}

string_view wpPrimariesToString(int wpPrimaries) {
    if (wpPrimaries == 10) {
        // Special case for Adobe RGB (1998) primaries, which is not in the H.273 spec
        return "adobe_rgb";
    }

    return ituth273::toString(ituth273::fromWpPrimaries(wpPrimaries));
}

// Partial implementation of https://www.itu.int/rec/T-REC-H.273-202407-I/en (no YCbCr conversion)
namespace ituth273 {

string_view toString(const EColorPrimaries primaries) {
    switch (primaries) {
        case EColorPrimaries::BT709: return "bt709";
        case EColorPrimaries::Unspecified: return "unspecified";
        case EColorPrimaries::BT470M: return "bt470m";
        case EColorPrimaries::BT470BG: return "bt470bg";
        case EColorPrimaries::SMPTE170M: return "smpte170m";
        case EColorPrimaries::SMPTE240M: return "smpte240m";
        case EColorPrimaries::Film: return "film";
        case EColorPrimaries::BT2020: return "bt2020";
        case EColorPrimaries::SMPTE428: return "smpte428";
        case EColorPrimaries::SMPTE431: return "smpte431";
        case EColorPrimaries::SMPTE432: return "smpte432";
        case EColorPrimaries::Weird: return "weird";
    }

    return "invalid";
}

array<Vector2f, 4> chroma(const EColorPrimaries primaries) {
    switch (primaries) {
        default: tlog::warning() << fmt::format("Unknown color primaries {}. Using Rec.709 chroma.", (int)primaries); return rec709Chroma();
        case EColorPrimaries::BT709: return rec709Chroma();
        case EColorPrimaries::Unspecified: tlog::warning() << "Unspecified color primaries. Using Rec.709 chroma."; return rec709Chroma();
        case EColorPrimaries::BT470M:
            return {
                {
                 {0.6700f, 0.3300f},
                 {0.2100f, 0.7100f},
                 {0.1400f, 0.0800f},
                 whiteC(),
                 }
            };
        case EColorPrimaries::BT470BG:
            return {
                {
                 {0.6400f, 0.3300f},
                 {0.2900f, 0.6000f},
                 {0.1500f, 0.0600f},
                 whiteD65(),
                 }
            };
        case EColorPrimaries::SMPTE170M:
        case EColorPrimaries::SMPTE240M:
            return {
                {
                 {0.6300f, 0.3400f},
                 {0.3100f, 0.5950f},
                 {0.1550f, 0.0700f},
                 whiteD65(),
                 }
            };
        case EColorPrimaries::Film:
            return {
                {
                 {0.6810f, 0.3190f}, // Wratten 25
                    {0.2430f, 0.6920f}, // Wratten 58
                    {0.1450f, 0.0490f}, // Wratten 47
                    whiteC(),
                 }
            };
        case EColorPrimaries::BT2020: return bt2020Chroma();
        case EColorPrimaries::SMPTE428:
            return {
                {
                 {1.0f, 0.0f},
                 {0.0f, 1.0f},
                 {0.0f, 0.0f},
                 whiteCenter(),
                 }
            };
        case EColorPrimaries::SMPTE431: return dciP3Chroma();
        case EColorPrimaries::SMPTE432: return displayP3Chroma();
        case EColorPrimaries::Weird:
            return {
                {
                 {0.6300f, 0.3400f},
                 {0.2950f, 0.6050f},
                 {0.1550f, 0.0770f},
                 whiteD65(),
                 }
            };
    }

    return rec709Chroma(); // Fallback to Rec.709 if unknown
}

string_view toString(const ETransferCharacteristics transfer) {
    switch (transfer) {
        case ETransferCharacteristics::BT709: return "bt709";
        case ETransferCharacteristics::Unspecified: return "unspecified";
        case ETransferCharacteristics::Gamma22: return "gamma22";
        case ETransferCharacteristics::Gamma28: return "gamma28";
        case ETransferCharacteristics::BT601: return "bt601";
        case ETransferCharacteristics::SMPTE240: return "smpte240";
        case ETransferCharacteristics::Linear: return "linear";
        case ETransferCharacteristics::Log100: return "log100";
        case ETransferCharacteristics::Log100Sqrt10: return "log100_sqrt10";
        case ETransferCharacteristics::IEC61966_2_4: return "iec61966";
        case ETransferCharacteristics::BT1361Extended: return "bt1361_extended";
        case ETransferCharacteristics::SRGB: return "srgb";
        case ETransferCharacteristics::BT202010bit: return "bt2020_10bit";
        case ETransferCharacteristics::BT202012bit: return "bt2020_12bit";
        case ETransferCharacteristics::PQ: return "pq";
        case ETransferCharacteristics::SMPTE428: return "smpte428";
        case ETransferCharacteristics::HLG: return "hlg";
    }

    return "invalid";
}

EColorPrimaries fromWpPrimaries(int wpPrimaries) {
    switch (wpPrimaries) {
        case 1: return ituth273::EColorPrimaries::BT709;
        case 2: return ituth273::EColorPrimaries::BT470M;
        case 3: return ituth273::EColorPrimaries::BT470BG;
        case 4: return ituth273::EColorPrimaries::SMPTE170M;
        case 5: return ituth273::EColorPrimaries::Film;
        case 6: return ituth273::EColorPrimaries::BT2020;
        case 7: return ituth273::EColorPrimaries::SMPTE428;
        case 8: return ituth273::EColorPrimaries::SMPTE431;
        case 9: return ituth273::EColorPrimaries::SMPTE432;
    }

    throw std::invalid_argument{"Unknown wp color primaries: " + std::to_string(wpPrimaries)};
}

bool isTransferImplemented(const ETransferCharacteristics transfer) {
    switch (transfer) {
        case ETransferCharacteristics::BT709:
        case ETransferCharacteristics::BT601:
        case ETransferCharacteristics::BT202010bit:
        case ETransferCharacteristics::BT202012bit:
        case ETransferCharacteristics::IEC61966_2_4: // handles negative values by mirroring
        case ETransferCharacteristics::BT1361Extended: // extended to negative values (weirdly)
        case ETransferCharacteristics::Gamma22:
        case ETransferCharacteristics::Gamma28:
        case ETransferCharacteristics::SMPTE240:
        case ETransferCharacteristics::Linear:
        case ETransferCharacteristics::Log100:
        case ETransferCharacteristics::Log100Sqrt10:
        case ETransferCharacteristics::SRGB:
        case ETransferCharacteristics::PQ:
        case ETransferCharacteristics::SMPTE428:
        case ETransferCharacteristics::HLG:
        case ETransferCharacteristics::Unspecified: return true;
    }

    return false;
}

ETransferCharacteristics fromWpTransfer(int wpTransfer) {
    switch (wpTransfer) {
        case 1: return ETransferCharacteristics::BT709;
        case 2: return ETransferCharacteristics::Gamma22;
        case 3: return ETransferCharacteristics::Gamma28;
        case 4: return ETransferCharacteristics::SMPTE240;
        case 5: return ETransferCharacteristics::Linear;
        case 6: return ETransferCharacteristics::Log100;
        case 7: return ETransferCharacteristics::Log100Sqrt10;
        case 8: return ETransferCharacteristics::IEC61966_2_4;
        case 9: return ETransferCharacteristics::SRGB;
        case 10: return ETransferCharacteristics::SRGB; // TODO: handle the fact that this is the extended SRGB variant
        case 11: return ETransferCharacteristics::PQ;
        case 12: return ETransferCharacteristics::SMPTE428;
        case 13: return ETransferCharacteristics::HLG;
    }

    throw std::invalid_argument{"Unknown transfer characteristics from color manager: " + std::to_string(wpTransfer)};
}

} // namespace ituth273

class GlobalCmsContext {
public:
    GlobalCmsContext() {
        cmsSetLogErrorHandler([](cmsContext, cmsUInt32Number errorCode, const char* message) {
            tlog::error() << fmt::format("lcms error #{}: {}", errorCode, message);
        });
    }
};

class CmsContext {
public:
    CmsContext(const CmsContext&) = delete;
    CmsContext& operator=(const CmsContext&) = delete;
    CmsContext(CmsContext&&) = delete;
    CmsContext& operator=(CmsContext&&) = delete;

    static const CmsContext& threadLocal() {
        static thread_local CmsContext threadCtx;
        return threadCtx;
    }

    cmsContext get() const { return mCtx; }

    cmsHPROFILE rec709Profile() const { return mRec709Profile; }
    cmsHPROFILE grayProfile() const { return mGrayProfile; }

private:
    CmsContext() {
        static GlobalCmsContext globalCtx;

        mCtx = cmsCreateContext(cmsFastFloatExtensions(), nullptr);
        if (!mCtx) {
            throw runtime_error{"Failed to create LCMS context."};
        }

        cmsCIExyY D65 = {0.3127, 0.3290, 1.0};
        cmsCIExyYTRIPLE Rec709Primaries = {
            {0.6400, 0.3300, 1.0},
            {0.3000, 0.6000, 1.0},
            {0.1500, 0.0600, 1.0}
        };

        cmsToneCurve* linearCurve[3];
        linearCurve[0] = linearCurve[1] = linearCurve[2] = cmsBuildGamma(mCtx, 1.0f);

        mRec709Profile = cmsCreateRGBProfileTHR(mCtx, &D65, &Rec709Primaries, linearCurve);
        if (!mRec709Profile) {
            throw runtime_error{"Failed to create Rec.709 color profile."};
        }

        cmsToneCurve* grayCurve = cmsBuildGamma(mCtx, 1.0f);
        mGrayProfile = cmsCreateGrayProfileTHR(mCtx, &D65, grayCurve);
        if (!mGrayProfile) {
            throw runtime_error{"Failed to create gray color profile."};
        }
    }

    ~CmsContext() {
        if (mGrayProfile) {
            cmsCloseProfile(mGrayProfile);
        }

        if (mRec709Profile) {
            cmsCloseProfile(mRec709Profile);
        }

        if (mCtx) {
            cmsDeleteContext(mCtx);
        }
    }

    cmsContext mCtx = nullptr;
    cmsHPROFILE mRec709Profile = nullptr;
    cmsHPROFILE mGrayProfile = nullptr;
};

ColorProfile::~ColorProfile() {
    if (mProfile) {
        cmsCloseProfile(mProfile);
    }
}

optional<ColorProfile::CICP> ColorProfile::cicp() const {
    const auto* cicp = (const cmsVideoSignalType*)cmsReadTag(get(), cmsSigcicpTag);
    if (!cicp) {
        return nullopt;
    }

    return ColorProfile::CICP{
        .primaries = (ituth273::EColorPrimaries)cicp->ColourPrimaries,
        .transfer = (ituth273::ETransferCharacteristics)cicp->TransferCharacteristics,
        .matrixCoeffs = cicp->MatrixCoefficients,
        .videoFullRangeFlag = cicp->VideoFullRangeFlag,
    };
}

ERenderingIntent ColorProfile::renderingIntent() const {
    const auto profile = get();
    auto intent = cmsGetHeaderRenderingIntent(profile);
    if (intent == INTENT_PERCEPTUAL) {
        // We *do* want to preserve whether the intent includes white point adaptation or not (i.e. whether the image is intended to contain
        // white point relative or white point absolute colors), but we do *not* want to squish to the sRGB gamut because tev is not bound to
        // the gamut in the first place (allows negative values and values larger than 1). Hence we treat Perceptual as Relative
        // Colorimetric and convert to floating point values (no gamut clipping).
        intent = INTENT_RELATIVE_COLORIMETRIC;
    }

    if (cmsIsIntentSupported(profile, intent, LCMS_USED_AS_INPUT)) {
        return (ERenderingIntent)intent;
    }

    if (cmsIsIntentSupported(profile, INTENT_RELATIVE_COLORIMETRIC, LCMS_USED_AS_INPUT)) {
        return ERenderingIntent::RelativeColorimetric;
    }

    if (cmsIsIntentSupported(profile, INTENT_PERCEPTUAL, LCMS_USED_AS_INPUT)) {
        return ERenderingIntent::Perceptual;
    }

    // Last resort — profile is probably broken or unsupported type
    return ERenderingIntent::RelativeColorimetric;
}

ColorProfile ColorProfile::fromIcc(const uint8_t* iccProfile, size_t iccProfileSize) {
    cmsHPROFILE srcProfile = cmsOpenProfileFromMemTHR(CmsContext::threadLocal().get(), iccProfile, (cmsUInt32Number)iccProfileSize);
    if (!srcProfile) {
        throw runtime_error{"Failed to create ICC profile from raw data."};
    }

    string desc = "<unnamed>";
    const size_t len = cmsGetProfileInfoUTF8(srcProfile, cmsInfoDescription, "en", "US", nullptr, 0);
    if (len > 0) {
        desc.resize(len);
        cmsGetProfileInfoUTF8(srcProfile, cmsInfoDescription, "en", "US", desc.data(), (cmsUInt32Number)desc.size());
    }

    tlog::debug() << fmt::format("Loaded ICC profile '{}'.", desc);

    return srcProfile;
}

Task<void> toLinearSrgbPremul(
    const ColorProfile& profile,
    const Vector2i& size,
    int numColorChannels,
    EAlphaKind alphaKind,
    EPixelFormat pixelFormat,
    uint8_t* __restrict src,
    float* __restrict rgbaDst,
    int numChannelsOut,
    int priority
) {
    const int numChannels = numColorChannels + (alphaKind != EAlphaKind::None ? 1 : 0);

    if (!profile.isValid()) {
        throw runtime_error{"Color profile must be valid."};
    }

    if (numColorChannels < 1 || numColorChannels > 4) {
        throw runtime_error{"Must have 1, 2, 3, or 4 color channels."};
    }

    if (alphaKind == EAlphaKind::PremultipliedNonlinear && pixelFormat != EPixelFormat::F32) {
        throw runtime_error{"Premultiplied nonlinear alpha is only supported for F32 pixel format."};
    }

    auto cicp = profile.cicp();
    if (cicp) {
        tlog::debug() << fmt::format("ICC profile has CICP tag. Attempting manual conversion.");
    }

    if (cicp && !ituth273::isTransferImplemented(cicp->transfer)) {
        tlog::warning() << fmt::format("Unsupported transfer '{}' in CICP tag. Falling back to LCMS2.", ituth273::toString(cicp->transfer));
        cicp = nullopt;
    }

    if (cicp && cicp->matrixCoeffs != 0) {
        tlog::warning() << fmt::format("Unsupported matrix coefficients in cICP chunk: {}. Falling back to LCMS2.", cicp->matrixCoeffs);
        cicp = nullopt;
    }

    if (cicp && pixelFormat != EPixelFormat::F32) {
        tlog::warning() << "Unsupported CICP color conversion from non-F32 pixel format. Falling back to LCMS2.";
        cicp = nullopt;
    }

    LimitedRange range = LimitedRange::full();
    Matrix3f toRec709 = Matrix3f{1.0f};

    const ERenderingIntent intent = profile.renderingIntent();
    if (cicp) {
        tlog::debug() << fmt::format(
            "CICP: primaries={} transfer={} coeffs={} fullRange={}",
            ituth273::toString(cicp->primaries),
            ituth273::toString(cicp->transfer),
            cicp->matrixCoeffs,
            cicp->videoFullRangeFlag == 1 ? "yes" : "no"
        );

        range = cicp->videoFullRangeFlag != 0 ? LimitedRange::full() : limitedRangeForBitsPerSample(nBits(pixelFormat));
        toRec709 = convertColorspaceMatrix(ituth273::chroma(cicp->primaries), rec709Chroma(), intent);
    }

    // Create transform from source profile to Rec.709
    cmsUInt32Number type = CHANNELS_SH(numColorChannels);

    size_t bytesPerSample = 0;
    switch (pixelFormat) {
        case EPixelFormat::U8:
            type |= BYTES_SH(1);
            bytesPerSample = 1;
            break;
        case EPixelFormat::U16:
            type |= BYTES_SH(2);
            bytesPerSample = 2;
            break;
        case EPixelFormat::F16:
            type |= BYTES_SH(2) | FLOAT_SH(1);
            bytesPerSample = 2;
            break;
        case EPixelFormat::F32:
            type |= BYTES_SH(4) | FLOAT_SH(1);
            bytesPerSample = 4;
            break;
        default: throw runtime_error{"Unsupported pixel format."};
    }

    if (pixelFormat != EPixelFormat::F32) {
        tlog::warning() << "Color conversion from non-F32 pixel format detected. This can be significantly slower than F32->F32.";
    }

    if (alphaKind != EAlphaKind::None) {
        type |= EXTRA_SH(1);
    }

    // We expressly *don't* tell lcms2 that the alpha channel is premultiplied, because it would divide the alpha channel prior to color
    // space conversion and inverse transfer function application. This would be bad for multiple reasons: first, EAlphaKind::Premultiplied
    // indicated premultiplication in linear space, and second, lcms2 does not handle the case of alpha close to 0 very accurately. This is
    // also the reason, why we manually unpremultiply in the case of EAlphaKind::PremultipliedNonlinear below rather than relying on lcms2.
    // if (alphaKind == EAlphaKind::Premultiplied) {
    //     type |= PREMUL_SH(1);
    // }

    // TODO: differentiate between single channel RGB and gray
    if (numColorChannels == 1) {
        type |= COLORSPACE_SH(PT_GRAY);
    } else if (numColorChannels == 4) {
        type |= COLORSPACE_SH(PT_CMYK);
    } else {
        type |= COLORSPACE_SH(PT_RGB);
    }

    cmsUInt32Number typeOut = 0;
    switch (numChannelsOut) {
        case 1: typeOut = TYPE_GRAY_FLT; break;
        case 2: typeOut = TYPE_GRAYA_FLT; break;
        case 3: typeOut = TYPE_RGB_FLT; break;
        case 4: typeOut = TYPE_RGBA_FLT; break;
        default: throw runtime_error{"Invalid number of output channels."};
    }

    const int numColorChannelsOut = numChannelsOut == 1 || numChannelsOut == 2 ? 1 : 3;
    const size_t numPixels = (size_t)size.x() * size.y();

    cmsHTRANSFORM transform = nullptr;
    if (!cicp) {
        // LCMS's fast float optimizations require a certain degree of precomputation which can be harmful for small images that would
        // convert quickly anyway. We'll disable optimizations arbitrarily for images with fewer than 512x512 pixels.
        const bool optimize = numPixels >= 512 * 512;

        tlog::debug() << fmt::format(
            "Creating LCMS color transform: numColorChannels={} alphaKind={} pixelFormat={} numChannels={} type={:#010x} -> numChannelsOut={} typeOut={:#010x} intent={} optimize={}",
            numColorChannels,
            (int)alphaKind,
            (int)pixelFormat,
            numChannels,
            type,
            numChannelsOut,
            typeOut,
            toString(intent),
            optimize
        );

        transform = cmsCreateTransformTHR(
            CmsContext::threadLocal().get(),
            profile.get(),
            type,
            numColorChannels == 1 && numColorChannelsOut == 1 ? CmsContext::threadLocal().grayProfile() :
                                                                CmsContext::threadLocal().rec709Profile(),
            // Always output in straight alpha. We would prefer to have the transform output in premultiplied alpha, but lcms2 throws an
            // error if we set this as the output type.
            typeOut,
            (cmsUInt32Number)intent,
            (optimize ? cmsFLAGS_HIGHRESPRECALC : cmsFLAGS_NOOPTIMIZE) | (alphaKind != EAlphaKind::None ? cmsFLAGS_COPY_ALPHA : 0)
        );

        if (!transform) {
            throw runtime_error{"Failed to create color transform to Rec.709."};
        }
    }

    const size_t nSrcSamplesPerRow = size.x() * numChannels;
    const size_t nDstSamplesPerRow = size.x() * numChannelsOut;

    const size_t numSamples = numPixels * numChannels;
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        size.y(),
        numSamples * 4, // arbitrary factor to reflect increased cost of color conversion
        [&](size_t y) {
            size_t srcOffset = y * nSrcSamplesPerRow * bytesPerSample;
            size_t dstOffset = y * nDstSamplesPerRow;

            uint8_t* __restrict srcPtr = &src[srcOffset];
            float* __restrict dstPtr = &rgbaDst[dstOffset];

            // If premultiplied alpha is in nonlinear space, we need to manually unpremultiply it before the color transform.
            if (alphaKind == EAlphaKind::PremultipliedNonlinear) {
                for (int x = 0; x < size.x(); ++x) {
                    const size_t baseIdx = x * numChannels;
                    const float alpha = *(float*)&srcPtr[(baseIdx + numColorChannels) * bytesPerSample];
                    const float factor = alpha == 0.0f ? 1.0f : 1.0f / alpha;
                    for (int c = 0; c < numChannels - 1; ++c) {
                        *(float*)&srcPtr[(baseIdx + c) * bytesPerSample] *= factor;
                    }
                }
            }

            if (cicp) {
                for (int x = 0; x < size.x(); ++x) {
                    const size_t baseIdxSrc = x * numChannels;
                    const size_t baseIdxDst = x * numChannelsOut;

                    Vector3f color;
                    for (int c = 0; c < numColorChannels; ++c) {
                        color[c] = (*(float*)&srcPtr[(baseIdxSrc + c) * bytesPerSample] - range.offset) * range.scale;
                    }

                    color = toRec709 * ituth273::invTransfer(cicp->transfer, color);
                    for (int c = 0; c < numColorChannelsOut; ++c) {
                        dstPtr[baseIdxDst + c] = color[c];
                    }

                    if (alphaKind != EAlphaKind::None) {
                        dstPtr[baseIdxDst + numColorChannelsOut] = *(float*)&srcPtr[(baseIdxSrc + numColorChannels) * bytesPerSample];
                    }
                }
            } else {
                // Armchair parallelization of lcms: cmsDoTransform is reentrant per the spec, i.e. it can be called from multiple threads.
                // So: call cmsDoTransform for each row in parallel.
                cmsDoTransform(transform, srcPtr, dstPtr, size.x());
            }

            // If we passed straight alpha data through lcms2, we need to multiply it by alpha again, hence we need to premultiply. If the
            // input image had non-linear premultiplied alpha, alpha==0 pixels were preserved as-is when converting to straight alpha, so we
            // also should not re-multiply those.
            if (alphaKind != EAlphaKind::None && alphaKind != EAlphaKind::Premultiplied) {
                for (int x = 0; x < size.x(); ++x) {
                    const size_t baseIdx = x * numChannelsOut;

                    float factor = dstPtr[baseIdx + numChannelsOut - 1];
                    if (factor == 0.0f && alphaKind == EAlphaKind::PremultipliedNonlinear) {
                        factor = 1.0f;
                    }

                    for (int c = 0; c < numChannelsOut - 1; ++c) {
                        dstPtr[baseIdx + c] *= factor;
                    }
                }
            }
        },
        priority
    );
}

LimitedRange limitedRangeForBitsPerSample(int bitsPerSample) {
    switch (bitsPerSample) {
        case 8: return {255.0f / 219.0f, 16.0f / 255.0f};
        case 10: return {1023.0f / 876.0f, 64.0f / 1023.0f};
        case 12: return {4095.0f / 3504.0f, 256.0f / 4095.0f};
    }

    tlog::warning() << fmt::format("Unsupported bits per sample {} with limited range flag.", bitsPerSample);
    return LimitedRange::full();
}

} // namespace tev
