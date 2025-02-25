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

#include <tev/ThreadPool.h>
#include <tev/imageio/HeifImageLoader.h>

#include <libheif/heif.h>

#include <lcms2.h>
// #include <lcms2_fast_float.h>

#include <ImfChromaticities.h>

using namespace nanogui;
using namespace std;

namespace tev {

HeifImageLoader::HeifImageLoader() {
    cmsSetLogErrorHandler([](cmsContext, cmsUInt32Number errorCode, const char* message) {
        tlog::error() << fmt::format("lcms error #{}: {}", errorCode, message);
    });

    // cmsPlugin(cmsFastFloatExtensions());
}

bool HeifImageLoader::canLoadFile(istream& iStream) const {
    // libheif's spec says it needs the first 12 bytes to determine whether the image can be read.
    uint8_t header[12];
    iStream.read((char*)header, 12);
    bool failed = !iStream || iStream.gcount() != 12;

    iStream.clear();
    iStream.seekg(0);
    if (failed) {
        return false;
    }

    return heif_check_filetype(header, 12) == heif_filetype_yes_supported;
}

Task<vector<ImageData>> HeifImageLoader::load(istream& iStream, const fs::path&, const string&, int priority) const {
    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    iStream.seekg(0, ios_base::end);
    int64_t fileSize = iStream.tellg();
    iStream.clear();
    iStream.seekg(0);

    struct ReaderContext {
        istream& stream;
        int64_t size;
    } readerContext = {iStream, fileSize};

    static const heif_reader reader = {
        .reader_api_version = 1,
        .get_position = [](void* context) { return (int64_t)reinterpret_cast<ReaderContext*>(context)->stream.tellg(); },
        .read =
            [](void* data, size_t size, void* context) {
                auto& stream = reinterpret_cast<ReaderContext*>(context)->stream;
                stream.read((char*)data, size);
                return stream.good() ? 0 : -1;
            },
        .seek =
            [](int64_t pos, void* context) {
                auto& stream = reinterpret_cast<ReaderContext*>(context)->stream;
                stream.seekg(pos);
                return stream.good() ? 0 : -1;
            },
        .wait_for_file_size =
            [](int64_t target_size, void* context) {
                return reinterpret_cast<ReaderContext*>(context)->size < target_size ? heif_reader_grow_status_size_beyond_eof :
                                                                                       heif_reader_grow_status_size_reached;
            },
        // Not used by API version 1
        .request_range = {},
        .preload_range_hint = {},
        .release_file_range = {},
        .release_error_msg = {},
    };

    heif_context* ctx = heif_context_alloc();
    if (!ctx) {
        throw invalid_argument{"Failed to allocate libheif context."};
    }

    ScopeGuard contextGuard{[ctx] { heif_context_free(ctx); }};

    if (auto error = heif_context_read_from_reader(ctx, &reader, &readerContext, nullptr); error.code != heif_error_Ok) {
        throw invalid_argument{fmt::format("Failed to read image: {}", error.message)};
    }

    // get a handle to the primary image
    heif_image_handle* handle;
    if (auto error = heif_context_get_primary_image_handle(ctx, &handle); error.code != heif_error_Ok) {
        throw invalid_argument{fmt::format("Failed to get primary image handle: {}", error.message)};
    }

    ScopeGuard handleGuard{[handle] { heif_image_handle_release(handle); }};

    int numChannels = heif_image_handle_has_alpha_channel(handle) ? 4 : 3;
    bool hasPremultipliedAlpha = numChannels == 4 && heif_image_handle_is_premultiplied_alpha(handle);

    const bool is_little_endian = std::endian::native == std::endian::little;
    auto format = numChannels == 4 ? (is_little_endian ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RRGGBBAA_BE) :
                                     (is_little_endian ? heif_chroma_interleaved_RRGGBB_LE : heif_chroma_interleaved_RRGGBB_BE);

    Vector2i size = {heif_image_handle_get_width(handle), heif_image_handle_get_height(handle)};

    if (size.x() == 0 || size.y() == 0) {
        throw invalid_argument{"Image has zero pixels."};
    }

    heif_image* img;
    if (auto error = heif_decode_image(handle, &img, heif_colorspace_RGB, format, nullptr); error.code != heif_error_Ok) {
        throw invalid_argument{fmt::format("Failed to decode image: {}", error.message)};
    }

    ScopeGuard imgGuard{[img] { heif_image_release(img); }};

    const int bitsPerPixel = heif_image_get_bits_per_pixel_range(img, heif_channel_interleaved);
    const float channelScale = 1.0f / float((1 << bitsPerPixel) - 1);

    int bytesPerLine;
    const uint8_t* data = heif_image_get_plane_readonly(img, heif_channel_interleaved, &bytesPerLine);
    if (!data) {
        throw invalid_argument{"Faild to get image data."};
    }

    auto getCmsTransform = [&]() {
        size_t profileSize = heif_image_handle_get_raw_color_profile_size(handle);
        if (profileSize == 0) {
            return (cmsHTRANSFORM) nullptr;
        }

        vector<uint8_t> profileData(profileSize);
        if (auto error = heif_image_handle_get_raw_color_profile(handle, profileData.data()); error.code != heif_error_Ok) {
            if (error.code == heif_error_Color_profile_does_not_exist) {
                return (cmsHTRANSFORM) nullptr;
            }

            tlog::warning() << "Failed to read ICC profile: " << error.message;
            return (cmsHTRANSFORM) nullptr;
        }

        // Create ICC profile from the raw data
        cmsHPROFILE srcProfile = cmsOpenProfileFromMem(profileData.data(), (cmsUInt32Number)profileSize);
        if (!srcProfile) {
            tlog::warning() << "Failed to create ICC profile from raw data";
            return (cmsHTRANSFORM) nullptr;
        }

        ScopeGuard srcProfileGuard{[srcProfile] { cmsCloseProfile(srcProfile); }};

        cmsCIExyY D65 = {0.3127, 0.3290, 1.0};
        cmsCIExyYTRIPLE Rec709Primaries = {
            {0.6400, 0.3300, 1.0},
            {0.3000, 0.6000, 1.0},
            {0.1500, 0.0600, 1.0}
        };

        cmsToneCurve* linearCurve[3];
        linearCurve[0] = linearCurve[1] = linearCurve[2] = cmsBuildGamma(0, 1.0f);

        cmsHPROFILE rec709Profile = cmsCreateRGBProfile(&D65, &Rec709Primaries, linearCurve);

        if (!rec709Profile) {
            tlog::warning() << "Failed to create Rec.709 color profile";
            return (cmsHTRANSFORM) nullptr;
        }

        ScopeGuard rec709ProfileGuard{[rec709Profile] { cmsCloseProfile(rec709Profile); }};

        // Create transform from source profile to Rec.709
        auto type = numChannels == 4 ? (hasPremultipliedAlpha ? TYPE_RGBA_FLT_PREMUL : TYPE_RGBA_FLT) : TYPE_RGB_FLT;
        cmsHTRANSFORM transform =
            cmsCreateTransform(srcProfile, type, rec709Profile, TYPE_RGBA_FLT, INTENT_PERCEPTUAL, cmsFLAGS_NOCACHE);

        if (!transform) {
            tlog::warning() << "Failed to create color transform from ICC profile to Rec.709";
            return (cmsHTRANSFORM) nullptr;
        }

        return transform;
    };

    resultData.channels = makeNChannels(numChannels, size);
    resultData.hasPremultipliedAlpha = hasPremultipliedAlpha;

    // If we've got an ICC color profile, apply that because it's the most detailed / standardized.
    auto transform = getCmsTransform();
    if (transform) {
        ScopeGuard transformGuard{[transform] { cmsDeleteTransform(transform); }};

        tlog::debug() << "Found ICC color profile.";

        // lcms can't perform alpha premultiplication, so we leave it up to downstream processing
        resultData.hasPremultipliedAlpha = false;

        size_t numPixels = (size_t)size.x() * size.y();
        vector<float> src(numPixels * numChannels);
        vector<float> dst(numPixels * numChannels);

        const size_t n_samples_per_row = size.x() * numChannels;
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            size.y(),
            [&](size_t y) {
                size_t src_offset = y * n_samples_per_row;
                for (size_t x = 0; x < n_samples_per_row; ++x) {
                    const uint16_t* typedData = reinterpret_cast<const uint16_t*>(data + y * bytesPerLine);
                    src[src_offset + x] = (float)typedData[x] * channelScale;
                }

                // Armchair parallelization of lcms: cmsDoTransform is reentrant per the spec, i.e. it can be called from multiple threads.
                // So: call cmsDoTransform for each row in parallel.
                // NOTE: This core depends on makeNChannels creating RGBA interleaved buffers!
                size_t dst_offset = y * (size_t)size.x() * 4;
                cmsDoTransform(transform, &src[src_offset], &resultData.channels[0].data()[dst_offset], size.x());
            },
            priority
        );

        co_return result;
    }

