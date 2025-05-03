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

#include <tev/Common.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/Chroma.h>

#include <nanogui/vector.h>

#include <ImfChromaticities.h>

#include <lcms2.h>
#include <lcms2_fast_float.h>

using namespace std;
using namespace nanogui;

namespace tev {

nanogui::Matrix4f convertChromaToRec709(std::array<nanogui::Vector2f, 4> chroma) {
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
        return nanogui::Matrix4f{1.0f};
    }

    Imath::M44f M = Imf::RGBtoXYZ(imfChroma, 1) * Imf::XYZtoRGB(rec709, 1);

    nanogui::Matrix4f toRec709;
    for (int m = 0; m < 4; ++m) {
        for (int n = 0; n < 4; ++n) {
            toRec709.m[m][n] = M.x[m][n];
        }
    }

    return toRec709;
}

struct GlobalCmsContext {
    GlobalCmsContext() {
        cmsSetLogErrorHandler([](cmsContext, cmsUInt32Number errorCode, const char* message) {
            tlog::error() << fmt::format("lcms error #{}: {}", errorCode, message);
        });
    }
};

struct CmsContext {
    CmsContext() {
        static GlobalCmsContext globalCtx;

        ctx = cmsCreateContext(cmsFastFloatExtensions(), nullptr);
        if (!ctx) {
            throw runtime_error{"Failed to create LCMS context."};
        }

        cmsCIExyY D65 = {0.3127, 0.3290, 1.0};
        cmsCIExyYTRIPLE Rec709Primaries = {
            {0.6400, 0.3300, 1.0},
            {0.3000, 0.6000, 1.0},
            {0.1500, 0.0600, 1.0}
        };

        cmsToneCurve* linearCurve[3];
        linearCurve[0] = linearCurve[1] = linearCurve[2] = cmsBuildGamma(ctx, 1.0f);

        rec709Profile = cmsCreateRGBProfileTHR(ctx, &D65, &Rec709Primaries, linearCurve);
        if (!rec709Profile) {
            throw runtime_error{"Failed to create Rec.709 color profile."};
        }
    }

    ~CmsContext() {
        if (rec709Profile) {
            cmsCloseProfile(rec709Profile);
        }

        if (ctx) {
            cmsDeleteContext(ctx);
        }
    }

    cmsContext ctx = nullptr;
    cmsHPROFILE rec709Profile = nullptr;
};

Task<void> convertIccToLinearSrgbPremultiplied(
    const vector<uint8_t>& iccProfile,
    const Vector2i& size,
    int numColorChannels,
    EAlphaKind alphaKind,
    float* __restrict src,
    float* __restrict rgbaDst,
    int priority
) {
    if (numColorChannels < 1 || numColorChannels > 4) {
        throw runtime_error{"Must have 1, 2, 3, or 4 color channels."};
    }

    static thread_local CmsContext threadCtx;

    cmsHPROFILE srcProfile = cmsOpenProfileFromMemTHR(threadCtx.ctx, iccProfile.data(), (cmsUInt32Number)iccProfile.size());
    if (!srcProfile) {
        throw runtime_error{"Failed to create ICC profile from raw data."};
    }

    ScopeGuard srcProfileGuard{[srcProfile] { cmsCloseProfile(srcProfile); }};

    // Create transform from source profile to Rec.709
    int numChannels = numColorChannels + (alphaKind != EAlphaKind::None ? 1 : 0);
    cmsUInt32Number type = FLOAT_SH(1) | CHANNELS_SH(numColorChannels) | BYTES_SH(4);

    if (alphaKind != EAlphaKind::None) {
        type |= EXTRA_SH(1);
    }

    // lcms2 will internally unmultiply linear-space premultiplied alpha after undoing the transfer function (preserving colors of alpha==0
    // pixels as-is). But in the case of non-linear premultiplied alpha, we have to manually unmultiply before the transform and initialize
    // the transform for straight alpha.
    if (alphaKind == EAlphaKind::Premultiplied) {
        type |= PREMUL_SH(1);
    }

    // TODO: differentiate between single channel RGB and gray
    if (numColorChannels == 1) {
        type |= COLORSPACE_SH(PT_GRAY);
    } else if (numColorChannels == 4) {
        type |= COLORSPACE_SH(PT_CMYK);
    } else {
        type |= COLORSPACE_SH(PT_RGB);
    }

    cmsHTRANSFORM transform = cmsCreateTransformTHR(
        threadCtx.ctx,
        srcProfile,
        type,
        threadCtx.rec709Profile,
        // Always output in straight alpha. We would prefer to have the transform output in premultiplied alpha, but lcms2 throws an error
        // if we set this as the output type.
        TYPE_RGBA_FLT,
        // Since tev's intent is to inspect images, we want to be as color-accurate as possible rather than perceptually pleasing. Hence the
        // following rendering intent.
        INTENT_RELATIVE_COLORIMETRIC,
        alphaKind != EAlphaKind::None ? cmsFLAGS_COPY_ALPHA : 0
    );

    if (!transform) {
        throw runtime_error{"Failed to create color transform from ICC profile to Rec.709."};
    }

    tlog::debug() << "Applying ICC color profile.";

    const size_t nSamplesPerRow = size.x() * numChannels;
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        size.y(),
        [&](size_t y) {
            size_t srcOffset = y * nSamplesPerRow;

            // If premultiplied alpha is in nonlinear space, we need to manually unpremultiply it before the color transform.
            if (alphaKind == EAlphaKind::PremultipliedNonlinear) {
                for (int x = 0; x < size.x(); ++x) {
                    const size_t baseIdx = srcOffset + x * numChannels;
                    const float alpha = src[baseIdx + numColorChannels];
                    const float factor = alpha == 0.0f ? 1.0f : 1.0f / alpha;
                    for (int c = 0; c < numChannels - 1; ++c) {
                        src[baseIdx + c] *= factor;
                    }
                }
            }

            // Armchair parallelization of lcms: cmsDoTransform is reentrant per the spec, i.e. it can be called from multiple threads. So:
            // call cmsDoTransform for each row in parallel.
            size_t dstOffset = y * (size_t)size.x() * 4;
            cmsDoTransform(transform, &src[srcOffset], &rgbaDst[dstOffset], size.x());

            // The output of the cms transform is always in straight alpha, hence we need to premultiply. If the input image had
            // premultiplied alpha, alpha==0 pixels were preserved as-is, when converting to straight alpha, so we also should not
            // re-multiply those.
            if (alphaKind != EAlphaKind::None) {
                for (int x = 0; x < size.x(); ++x) {
                    const size_t baseIdx = dstOffset + x * 4;
                    float factor = rgbaDst[baseIdx + 3];
                    if (factor == 0.0f && (alphaKind == EAlphaKind::PremultipliedNonlinear || alphaKind == EAlphaKind::Premultiplied)) {
                        factor = 1.0f;
                    }

                    for (int c = 0; c < 3; ++c) {
                        rgbaDst[baseIdx + c] *= factor;
                    }
                }
            }
        },
        priority
    );
}

} // namespace tev
