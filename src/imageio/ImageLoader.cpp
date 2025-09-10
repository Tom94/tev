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

#include <tev/imageio/ClipboardImageLoader.h>
#include <tev/imageio/EmptyImageLoader.h>
#include <tev/imageio/ExrImageLoader.h>
#include <tev/imageio/ImageLoader.h>
#include <tev/imageio/JpegTurboImageLoader.h>
#include <tev/imageio/PfmImageLoader.h>
#include <tev/imageio/PngImageLoader.h>
#include <tev/imageio/QoiImageLoader.h>
#include <tev/imageio/RawImageLoader.h>
#include <tev/imageio/StbiImageLoader.h>
#include <tev/imageio/TiffImageLoader.h>
#include <tev/imageio/UltraHdrImageLoader.h>
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
        // UltraHdr must come before JpegTurbo, because it is meant to load specially tagged JPEG files. Those would be loaded without HDR
        // feature by JpegTurbo otherwise. JPEGs without HDR gainmaps will be skipped by UltraHdr and then loaded by JpegTurbo.
        imageLoaders.emplace_back(new RawImageLoader());
        imageLoaders.emplace_back(new TiffImageLoader());
        imageLoaders.emplace_back(new UltraHdrImageLoader());
        imageLoaders.emplace_back(new JpegTurboImageLoader());
        imageLoaders.emplace_back(new PngImageLoader());
        imageLoaders.emplace_back(new StbiImageLoader());
        return imageLoaders;
    };

    static const vector imageLoaders = makeLoaders();
    return imageLoaders;
}

const vector<string_view>& ImageLoader::supportedMimeTypes() {
    static const vector<string_view> mimeTypes = {
        "image/png",
        "image/webp",
        "image/jpeg",
        "image/tiff",
        "image/gif",
        "image/tga",
        "image/bmp",
#ifdef TEV_SUPPORT_AVIF
        "image/avif",
#endif
#ifdef TEV_SUPPORT_HEIC
        "image/heic",
#endif
#ifdef _WIN32
        "image/x-dds",
        "image/x-direct-draw-surface",
#endif
        "image/x-adobe-dng",
        "image/x-exr",
        "image/x-portable-anymap",
    };

    return mimeTypes;
}

vector<Channel> ImageLoader::makeRgbaInterleavedChannels(
    int numChannels, bool hasAlpha, const Vector2i& size, EPixelFormat format, EPixelFormat desiredFormat, string_view namePrefix
) {
    vector<Channel> channels;
    if (numChannels > 4) {
        throw ImageLoadError{"Image has too many RGBA channels."};
    }

    int numColorChannels = numChannels - (hasAlpha ? 1 : 0);
    if (numColorChannels <= 0 || numColorChannels > 3) {
        throw ImageLoadError{fmt::format("Image has invalid number of color channels: {}", numColorChannels)};
    }

    const size_t numPixels = (size_t)size.x() * size.y();
    const size_t numBytesPerSample = nBytes(format);
    shared_ptr<vector<uint8_t>> data = make_shared<vector<uint8_t>>(numBytesPerSample * numPixels * 4);

    // Initialize pattern [0,0,0,1] efficiently using multi-byte writes
    auto init = [numPixels](auto* ptr) {
        using float_t = std::remove_pointer_t<decltype(ptr)>;
        const float_t pattern[4] = {(float_t)0.0, (float_t)0.0, (float_t)0.0, (float_t)1.0};
        for (size_t i = 0; i < numPixels; ++i) {
            memcpy(ptr + i * 4, pattern, sizeof(float_t) * 4);
        }
    };

    if (format == EPixelFormat::F32) {
        init((float*)data->data());
    } else if (format == EPixelFormat::F16) {
        init((half*)data->data());
    } else {
        throw ImageLoadError{"Unsupported pixel format."};
    }

    if (numColorChannels > 1) {
        const vector<string_view> channelNames = {"R", "G", "B"};
        for (int c = 0; c < numColorChannels; ++c) {
            string name = fmt::format("{}{}", namePrefix, (c < (int)channelNames.size() ? channelNames[c] : to_string(c)));

            // We assume that the channels are interleaved.
            channels.emplace_back(name, size, format, desiredFormat, data, c * numBytesPerSample, 4 * numBytesPerSample);
        }
    } else {
        channels.emplace_back(fmt::format("{}L", namePrefix), size, format, desiredFormat, data, 0, 4 * numBytesPerSample);
    }

    if (hasAlpha) {
        channels.emplace_back(fmt::format("{}A", namePrefix), size, format, desiredFormat, data, 3 * numBytesPerSample, 4 * numBytesPerSample);
    }

    return channels;
}

vector<Channel> ImageLoader::makeNChannels(
    int numChannels, const Vector2i& size, EPixelFormat format, EPixelFormat desiredFormat, string_view namePrefix
) {
    vector<Channel> channels;
    for (int c = 0; c < numChannels; ++c) {
        channels.emplace_back(fmt::format("{}{}", namePrefix, to_string(c)), size, format, desiredFormat);
    }

    return channels;
}

Task<void> ImageLoader::resizeChannelsAsync(const vector<Channel>& srcChannels, vector<Channel>& dstChannels, int priority) {
    TEV_ASSERT(srcChannels.size() == dstChannels.size(), "Number of source and destination channels must match.");
    if (srcChannels.empty()) {
        co_return;
    }

    const Vector2i size = srcChannels.front().size();
    const Vector2i targetSize = dstChannels.front().size();
    const int numChannels = (int)srcChannels.size();

    for (int i = 1; i < numChannels; ++i) {
        TEV_ASSERT(srcChannels[i].size() == size, "Source channels' size must match.");
        TEV_ASSERT(dstChannels[i].size() == targetSize, "Destination channels' size must match.");
    }

    co_await ThreadPool::global().parallelForAsync<int>(
        0,
        targetSize.y(),
        [&](int dstY) {
            const float scaleX = (float)size.x() / targetSize.x();
            const float scaleY = (float)size.y() / targetSize.y();

            for (int dstX = 0; dstX < targetSize.x(); ++dstX) {
                const float srcX = (dstX + 0.5f) * scaleX - 0.5f;
                const float srcY = (dstY + 0.5f) * scaleY - 0.5f;

                const int x0 = std::max((int)std::floor(srcX), 0);
                const int y0 = std::max((int)std::floor(srcY), 0);
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

                const size_t dstIdx = dstY * (size_t)targetSize.x() + dstX;

                const size_t srcIdx00 = y0 * (size_t)size.x() + x0;
                const size_t srcIdx01 = y0 * (size_t)size.x() + x1;
                const size_t srcIdx10 = y1 * (size_t)size.x() + x0;
                const size_t srcIdx11 = y1 * (size_t)size.x() + x1;

                for (int c = 0; c < numChannels; ++c) {
                    const float p00 = srcChannels[c].at(srcIdx00);
                    const float p01 = srcChannels[c].at(srcIdx01);
                    const float p10 = srcChannels[c].at(srcIdx10);
                    const float p11 = srcChannels[c].at(srcIdx11);

                    dstChannels[c].setAt(dstIdx, w00 * p00 + w01 * p01 + w10 * p10 + w11 * p11);
                }
            }
        },
        priority
    );
}

} // namespace tev
