// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>
#include <tev/Task.h>

#include <nanogui/vector.h>

#include <future>
#include <string>
#include <vector>

TEV_NAMESPACE_BEGIN

class Channel {
public:
    static std::pair<std::string, std::string> split(const std::string& fullChannel);

    static std::string tail(const std::string& fullChannel);
    static std::string head(const std::string& fullChannel);

    static bool isTopmost(const std::string& fullChannel);
    static bool isAlpha(const std::string& fullChannel);

    static nanogui::Color color(std::string fullChannel);

    Channel(const std::string& name, const nanogui::Vector2i& size);

    const std::string& name() const {
        return mName;
    }

    const std::vector<float>& data() const {
        return mData;
    }

    float eval(size_t index) const {
        if (index >= mData.size()) {
            return 0;
        }
        return mData[index];
    }

    float eval(nanogui::Vector2i index) const {
        if (index.x() < 0 || index.x() >= mSize.x() ||
            index.y() < 0 || index.y() >= mSize.y()) {
            return 0;
        }

        return mData[index.x() + index.y() * mSize.x()];
    }

    float& at(size_t index) {
        return mData[index];
    }

    float at(size_t index) const {
        return mData[index];
    }

    float& at(nanogui::Vector2i index) {
        return at(index.x() + index.y() * mSize.x());
    }

    float at(nanogui::Vector2i index) const {
        return at(index.x() + index.y() * mSize.x());
    }

    size_t numPixels() const {
        return mData.size();
    }

    const nanogui::Vector2i& size() const {
        return mSize;
    }

    std::tuple<float, float, float> minMaxMean() const {
        float min = std::numeric_limits<float>::infinity();
        float max = -std::numeric_limits<float>::infinity();
        float mean = 0;
        for (float f : mData) {
            mean += f;
            if (f < min) {
                min = f;
            }
            if (f > max) {
                max = f;
            }
        }
        return {min, max, mean/numPixels()};
    }

    Task<void> divideByAsync(const Channel& other, int priority);

    Task<void> multiplyWithAsync(const Channel& other, int priority);

    void setZero() { memset(mData.data(), 0, mData.size()*sizeof(float)); }

    void updateTile(int x, int y, int width, int height, const std::vector<float>& newData);

private:
    std::string mName;
    nanogui::Vector2i mSize;
    std::vector<float> mData;
};

TEV_NAMESPACE_END
