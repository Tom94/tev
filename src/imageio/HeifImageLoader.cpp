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
#include <tev/imageio/AppleMakerNote.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/Exif.h>
#include <tev/imageio/GainMap.h>
#include <tev/imageio/HeifImageLoader.h>
#include <tev/imageio/Xmp.h>

#include <nanogui/vector.h>

#include <libheif/heif.h>

using namespace nanogui;
using namespace std;

namespace tev {

Task<vector<ImageData>>
    HeifImageLoader::load(istream& iStream, const fs::path&, string_view channelSelector, int priority, bool applyGainmaps) const {

    // libheif's spec says it needs the first 12 bytes to determine whether the image can be read.
    uint8_t header[12];
    iStream.read((char*)header, 12);

    if (!iStream || iStream.gcount() != 12 || heif_check_filetype(header, 12) != heif_filetype_yes_supported) {
        throw FormatNotSupported{"File is not a HEIF image."};
    }

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
        throw ImageLoadError{"Failed to allocate libheif context."};
    }

    ScopeGuard contextGuard{[ctx] { heif_context_free(ctx); }};

    if (auto error = heif_context_read_from_reader(ctx, &reader, &readerContext, nullptr); error.code != heif_error_Ok) {
        throw ImageLoadError{fmt::format("Failed to read image: {}", error.message)};
    }

    // get a handle to the primary image
    heif_image_handle* handle;
    if (auto error = heif_context_get_primary_image_handle(ctx, &handle); error.code != heif_error_Ok) {
        throw ImageLoadError{fmt::format("Failed to get primary image handle: {}", error.message)};
    }

    ScopeGuard handleGuard{[handle] { heif_image_handle_release(handle); }};

    auto decodeImage =
        [priority](heif_image_handle* imgHandle, const Vector2i& targetSize = {0}, string_view namePrefix = "") -> Task<ImageData> {
        tlog::debug() << fmt::format("Decoding HEIF image {}", namePrefix.empty() ? "main." : namePrefix);

        ImageData resultData;

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
        resultData.hasPremultipliedAlpha = hasAlpha && heif_image_handle_is_premultiplied_alpha(imgHandle);

        const bool is_little_endian = std::endian::native == std::endian::little;

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

        const Vector2i size = {heif_image_handle_get_width(imgHandle), heif_image_handle_get_height(imgHandle)};
        if (size.x() == 0 || size.y() == 0) {
            throw ImageLoadError{"Image has zero pixels."};
        }

        heif_image* img = nullptr;
        if (const auto error = heif_decode_image(imgHandle, &img, decodingColorspace, decodingChroma, nullptr); error.code != heif_error_Ok) {
            throw ImageLoadError{fmt::format("Failed to decode image: {}", error.message)};
        }

        const ScopeGuard imgGuard{[img] { heif_image_release(img); }};

        const heif_channel channelType = isMonochrome ? heif_channel_Y : heif_channel_interleaved;

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
            resultData.channels.emplace_back(fmt::format("{}L", namePrefix), size, EPixelFormat::F32, EPixelFormat::F16);
        } else {
            resultData.channels = co_await makeRgbaInterleavedChannels(
                numChannels, hasAlpha, size, EPixelFormat::F32, EPixelFormat::F16, namePrefix, priority
            );
        }

        const int numInterleavedChannels = numChannels == 1 ? 1 : 4;

