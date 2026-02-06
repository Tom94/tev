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
#include <tev/imageio/Exif.h>
#include <tev/imageio/GainMap.h>
#include <tev/imageio/HeifImageLoader.h>
#include <tev/imageio/Ifd.h>
#include <tev/imageio/IsoGainMapMetadata.h>
#include <tev/imageio/Xmp.h>

#include <nanogui/vector.h>

#include <libheif/heif.h>
#include <libheif/heif_sequences.h>

#include <optional>

using namespace nanogui;
using namespace std;

namespace tev {

Task<vector<ImageData>> HeifImageLoader::load(
    istream& iStream, const fs::path&, string_view channelSelector, int priority, const GainmapHeadroom& gainmapHeadroom
) const {

    // libheif's spec says it needs the first 12 bytes to determine whether the image can be read.
    uint8_t header[12];
    iStream.read((char*)header, 12);

    if (!iStream || iStream.gcount() != 12) {
        throw FormatNotSupported{"File is too short to be an HEIF image."};
    }

    const auto mimeTypeParts = split(heif_get_file_mime_type(header, 12), "/");
    if (mimeTypeParts.size() != 2) {
        throw FormatNotSupported{"Could not determine HEIF mime type."};
    }

    // We support heic, heif and avif formats, with and without sequence.
    const auto format = mimeTypeParts.back();
    if (format != "heic" && format != "heic-sequence" && format != "heif" && format != "heif-sequence" && format != "avif" &&
        format != "avif-sequence") {
        throw FormatNotSupported{fmt::format("HEIF format '{}' is not supported.", format)};
    }

    iStream.seekg(0, ios_base::end);
    const int64_t fileSize = iStream.tellg();
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

    struct DecodedImageHandle {
        heif_image* img;
        bool hasAlpha;
    };

    const auto getIccProfileFromImgAndHandle = [](heif_image* img, heif_image_handle* handle) -> optional<HeapArray<uint8_t>> {
        if (handle) {
            const size_t handleProfileSize = heif_image_handle_get_raw_color_profile_size(handle);
            if (handleProfileSize > 0) {
                HeapArray<uint8_t> handleProfileData(handleProfileSize);
                if (const auto error = heif_image_handle_get_raw_color_profile(handle, handleProfileData.data());
                    error.code != heif_error_Ok) {
                    if (error.code == heif_error_Color_profile_does_not_exist) {
                        tlog::warning() << "ICC color profile does not exist in handle.";
                    } else {
                        tlog::warning() << fmt::format("Failed to read ICC profile from handle: {}", error.message);
                    }

                    return nullopt;
                }

                return handleProfileData;
            }
        }

        const size_t profileSize = heif_image_get_raw_color_profile_size(img);
        if (profileSize > 0) {
            HeapArray<uint8_t> profileData(profileSize);
            if (const auto error = heif_image_get_raw_color_profile(img, profileData.data()); error.code != heif_error_Ok) {
                if (error.code == heif_error_Color_profile_does_not_exist) {
                    tlog::warning() << "ICC color profile does not exist in img.";
                } else {
                    tlog::warning() << fmt::format("Failed to read ICC profile from img: {}", error.message);
                }

                return nullopt;
            }

            return profileData;
        }

        return nullopt;
    };

    const auto decodeImage = [priority, &getIccProfileFromImgAndHandle](
                                 heif_image* img,
                                 heif_image_handle* imgHandle, // may be nullptr
                                 int numChannels,
                                 bool hasAlpha,
                                 bool skipColorProcessing,
                                 const Vector2i& targetSize = {0},
                                 string_view layer = "",
                                 string_view partName = ""
                             ) -> Task<ImageData> {
        tlog::debug() << fmt::format("Decoding HEIF image '{}'", layer);

        TEV_ASSERT(numChannels >= 1 && numChannels <= 4, "Invalid number of channels.");
        const int numColorChannels = hasAlpha ? numChannels - 1 : numChannels;

        ImageData resultData;
        resultData.hasPremultipliedAlpha = hasAlpha && heif_image_is_premultiplied_alpha(img);
        resultData.partName = partName;

        const Vector2i size = {heif_image_get_primary_width(img), heif_image_get_primary_height(img)};
        if (size.x() == 0 || size.y() == 0) {
            throw ImageLoadError{"Image has zero pixels."};
        }

        const ScopeGuard imgGuard{[img] { heif_image_release(img); }};

        const heif_channel channelType = numChannels == 1 ? heif_channel_Y : heif_channel_interleaved;

        const int bitDepth = heif_image_get_bits_per_pixel(img, channelType) / numChannels;
        if (bitDepth != 8 && bitDepth != 16) {
            throw ImageLoadError{fmt::format("Unsupported HEIF bit depth: {}", bitDepth)};
        }

        const int bitsPerSample = heif_image_get_bits_per_pixel_range(img, channelType);
        const float channelScale = 1.0f / float((1 << bitsPerSample) - 1);

        if (bitsPerSample > bitDepth) {
            throw ImageLoadError{fmt::format("Image has {} bits per sample, but expected at most {} bits.", bitsPerSample, bitDepth)};
        }

        int bytesPerRow = 0;
        const uint8_t* data = heif_image_get_plane_readonly(img, channelType, &bytesPerRow);
        if (!data) {
            throw ImageLoadError{"Faild to get image data."};
        }

        if (bytesPerRow % (bitDepth / 8) != 0) {
            throw ImageLoadError{"Row size not a multiple of sample size."};
        }

        // HEIF images have a fixed point representation of up to 16 bits per channel in TF space. FP16 is perfectly adequate to represent
        // such values after conversion to linear space.
        if (numChannels == 1) {
            resultData.channels.emplace_back(Channel::joinIfNonempty(layer, "L"), size, EPixelFormat::F32, EPixelFormat::F16);
        } else {
            resultData.channels =
                co_await makeRgbaInterleavedChannels(numChannels, hasAlpha, size, EPixelFormat::F32, EPixelFormat::F16, layer, priority);
        }

        const int numInterleavedChannels = numChannels == 1 ? 1 : 4;

        const auto tryIccTransform = [&](const HeapArray<uint8_t>& iccProfile) -> Task<void> {
            HeapArray<float> dataF32((size_t)size.x() * size.y() * numChannels);
            if (bitDepth == 16) {
                co_await toFloat32(
                    (const uint16_t*)data,
                    numChannels,
                    dataF32.data(),
                    numChannels,
                    size,
                    hasAlpha,
                    priority,
                    channelScale,
                    bytesPerRow / sizeof(uint16_t)
                );
            } else {
                co_await toFloat32<uint8_t>(
                    data, numChannels, dataF32.data(), numChannels, size, hasAlpha, priority, channelScale, bytesPerRow / sizeof(uint8_t)
                );
            }

            const auto profile = ColorProfile::fromIcc(iccProfile);
            co_await toLinearSrgbPremul(
                profile,
                size,
                numColorChannels,
                hasAlpha ? (resultData.hasPremultipliedAlpha ? EAlphaKind::PremultipliedNonlinear : EAlphaKind::Straight) : EAlphaKind::None,
                EPixelFormat::F32,
                (uint8_t*)dataF32.data(),
                resultData.channels.front().floatData(),
                numInterleavedChannels,
                priority
            );
            resultData.hasPremultipliedAlpha = true;

            resultData.readMetadataFromIcc(profile);
        };

        if (heif_image_has_content_light_level(img)) {
            heif_content_light_level cll;
            heif_image_get_content_light_level(img, &cll);

            resultData.hdrMetadata.maxCLL = cll.max_content_light_level;
            resultData.hdrMetadata.maxFALL = cll.max_pic_average_light_level;

            tlog::debug() << fmt::format(
                "Found content light level information: maxCLL={} maxFALL={}", resultData.hdrMetadata.maxCLL, resultData.hdrMetadata.maxFALL
            );
        }

        heif_decoded_mastering_display_colour_volume mdcv;
        if (heif_image_has_mastering_display_colour_volume(img)) {
            heif_mastering_display_colour_volume codedMdcv;
            heif_image_get_mastering_display_colour_volume(img, &codedMdcv);

            if (const auto error = heif_mastering_display_colour_volume_decode(&codedMdcv, &mdcv); error.code != heif_error_Ok) {
                tlog::debug() << fmt::format("Failed to decode mastering display color volume: {}", error.message);
            } else {
                resultData.hdrMetadata.masteringChroma = {
                    {
                     {mdcv.display_primaries_x[0], mdcv.display_primaries_y[0]},
                     {mdcv.display_primaries_x[1], mdcv.display_primaries_y[1]},
                     {mdcv.display_primaries_x[2], mdcv.display_primaries_y[2]},
                     {mdcv.white_point_x, mdcv.white_point_y},
                     }
                };
                resultData.hdrMetadata.masteringMinLum = (float)mdcv.min_display_mastering_luminance;
                resultData.hdrMetadata.masteringMaxLum = (float)mdcv.max_display_mastering_luminance;

                tlog::debug() << fmt::format(
                    "Found mastering display color volume: minLum={} maxLum={} chroma={}",
                    resultData.hdrMetadata.masteringMinLum,
                    resultData.hdrMetadata.masteringMaxLum,
                    resultData.hdrMetadata.masteringChroma
                );
            }
        }

        // If we've got an ICC color profile, apply that because it's the most detailed / standardized.
        const auto iccProfileData = skipColorProcessing ? nullopt : getIccProfileFromImgAndHandle(img, imgHandle);
        if (iccProfileData) {
            tlog::debug() << "Found ICC color profile. Attempting to apply...";

            try {
                co_await tryIccTransform(*iccProfileData);
                co_return resultData;
            } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC color profile: {}", e.what()); }
        }

        if (bitDepth == 16) {
            co_await toFloat32(
                (const uint16_t*)data,
                numChannels,
                resultData.channels.front().floatData(),
                numInterleavedChannels,
                size,
                hasAlpha,
                priority,
                channelScale,
                bytesPerRow / sizeof(uint16_t)
            );
        } else {
            co_await toFloat32(
                data,
                numChannels,
                resultData.channels.front().floatData(),
                numInterleavedChannels,
                size,
                hasAlpha,
                priority,
                channelScale,
                bytesPerRow / sizeof(uint8_t)
            );
        }

        if (skipColorProcessing) {
            tlog::debug() << "Skipping color processing.";
            co_return resultData;
        }

        // Otherwise, check for an NCLX color profile and, if not present, assume the image is in Rec.709/sRGB.
        // See: https://github.com/AOMediaCodec/libavif/wiki/CICP
        heif_color_profile_nclx* nclx = nullptr;
        if (const auto error = imgHandle ? heif_image_handle_get_nclx_color_profile(imgHandle, &nclx) :
                                           heif_error{heif_error_Color_profile_does_not_exist, heif_suberror_Unspecified, ""};
            error.code != heif_error_Ok) {
            if (error.code != heif_error_Color_profile_does_not_exist) {
                tlog::warning() << "Failed to read NCLX color profile from handle: " << error.message;
            }
        } else if (const auto error = heif_image_get_nclx_color_profile(img, &nclx); error.code != heif_error_Ok) {
            if (error.code != heif_error_Color_profile_does_not_exist) {
                tlog::warning() << "Failed to read NCLX color profile from img: " << error.message;
            }
        } else {
            tlog::debug() << "Found NCLX color profile. Deriving CICP from it.";
        }

        const ScopeGuard nclxGuard{[nclx] {
            if (nclx) {
                heif_nclx_color_profile_free(nclx);
            }
        }};

        LimitedRange range = LimitedRange::full();
        if (nclx && nclx->full_range_flag == 0) {
            range = limitedRangeForBitsPerSample(bitsPerSample);
        }

        auto cicpTransfer = nclx ? static_cast<ituth273::ETransfer>(nclx->transfer_characteristics) : ituth273::ETransfer::SRGB;

        const auto primaries = (ituth273::EColorPrimaries)(nclx ? nclx->color_primaries : heif_color_primaries_ITU_R_BT_709_5);

        tlog::debug() << fmt::format(
            "CICP: primaries={}, transfer={}, full_range={}",
            ituth273::toString(primaries),
            ituth273::toString(cicpTransfer),
            range == LimitedRange::full() ? "yes" : "no"
        );

        if (!ituth273::isTransferImplemented(cicpTransfer)) {
            tlog::warning() << fmt::format("Unsupported transfer '{}' in NCLX. Using sRGB instead.", ituth273::toString(cicpTransfer));
            cicpTransfer = ituth273::ETransfer::SRGB;
        }

        const size_t numPixels = size.x() * (size_t)size.y();
        auto* const pixelData = resultData.channels.front().floatData();
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels * numInterleavedChannels,
            [&](size_t i) {
                // HEIF/AVIF unfortunately tends to have the alpha channel premultiplied in non-linear space (after application of the
                // transfer), so we must unpremultiply prior to the color space conversion and transfer function inversion.
                const float alpha = hasAlpha ? pixelData[i * 4 + 3] : 1.0f;
                const float factor = resultData.hasPremultipliedAlpha && alpha > 0.0001f ? (1.0f / alpha) : 1.0f;
                const float invFactor = resultData.hasPremultipliedAlpha && alpha > 0.0001f ? alpha : 1.0f;

                Vector3f color;
                for (int c = 0; c < numColorChannels; ++c) {
                    color[c] = (pixelData[i * numInterleavedChannels + c] - range.offset) * range.scale;
                }

                color = ituth273::invTransfer(cicpTransfer, color * factor) * invFactor;
                for (int c = 0; c < numColorChannels; ++c) {
                    pixelData[i * numInterleavedChannels + c] = color[c];
                }
            },
            priority
        );

        // Assume heic/avif image is display referred and wants white point adaptation if mismatched. Matches browser behavior.
        resultData.renderingIntent = ERenderingIntent::RelativeColorimetric;

        resultData.hdrMetadata.bestGuessWhiteLevel = ituth273::bestGuessReferenceWhiteLevel(cicpTransfer);
        resultData.nativeMetadata.transfer = cicpTransfer;

        // Only convert color space if not already in Rec.709/sRGB *and* if primaries are actually specified
        if (nclx && nclx->color_primaries != heif_color_primaries_ITU_R_BT_709_5 &&
            nclx->color_primaries != heif_color_primaries_unspecified) {

            const chroma_t chroma = {
                {
                 {nclx->color_primary_red_x, nclx->color_primary_red_y},
                 {nclx->color_primary_green_x, nclx->color_primary_green_y},
                 {nclx->color_primary_blue_x, nclx->color_primary_blue_y},
                 {nclx->color_primary_white_x, nclx->color_primary_white_y},
                 }
            };

            resultData.toRec709 = convertColorspaceMatrix(chroma, rec709Chroma(), resultData.renderingIntent);
            resultData.nativeMetadata.chroma = chroma;
        } else {
            resultData.nativeMetadata.chroma = rec709Chroma();
        }

        co_return resultData;
    };

