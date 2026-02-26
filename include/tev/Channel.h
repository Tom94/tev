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

#include <memory>
#include <span>
#include <string>

namespace tev {

template <typename T> EPixelFormat pixelFormatForType() {
    using base_t = std::remove_cv_t<T>;
    if constexpr (std::is_same_v<base_t, uint8_t>) {
        return EPixelFormat::U8;
    } else if constexpr (std::is_same_v<base_t, uint16_t>) {
        return EPixelFormat::U16;
    } else if constexpr (std::is_same_v<base_t, int8_t>) {
        return EPixelFormat::I8;
    } else if constexpr (std::is_same_v<base_t, int16_t>) {
        return EPixelFormat::I16;
    } else if constexpr (std::is_same_v<base_t, half>) {
        return EPixelFormat::F16;
    } else if constexpr (std::is_same_v<base_t, float>) {
        return EPixelFormat::F32;
    } else {
        static_assert(false, "Unsupported type for pixel format.");
    }
}

template <typename T> class ChannelView {
public:
    ChannelView(T* data, size_t dataStride, size_t dataOffset, const nanogui::Vector2i& size) :
        mData{data}, mDataOffset{dataOffset}, mDataStride{dataStride}, mSize{size} {}

    float operator[](size_t i) const {
        const auto v = mData[mDataOffset + i * mDataStride];
        if constexpr (std::is_integral_v<T>) {
            return (float)v * (1.0f / (float)std::numeric_limits<T>::max());
        } else {
            return (float)v;
        }
    }

    float operator[](int x, int y) const { return operator[](x + y * (size_t)mSize.x()); }

    void setAt(size_t i, float value) const {
        T& val = mData[mDataOffset + i * mDataStride];
        if constexpr (std::is_integral_v<T>) {
            val = (T)(value * (float)std::numeric_limits<T>::max() + 0.5f);
        } else {
            val = (T)value;
        }
    }

    void setAt(int x, int y, float value) const { setAt(x + y * (size_t)mSize.x(), value); }

    const nanogui::Vector2i& size() const { return mSize; }

private:
    T* mData = nullptr;
    size_t mDataOffset = 0;
    size_t mDataStride = 1;
    nanogui::Vector2i mSize = {0};
};

class Channel {
public:
    using Data = HeapArray<uint8_t>;

    static std::pair<std::string_view, std::string_view> split(std::string_view fullChannel);
    static std::string join(std::string_view layer, std::string_view channel);
    static std::string joinIfNonempty(std::string_view layer, std::string_view channel);

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
        std::shared_ptr<Data> data = nullptr,
        size_t dataOffset = 0,
        size_t dataStride = 1
    );

    std::string_view name() const { return mName; }
    void setName(std::string_view name) { mName = name; }

    bool isAlpha() const { return Channel::isAlpha(mName); }
    bool isTopmost() const { return Channel::isTopmost(mName); }

    size_t numPixels() const { return (size_t)mSize.x() * mSize.y(); }

    const nanogui::Vector2i& size() const { return mSize; }
    void setSize(const nanogui::Vector2i& size) { mSize = size; }