        auto tryIccTransform = [&](const HeapArray<uint8_t>& iccProfile) -> Task<void> {
            const size_t profileSize = heif_image_handle_get_raw_color_profile_size(imgHandle);
            if (profileSize == 0) {
                throw ImageLoadError{"No ICC color profile found."};
            }

            HeapArray<uint8_t> profileData(profileSize);
            if (auto error = heif_image_handle_get_raw_color_profile(imgHandle, profileData.data()); error.code != heif_error_Ok) {
                if (error.code == heif_error_Color_profile_does_not_exist) {
                    throw ImageLoadError{"ICC color profile does not exist."};
                }

                throw ImageLoadError{fmt::format("Failed to read ICC profile: {}", error.message)};
            }

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

            const auto profile = ColorProfile::fromIcc(profileData.data(), profileData.size());
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

            resultData.renderingIntent = profile.renderingIntent();
            if (const auto cicp = profile.cicp()) {
                resultData.hdrMetadata.bestGuessWhiteLevel = ituth273::bestGuessReferenceWhiteLevel(cicp->transfer);
            }

            resultData.hasPremultipliedAlpha = true;
        };

        if (heif_content_light_level cll; heif_image_handle_get_content_light_level(imgHandle, &cll) != 0) {
            resultData.hdrMetadata.maxCLL = cll.max_content_light_level;
            resultData.hdrMetadata.maxFALL = cll.max_pic_average_light_level;

            tlog::debug() << fmt::format(
                "Found content light level information: maxCLL={} maxFALL={}", resultData.hdrMetadata.maxCLL, resultData.hdrMetadata.maxFALL
            );
        }

