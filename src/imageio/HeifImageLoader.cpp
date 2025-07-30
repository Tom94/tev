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

        Vector2i size = {heif_image_handle_get_width(imgHandle), heif_image_handle_get_height(imgHandle)};

        if (size.x() == 0 || size.y() == 0) {
            throw ImageLoadError{"Image has zero pixels."};
        }

        heif_image* img = nullptr;
        if (auto error = heif_decode_image(imgHandle, &img, decodingColorspace, decodingChroma, nullptr); error.code != heif_error_Ok) {
            throw ImageLoadError{fmt::format("Failed to decode image: {}", error.message)};
        }

        ScopeGuard imgGuard{[img] { heif_image_release(img); }};

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

        if (numChannels == 1) {
            resultData.channels.emplace_back(fmt::format("{}L", namePrefix), size);
        } else {
            resultData.channels = makeRgbaInterleavedChannels(numChannels, hasAlpha, size, namePrefix);
        }

        const int numInterleavedChannels = numChannels == 1 ? 1 : 4;

        auto tryIccTransform = [&](const vector<uint8_t>& iccProfile) -> Task<void> {
            const size_t profileSize = heif_image_handle_get_raw_color_profile_size(imgHandle);
            if (profileSize == 0) {
                throw ImageLoadError{"No ICC color profile found."};
            }

            vector<uint8_t> profileData(profileSize);
            if (auto error = heif_image_handle_get_raw_color_profile(imgHandle, profileData.data()); error.code != heif_error_Ok) {
                if (error.code == heif_error_Color_profile_does_not_exist) {
                    throw ImageLoadError{"ICC color profile does not exist."};
                }

                throw ImageLoadError{fmt::format("Failed to read ICC profile: {}", error.message)};
            }

            vector<float> dataF32((size_t)size.x() * size.y() * numChannels);
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

            co_await toLinearSrgbPremul(
                ColorProfile::fromIcc(profileData.data(), profileData.size()),
                size,
                numChannels,
                hasAlpha ? (resultData.hasPremultipliedAlpha ? EAlphaKind::PremultipliedNonlinear : EAlphaKind::Straight) : EAlphaKind::None,
                EPixelFormat::F32,
                (uint8_t*)dataF32.data(),
                resultData.channels.front().data(),
                numInterleavedChannels,
                priority
            );

            resultData.hasPremultipliedAlpha = true;
        };

        // If we've got an ICC color profile, apply that because it's the most detailed / standardized.
        size_t profileSize = heif_image_handle_get_raw_color_profile_size(imgHandle);
        if (profileSize != 0) {
            tlog::debug() << "Found ICC color profile. Attempting to apply...";
            vector<uint8_t> profileData(profileSize);
            if (auto error = heif_image_handle_get_raw_color_profile(imgHandle, profileData.data()); error.code != heif_error_Ok) {
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
        if (auto error = heif_image_handle_get_nclx_color_profile(imgHandle, &nclx); error.code != heif_error_Ok) {
            if (error.code != heif_error_Color_profile_does_not_exist) {
                tlog::warning() << "Failed to read NCLX color profile: " << error.message;
            }

            if (bitDepth == 16) {
                co_await toFloat32<uint16_t, true>(
                    (const uint16_t*)data,
                    numChannels,
                    resultData.channels.front().data(),
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
                    resultData.channels.front().data(),
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

        ScopeGuard nclxGuard{[nclx] { heif_nclx_color_profile_free(nclx); }};

        if (bitDepth == 16) {
            co_await toFloat32(
                (const uint16_t*)data,
                numChannels,
                resultData.channels.front().data(),
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
                resultData.channels.front().data(),
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

        auto cicpTransfer = static_cast<ituth273::ETransferCharacteristics>(nclx->transfer_characteristics);
        tlog::debug() << fmt::format(
            "NCLX: primaries={}, transfer={}, full_range={}",
            ituth273::toString((ituth273::EColorPrimaries)nclx->color_primaries),
            ituth273::toString(cicpTransfer),
            nclx->full_range_flag == 1 ? "yes" : "no"
        );

        if (!ituth273::isTransferImplemented(cicpTransfer)) {
            tlog::warning() << fmt::format("Unsupported transfer '{}' in NCLX. Using sRGB instead.", ituth273::toString(cicpTransfer));
            cicpTransfer = ituth273::ETransferCharacteristics::SRGB;
        }

        auto* pixelData = resultData.channels.front().data();
        const size_t numPixels = size.x() * (size_t)size.y();
        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            [&](size_t i) {
                // HEIF/AVIF unfortunately tends to have the alpha channel premultiplied in non-linear space (after application of the
                // transfer), so we must unpremultiply prior to the color space conversion and transfer function inversion.
                const float alpha = hasAlpha ? pixelData[i * 4 + 3] : 1.0f;
                const float factor = resultData.hasPremultipliedAlpha && alpha > 0.0001f ? (1.0f / alpha) : 1.0f;
                const float invFactor = resultData.hasPremultipliedAlpha && alpha > 0.0001f ? alpha : 1.0f;

                for (int c = 0; c < numColorChannels; ++c) {
                    const float val = (pixelData[i * numInterleavedChannels + c] - range.offset) * range.scale;
                    pixelData[i * numInterleavedChannels + c] = invFactor * ituth273::invTransfer(cicpTransfer, factor * val);
                }
            },
            priority
        );

        // Only convert color space if not already in Rec.709/sRGB *and* if primaries are actually specified
        if (nclx->color_primaries != heif_color_primaries_ITU_R_BT_709_5 && nclx->color_primaries != heif_color_primaries_unspecified) {
            array<Vector2f, 4> chroma = {
                {
                 {nclx->color_primary_red_x, nclx->color_primary_red_y},
                 {nclx->color_primary_green_x, nclx->color_primary_green_y},
                 {nclx->color_primary_blue_x, nclx->color_primary_blue_y},
                 {nclx->color_primary_white_x, nclx->color_primary_white_y},
                 }
            };

            resultData.toRec709 = chromaToRec709Matrix(chroma);
        }

        co_return resultData;
    };

    // Read main image
    vector<ImageData> result;
    result.emplace_back(co_await decodeImage(handle));
    ImageData& mainImage = result.front();

    unique_ptr<Exif> exif;
    try {
        int numMetadataBlocks = heif_image_handle_get_number_of_metadata_blocks(handle, "Exif");
        if (numMetadataBlocks > 0) {
            tlog::debug() << "Found " << numMetadataBlocks << " EXIF metadata block(s). Attempting to decode...";

            vector<heif_item_id> metadataIDs(numMetadataBlocks);
            heif_image_handle_get_list_of_metadata_block_IDs(handle, "Exif", metadataIDs.data(), numMetadataBlocks);

            for (int i = 0; i < numMetadataBlocks; ++i) {
                size_t exifSize = heif_image_handle_get_metadata_size(handle, metadataIDs[i]);
                if (exifSize <= 4) {
                    tlog::warning() << "Failed to get size of EXIF data.";
                    continue;
                }

                vector<uint8_t> exifData(exifSize);
                if (auto error = heif_image_handle_get_metadata(handle, metadataIDs[i], exifData.data()); error.code != heif_error_Ok) {
                    tlog::warning() << "Failed to read EXIF data: " << error.message;
                    continue;
                }

                // The first four elements are the length and not strictly part of the exif data.
                exifData.erase(exifData.begin(), exifData.begin() + 4);
                exif = make_unique<Exif>(exifData);
            }
        }
    } catch (const invalid_argument& e) { tlog::warning() << fmt::format("Failed to read EXIF metadata: {}", e.what()); }

    if (exif) {
        mainImage.attributes.emplace_back(exif->toAttributes());
    }

    auto findAppleMakerNote = [&]() -> unique_ptr<AppleMakerNote> {
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

    auto resizeImage = [priority](ImageData& resultData, const Vector2i& targetSize, string_view namePrefix) -> Task<void> {
        Vector2i size = resultData.channels.front().size();
        if (size == targetSize) {
            co_return;
        }

        const int numChannels = (int)resultData.channels.size();

        ImageData scaledResultData;
        scaledResultData.hasPremultipliedAlpha = resultData.hasPremultipliedAlpha;

        if (numChannels == 1) {
            scaledResultData.channels.emplace_back(fmt::format("{}L", namePrefix), targetSize);
        } else {
            scaledResultData.channels =
                makeRgbaInterleavedChannels(numChannels, resultData.hasChannel(fmt::format("{}A", namePrefix)), targetSize, namePrefix);
        }

        co_await resizeChannelsAsync(resultData.channels, scaledResultData.channels, priority);
        resultData = std::move(scaledResultData);
    };

    // Read auxiliary images
    int num_aux = heif_image_handle_get_number_of_auxiliary_images(handle, 0);
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
                tlog::debug(
                ) << fmt::format("Found Apple HDR gain map: {}. Checking EXIF maker notes for application parameters.", auxLayerName);
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