    const auto decodeImageHandle = [&decodeImage](
                                       heif_image_handle* imgHandle,
                                       bool skipColorProcessing,
                                       const Vector2i& targetSize = {0},
                                       string_view layer = "",
                                       string_view partName = ""
                                   ) -> Task<ImageData> {
        tlog::debug() << fmt::format("Decoding HEIF image handle '{}'", layer);

        heif_colorspace preferredColorspace = heif_colorspace_undefined;
        heif_chroma preferredChroma = heif_chroma_undefined;
        if (auto error = heif_image_handle_get_preferred_decoding_colorspace(imgHandle, &preferredColorspace, &preferredChroma);
            error.code != heif_error_Ok) {
            throw ImageLoadError{fmt::format("Failed to get preferred decoding colorspace: {}", error.message)};
        }

        const bool hasAlpha = heif_image_handle_has_alpha_channel(imgHandle);

        bool isMonochrome = preferredColorspace == heif_colorspace_monochrome;
        if (isMonochrome != (preferredChroma == heif_chroma_monochrome)) {
            throw ImageLoadError{"Monochrome colorspace and chroma mismatch."};
        }

        if (hasAlpha) {
            // We could handle monochrome images with an alpha channel ourselves, but our life becomes easier if we let libheif convert
            // these to RGBA for us.
            isMonochrome = false;
        }

        const int numColorChannels = isMonochrome ? 1 : 3;
        const int numChannels = numColorChannels + (hasAlpha ? 1 : 0);

        const bool is_little_endian = endian::native == endian::little;

        heif_chroma decodingChroma = heif_chroma_undefined;
        switch (numChannels) {
            case 1: decodingChroma = heif_chroma_monochrome; break;
            case 2: throw ImageLoadError{"Heif images with 2 channels are not supported."};
            case 3: decodingChroma = is_little_endian ? heif_chroma_interleaved_RRGGBB_LE : heif_chroma_interleaved_RRGGBB_BE; break;
            case 4: decodingChroma = is_little_endian ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RRGGBBAA_BE; break;
            default: throw ImageLoadError{"Unsupported number of channels."};
        }

        // If the preferred colorspace isn't monochrome (even if undefined or YCC), we specify RGB and let libheif handle the conversion.
        const heif_colorspace decodingColorspace = isMonochrome ? heif_colorspace_monochrome : heif_colorspace_RGB;

        heif_image* img = nullptr;
        if (const auto error = heif_decode_image(imgHandle, &img, decodingColorspace, decodingChroma, nullptr); error.code != heif_error_Ok) {
            throw ImageLoadError{fmt::format("Failed to decode image: {}", error.message)};
        }

        co_return co_await decodeImage(img, imgHandle, numChannels, hasAlpha, skipColorProcessing, targetSize, layer, partName);
    };

