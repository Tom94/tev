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
#include <tev/imageio/UltraHdrImageLoader.h>

// From Imath. Required to convert UltraHDR fp16 images to float.
#include <half.h>

#include <ultrahdr_api.h>

using namespace nanogui;
using namespace std;

namespace tev {

UltraHdrImageLoader::UltraHdrImageLoader() {}

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

Task<vector<ImageData>> UltraHdrImageLoader::load(istream& iStream, const fs::path&, const string&, int priority, bool applyGainmaps) const {
    if (!applyGainmaps) {
        throw FormatNotSupportedException{"Ultra HDR images must have gainmaps applied."};
    }

    iStream.seekg(0, ios_base::end);
    int64_t fileSize = iStream.tellg();
    iStream.clear();
    iStream.seekg(0);

    if (fileSize < 3) {
        throw FormatNotSupportedException{"File is too small."};
    }

    vector<char> buffer(fileSize);
    iStream.read(buffer.data(), 3);

    if ((uint8_t)buffer[0] != 0xFF || (uint8_t)buffer[1] != 0xD8 || (uint8_t)buffer[2] != 0xFF) {
        throw FormatNotSupportedException{"File is not a JPEG."};
    }

    iStream.read(buffer.data() + 3, fileSize - 3);

    auto decoder = uhdr_create_decoder();
    if (!decoder) {
        throw invalid_argument{"Could not create UltraHDR decoder."};
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
        throw invalid_argument{fmt::format("Failed to set image: {}", toString(status))};
    }

    if (auto status = uhdr_dec_set_out_img_format(decoder, UHDR_IMG_FMT_64bppRGBAHalfFloat); !isOkay(status)) {
        throw invalid_argument{fmt::format("Failed to set output format: {}", toString(status))};
    }

    if (auto status = uhdr_dec_set_out_color_transfer(decoder, UHDR_CT_LINEAR); !isOkay(status)) {
        throw invalid_argument{fmt::format("Failed to set output color transfer: {}", toString(status))};
    }

    if (auto status = uhdr_dec_probe(decoder); !isOkay(status)) {
        throw FormatNotSupportedException{fmt::format("Failed to probe: {}", toString(status))};
    }

    if (auto status = uhdr_decode(decoder); !isOkay(status)) {
        throw invalid_argument{fmt::format("Failed to decode: {}", toString(status))};
    }

    uhdr_raw_image_t* decodedImage = uhdr_get_decoded_image(decoder);
    if (!decodedImage) {
        throw invalid_argument{"No decoded image."};
    }

    auto readImage = [priority](uhdr_raw_image_t* image) -> Task<ImageData> {
        if (image->fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat) {
            throw invalid_argument{"Decoded image is not UHDR_IMG_FMT_64bppRGBAHalfFloat."};
        }

        Vector2i size = {(int)image->w, (int)image->h};
        if (size.x() <= 0 || size.y() <= 0) {
            throw invalid_argument{"Invalid image size."};
        }

        const int numChannels = 4;

        ImageData imageData;
        imageData.channels = makeNChannels(numChannels, size);
        imageData.hasPremultipliedAlpha = false;

        size_t numPixels = (size_t)size.x() * size.y();
        vector<float> src(numPixels * numChannels);

        auto data = reinterpret_cast<half*>(image->planes[UHDR_PLANE_PACKED]);
        size_t samplesPerLine = image->stride[UHDR_PLANE_PACKED] * numChannels;

        co_await ThreadPool::global().parallelForAsync<int>(
            0,
            size.y(),
            [&](int y) {
                for (int x = 0; x < size.x(); ++x) {
                    size_t i = y * (size_t)size.x() + x;
                    auto typedData = reinterpret_cast<const half*>(data + y * samplesPerLine);
                    int baseIdx = x * numChannels;

                    for (int c = 0; c < numChannels; ++c) {
                        imageData.channels[c].at(i) = typedData[baseIdx + c];
                    }
                }
            },
            priority
        );

        // Convert to Rec.709 if necessary
        tlog::debug(fmt::format("Ultra HDR image has color gamut: {}", toString(image->cg)));

        switch (image->cg) {
            case UHDR_CG_DISPLAY_P3:
                imageData.toRec709 = convertChromaToRec709({
                    {{0.6800f, 0.3200f}, {0.2650f, 0.6900f}, {0.1500f, 0.0600f}, {0.31271f, 0.32902f}}
                });
                break;
            case UHDR_CG_BT_2100:
                imageData.toRec709 = convertChromaToRec709({
                    {{0.7080f, 0.2920f}, {0.1700f, 0.7970f}, {0.1310f, 0.0460f}, {0.31271f, 0.32902f}}
                });
                break;
            case UHDR_CG_UNSPECIFIED: tlog::warning() << "Ultra HDR image has unspecified color gamut. Assuming BT.709."; break;
            case UHDR_CG_BT_709: break;
            default: tlog::warning() << "Ultra HDR image has invalid color gamut. Assuming BT.709."; break;
        }

        co_return imageData;
    };

    vector<ImageData> result;
    result.emplace_back(co_await readImage(decodedImage));
    co_return result;
}

} // namespace tev