    // Otherwise, assume the image is in Rec.709/sRGB and convert it to linear space, followed by an optional change in color space if an
    // NCLX profile is present.
    co_await ThreadPool::global().parallelForAsync<int>(
        0,
        size.y(),
        [&](int y) {
            for (int x = 0; x < size.x(); ++x) {
                size_t i = y * (size_t)size.x() + x;
                auto typedData = reinterpret_cast<const unsigned short*>(data + y * bytesPerLine);
                int baseIdx = x * numChannels;

                for (int c = 0; c < numChannels; ++c) {
                    if (c == 3) {
                        resultData.channels[c].at(i) = typedData[baseIdx + c] * channelScale;
                    } else {
                        resultData.channels[c].at(i) = toLinear(typedData[baseIdx + c] * channelScale);
                    }
                }
            }
        },
        priority
    );

    heif_color_profile_nclx* nclx = nullptr;
    if (auto error = heif_image_handle_get_nclx_color_profile(handle, &nclx); error.code != heif_error_Ok) {
        if (error.code == heif_error_Color_profile_does_not_exist) {
            co_return result;
        }

        tlog::warning() << "Failed to read ICC profile: " << error.message;
        co_return result;
    }

    ScopeGuard nclxGuard{[nclx] { heif_nclx_color_profile_free(nclx); }};

    tlog::debug() << "Found NCLX color profile.";

    // Only convert if not already in Rec.709/sRGB
    if (nclx->color_primaries != heif_color_primaries_ITU_R_BT_709_5) {
        Imf::Chromaticities rec709; // default rec709 (sRGB) primaries
        Imf::Chromaticities chroma = {
            {nclx->color_primary_red_x,   nclx->color_primary_red_y  },
            {nclx->color_primary_green_x, nclx->color_primary_green_y},
            {nclx->color_primary_blue_x,  nclx->color_primary_blue_y },
            {nclx->color_primary_white_x, nclx->color_primary_white_y}
        };

        Imath::M44f M = Imf::RGBtoXYZ(chroma, 1) * Imf::XYZtoRGB(rec709, 1);
        for (int m = 0; m < 4; ++m) {
            for (int n = 0; n < 4; ++n) {
                resultData.toRec709.m[m][n] = M.x[m][n];
            }
        }
    }

    co_return result;
}

} // namespace tev
