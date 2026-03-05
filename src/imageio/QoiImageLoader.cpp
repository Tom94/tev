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
#include <tev/imageio/QoiImageLoader.h>

#define QOI_NO_STDIO
#define QOI_IMPLEMENTATION
#include <qoi.h>

using namespace nanogui;
using namespace std;

namespace tev {

Task<vector<ImageData>> QoiImageLoader::load(istream& iStream, const fs::path&, string_view, const ImageLoaderSettings&, int priority) const {
    char magic[4];
    iStream.read(magic, 4);
    string magicString(magic, 4);

    if (magicString != "qoif") {
        throw FormatNotSupported{fmt::format("Invalid magic QOI string {}.", magicString)};
    }

    iStream.clear();
    iStream.seekg(0, iStream.end);
    const auto dataSize = iStream.tellg();
    iStream.seekg(0, iStream.beg);

    HeapArray<char> data(dataSize);
    iStream.read(data.data(), dataSize);

    qoi_desc desc;

    using DataPtr = unique_ptr<uint8_t, decltype(&free)>;
    const auto decodedData = DataPtr{static_cast<uint8_t*>(qoi_decode(data.data(), (int)dataSize, &desc, 0)), &free};
    if (!decodedData) {
        throw ImageLoadError{"Failed to decode data from the QOI format."};
    }

    const Vector2i size{(int)desc.width, (int)desc.height};
    const auto numPixels = (size_t)size.x() * size.y();
    if (numPixels == 0) {
        throw ImageLoadError{"Image has zero pixels."};
    }

    const auto numChannels = (size_t)desc.channels;
    if (numChannels != 4 && numChannels != 3) {
        throw ImageLoadError{fmt::format("Invalid number of channels {}.", numChannels)};
    }

    const bool hasAlpha = numChannels == 4;
    const auto numInterleavedChannels = nextSupportedTextureChannelCount(numChannels);

    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    // QOI images are 8 bit per pixel which easily fits into F16.
    resultData.channels = co_await makeRgbaInterleavedChannels(
        numChannels, numInterleavedChannels, hasAlpha, size, EPixelFormat::F32, EPixelFormat::F16, "", priority
    );
    resultData.hasPremultipliedAlpha = false;
    resultData.nativeMetadata.chroma = rec709Chroma();

    const auto outView = MultiChannelView<float>{resultData.channels};
    const auto s = span<const uint8_t>{decodedData.get(), numPixels * numChannels};

    if (desc.colorspace == QOI_LINEAR) {
        co_await toFloat32<uint8_t, false>(s, numChannels, outView, hasAlpha, priority);

        resultData.nativeMetadata.transfer = ituth273::ETransfer::Linear;
    } else {
        co_await toFloat32<uint8_t, true>(s, numChannels, outView, hasAlpha, priority);

        resultData.nativeMetadata.transfer = ituth273::ETransfer::SRGB;
    }

    co_return result;
}

} // namespace tev
