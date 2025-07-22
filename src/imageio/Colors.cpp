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

#include <ImfChromaticities.h>

#include <lcms2.h>
#include <lcms2_fast_float.h>

using namespace std;
using namespace nanogui;

namespace tev {

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

Matrix4f transpose(const Imath::M44f& mat) {
    Matrix4f result;
    for (int m = 0; m < 4; ++m) {
        for (int n = 0; n < 4; ++n) {
            // No flipping of indices needed, because Imath::M44f is row-major while Matrix4f is column-major
            result.m[m][n] = mat.x[m][n];
        }
    }

    return result;
}

Matrix4f xyzToChromaMatrix(const std::array<Vector2f, 4>& chroma) {
    Imf::Chromaticities imfChroma = {
        {chroma[0].x(), chroma[0].y()},
        {chroma[1].x(), chroma[1].y()},
        {chroma[2].x(), chroma[2].y()},
        {chroma[3].x(), chroma[3].y()},
    };

    return transpose(Imf::XYZtoRGB(imfChroma, 1));
}

Matrix4f chromaToRec709Matrix(const std::array<Vector2f, 4>& chroma) {
    Imf::Chromaticities rec709; // default rec709 (sRGB) primaries
    Imf::Chromaticities imfChroma = {
        {chroma[0].x(), chroma[0].y()},
        {chroma[1].x(), chroma[1].y()},
        {chroma[2].x(), chroma[2].y()},
        {chroma[3].x(), chroma[3].y()},
    };

    // equality comparison for Imf::Chromaticities instances
    auto chromaEq = [](const Imf::Chromaticities& a, const Imf::Chromaticities& b) {
        return (a.red - b.red).length2() + (a.green - b.green).length2() + (a.blue - b.blue).length2() + (a.white - b.white).length2() <
            1e-6f;
    };

    if (chromaEq(imfChroma, rec709)) {
        return Matrix4f{1.0f};
    }

    return transpose(Imf::RGBtoXYZ(imfChroma, 1) * Imf::XYZtoRGB(rec709, 1));
}

Matrix4f xyzToRec709Matrix() {
    Imf::Chromaticities rec709; // default rec709 (sRGB) primaries
    return transpose(Imf::XYZtoRGB(rec709, 1));
}

Vector2f whiteD65() { return {0.31271f, 0.32902f}; }
Vector2f whiteCenter() { return {0.333333f, 0.333333f}; }
Vector2f whiteC() { return {0.310f, 0.316f}; }
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
         {0.345704f, 0.358540f},
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

// Adapted from LittleCMS's AdaptToXYZD50 function
Matrix3f adaptToXYZD50Bradford(const Vector2f& w) {
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

    const Vector3f white{w.x() / w.y(), 1.0f, (1.0f - w.x() - w.y()) / w.y()};
    const Vector3f white50{0.96422f, 1.0f, 0.82521f};

    const auto lms = kBradford * white;
    const auto lms50 = kBradford * white50;

    const float a[3][3] = {
        {lms50[0] / lms[0], 0,                 0                },
        {0,                 lms50[1] / lms[1], 0                },
        {0,                 0,                 lms50[2] / lms[2]},
    };
    const auto aMat = toMatrix3(a);

    const auto b = aMat * kBradford;
    return kBradfordInv * b;
}

Matrix4f toMatrix4(const Matrix3f& mat) {
    Matrix4f result{1.0f};
    for (int m = 0; m < 3; ++m) {
        for (int n = 0; n < 3; ++n) {
            result.m[m][n] = mat.m[m][n];
        }
    }

    return result;
}

Matrix3f toMatrix3(const Matrix4f& mat) {
    Matrix3f result;
    for (int m = 0; m < 3; ++m) {
        for (int n = 0; n < 3; ++n) {
            result.m[m][n] = mat.m[m][n];
        }
    }

    return result;
}

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
    static const CmsContext& threadLocal() {
        static thread_local CmsContext threadCtx;
        return threadCtx;
    }

    cmsContext get() const { return mCtx; }

    cmsHPROFILE rec709Profile() const { return mRec709Profile; }

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
    }

    ~CmsContext() {
        if (mRec709Profile) {
            cmsCloseProfile(mRec709Profile);
        }

        if (mCtx) {
            cmsDeleteContext(mCtx);
        }
    }

    cmsContext mCtx = nullptr;
    cmsHPROFILE mRec709Profile = nullptr;
};

ColorProfile::~ColorProfile() {
    if (mProfile) {
        cmsCloseProfile(mProfile);
    }
}

