/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2025 Thomas Müller <contact@tom94.net>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
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

Task<vector<ImageData>>
    QoiImageLoader::load(istringstream& iStream, const fs::path&, string_view, const ImageLoaderSettings&, int priority) const {
    const auto data = toSpan<const uint8_t>(iStream).subspan(iStream.tellg());
    if (data.size() < 4 || string_view{(const char*)data.data(), 4} != "qoif") {
        throw FormatNotSupported{"Invalid magic QOI string."};
    }

    qoi_desc desc;

    using DataPtr = unique_ptr<uint8_t, decltype(&free)>;
    const auto decodedData = DataPtr{static_cast<uint8_t*>(qoi_decode(data.data(), (int)data.size(), &desc, 0)), &free};
    if (!decodedData) {
        throw ImageLoadError{"Failed to decode data from the QOI format."};
    }

    const Vector2i size{(int)desc.width, (int)desc.height};
    const auto numPixels = posProd(size);
    if (numPixels == 0) {
        throw ImageLoadError{"Image has zero pixels."};
    }

    const auto numChannels = (size_t)desc.channels;
    if (numChannels != 4 && numChannels != 3) {
        throw ImageLoadError{fmt::format("Invalid number of channels {}.", numChannels)};
    }

    const bool hasAlpha = numChannels == 4;
    const auto alphaKind = hasAlpha ? EAlphaKind::Straight : EAlphaKind::None;
    const auto numInterleavedChannels = nextSupportedTextureChannelCount(numChannels);

    vector<ImageData> result(1);
    ImageData& resultData = result.front();

    // QOI images are 8 bit per pixel which easily fits into F16.
    resultData.channels = co_await makeInterleavedChannels(
        numChannels, numInterleavedChannels, hasAlpha, size, EPixelFormat::F32, EPixelFormat::F16, "", priority
    );
    resultData.nativeMetadata.chroma = rec709Chroma();

    const auto outView = MultiChannelView<float>{resultData.channels};
    const auto s = span<const uint8_t>{decodedData.get(), numPixels * numChannels};

    if (desc.colorspace == QOI_LINEAR) {
        co_await toFloat32<ituth273::ETransfer::Linear, true>(s, numChannels, outView, alphaKind, priority);
        resultData.hasPremultipliedAlpha = true;

        resultData.nativeMetadata.transfer = ituth273::ETransfer::Linear;
    } else {
        co_await toFloat32<ituth273::ETransfer::SRGB, true>(s, numChannels, outView, alphaKind, priority);
        resultData.hasPremultipliedAlpha = true;

        resultData.nativeMetadata.transfer = ituth273::ETransfer::SRGB;
    }

    co_return result;
}

} // namespace tev
