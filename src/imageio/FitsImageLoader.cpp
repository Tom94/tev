/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2026 Thomas Müller <contact@tom94.net>
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

#include <tev/imageio/FitsImageLoader.h>

#define TINYFITS_IMPLEMENTATION
#include <tinyfits/tinyfits.h>

using namespace nanogui;
using namespace std;

namespace tev {

Task<vector<ImageData>> FitsImageLoader::load(istream& iStream, const fs::path&, string_view, const ImageLoaderSettings&, int priority) const {
    char magic[9] = {};
    iStream.read(magic, sizeof(magic));
    if (!iStream || string_view{magic, sizeof(magic)} != "SIMPLE  =") {
        throw FormatNotSupported{"File is not a FITS image."};
    }

    iStream.clear();
    iStream.seekg(0, iStream.end);
    const auto dataSize = iStream.tellg();
    iStream.seekg(0, iStream.beg);

    HeapArray<char> data(dataSize);
    iStream.read(data.data(), dataSize);
    if (!iStream) {
        throw ImageLoadError{fmt::format("Failed to read FITS data of size {}", (size_t)dataSize)};
    }

    TinyFitsHeader fits = {0};
    void* pixels = nullptr;
    int err = tinyfits_load_from_memory(&fits, data.data(), (size_t)dataSize, &pixels);
    auto fitsGuard = ScopeGuard{[&]() { tinyfits_free_buffer(pixels); tinyfits_free_header(&fits); }};
    if (err != TINYFITS_OK) {
        throw ImageLoadError{fmt::format("Failed to load FITS image: {}", fits.last_error)};
    }

    const Vector2i size{fits.width, fits.height};
    const auto numPixels = (size_t)fits.width * fits.height;
    if (numPixels == 0) {
        throw ImageLoadError{"Image has zero pixels."};
    }

    const auto numChannels = (size_t)fits.num_channels;

    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    const string_view rgbNames[] = {"R", "G", "B"};
    for (size_t c = 0; c < numChannels; ++c) {
        string name;
        if (numChannels == 1) {
            name = "L";
        } else if (numChannels == 3) {
            name = rgbNames[c];
        } else {
            name = to_string(c);
        }

        resultData.channels.emplace_back(name, size, EPixelFormat::F32, EPixelFormat::F32);
        auto view = resultData.channels[c].view<float>();

        switch (fits.pixel_type)
        {
            case TINYFITS_UINT8:
                co_await toFloat32(std::span{(uint8_t*)pixels + numPixels * c, numPixels}, 1, view, false, priority);
                break;
            case TINYFITS_INT16:
                co_await toFloat32(std::span{(int16_t*)pixels + numPixels * c, numPixels}, 1, view, false, priority);
                break;
            case TINYFITS_UINT16:
                co_await toFloat32(std::span{(uint16_t*)pixels + numPixels * c, numPixels}, 1, view, false, priority);
                break;
            case TINYFITS_INT32:
                co_await toFloat32(std::span{(int32_t*)pixels + numPixels * c, numPixels}, 1, view, false, priority);
                break;
            case TINYFITS_UINT32:
                co_await toFloat32(std::span{(uint32_t*)pixels + numPixels * c, numPixels}, 1, view, false, priority);
                break;
            case TINYFITS_FLOAT32:
                co_await toFloat32(std::span{(float*)pixels + numPixels * c, numPixels}, 1, view, false, priority);
                break;
            case TINYFITS_FLOAT64:
                co_await toFloat32(std::span{(double*)pixels + numPixels * c, numPixels}, 1, view, false, priority);
                break;
            case TINYFITS_UNKNOWN:
            default:
                throw ImageLoadError{fmt::format("Unknown FITS pixel type: {}", fits.pixel_type)};
        }
    }

    resultData.hasPremultipliedAlpha = true;
    resultData.nativeMetadata.transfer = ituth273::ETransfer::Linear;

    AttributeNode& root = resultData.attributes.emplace_back();
    root.name = "FITS header";
    AttributeNode& section = root.children.emplace_back();
    section.name = "";
    for (int i = 0; i < fits.num_keywords; ++i) {
        const TinyFitsKeyword& kw = fits.keywords[i];
        AttributeNode& node = section.children.emplace_back();
        node.name = kw.key;
        node.value = kw.value;
        // Hijack node.type to display comment text.
        node.type = kw.comment;
    }

    co_return result;
}

} // namespace tev