ColorProfile ColorProfile::fromIcc(const uint8_t* iccProfile, size_t iccProfileSize) {
    cmsHPROFILE srcProfile = cmsOpenProfileFromMemTHR(CmsContext::threadLocal().get(), iccProfile, (cmsUInt32Number)iccProfileSize);
    if (!srcProfile) {
        throw runtime_error{"Failed to create ICC profile from raw data."};
    }

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
    int priority
) {
    if (!profile.isValid()) {
        throw runtime_error{"Color profile must be valid."};
    }

    if (numColorChannels < 1 || numColorChannels > 4) {
        throw runtime_error{"Must have 1, 2, 3, or 4 color channels."};
    }

    if (alphaKind == EAlphaKind::PremultipliedNonlinear && pixelFormat != EPixelFormat::F32) {
        throw runtime_error{"Premultiplied nonlinear alpha is only supported for F32 pixel format."};
    }

    // Create transform from source profile to Rec.709
    int numChannels = numColorChannels + (alphaKind != EAlphaKind::None ? 1 : 0);
    cmsUInt32Number type = CHANNELS_SH(numColorChannels);

    size_t bytesPerPixel = 0;
    switch (pixelFormat) {
        case EPixelFormat::U8:
            type |= BYTES_SH(1);
            bytesPerPixel = 1;
            break;
        case EPixelFormat::U16:
            type |= BYTES_SH(2);
            bytesPerPixel = 2;
            break;
        case EPixelFormat::F16:
            type |= BYTES_SH(2) | FLOAT_SH(1);
            bytesPerPixel = 2;
            break;
        case EPixelFormat::F32:
            type |= BYTES_SH(4) | FLOAT_SH(1);
            bytesPerPixel = 4;
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

    cmsHTRANSFORM transform = cmsCreateTransformTHR(
        CmsContext::threadLocal().get(),
        profile.get(),
        type,
        CmsContext::threadLocal().rec709Profile(),
        // Always output in straight alpha. We would prefer to have the transform output in premultiplied alpha, but lcms2 throws an error
        // if we set this as the output type.
        TYPE_RGBA_FLT,
        // Since tev's intent is to inspect images, we want to be as color-accurate as possible rather than perceptually pleasing. Hence the
        // following rendering intent.
        INTENT_RELATIVE_COLORIMETRIC,
        alphaKind != EAlphaKind::None ? cmsFLAGS_COPY_ALPHA : 0
    );

    if (!transform) {
        throw runtime_error{"Failed to create color transform to Rec.709."};
    }

    const size_t nSamplesPerRow = size.x() * numChannels;
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        size.y(),
        [&](size_t y) {
            size_t srcOffset = y * nSamplesPerRow * bytesPerPixel;
            size_t dstOffset = y * (size_t)size.x() * 4;

            uint8_t* __restrict srcPtr = &src[srcOffset];
            float* __restrict dstPtr = &rgbaDst[dstOffset];

            // If premultiplied alpha is in nonlinear space, we need to manually unpremultiply it before the color transform.
            if (alphaKind == EAlphaKind::PremultipliedNonlinear) {
                for (int x = 0; x < size.x(); ++x) {
                    const size_t baseIdx = x * numChannels;
                    const float alpha = *(float*)&srcPtr[(baseIdx + numColorChannels) * bytesPerPixel];
                    const float factor = alpha == 0.0f ? 1.0f : 1.0f / alpha;
                    for (int c = 0; c < numChannels - 1; ++c) {
                        *(float*)&srcPtr[(baseIdx + c) * bytesPerPixel] *= factor;
                    }
                }
            }

            // Armchair parallelization of lcms: cmsDoTransform is reentrant per the spec, i.e. it can be called from multiple threads. So:
            // call cmsDoTransform for each row in parallel.
            cmsDoTransform(transform, srcPtr, dstPtr, size.x());

            // If we passed straight alpha data through lcms2, we need to multiply it by alpha again, hence we need to premultiply. If the
            // input image had non-linear premultiplied alpha, alpha==0 pixels were preserved as-is when converting to straight alpha, so we
            // also should not re-multiply those.
            if (alphaKind != EAlphaKind::None && alphaKind != EAlphaKind::Premultiplied) {
                for (int x = 0; x < size.x(); ++x) {
                    const size_t baseIdx = x * 4;
                    float factor = dstPtr[baseIdx + 3];
                    if (factor == 0.0f && alphaKind == EAlphaKind::PremultipliedNonlinear) {
                        factor = 1.0f;
                    }

                    for (int c = 0; c < 3; ++c) {
                        dstPtr[baseIdx + c] *= factor;
                    }
                }
            }
        },
        priority
    );
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
        case ETransferCharacteristics::BT470M: return "bt470m";
        case ETransferCharacteristics::BT470BG: return "bt470bg";
        case ETransferCharacteristics::BT601: return "bt601";
        case ETransferCharacteristics::SMPTE240: return "smpte240";
        case ETransferCharacteristics::Linear: return "linear";
        case ETransferCharacteristics::Log100: return "log100";
        case ETransferCharacteristics::Log100Sqrt10: return "log100_sqrt10";
        case ETransferCharacteristics::IEC61966: return "iec61966";
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
        case ETransferCharacteristics::IEC61966: // handles negative values by mirroring
        case ETransferCharacteristics::BT1361Extended: // extended to negative values (weirdly)
        case ETransferCharacteristics::BT470M:
        case ETransferCharacteristics::BT470BG:
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
        case 2: return ETransferCharacteristics::BT470M;
        case 3: return ETransferCharacteristics::BT470BG;
        case 4: return ETransferCharacteristics::SMPTE240;
        case 5: return ETransferCharacteristics::Linear;
        case 6: return ETransferCharacteristics::Log100;
        case 7: return ETransferCharacteristics::Log100Sqrt10;
        case 8: return ETransferCharacteristics::IEC61966;
        case 9: return ETransferCharacteristics::SRGB;
        case 10: return ETransferCharacteristics::SRGB; // TODO: handle the fact that this is the extended SRGB variant
        case 11: return ETransferCharacteristics::PQ;
        case 12: return ETransferCharacteristics::SMPTE428;
        case 13: return ETransferCharacteristics::HLG;
    }

    throw std::invalid_argument{"Unknown transfer characteristics from color manager: " + std::to_string(wpTransfer)};
}

} // namespace ituth273

LimitedRange limitedRangeForBitsPerPixel(int bitsPerPixel) {
    switch (bitsPerPixel) {
        case 8: return {255.0f / 219.0f, 16.0f / 255.0f};
        case 10: return {1023.0f / 876.0f, 64.0f / 1023.0f};
        case 12: return {4095.0f / 3504.0f, 256.0f / 4095.0f};
    }

    tlog::warning() << fmt::format("Unsupported bits per pixel {} with limited range flag.", bitsPerPixel);
    return {1.0f, 0.0f};
}

} // namespace tev
