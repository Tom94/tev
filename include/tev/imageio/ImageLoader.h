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

#pragma once

#include <tev/Channel.h>
#include <tev/Common.h>
#include <tev/Image.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/GainMap.h>

#include <nanogui/vector.h>

#include <istream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace tev {

template <bool SRGB_TO_LINEAR = false>
Task<void> yCbCrToRgb(
    float* data,
    const nanogui::Vector2i& size,
    size_t numSamplesPerPixel,
    int priority,
    const nanogui::Vector4f& coeffs = {1.402f, -0.344136f, -0.714136f, 1.772f}
) {
    if (numSamplesPerPixel < 3) {
        tlog::warning() << "Cannot convert from YCbCr to RGB: not enough channels.";
        co_return;
    }

    const auto numPixels = (size_t)size.x() * size.y();
    co_await ThreadPool::global().parallelForAsync<size_t>(
        0,
        numPixels,
        numPixels * 3,
        [&coeffs, numSamplesPerPixel, data](size_t i) {
            const float Y = data[i * numSamplesPerPixel + 0];
            const float Cb = data[i * numSamplesPerPixel + 1] - 0.5f;
            const float Cr = data[i * numSamplesPerPixel + 2] - 0.5f;

            // BT.601 conversion
            float r = Y + coeffs[0] * Cr;
            float g = Y + coeffs[1] * Cb + coeffs[2] * Cr;
            float b = Y + coeffs[3] * Cb;

            if constexpr (SRGB_TO_LINEAR) {
                r = toLinear(r);
                g = toLinear(g);
                b = toLinear(b);
            }

            data[i * numSamplesPerPixel + 0] = r;
            data[i * numSamplesPerPixel + 1] = g;
            data[i * numSamplesPerPixel + 2] = b;
        },
        priority
    );
}

template <typename T, bool SRGB_TO_LINEAR = false, bool MULTIPLY_ALPHA = false>
Task<void> toFloat32(
    const T* __restrict imageData,
    size_t numSamplesPerPixelIn,
    float* __restrict floatData,
    size_t numSamplesPerPixelOut,
    const nanogui::Vector2i& size,
    bool hasAlpha,
    int priority,
    // 0 defaults to 1/(2**bitsPerSample-1)
    float scale = 0.0f,
    // 0 defaults to numSamplesPerPixelIn * size.x()
    size_t numSamplesPerRowIn = 0,
    size_t numSamplesPerRowOut = 0
) {
    if constexpr (std::is_integral_v<T>) {
        if (scale == 0.0f) {
            scale = 1.0f / (((size_t)1 << (sizeof(T) * 8)) - 1);
        }
    } else {
        if (scale == 0.0f) {
            scale = 1.0f;
        }
    }

    if (numSamplesPerRowIn == 0) {
        numSamplesPerRowIn = numSamplesPerPixelIn * size.x();
    }

    if (numSamplesPerRowOut == 0) {
        numSamplesPerRowOut = numSamplesPerPixelOut * size.x();
    }

    const size_t numSamplesPerPixel = std::min(numSamplesPerPixelIn, numSamplesPerPixelOut);
    const size_t numPixels = (size_t)size.x() * size.y();

    co_await ThreadPool::global().parallelForAsync<int>(
        0,
        size.y(),
        numPixels * numSamplesPerPixel,
        [&](int y) {
            size_t rowIdxIn = y * numSamplesPerRowIn;
            size_t rowIdxOut = y * numSamplesPerRowOut;

            for (int x = 0; x < size.x(); ++x) {
                size_t baseIdxIn = rowIdxIn + x * numSamplesPerPixelIn;
                size_t baseIdxOut = rowIdxOut + x * numSamplesPerPixelOut;

                for (size_t c = 0; c < numSamplesPerPixel; ++c) {
                    if (hasAlpha && c == numSamplesPerPixelIn - 1) {
                        // Copy alpha channel to the last output channel without conversion
                        floatData[baseIdxOut + numSamplesPerPixelOut - 1] = (float)imageData[baseIdxIn + c] * scale;
                    } else {
                        float result;
                        if constexpr (SRGB_TO_LINEAR) {
                            result = toLinear((float)imageData[baseIdxIn + c] * scale);
                        } else {
                            result = (float)imageData[baseIdxIn + c] * scale;
                        }

                        if constexpr (MULTIPLY_ALPHA) {
                            if (hasAlpha) {
                                result *= (float)imageData[baseIdxIn + numSamplesPerPixelIn - 1] * scale;
                            }
                        }

                        floatData[baseIdxOut + c] = result;
                    }
                }
            }
        },
        priority
    );
}

struct ImageLoaderSettings {
    GainmapHeadroom gainmapHeadroom = {};
    bool dngApplyCameraProfile = false;
};

class ImageLoader {
public:
    class FormatNotSupported : public std::runtime_error {
    public:
        FormatNotSupported(const std::string& message) : std::runtime_error{message} {}
    };

    virtual ~ImageLoader() {}

    virtual Task<std::vector<ImageData>> load(
        std::istream& iStream, const fs::path& path, std::string_view channelSelector, const ImageLoaderSettings& settings, int priority
    ) const = 0;

    virtual std::string name() const = 0;

    static const std::vector<std::unique_ptr<ImageLoader>>& getLoaders();

    // Returns a list of all supported mime types, sorted by decoding preference.
    static const std::vector<std::string_view>& supportedMimeTypes();

    static Task<std::vector<Channel>> makeRgbaInterleavedChannels(
        size_t numChannels,
        size_t numInterleavedDims,
        bool hasAlpha,
        const nanogui::Vector2i& size,
        EPixelFormat format,
        EPixelFormat desiredFormat,
        std::string_view layer,
        int priority
    );

    static std::vector<Channel> makeNChannels(
        size_t numChannels, const nanogui::Vector2i& size, EPixelFormat format, EPixelFormat desiredFormat, std::string_view layer
    );

    static Task<void> resizeChannelsAsync(
        std::span<const Channel> srcChannels, std::vector<Channel>& dstChannels, const std::optional<Box2i>& dstBox, int priority
    );
    static Task<void>
        resizeImageData(ImageData& resultData, const nanogui::Vector2i& targetSize, const std::optional<Box2i>& targetBox, int priority);
};

} // namespace tev
