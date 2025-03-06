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
#include <tev/imageio/UltraHdrImageLoader.h>

#include <ImfChromaticities.h>

#include <ultrahdr_api.h>

using namespace nanogui;
using namespace std;

namespace tev {

UltraHdrImageLoader::UltraHdrImageLoader() {}

bool UltraHdrImageLoader::canLoadFile(istream& iStream) const {
    uint8_t header[3] = {};
    iStream.read((char*)header, 3);

    // Early return if not a JPEG
    if (header[0] != 0xFF || header[1] != 0xD8 || header[2] != 0xFF) {
        iStream.clear();
        iStream.seekg(0);
        return false;
    }

    // TODO: avoid loading the whole file to memory just to check whether ultrahdr can load it. At least we only have to do this for JPG
    // images... and hopefully our caches stay hot for when the image *actually* gets loaded later on.
    iStream.seekg(0, ios_base::end);
    int64_t fileSize = iStream.tellg();
    iStream.clear();
    iStream.seekg(0);

    vector<char> buffer(fileSize);
    iStream.read(buffer.data(), fileSize);

    iStream.clear();
    iStream.seekg(0);

    return is_uhdr_image(buffer.data(), (int)fileSize);
}

static bool isOkay(uhdr_error_info_t status) { return status.error_code == UHDR_CODEC_OK; }

static string toString(uhdr_error_info_t status) {
    if (isOkay(status)) {
        return "Okay";
    } else if (status.has_detail) {
        return fmt::format("Error #{}: {}", (uint32_t)status.error_code, status.detail);
    } else {
        return fmt::format("Error #{}", (uint32_t)status.error_code);
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

Task<vector<ImageData>> UltraHdrImageLoader::load(istream& iStream, const fs::path&, const string& channelSelector, int priority) const {
    vector<ImageData> result;

    iStream.seekg(0, ios_base::end);
    int64_t fileSize = iStream.tellg();
    iStream.clear();
    iStream.seekg(0);

    vector<char> buffer(fileSize);
    iStream.read(buffer.data(), fileSize);

    auto decoder = uhdr_create_decoder();
    if (!decoder) {
        throw runtime_error{"Could not create UltraHDR decoder."};
    }

    ScopeGuard decoderGuard{[decoder] { uhdr_release_decoder(decoder); }};

    uhdr_compressed_image_t uhdr_image;
    uhdr_image.data = buffer.data();
    uhdr_image.data_sz = fileSize;
    uhdr_image.capacity = fileSize;
    uhdr_image.cg = UHDR_CG_UNSPECIFIED;
    uhdr_image.ct = UHDR_CT_UNSPECIFIED;
    uhdr_image.range = UHDR_CR_UNSPECIFIED;

    if (auto status = uhdr_dec_set_image(decoder, &uhdr_image); !isOkay(status)) {
        throw runtime_error{fmt::format("Failed to set image: {}", toString(status))};
    }

    if (auto status = uhdr_dec_set_out_img_format(decoder, UHDR_IMG_FMT_64bppRGBAHalfFloat); !isOkay(status)) {
        throw runtime_error{fmt::format("Failed to set output format: {}", toString(status))};
    }

    if (auto status = uhdr_dec_set_out_color_transfer(decoder, UHDR_CT_LINEAR); !isOkay(status)) {
        throw runtime_error{fmt::format("Failed to set output color transfer: {}", toString(status))};
    }

    if (auto status = uhdr_decode(decoder); !isOkay(status)) {
        throw runtime_error{fmt::format("Failed to decode: {}", toString(status))};
    }

    uhdr_raw_image_t* decoded_image = uhdr_get_decoded_image(decoder);
    if (!decoded_image) {
        throw runtime_error{"No decoded image."};
    }

    auto readImage = [](uhdr_raw_image_t* image, int priority) -> Task<ImageData> {
        if (image->fmt != UHDR_IMG_FMT_64bppRGBAHalfFloat) {
            throw runtime_error{"Decoded image is not UHDR_IMG_FMT_64bppRGBAHalfFloat."};
        }

        Vector2i size = {(int)image->w, (int)image->h};
        if (size.x() <= 0 || size.y() <= 0) {
            throw runtime_error{"Invalid image size."};
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
        Imf::Chromaticities rec709; // default constructor yields rec709 (sRGB) primaries
        Imf::Chromaticities chroma;

        tlog::debug(fmt::format("Ultra HDR image has color gamut: {}", toString(image->cg)));

        switch (image->cg) {
            case UHDR_CG_DISPLAY_P3:
                chroma = {
                    {0.6800f,  0.3200f },
                    {0.2650f,  0.6900f },
                    {0.1500f,  0.0600f },
                    {0.31271f, 0.32902f}
                };
                break;
            case UHDR_CG_BT_2100:
                chroma = {
                    {0.7080f,  0.2920f },
                    {0.1700f,  0.7970f },
                    {0.1310f,  0.0460f },
                    {0.31271f, 0.32902f}
                };
                break;
            case UHDR_CG_UNSPECIFIED: tlog::warning() << "Ultra HDR image has unspecified color gamut. Assuming BT.709."; break;
            case UHDR_CG_BT_709: break;
            default: tlog::warning() << "Ultra HDR image has invalid color gamut. Assuming BT.709."; break;
        }

        if (chroma != rec709) {
            Imath::M44f M = Imf::RGBtoXYZ(chroma, 1) * Imf::XYZtoRGB(rec709, 1);
            for (int m = 0; m < 4; ++m) {
                for (int n = 0; n < 4; ++n) {
                    imageData.toRec709.m[m][n] = M.x[m][n];
                }
            }
        }

        co_return imageData;
    };

    result.emplace_back(co_await readImage(decoded_image, priority));
    co_return result;
}

} // namespace tev
