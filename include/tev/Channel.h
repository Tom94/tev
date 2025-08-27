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

#include <tev/Common.h>
#include <tev/Task.h>

#include <nanogui/vector.h>

#include <half.h>

#include <span>
#include <string>
#include <vector>

namespace tev {

class Channel {
public:
    static std::pair<std::string_view, std::string_view> split(std::string_view fullChannel);

    static std::string_view tail(std::string_view fullChannel);
    static std::string_view head(std::string_view fullChannel);

    static bool isTopmost(std::string_view fullChannel);
    static bool isAlpha(std::string_view fullChannel);

    static nanogui::Color color(std::string_view fullChannel, bool pastel);

    Channel(
        std::string_view name,
        const nanogui::Vector2i& size,
        EPixelFormat format,
        EPixelFormat desiredFormat,
        std::shared_ptr<std::vector<uint8_t>> data = nullptr,
        size_t dataOffset = 0,
        size_t dataStride = 1
    );

    std::string_view name() const { return mName; }
    void setName(std::string_view name) { mName = name; }

    size_t numPixels() const { return (size_t)mSize.x() * mSize.y(); }

    const nanogui::Vector2i& size() const { return mSize; }
    void setSize(const nanogui::Vector2i& size) { mSize = size; }

    std::tuple<float, float, float> minMaxMean() const {
        float min = std::numeric_limits<float>::infinity();
        float max = -std::numeric_limits<float>::infinity();
        float mean = 0;

        const size_t nPixels = numPixels();
        for (size_t i = 0; i < nPixels; ++i) {
            const float f = at(i);

            mean += f;
            if (f < min) {
                min = f;
            }

            if (f > max) {
                max = f;
            }
        }

        return {min, max, mean / nPixels};
    }

    Task<void> divideByAsync(const Channel& other, int priority);
    Task<void> multiplyWithAsync(const Channel& other, int priority);

    void setZero() {
        const size_t nBytesPerPixel = nBytes(mPixelFormat);
        if (mDataStride == 1) {
            std::memset(data(), 0, numPixels() * nBytesPerPixel);
        } else {
            const size_t nPixels = numPixels();
            for (size_t i = 0; i < nPixels; ++i) {
                std::memset(data() + i * mDataStride, 0, nBytesPerPixel);
            }
        }
    }

    void updateTile(int x, int y, int width, int height, std::span<const float> newData);

    float at(nanogui::Vector2i index) const { return at(index.x() + index.y() * (size_t)mSize.x()); }
    float at(size_t index) const {
        switch (mPixelFormat) {
            case EPixelFormat::U8: return *dataAt(index);
            case EPixelFormat::U16: return *(const uint16_t*)dataAt(index);
            case EPixelFormat::F16: return *(const half*)dataAt(index);
            case EPixelFormat::F32: return *(const float*)dataAt(index);
        }

        return 0;
    }

    void setAt(nanogui::Vector2i index, float value) { setAt(index.x() + index.y() * (size_t)mSize.x(), value); }
    void setAt(size_t index, float value) {
        switch (mPixelFormat) {
            case EPixelFormat::U8: *dataAt(index) = (uint8_t)value; break;
            case EPixelFormat::U16: *(uint16_t*)dataAt(index) = (uint16_t)value; break;
            case EPixelFormat::F16: *(half*)dataAt(index) = (half)value; break;
            case EPixelFormat::F32: *(float*)dataAt(index) = value; break;
        }
    }

    float eval(nanogui::Vector2i index) const {
        if (index.x() < 0 || index.x() >= mSize.x() || index.y() < 0 || index.y() >= mSize.y()) {
            return 0;
        }

        return at(index.x() + (size_t)index.y() * (size_t)mSize.x());
    }

    uint8_t* data() const { return mData->data() + mDataOffset; }
    uint8_t* dataAt(size_t index) const { return data() + index * mDataStride; }

    float* floatData() const {
        if (mPixelFormat != EPixelFormat::F32) {
            throw std::runtime_error{"Channel is not in F32 format."};
        }

        return (float*)data();
    }

    half* halfData() const {
        if (mPixelFormat != EPixelFormat::F16) {
            throw std::runtime_error{"Channel is not in F16 format."};
        }

        return (half*)data();
    }

    void setOffset(size_t offset) { mDataOffset = offset; }
    size_t offset() const { return mDataOffset; }

    void setStride(size_t stride) { mDataStride = stride; }
    size_t stride() const { return mDataStride; }

    std::shared_ptr<std::vector<uint8_t>>& dataBuf() { return mData; }

    EPixelFormat desiredPixelFormat() const { return mDesiredPixelFormat; }

    void setPixelFormat(EPixelFormat format) { mPixelFormat = format; }
    EPixelFormat pixelFormat() const { return mPixelFormat; }

private:
    std::string mName;
    nanogui::Vector2i mSize;

    EPixelFormat mPixelFormat = EPixelFormat::F32;

    // tev defaults to storing images in fp32 for maximum precision. However, many images only require fp16 to be displayed as good as
    // losslessly. For such images, loaders can set this to F16 to save memory.
    EPixelFormat mDesiredPixelFormat = EPixelFormat::F32;

    std::shared_ptr<std::vector<uint8_t>> mData;
    size_t mDataOffset;
    size_t mDataStride;
};

} // namespace tev
