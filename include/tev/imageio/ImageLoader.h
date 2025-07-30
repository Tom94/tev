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

#include "tev/Common.h"
#include <tev/Channel.h>
#include <tev/Image.h>
#include <tev/ThreadPool.h>

#include <nanogui/vector.h>

#include <istream>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <vector>

namespace tev {

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

    size_t numSamplesPerPixel = std::min(numSamplesPerPixelIn, numSamplesPerPixelOut);
    co_await ThreadPool::global().parallelForAsync<int>(
        0,
        size.y(),
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
                            result = toLinear(imageData[baseIdxIn + c] * scale);
                        } else {
                            result = imageData[baseIdxIn + c] * scale;
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

class ImageLoader {
public:
    class FormatNotSupported : public std::runtime_error {
    public:
        FormatNotSupported(const std::string& message) : std::runtime_error{message} {}
    };

    virtual ~ImageLoader() {}

    virtual Task<std::vector<ImageData>>
        load(std::istream& iStream, const fs::path& path, std::string_view channelSelector, int priority, bool applyGainmaps) const = 0;

    virtual std::string name() const = 0;

    static const std::vector<std::unique_ptr<ImageLoader>>& getLoaders();

    // Returns a list of all supported mime types, sorted by decoding preference.
    static const std::vector<std::string_view>& supportedMimeTypes();

    static std::vector<Channel> makeRgbaInterleavedChannels(
        int numChannels, bool hasAlpha, const nanogui::Vector2i& size, EPixelFormat desiredPixelFormat, std::string_view namePrefix = ""
    );

    static std::vector<Channel>
        makeNChannels(int numChannels, const nanogui::Vector2i& size, EPixelFormat desiredPixelFormat, std::string_view namePrefix = "");

    static Task<void> resizeChannelsAsync(const std::vector<Channel>& srcChannels, std::vector<Channel>& dstChannels, int priority);
};

} // namespace tev