        heif_decoded_mastering_display_colour_volume mdcv;
        if (heif_mastering_display_colour_volume codedMdcv;
            heif_image_handle_get_mastering_display_colour_volume(imgHandle, &codedMdcv) != 0) {
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
        const size_t profileSize = heif_image_handle_get_raw_color_profile_size(imgHandle);
        if (profileSize != 0) {
            tlog::debug() << "Found ICC color profile. Attempting to apply...";
            HeapArray<uint8_t> profileData(profileSize);
            if (const auto error = heif_image_handle_get_raw_color_profile(imgHandle, profileData.data()); error.code != heif_error_Ok) {
                if (error.code != heif_error_Color_profile_does_not_exist) {
                    tlog::warning() << "Failed to read ICC profile: " << error.message;
                }
            } else {
                try {
                    co_await tryIccTransform(profileData);
                    co_return resultData;
                } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC color profile: {}", e.what()); }
            }
        }

        // Otherwise, check for an NCLX color profile and, if not present, assume the image is in Rec.709/sRGB.
        // See: https://github.com/AOMediaCodec/libavif/wiki/CICP
        heif_color_profile_nclx* nclx = nullptr;
        if (const auto error = heif_image_handle_get_nclx_color_profile(imgHandle, &nclx); error.code != heif_error_Ok) {
            if (error.code != heif_error_Color_profile_does_not_exist) {
                tlog::warning() << "Failed to read NCLX color profile: " << error.message;
            }

            if (bitDepth == 16) {
                co_await toFloat32<uint16_t, true>(
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
                co_await toFloat32<uint8_t, true>(
                    (const uint8_t*)data,
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

            co_return resultData;
        }

        const ScopeGuard nclxGuard{[nclx] { heif_nclx_color_profile_free(nclx); }};

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

        LimitedRange range = LimitedRange::full();
        if (nclx->full_range_flag == 0) {
            range = limitedRangeForBitsPerSample(bitsPerSample);
        }

        auto cicpTransfer = static_cast<ituth273::ETransfer>(nclx->transfer_characteristics);
        tlog::debug() << fmt::format(
            "NCLX: primaries={}, transfer={}, full_range={}",
            ituth273::toString((ituth273::EColorPrimaries)nclx->color_primaries),
            ituth273::toString(cicpTransfer),
            nclx->full_range_flag == 1 ? "yes" : "no"
        );

        if (!ituth273::isTransferImplemented(cicpTransfer)) {
            tlog::warning() << fmt::format("Unsupported transfer '{}' in NCLX. Using sRGB instead.", ituth273::toString(cicpTransfer));
            cicpTransfer = ituth273::ETransfer::SRGB;
        }

        auto* const pixelData = resultData.channels.front().floatData();
        const size_t numPixels = size.x() * (size_t)size.y();
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

        resultData.hdrMetadata.bestGuessWhiteLevel = ituth273::bestGuessReferenceWhiteLevel(cicpTransfer);

        // Only convert color space if not already in Rec.709/sRGB *and* if primaries are actually specified
        if (nclx->color_primaries != heif_color_primaries_ITU_R_BT_709_5 && nclx->color_primaries != heif_color_primaries_unspecified) {
            // Assume heic/avif image is display referred and wants white point adaptation if mismatched. Matches browser behavior.
            resultData.renderingIntent = ERenderingIntent::RelativeColorimetric;
            resultData.toRec709 = convertColorspaceMatrix(
                {
                    {
                     {nclx->color_primary_red_x, nclx->color_primary_red_y},
                     {nclx->color_primary_green_x, nclx->color_primary_green_y},
                     {nclx->color_primary_blue_x, nclx->color_primary_blue_y},
                     {nclx->color_primary_white_x, nclx->color_primary_white_y},
                     }
            },
                rec709Chroma(),
                resultData.renderingIntent
            );
        }

        co_return resultData;
    };

    // Read main image
    vector<ImageData> result;
    result.emplace_back(co_await decodeImage(handle));
    ImageData& mainImage = result.front();

    unique_ptr<Exif> exif;
    const int numMetadataBlocks = heif_image_handle_get_number_of_metadata_blocks(handle, nullptr);
    vector<heif_item_id> metadataIDs((size_t)numMetadataBlocks);

    if (numMetadataBlocks > 0) {
        tlog::debug() << fmt::format("Found {} metadata block(s).", numMetadataBlocks);
    }

    heif_image_handle_get_list_of_metadata_block_IDs(handle, nullptr, metadataIDs.data(), numMetadataBlocks);
    for (heif_item_id id : metadataIDs) {
        const string_view type = heif_image_handle_get_metadata_type(handle, id);
        const string_view contentType = heif_image_handle_get_metadata_content_type(handle, id);
        const size_t size = heif_image_handle_get_metadata_size(handle, id);

        if (size <= 4) {
            tlog::warning() << "Failed to get size of metadata.";
            continue;
        }

        HeapArray<uint8_t> metadata(size);
        if (const auto error = heif_image_handle_get_metadata(handle, id, metadata.data()); error.code != heif_error_Ok) {
            tlog::warning() << "Failed to read metadata: " << error.message;
            continue;
        }

        if (type == "Exif") {
            tlog::debug() << fmt::format("Found EXIF data of size {} bytes", metadata.size());

            try {
                // The first four bytes are the length of the exif data and not strictly part of the exif data.
                exif = make_unique<Exif>(span<uint8_t>{metadata}.subspan(4));
                mainImage.attributes.emplace_back(exif->toAttributes());
            } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read EXIF metadata: {}", e.what()); }
        } else if (contentType == "application/rdf+xml") {
            tlog::debug() << fmt::format("Found XMP data of size {} bytes", metadata.size());

            try {
                Xmp xmp{
                    string_view{(const char*)metadata.data(), metadata.size()}
                };
                mainImage.attributes.emplace_back(xmp.attributes());
            } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read XMP metadata: {}", e.what()); }
        } else {
            tlog::debug() << fmt::format("Skipping unknown metadata block of type '{}/{}' ({} bytes).", type, contentType, size);
        }
    }

    const auto findAppleMakerNote = [&]() -> unique_ptr<AppleMakerNote> {
        if (!exif) {
            tlog::warning() << "No EXIF metadata found.";
            return nullptr;
        }

        try {
            return make_unique<AppleMakerNote>(exif->tryGetAppleMakerNote());
        } catch (const invalid_argument& e) {
            tlog::warning() << fmt::format("Failed to extract Apple maker note from exif: {}", e.what());
        }

        return nullptr;
    };

    const auto resizeImage = [priority](ImageData& resultData, const Vector2i& targetSize, string_view namePrefix) -> Task<void> {
        Vector2i size = resultData.channels.front().size();
        if (size == targetSize) {
            co_return;
        }

        const int numChannels = (int)resultData.channels.size();

        ImageData scaledResultData;
        scaledResultData.hasPremultipliedAlpha = resultData.hasPremultipliedAlpha;

        if (numChannels == 1) {
            scaledResultData.channels.emplace_back(fmt::format("{}L", namePrefix), targetSize, EPixelFormat::F32, EPixelFormat::F16);
        } else {
            scaledResultData.channels = co_await makeRgbaInterleavedChannels(
                numChannels, resultData.hasChannel(fmt::format("{}A", namePrefix)), targetSize, EPixelFormat::F32, EPixelFormat::F16, namePrefix, priority
            );
        }

        co_await resizeChannelsAsync(resultData.channels, scaledResultData.channels, priority);
        resultData = std::move(scaledResultData);
    };

    // Read auxiliary images
    const int num_aux = heif_image_handle_get_number_of_auxiliary_images(handle, 0);
    if (num_aux > 0) {
        tlog::debug() << "Found " << num_aux << " auxiliary image(s)";

        vector<heif_item_id> aux_ids(num_aux);
        heif_image_handle_get_list_of_auxiliary_image_IDs(handle, 0, aux_ids.data(), num_aux);

        for (int i = 0; i < num_aux; ++i) {
            heif_image_handle* auxImgHandle;
            if (auto error = heif_image_handle_get_auxiliary_image_handle(handle, aux_ids[i], &auxImgHandle); error.code != heif_error_Ok) {
                tlog::warning() << fmt::format("Failed to get auxiliary image handle: {}", error.message);
                continue;
            }

            const char* auxType = nullptr;
            if (auto error = heif_image_handle_get_auxiliary_type(auxImgHandle, &auxType); error.code != heif_error_Ok) {
                tlog::warning() << fmt::format("Failed to get auxiliary image type: {}", error.message);
                continue;
            }

            ScopeGuard typeGuard{[auxImgHandle, &auxType] { heif_image_handle_release_auxiliary_type(auxImgHandle, &auxType); }};
            string auxLayerName = auxType ? fmt::format("{}.", auxType) : fmt::format("{}.", num_aux);
            replace(auxLayerName.begin(), auxLayerName.end(), ':', '.');

            const bool retainAuxLayer = matchesFuzzy(auxLayerName, channelSelector);
            const bool loadGainmap = applyGainmaps && auxLayerName.find("apple") != string::npos &&
                auxLayerName.find("hdrgainmap") != string::npos;

            if (!retainAuxLayer && !loadGainmap) {
                continue;
            }

            auto auxImgData = co_await decodeImage(auxImgHandle, mainImage.channels.front().size(), auxLayerName);
            co_await resizeImage(auxImgData, mainImage.channels.front().size(), auxLayerName);

            // If we found an apple-style gainmap, apply it to the main image.
            if (loadGainmap) {
                tlog::debug()
                    << fmt::format("Found Apple HDR gain map: {}. Checking EXIF maker notes for application parameters.", auxLayerName);
                auto amn = findAppleMakerNote();
                if (amn) {
                    tlog::debug() << "Successfully decoded Apple maker note; applying gain map.";
                } else {
                    tlog::warning() << "No Apple maker note was found; applying gain map with headroom defaults.";
                }

                co_await applyAppleGainMap(mainImage, auxImgData, priority, amn.get());
            }

            if (retainAuxLayer) {
                // TODO:Handle the case where the auxiliary image has different color space, attributes, alpha premultiplication, etc.
                // as the main image. Simply copying and attaching the channels is not sufficient in that case.
                mainImage.channels.insert(
                    mainImage.channels.end(),
                    std::make_move_iterator(auxImgData.channels.begin()),
                    std::make_move_iterator(auxImgData.channels.end())
                );
            }
        }
    }

    co_return result;
}

} // namespace tev