    std::tuple<float, float, float> minMaxMean() const {
        float min = std::numeric_limits<float>::infinity();
        float max = -std::numeric_limits<float>::infinity();
        float mean = 0;

        const size_t nPixels = numPixels();
        for (size_t i = 0; i < nPixels; ++i) {
            const float f = dynamicAt(i);

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

    template <typename T> ChannelView<T> view() const & {
        static_assert(std::is_const_v<T>, "ChannelView must be const when returned from a const Channel.");
        if (pixelFormatForType<T>() != mPixelFormat) {
            throw std::runtime_error{"Channel pixel format does not match requested type."};
        }

        return ChannelView<T>{reinterpret_cast<T*>(mData->data()), mDataStride / sizeof(T), mDataOffset / sizeof(T), mSize};
    }

    template <typename T> ChannelView<T> view() & {
        if (pixelFormatForType<T>() != mPixelFormat) {
            throw std::runtime_error{"Channel pixel format does not match requested type."};
        }

        return ChannelView<T>{reinterpret_cast<T*>(mData->data()), mDataStride / sizeof(T), mDataOffset / sizeof(T), mSize};
    }

    // NOTE: Prefer using view<T>() for better performance when the type of the channel is known. E.g. most of tev's image loading routines
    // use view<float>(), because that's the format that tev used until an image is finished loading. Only use the dynamicAt()/dynamicSetAt()/evalOrZero()
    // members when accessing channels of images that have already completed loading (e.g. for UI or statistics purposes).
    float dynamicAt(nanogui::Vector2i index) const { return dynamicAt(index.x() + index.y() * (size_t)mSize.x()); }
    float dynamicAt(size_t index) const {
        switch (mPixelFormat) {
            case EPixelFormat::U8: return *dataAt(index) / (float)std::numeric_limits<uint8_t>::max();
            case EPixelFormat::U16: return *(const uint16_t*)dataAt(index) / (float)std::numeric_limits<uint16_t>::max();
            case EPixelFormat::I8: return *(const int8_t*)dataAt(index) / (float)std::numeric_limits<int8_t>::max();
            case EPixelFormat::I16: return *(const int16_t*)dataAt(index) / (float)std::numeric_limits<int16_t>::max();
            case EPixelFormat::F16: return *(const half*)dataAt(index);
            case EPixelFormat::F32: return *(const float*)dataAt(index);
        }

        return 0;
    }

    void dynamicSetAt(nanogui::Vector2i index, float value) { dynamicSetAt(index.x() + index.y() * (size_t)mSize.x(), value); }
    void dynamicSetAt(size_t index, float value) {
        switch (mPixelFormat) {
            case EPixelFormat::U8:
                *dataAt(index) = (uint8_t)(std::clamp(value, 0.0f, 1.0f) * std::numeric_limits<uint8_t>::max() + 0.5f);
                break;
            case EPixelFormat::U16:
                *(uint16_t*)dataAt(index) = (uint16_t)(std::clamp(value, 0.0f, 1.0f) * std::numeric_limits<uint16_t>::max() + 0.5f);
                break;
            case EPixelFormat::I8:
                *(int8_t*)dataAt(index) = (int8_t)(std::clamp(value, -1.0f, 1.0f) * std::numeric_limits<int8_t>::max() + 0.5f);
                break;
            case EPixelFormat::I16:
                *(int16_t*)dataAt(index) = (int16_t)(std::clamp(value, -1.0f, 1.0f) * std::numeric_limits<int16_t>::max() + 0.5f);
                break;
            case EPixelFormat::F16: *(half*)dataAt(index) = (half)value; break;
            case EPixelFormat::F32: *(float*)dataAt(index) = value; break;
        }
    }

    float evalOrZero(nanogui::Vector2i index) const {
        if (index.x() < 0 || index.x() >= mSize.x() || index.y() < 0 || index.y() >= mSize.y()) {
            return 0;
        }

        return dynamicAt(index);
    }

    // TODO: floatData is currently used in a *very* unsafe manner to access interleaved channel buffers. Better to have a MultiChannelView
    // class that provides typed access to interleaved buffers. Potentially Data could know about its format and interleaving to make this
    // seamless.
    float* floatData() const {
        if (mPixelFormat != EPixelFormat::F32) {
            throw std::runtime_error{"Channel is not in F32 format."};
        }

        return (float*)data();
    }

    void setOffset(size_t offset) { mDataOffset = offset; }
    size_t offset() const { return mDataOffset; }

    void setStride(size_t stride) { mDataStride = stride; }
    size_t stride() const { return mDataStride; }

    std::shared_ptr<Data>& dataBuf() { return mData; }
    const std::shared_ptr<Data>& dataBuf() const { return mData; }

    EPixelFormat desiredPixelFormat() const { return mDesiredPixelFormat; }

    void setPixelFormat(EPixelFormat format) { mPixelFormat = format; }
    EPixelFormat pixelFormat() const { return mPixelFormat; }

private:
    uint8_t* data() const { return mData->data() + mDataOffset; }

    uint8_t* dataAt(nanogui::Vector2i index) const { return dataAt(index.x() + index.y() * (size_t)mSize.x()); }
    uint8_t* dataAt(size_t index) const { return data() + index * mDataStride; }

    std::string mName;
    nanogui::Vector2i mSize;

    EPixelFormat mPixelFormat = EPixelFormat::F32;

    // tev defaults to storing images in fp32 for maximum precision. However, many images only require fp16 to be displayed as good as
    // losslessly. For such images, loaders can set this to F16 to save memory.
    EPixelFormat mDesiredPixelFormat = EPixelFormat::F32;

    std::shared_ptr<Data> mData;
    size_t mDataOffset;
    size_t mDataStride;
};

} // namespace tev
