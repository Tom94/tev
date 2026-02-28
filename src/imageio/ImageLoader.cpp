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

#include <tev/imageio/BmpImageLoader.h>
#include <tev/imageio/ClipboardImageLoader.h>
#include <tev/imageio/EmptyImageLoader.h>
#include <tev/imageio/ExrImageLoader.h>
#include <tev/imageio/IcoImageLoader.h>
#include <tev/imageio/ImageLoader.h>
#include <tev/imageio/Jpeg2000ImageLoader.h>
#include <tev/imageio/JpegTurboImageLoader.h>
#include <tev/imageio/PfmImageLoader.h>
#include <tev/imageio/PngImageLoader.h>
#include <tev/imageio/QoiImageLoader.h>
#include <tev/imageio/RawImageLoader.h>
#include <tev/imageio/StbiImageLoader.h>
#include <tev/imageio/TiffImageLoader.h>
#include <tev/imageio/WebpImageLoader.h>

#ifdef _WIN32
#    include <tev/imageio/DdsImageLoader.h>
#endif
#ifdef TEV_USE_LIBHEIF
#    include <tev/imageio/HeifImageLoader.h>
#endif
#ifdef TEV_SUPPORT_JXL
#    include <tev/imageio/JxlImageLoader.h>
#endif

#include <half.h>

using namespace nanogui;
using namespace std;

