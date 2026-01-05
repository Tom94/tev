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
#include <tev/imageio/UltraHdrImageLoader.h>

// From Imath. Required to convert UltraHDR fp16 images to float.
#include <half.h>

#include <ultrahdr_api.h>

using namespace nanogui;
using namespace std;

namespace tev {

static bool isOkay(uhdr_error_info_t status) { return status.error_code == UHDR_CODEC_OK; }

static string toString(uhdr_error_info_t status) {
    if (isOkay(status)) {
        return "Okay";
    } else if (status.has_detail) {
        return fmt::format("Error #{}: {}.", (uint32_t)status.error_code, status.detail);
    } else {
        return fmt::format("Error #{}.", (uint32_t)status.error_code);
    }
}

static string toString(uhdr_color_gamut_t cg) {
    switch (cg) {
        case UHDR_CG_UNSPECIFIED: return "Unspecified";
        case UHDR_CG_BT_709: return "BT.709";
        case UHDR_CG_BT_2100: return "BT.2100";
        case UHDR_CG_DISPLAY_P3: return "Display P3";
        default: return "Unknown";
    }
}

ituth273::ETransferCharacteristics toCicpTransfer(uhdr_color_transfer_t ct) {
    switch (ct) {
        case UHDR_CT_UNSPECIFIED: return ituth273::ETransferCharacteristics::Unspecified;
        case UHDR_CT_LINEAR: return ituth273::ETransferCharacteristics::Linear;
        case UHDR_CT_HLG: return ituth273::ETransferCharacteristics::HLG;
        case UHDR_CT_PQ: return ituth273::ETransferCharacteristics::PQ;
        case UHDR_CT_SRGB: return ituth273::ETransferCharacteristics::SRGB;
        default: return ituth273::ETransferCharacteristics::Unspecified;
    }
}

Task<vector<ImageData>> UltraHdrImageLoader::load(istream& iStream, const fs::path&, string_view, int priority, bool applyGainmaps) const {
    if (!applyGainmaps) {
        throw FormatNotSupported{"Ultra HDR images must have gainmaps applied."};
    }

    iStream.seekg(0, ios_base::end);
    const int64_t fileSize = iStream.tellg();
    iStream.clear();
    iStream.seekg(0);

    if (fileSize < 3) {
        throw FormatNotSupported{"File is too small."};
    }

    HeapArray<char> buffer(fileSize);
    iStream.read(buffer.data(), 3);

    if ((uint8_t)buffer[0] != 0xFF || (uint8_t)buffer[1] != 0xD8 || (uint8_t)buffer[2] != 0xFF) {
        throw FormatNotSupported{"File is not a JPEG."};
    }

    const int64_t remainingSize = fileSize - 3;
    iStream.read(buffer.data() + 3, remainingSize);

    auto decoder = uhdr_create_decoder();
    if (!decoder) {
        throw ImageLoadError{"Could not create UltraHDR decoder."};
    }

    ScopeGuard decoderGuard{[decoder] { uhdr_release_decoder(decoder); }};

    uhdr_compressed_image_t uhdrImage;
    uhdrImage.data = buffer.data();
    uhdrImage.data_sz = fileSize;
    uhdrImage.capacity = fileSize;
    uhdrImage.cg = UHDR_CG_UNSPECIFIED;
    uhdrImage.ct = UHDR_CT_UNSPECIFIED;
    uhdrImage.range = UHDR_CR_UNSPECIFIED;

    if (auto status = uhdr_dec_set_image(decoder, &uhdrImage); !isOkay(status)) {
        throw ImageLoadError{fmt::format("Failed to set image: {}", toString(status))};
    }

    if (auto status = uhdr_dec_set_out_img_format(decoder, UHDR_IMG_FMT_64bppRGBAHalfFloat); !isOkay(status)) {
        throw ImageLoadError{fmt::format("Failed to set output format: {}", toString(status))};
    }

    if (auto status = uhdr_dec_set_out_color_transfer(decoder, UHDR_CT_LINEAR); !isOkay(status)) {
        throw ImageLoadError{fmt::format("Failed to set output color transfer: {}", toString(status))};
    }

    if (auto status = uhdr_dec_probe(decoder); !isOkay(status)) {
        throw FormatNotSupported{fmt::format("Failed to probe: {}", toString(status))};
    }

    if (auto status = uhdr_decode(decoder); !isOkay(status)) {
        throw ImageLoadError{fmt::format("Failed to decode: {}", toString(status))};
    }

    uhdr_raw_image_t* image = uhdr_get_decoded_image(decoder);
    if (!image) {
        throw ImageLoadError{"No decoded image."};
    }

    const uhdr_mem_block_t* exifData = uhdr_dec_get_exif(decoder);

    // We can technically obtain an ICC profile via the uhdr API, but it appears to not correspond directly to the color space of the
    // decoded image with gainmap applied. Hence we will not use the ICC profile for now and instead rely on manual conversion to Rec.709
    // via simple matrix color transform. (No need for transfer functions, because we're already getting linear colors.)
    const uhdr_mem_block_t* iccProfile = nullptr; // uhdr_dec_get_icc(decoder);

    vector<ImageData> result;
    ImageData& imageData = result.emplace_back();

    if (image->fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat) {
        throw ImageLoadError{"Image is not UHDR_IMG_FMT_64bppRGBAHalfFloat."};
    }

    Vector2i size = {(int)image->w, (int)image->h};
    if (size.x() <= 0 || size.y() <= 0) {
        throw ImageLoadError{"Invalid image size."};
    }

    // Ultra HDR always outputs 4 channels (RGBA), even though alpha is always 1.
    const int numChannels = 4;

    // Ultra HDR gives us at most F16 data. See https://github.com/google/libultrahdr?tab=readme-ov-file#decoding-api-outline
    imageData.channels = makeRgbaInterleavedChannels(numChannels, true, size, EPixelFormat::F32, EPixelFormat::F16);

    // JPEG always has alpha == 1 in which case there's no distinction between premultiplied and straight alpha
    imageData.hasPremultipliedAlpha = true;

    auto data = reinterpret_cast<const half*>(image->planes[UHDR_PLANE_PACKED]);
    size_t samplesPerRow = image->stride[UHDR_PLANE_PACKED] * numChannels;

    co_await toFloat32((const half*)data, numChannels, imageData.channels.front().floatData(), 4, size, true, priority, 1.0f, samplesPerRow);

    // Convert to Rec.709 if necessary
    tlog::debug(fmt::format("Ultra HDR image has color gamut: {}", toString(image->cg)));

    // If we have an ICC profile, we will use that to convert to Rec.709. Otherwise, we will use the less rich color gamut information.
    // The offset of 14 bytes in the below check does not come from documentation, but rather was empirically determined by inspecting
    // the raw data of the ICC profile. The first 14 bytes appear to be a header of some sort.
    if (iccProfile && iccProfile->data && iccProfile->data_sz > 14) {
        tlog::warning() << "Found ICC color profile. Attempting to apply... " << iccProfile->data_sz;

        auto channels = makeRgbaInterleavedChannels(numChannels, true, size, EPixelFormat::F32, EPixelFormat::F16);
        try {
            const auto profile = ColorProfile::fromIcc((uint8_t*)iccProfile->data + 14, iccProfile->data_sz - 14);
            co_await toLinearSrgbPremul(
                profile,
                size,
                3,
                EAlphaKind::Straight,
                imageData.channels.front().pixelFormat(),
                imageData.channels.front().data(),
                channels.front().floatData(),
                4,
                priority
            );

            imageData.renderingIntent = profile.renderingIntent();
            if (const auto cicp = profile.cicp()) {
                imageData.hdrMetadata.bestGuessWhiteLevel = ituth273::bestGuessReferenceWhiteLevel(cicp->transfer);
            }

            swap(imageData.channels, channels);
        } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to apply ICC color profile: {}", e.what()); }
    } else {
        // Since UltraHDR images are HDR relative to display-referred SDR content, they're display-referred as well.
        imageData.renderingIntent = ERenderingIntent::RelativeColorimetric;

        switch (image->cg) {
            case UHDR_CG_DISPLAY_P3:
                imageData.toRec709 = convertColorspaceMatrix(displayP3Chroma(), rec709Chroma(), imageData.renderingIntent);
                break;
            case UHDR_CG_BT_2100:
                imageData.toRec709 = convertColorspaceMatrix(bt2100Chroma(), rec709Chroma(), imageData.renderingIntent);
                break;
            case UHDR_CG_UNSPECIFIED: tlog::warning() << "Ultra HDR image has unspecified color gamut. Assuming BT.709."; break;
            case UHDR_CG_BT_709: break; // This is already linear sRGB / Rec.709, so no conversion needed.
            default: tlog::warning() << "Ultra HDR image has invalid color gamut. Assuming BT.709."; break;
        }

        imageData.hdrMetadata.bestGuessWhiteLevel = ituth273::bestGuessReferenceWhiteLevel(toCicpTransfer(image->ct));
    }

    try {
        if (exifData && exifData->data && exifData->data_sz > 0) {
            tlog::debug(fmt::format("Found EXIF data of size {} bytes", exifData->data_sz));
            imageData.attributes.emplace_back(Exif({(uint8_t*)exifData->data, exifData->data_sz}).toAttributes());
        }
    } catch (const runtime_error& e) { tlog::warning() << fmt::format("Failed to read EXIF metadata: {}", e.what()); }

    co_return result;
}

} // namespace tev
