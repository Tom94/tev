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

#include <tev/Common.h>
#include <tev/Task.h>

#include <nanogui/vector.h>

#include <string>
#include <vector>

namespace tev {

class Channel {
public:
    static std::pair<std::string, std::string> split(const std::string& fullChannel);

    static std::string tail(const std::string& fullChannel);
    static std::string head(const std::string& fullChannel);

    static bool isTopmost(const std::string& fullChannel);
    static bool isAlpha(const std::string& fullChannel);

    static nanogui::Color color(std::string fullChannel);

    Channel(
        const std::string& name,
        const nanogui::Vector2i& size,
        std::shared_ptr<std::vector<float>> data = nullptr,
        size_t dataOffset = 0,
        size_t dataStride = 1
    );

    const std::string& name() const { return mName; }
    void setName(const std::string& name) { mName = name; }

    float eval(nanogui::Vector2i index) const {
        if (index.x() < 0 || index.x() >= mSize.x() || index.y() < 0 || index.y() >= mSize.y()) {
            return 0;
        }

        return at(index.x() + (size_t)index.y() * (size_t)mSize.x());
    }

    float& at(nanogui::Vector2i index) { return at(index.x() + index.y() * (size_t)mSize.x()); }

    float at(nanogui::Vector2i index) const { return at(index.x() + index.y() * (size_t)mSize.x()); }

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
        if (mDataStride == 1) {
            memset(data(), 0, numPixels() * sizeof(float));
        } else {
            const size_t nPixels = numPixels();
            for (size_t i = 0; i < nPixels; ++i) {
                at(i) = 0.0f;
            }
        }
    }

    void updateTile(int x, int y, int width, int height, const std::vector<float>& newData);

    float& at(size_t index) { return data()[index * mDataStride]; }

    float at(size_t index) const { return data()[index * mDataStride]; }

    float* data() const { return mData->data() + mDataOffset; }

    size_t offset() const { return mDataOffset; }
    size_t stride() const { return mDataStride; }

    std::shared_ptr<std::vector<float>>& dataBuf() { return mData; }

private:
    std::string mName;
    nanogui::Vector2i mSize;
    std::shared_ptr<std::vector<float>> mData;
    size_t mDataOffset;
    size_t mDataStride;
};

} // namespace tev