namespace tev {

const vector<unique_ptr<ImageLoader>>& ImageLoader::getLoaders() {
    auto makeLoaders = [] {
        vector<unique_ptr<ImageLoader>> imageLoaders;
        imageLoaders.emplace_back(new ExrImageLoader());
        imageLoaders.emplace_back(new PfmImageLoader());
        imageLoaders.emplace_back(new ClipboardImageLoader());
        imageLoaders.emplace_back(new EmptyImageLoader());
#ifdef _WIN32
        imageLoaders.emplace_back(new DdsImageLoader());
#endif
#ifdef TEV_USE_LIBHEIF
        imageLoaders.emplace_back(new HeifImageLoader());
#endif
#ifdef TEV_SUPPORT_JXL
        imageLoaders.emplace_back(new JxlImageLoader());
#endif
        imageLoaders.emplace_back(new QoiImageLoader());
        imageLoaders.emplace_back(new WebpImageLoader());
        imageLoaders.emplace_back(new Jpeg2000ImageLoader());
        imageLoaders.emplace_back(new JpegTurboImageLoader());
        imageLoaders.emplace_back(new PngImageLoader());
        imageLoaders.emplace_back(new RawImageLoader());
        imageLoaders.emplace_back(new TiffImageLoader());
        imageLoaders.emplace_back(new BmpImageLoader());
        imageLoaders.emplace_back(new IcoImageLoader());
        imageLoaders.emplace_back(new StbiImageLoader());
        return imageLoaders;
    };

    static const vector imageLoaders = makeLoaders();
    return imageLoaders;
}

const vector<string_view>& ImageLoader::supportedMimeTypes() {
    static const vector<string_view> mimeTypes = {
#ifdef TEV_SUPPORT_AVIF
        "image/avif",
#endif
        "image/apng",
        "image/bmp",
        "image/gif",
#ifdef TEV_SUPPORT_HEIC
        "image/heic",
        "image/heif",
#endif
        "image/ico",
        "image/jpeg",
        "image/jxl",
        "image/png",
        "image/qoi",
        "image/tga",
        "image/tiff",
        "image/vnd.microsoft.icon",
        "image/vnd.mozilla.apng",
        "image/vnd.radiance",
        "image/webp",
        "image/x-adobe-dng",
#ifdef _WIN32
        "image/x-dds",
        "image/x-direct-draw-surface",
#endif
        "image/x-exr",
        "image/x-hdr",
        "image/x-icon",
        "image/x-pfm",
        "image/x-portable-anymap",
        "image/x-portable-arbitrarymap",
        "image/x-portable-bitmap",
        "image/x-portable-floatmap",
        "image/x-portable-graymap",
        "image/x-portable-pixmap",
    };

    return mimeTypes;
}

Task<vector<Channel>> ImageLoader::makeRgbaInterleavedChannels(
    size_t numChannels,
    size_t numInterleavedDims,
    bool hasAlpha,
    const Vector2i& size,
    EPixelFormat format,
    EPixelFormat desiredFormat,
    string_view layer,
    int priority
) {
    if (numChannels > 4 || numChannels == 0) {
        throw ImageLoadError{"Invalid number of rgba channels."};
    }

    if (numInterleavedDims < numChannels) {
        throw ImageLoadError{"Number of interleaved dimensions must be at least the number of channels."};
    }

    vector<Channel> channels;

    const auto numColorChannels = numChannels - (hasAlpha ? 1 : 0);
    if (numColorChannels <= 0 || numColorChannels > 3) {
        throw ImageLoadError{fmt::format("Image has invalid number of color channels: {}", numColorChannels)};
    }

    const size_t numPixels = (size_t)size.x() * size.y();
    const auto data = make_shared<PixelBuffer>(PixelBuffer::alloc(numPixels * numInterleavedDims, format));

    // Initialize pattern [0,0,0,1] efficiently using multi-byte writes
    const auto init = [numPixels, numInterleavedDims, hasAlpha, priority](auto* ptr) -> Task<void> {
        using ptr_float_t = remove_pointer_t<decltype(ptr)>;
        ptr_float_t pattern[4] = {(ptr_float_t)0.0f, (ptr_float_t)0.0f, (ptr_float_t)0.0f, (ptr_float_t)1.0f};
        if (hasAlpha) {
            pattern[numInterleavedDims - 1] = (ptr_float_t)1.0f;
        }

        co_await ThreadPool::global().parallelForAsync<size_t>(
            0,
            numPixels,
            numPixels,
            [pattern, numInterleavedDims, ptr](size_t i) {
                memcpy(ptr + i * numInterleavedDims, pattern, sizeof(ptr_float_t) * numInterleavedDims);
            },
            priority
        );
    };

    if (format == EPixelFormat::F32) {
        co_await init(data->data<float>());
    } else if (format == EPixelFormat::F16) {
        co_await init(data->data<half>());
    } else {
        throw ImageLoadError{"Unsupported pixel format."};
    }

    if (numColorChannels > 1) {
        const vector<string_view> channelNames = {"R", "G", "B"};
        for (size_t c = 0; c < numColorChannels; ++c) {
            channels.emplace_back(
                c < channelNames.size() ? channelNames[c] : to_string(c), size, format, desiredFormat, data, c, numInterleavedDims
            );
        }
    } else {
        channels.emplace_back("L", size, format, desiredFormat, data, 0, numInterleavedDims);
    }

    if (hasAlpha) {
        channels.emplace_back("A", size, format, desiredFormat, data, numColorChannels, numInterleavedDims);
    }

    for (auto& channel : channels) {
        channel.setName(Channel::joinIfNonempty(layer, channel.name()));
    }

    co_return channels;
}

vector<Channel>
    ImageLoader::makeNChannels(size_t numChannels, const Vector2i& size, EPixelFormat format, EPixelFormat desiredFormat, string_view layer) {
    vector<Channel> channels;
    for (size_t c = 0; c < numChannels; ++c) {
        channels.emplace_back(to_string(c), size, format, desiredFormat);
    }

    for (auto& channel : channels) {
        channel.setName(Channel::joinIfNonempty(layer, channel.name()));
    }

    return channels;
}

Task<void> ImageLoader::resizeChannelsAsync(
    span<const Channel> srcChannels, vector<Channel>& dstChannels, const optional<Box2i>& dstArea, int priority
) {
    TEV_ASSERT(srcChannels.size() == dstChannels.size(), "Number of source and destination channels must match.");
    if (srcChannels.empty()) {
        co_return;
    }

    const auto srcViews = srcChannels | views::transform([](const Channel& c) { return c.view<const float>(); }) | to_vector;
    const auto dstViews = dstChannels | views::transform([](Channel& c) { return c.view<float>(); }) | to_vector;

    const Vector2i size = srcChannels.front().size();
    const Vector2i targetSize = dstChannels.front().size();
    const size_t numChannels = srcChannels.size();

    const Box2i box = dstArea.value_or(Box2i{Vector2i(0, 0), targetSize});

    for (size_t i = 1; i < numChannels; ++i) {
        TEV_ASSERT(srcChannels[i].size() == size, "Source channels' size must match.");
        TEV_ASSERT(dstChannels[i].size() == targetSize, "Destination channels' size must match.");
    }

    const size_t numSamples = (size_t)targetSize.x() * targetSize.y() * numChannels;

    const float scaleX = (float)size.x() / box.size().x();
    const float scaleY = (float)size.y() / box.size().y();

    co_await ThreadPool::global().parallelForAsync<int>(
        0,
        targetSize.y(),
        numSamples,
        [&](int dstY) {
            for (int dstX = 0; dstX < targetSize.x(); ++dstX) {
                if (dstX < box.min.x() || dstX >= box.max.x() || dstY < box.min.y() || dstY >= box.max.y()) {
                    for (size_t c = 0; c < numChannels; ++c) {
                        dstViews[c][dstX, dstY] = 0.0f;
                    }

                    continue;
                }

                const float srcX = ((dstX - box.min.x()) + 0.5f) * scaleX - 0.5f;
                const float srcY = ((dstY - box.min.y()) + 0.5f) * scaleY - 0.5f;

                const int x0 = std::max((int)floor(srcX), 0);
                const int y0 = std::max((int)floor(srcY), 0);
                const int x1 = std::min(x0 + 1, size.x() - 1);
                const int y1 = std::min(y0 + 1, size.y() - 1);

                const float wx1 = srcX - x0;
                const float wy1 = srcY - y0;
                const float wx0 = 1.0f - wx1;
                const float wy0 = 1.0f - wy1;

                const float w00 = wx0 * wy0;
                const float w01 = wx1 * wy0;
                const float w10 = wx0 * wy1;
                const float w11 = wx1 * wy1;

                for (size_t c = 0; c < numChannels; ++c) {
                    const float p00 = srcViews[c][x0, y0];
                    const float p01 = srcViews[c][x1, y0];
                    const float p10 = srcViews[c][x0, y1];
                    const float p11 = srcViews[c][x1, y1];

                    dstViews[c][dstX, dstY] = w00 * p00 + w01 * p01 + w10 * p10 + w11 * p11;
                }
            }
        },
        priority
    );
}

Task<void> ImageLoader::resizeImageData(ImageData& resultData, const Vector2i& targetSize, const optional<Box2i>& targetArea, int priority) {
    const Vector2i size = resultData.channels.front().size();
    if (size == targetSize) {
        co_return;
    }

    const auto prevChannels = std::move(resultData.channels);
    resultData.channels.clear();

    for (auto& c : prevChannels) {
        resultData.channels.emplace_back(c.name(), targetSize, c.pixelFormat(), c.desiredPixelFormat());
    }

    co_await resizeChannelsAsync(prevChannels, resultData.channels, targetArea, priority);
};

} // namespace tev