    const auto decodeSingleTrackImage =
        [&decodeImage](heif_track* track, const Vector2i& targetSize = {0}, string_view partName = "") -> Task<optional<ImageData>> {
        tlog::debug() << fmt::format("Decoding HEIF track '{}'", partName);

        const bool hasAlpha = heif_track_has_alpha_channel(track);
        const bool isMonochrome = false; // TODO: libheif doesn't seem to support monochrome tracks

        const int numColorChannels = 3;
        const int numChannels = numColorChannels + (hasAlpha ? 1 : 0);

        const bool is_little_endian = endian::native == endian::little;

        heif_chroma decodingChroma = heif_chroma_undefined;
        switch (numChannels) {
            case 1: decodingChroma = heif_chroma_monochrome; break;
            case 2: throw ImageLoadError{"Heif images with 2 channels are not supported."};
            case 3: decodingChroma = is_little_endian ? heif_chroma_interleaved_RRGGBB_LE : heif_chroma_interleaved_RRGGBB_BE; break;
            case 4: decodingChroma = is_little_endian ? heif_chroma_interleaved_RRGGBBAA_LE : heif_chroma_interleaved_RRGGBBAA_BE; break;
            default: throw ImageLoadError{"Unsupported number of channels."};
        }

        // If the preferred colorspace isn't monochrome (even if undefined or YCC), we specify RGB and let libheif handle the conversion.
        const heif_colorspace decodingColorspace = isMonochrome ? heif_colorspace_monochrome : heif_colorspace_RGB;

        heif_image* img = nullptr;
        if (const auto error = heif_track_decode_next_image(track, &img, decodingColorspace, decodingChroma, nullptr);
            error.code != heif_error_Ok) {
            if (error.code == heif_error_End_of_sequence) {
                tlog::debug() << "End of sequence reached for track.";
                co_return nullopt;
            }

            throw ImageLoadError{fmt::format("Failed to decode track image: {}", error.message)};
        }

        co_return co_await decodeImage(img, nullptr, numChannels, hasAlpha, false, targetSize, partName, partName);
    };

