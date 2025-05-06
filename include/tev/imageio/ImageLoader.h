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

#pragma once

#include <tev/Channel.h>
#include <tev/Image.h>
#include <tev/ThreadPool.h>

#include <nanogui/vector.h>

#include <istream>
#include <stdexcept>
#include <string>
#include <type_traits>

namespace tev {

// Modifies both `data` and `size`
template <typename T> Task<void> orientToTopLeft(std::vector<T>& data, nanogui::Vector2i& size, EOrientation orientation, int priority) {
    if (orientation == EOrientation::TopLeft) {
        co_return;
    }

    const size_t numPixels = (size_t)size.x() * size.y();
    const size_t numSamplesPerPixel = data.size() / numPixels;

    bool swapAxes = orientation >= EOrientation::LeftTop;
    size = swapAxes ? nanogui::Vector2i{size.y(), size.x()} : size;
    nanogui::Vector2i otherSize = swapAxes ? nanogui::Vector2i{size.y(), size.x()} : size;
    std::vector<T> reorientedData(data.size());

    co_await ThreadPool::global().parallelForAsync<int>(
        0,
        size.y(),
        [&](int y) {
            for (int x = 0; x < size.x(); ++x) {
                const size_t i = y * (size_t)size.x() + x;

                const auto other = applyOrientation(orientation, {x, y}, size);
                const size_t j = other.y() * (size_t)otherSize.x() + other.x();

                for (size_t s = 0; s < numSamplesPerPixel; ++s) {
                    reorientedData[i * numSamplesPerPixel + s] = data[j * numSamplesPerPixel + s];
                }
            }
        },
        priority
    );

    std::swap(data, reorientedData);
}

template <typename T, bool SRGB_TO_LINEAR = false>
Task<void> toFloat32(
    const T* __restrict imageData,
    size_t numSamplesPerPixelIn,
    float* __restrict floatData,
    size_t numSamplesPerPixelOut,
    const nanogui::Vector2i& size,
    bool hasAlpha,
    int priority,
    // 0 defaults to 1/2**bitsPerSample
    float scale = 0.0f,
    // 0 defaults to numSamplesPerPixelIn * size.x()
    size_t numSamplesPerRowIn = 0,
    size_t numSamplesPerRowOut = 0
) {
    if constexpr (std::is_integral_v<T>) {
        if (scale == 0.0f) {
            scale = 1.0f / ((1 << (sizeof(T) * 8)) - 1);
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

    co_await ThreadPool::global().parallelForAsync<int>(
        0,
        size.y(),
        [&](int y) {
            size_t rowIdxIn = y * numSamplesPerRowIn;
            size_t rowIdxOut = y * numSamplesPerRowOut;

            for (int x = 0; x < size.x(); ++x) {
                size_t baseIdxIn = rowIdxIn + x * numSamplesPerPixelIn;
                size_t baseIdxOut = rowIdxOut + x * numSamplesPerPixelOut;

                for (size_t c = 0; c < numSamplesPerPixelIn; ++c) {
                    floatData[baseIdxOut + c] = imageData[baseIdxIn + c] * scale;
                    if constexpr (SRGB_TO_LINEAR) {
                        if (!hasAlpha || c != numSamplesPerPixelOut - 1) {
                            floatData[baseIdxOut + c] = toLinear(floatData[baseIdxOut + c]);
                        }
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

    class LoadError : public std::runtime_error {
    public:
        LoadError(const std::string& message) : std::runtime_error{message} {}
    };

    virtual ~ImageLoader() {}

    virtual Task<std::vector<ImageData>>
        load(std::istream& iStream, const fs::path& path, const std::string& channelSelector, int priority, bool applyGainmaps) const = 0;

    virtual std::string name() const = 0;

    static const std::vector<std::unique_ptr<ImageLoader>>& getLoaders();

protected:
    static std::vector<Channel> makeNChannels(int numChannels, const nanogui::Vector2i& size, const std::string& namePrefix = "");
    static Task<void> resizeChannelsAsync(const std::vector<Channel>& srcChannels, std::vector<Channel>& dstChannels, int priority);
};

} // namespace tev