    heif_context* ctx = heif_context_alloc();
    if (!ctx) {
        throw ImageLoadError{"Failed to allocate libheif context."};
    }

    const ScopeGuard contextGuard{[ctx] { heif_context_free(ctx); }};

    if (const auto error = heif_context_read_from_reader(ctx, &reader, &readerContext, nullptr); error.code != heif_error_Ok) {
        throw ImageLoadError{fmt::format("Failed to read image: {}", error.message)};
    }

    vector<ImageData> result;

    // If we're an image *sequence*, load the sequence tracks instead of individual images.
    const auto seqTrackCount = heif_context_number_of_sequence_tracks(ctx);
    if (seqTrackCount > 0) {
        tlog::debug() << fmt::format("HEIF image contains {} sequence track(s). Loading tracks instead of image.", seqTrackCount);

        vector<int> trackIds(seqTrackCount);
        heif_context_get_track_ids(ctx, reinterpret_cast<uint32_t*>(trackIds.data()));

        for (int i = 0; i < seqTrackCount; ++i) {
            const heif_track* track = heif_context_get_track(ctx, trackIds[i]);

            for (size_t frameIdx = 0;; ++frameIdx) {
                const auto partName = seqTrackCount > 1 ? fmt::format("tracks.{}.frames.{}", trackIds[i], frameIdx) :
                                                          fmt::format("frames.{}", frameIdx);

                if (auto imageData = co_await decodeSingleTrackImage(const_cast<heif_track*>(track), {0}, partName)) {
                    result.emplace_back(std::move(*imageData));
                } else {
                    break;
                }
            }
        }

        // We're done loading the sequence tracks. The below code for handling the primary image would work, but it'd be a fallback
        // implemented in libheif that just redundantly loads the first image of the first sequence track again.
        co_return result;
    }

    const size_t numImages = heif_context_get_number_of_top_level_images(ctx);
    vector<heif_item_id> imageIds(numImages);
    heif_context_get_list_of_top_level_image_IDs(ctx, imageIds.data(), (int)numImages);

    const auto decodeTopLevelImgIdAndAuxImages = [&](heif_item_id id, string partName) -> Task<void> {
        tlog::debug() << fmt::format("Decoding top-level HEIF image ID '{}'", id);

        heif_image_handle* imgHandle;
        if (const auto error = heif_context_get_image_handle(ctx, id, &imgHandle); error.code != heif_error_Ok) {
            throw ImageLoadError{fmt::format("Failed to get image handle for top-level image ID {}: {}", id, error.message)};
        }

        // Read main image
        result.emplace_back(co_await decodeImageHandle(imgHandle, false, {0}, partName, partName));
        ImageData& mainImage = result.back();

        optional<Exif> exif;
        optional<IsoGainMapMetadata> isoGainMapMetadata;

        const int numMetadataBlocks = heif_image_handle_get_number_of_metadata_blocks(imgHandle, nullptr);
        vector<heif_item_id> metadataIDs((size_t)numMetadataBlocks);

        if (numMetadataBlocks > 0) {
            tlog::debug() << fmt::format("Found {} metadata block(s).", numMetadataBlocks);
        }

        heif_image_handle_get_list_of_metadata_block_IDs(imgHandle, nullptr, metadataIDs.data(), numMetadataBlocks);
        for (heif_item_id id : metadataIDs) {
            const string_view type = heif_image_handle_get_metadata_type(imgHandle, id);
            const string_view contentType = heif_image_handle_get_metadata_content_type(imgHandle, id);
            const size_t size = heif_image_handle_get_metadata_size(imgHandle, id);

            if (size <= 4) {
                tlog::warning() << "Failed to get size of metadata.";
                continue;
            }

            HeapArray<uint8_t> metadata(size);
            if (const auto error = heif_image_handle_get_metadata(imgHandle, id, metadata.data()); error.code != heif_error_Ok) {
                tlog::warning() << "Failed to read metadata: " << error.message;
                continue;
            }

            if (type == "Exif") {
                tlog::debug() << fmt::format("Found EXIF data of size {} bytes", metadata.size());

                try {
                    // The first four bytes are the length of the exif data and not strictly part of the exif data.
                    exif = Exif{span<uint8_t>{metadata}.subspan(4)};
                    mainImage.attributes.emplace_back(exif->toAttributes());
                } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read EXIF metadata: {}", e.what()); }
            } else if (contentType == "application/rdf+xml") {
                tlog::debug() << fmt::format("Found XMP data '{}/{}' of size {} bytes", type, contentType, metadata.size());

                try {
                    Xmp xmp{
                        string_view{(const char*)metadata.data(), metadata.size()}
                    };

                    if (!isoGainMapMetadata) {
                        isoGainMapMetadata = xmp.isoGainMapMetadata();
                    }

                    mainImage.attributes.emplace_back(xmp.attributes());
                } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read XMP metadata: {}", e.what()); }
            } else if (type == "tmap") {
                tlog::debug() << fmt::format("Found tmap data of size {} bytes", metadata.size());

                try {
                    isoGainMapMetadata = IsoGainMapMetadata{metadata};
                    tlog::debug() << "Successfully parsed tmap ISO 21496-1 gain map metadata.";
                } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read tmap metadata: {}", e.what()); }
            } else {
                tlog::debug() << fmt::format("Skipping unknown metadata block of type '{}/{}' ({} bytes).", type, contentType, size);
            }
        }

        const auto findAppleMakerNote = [&]() -> optional<Ifd> {
            if (!exif) {
                tlog::warning() << "No EXIF metadata found.";
                return nullopt;
            }

            try {
                return make_optional<Ifd>(exif->tryGetAppleMakerNote());
            } catch (const invalid_argument& e) {
                tlog::warning() << fmt::format("Failed to extract Apple maker note from exif: {}", e.what());
            }

            return nullopt;
        };

        // Read auxiliary images
        vector<heif_item_id> auxIds;
        vector<heif_image_handle*> auxImgHandles;
        ScopeGuard auxImgHandlesGuard{[&auxImgHandles] {
            for (heif_image_handle* handle : auxImgHandles) {
                heif_image_handle_release(handle);
            }
        }};

        if (const int numAux = heif_image_handle_get_number_of_auxiliary_images(imgHandle, 0); numAux > 0) {
            auxIds.resize((size_t)numAux);
            heif_image_handle_get_list_of_auxiliary_image_IDs(imgHandle, 0, auxIds.data(), numAux);

            for (const auto auxId : auxIds) {
                if (heif_image_handle* auxImgHandle;
                    heif_image_handle_get_auxiliary_image_handle(imgHandle, auxId, &auxImgHandle).code == heif_error_Ok) {
                    auxImgHandles.emplace_back(auxImgHandle);
                } else {
                    tlog::warning() << fmt::format("Failed to get auxiliary image handle for ID {}.", auxId);
                }
            }
        }

        heif_image_handle* gainmapImgHandle = nullptr;
        if (heif_image_handle_get_gain_map_image_handle(imgHandle, &gainmapImgHandle).code == heif_error_Ok) {
            const auto gainmapItemId = heif_image_handle_get_item_id(gainmapImgHandle);
            tlog::debug() << fmt::format(
                "Found ISO 21496-1 gain map image with ID '{}'. Will be processed while reading auxiliary images.", gainmapItemId
            );

            // If gainmap isn't an aux image, but a separate item, add it to the aux image list to be processed below.
            const auto it = find(auxIds.begin(), auxIds.end(), gainmapItemId);
            if (it == auxIds.end()) {
                auxIds.emplace_back(gainmapItemId);
                auxImgHandles.emplace_back(gainmapImgHandle);
            } else {
                heif_image_handle_release(gainmapImgHandle);
                gainmapImgHandle = auxImgHandles.at(distance(auxIds.begin(), it));
            }
        }

        tlog::debug() << "Found " << auxImgHandles.size() << " auxiliary image(s)";

        for (auto* auxImgHandle : auxImgHandles) {
            const heif_item_id auxId = heif_image_handle_get_item_id(auxImgHandle);

            const char* auxType = nullptr;
            if (auto error = heif_image_handle_get_auxiliary_type(auxImgHandle, &auxType); error.code != heif_error_Ok) {
                tlog::warning() << fmt::format("Failed to get auxiliary image type: {}", error.message);
                continue;
            }

            ScopeGuard typeGuard{[auxImgHandle, &auxType] { heif_image_handle_release_auxiliary_type(auxImgHandle, &auxType); }};
            string auxLayerName = auxType ? auxType : "";
            replace(auxLayerName.begin(), auxLayerName.end(), ':', '.');

            const bool isIsoGainmap = auxImgHandle == gainmapImgHandle;
            if (auxLayerName.empty()) {
                auxLayerName = isIsoGainmap ? "gainmap" : fmt::format("aux.{}", auxId);
            }

            const bool isAppleGainmap = auxLayerName.find("apple") != string::npos && auxLayerName.find("hdrgainmap") != string::npos;

            const bool isGainmap = isIsoGainmap || isAppleGainmap;

            const bool retainAuxLayer = matchesFuzzy(auxLayerName, channelSelector);

            if (!retainAuxLayer && !isGainmap) {
                continue;
            }

            auto auxImgData = co_await decodeImageHandle(
                auxImgHandle, isGainmap, mainImage.channels.front().size(), Channel::joinIfNonempty(partName, auxLayerName), partName
            );

            if (isGainmap) {
                optional<chroma_t> altImgChroma = nullopt;

                if (isIsoGainmap) {
                    tlog::debug() << fmt::format("Found ISO 21496-1 gain map image: {}. Checking for metadata.", auxLayerName);

                    HeapArray<uint8_t> metadataData(heif_image_handle_get_gain_map_metadata_size(imgHandle));
                    if (metadataData.size() > 0 &&
                        heif_image_handle_get_gain_map_metadata(imgHandle, metadataData.data()).code == heif_error_Ok) {

                        tlog::debug()
                            << fmt::format("Read {} bytes of gainmap metadata. Attempting to override if existing.", metadataData.size());

                        try {
                            isoGainMapMetadata = IsoGainMapMetadata{
                                span<uint8_t>{metadataData.data(), metadataData.size()}
                            };

                            tlog::debug() << "Successfully parsed ISO 21496-1 gain map metadata.";
                        } catch (const invalid_argument& e) {
                            tlog::warning() << fmt::format("Failed to read gainmap metadata: {}", e.what());
                        }
                    } else if (!isoGainMapMetadata) {
                        tlog::warning() << "No gainmap metadata found for ISO 21496-1 gain map image.";
                    }

                    HeapArray<uint8_t> profileData(heif_image_handle_get_derived_image_raw_color_profile_size(imgHandle));
                    if (profileData.size() > 0 &&
                        heif_image_handle_get_derived_image_raw_color_profile(imgHandle, profileData.data()).code == heif_error_Ok) {

                        try {
                            altImgChroma = ColorProfile::fromIcc(profileData).chroma();
                            if (altImgChroma) {
                                tlog::debug() << fmt::format("ISO 21496-1 alt. image chroma from ICC: {}", *altImgChroma);
                            }
                        } catch (const invalid_argument& e) {
                            tlog::warning() << fmt::format("Failed to read alt. image ICC profile: {}", e.what());
                        }
                    } else if (heif_color_profile_nclx* nclx;
                               heif_image_handle_get_derived_image_nclx_color_profile(imgHandle, &nclx).code == heif_error_Ok &&
                               nclx->color_primaries != heif_color_primaries_unspecified) {

                        const ScopeGuard nclxGuard{[nclx] { heif_nclx_color_profile_free(nclx); }};

                        altImgChroma = {
                            {
                             {nclx->color_primary_red_x, nclx->color_primary_red_y},
                             {nclx->color_primary_green_x, nclx->color_primary_green_y},
                             {nclx->color_primary_blue_x, nclx->color_primary_blue_y},
                             {nclx->color_primary_white_x, nclx->color_primary_white_y},
                             }
                        };

                        tlog::debug() << fmt::format("ISO 21496-1 alt. image chroma from NCLX: {}", *altImgChroma);
                    }
                }

                // Prioritize ISO 21496-1 gain map application if both types are present. If the gain map is of Apple's type, we can fall
                // back to their vendor-specific handling (optionally with maker note parameters, but also handles default).
                if (isoGainMapMetadata) {
                    tlog::debug() << fmt::format("Found ISO 21496-1 gain map w/ metadata: '{}'. Applying.", auxLayerName);
                    co_await preprocessAndApplyIsoGainMap(
                        mainImage, auxImgData, *isoGainMapMetadata, mainImage.nativeMetadata.chroma, altImgChroma, gainmapHeadroom, priority
                    );
                } else if (isAppleGainmap) {
                    tlog::debug()
                        << fmt::format("Found Apple HDR gain map: {}. Checking EXIF maker notes for application parameters.", auxLayerName);
                    co_await preprocessAndApplyAppleGainMap(mainImage, auxImgData, findAppleMakerNote(), gainmapHeadroom, priority);
                } else {
                    tlog::warning() << fmt::format(
                        "Found ISO 21496-1 gain map '{}' but no associated metadata. Skipping gain map application.", auxLayerName
                    );
                }
            }

            if (retainAuxLayer) {
                // TODO:Handle the case where the auxiliary image has different color space, attributes, alpha premultiplication, etc. as
                // the main image. Simply copying and attaching the channels is not sufficient in that case -- and we can avoid resizing.

                co_await ImageLoader::resizeImageData(auxImgData, mainImage.channels.front().size(), priority);

                mainImage.channels.insert(
                    mainImage.channels.end(),
                    make_move_iterator(auxImgData.channels.begin()),
                    make_move_iterator(auxImgData.channels.end())
                );
            }
        }

        if (isoGainMapMetadata) {
            mainImage.attributes.emplace_back(isoGainMapMetadata->toAttributes());
        }
    };

    for (size_t i = 0; i < numImages; ++i) {
        const heif_item_id id = imageIds[i];
        const string partName = numImages > 1 ? fmt::format("frames.{}", id) : "";

        co_await decodeTopLevelImgIdAndAuxImages(id, partName);
    }

    co_return result;
}

} // namespace tev
