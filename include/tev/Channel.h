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
    Channel(const std::string& name, nanogui::Vector2i size);

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
        if (index.x() < 0 || index.x() >= mCols ||
            index.y() < 0 || index.y() >= mRows) {
            return 0;
        }

        return mData[index.x() + index.y() * mCols];
    }

    float& at(size_t index) {
        return mData[index];
    }

    float at(size_t index) const {
        return mData[index];
    }

    float& at(nanogui::Vector2i index) {
        return at(index.x() + index.y() * mCols);
    }

    float at(nanogui::Vector2i index) const {
        return at(index.x() + index.y() * mCols);
    }

    size_t count() const {
        return mData.size();
    }

    nanogui::Vector2i size() const {
        return {mCols, mRows};
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
        return {min, max, mean/count()};
    }

    Task<void> divideByAsync(const Channel& other, int priority);

    Task<void> multiplyWithAsync(const Channel& other, int priority);

    void setZero() { std::memset(mData.data(), 0, mData.size()*sizeof(float)); }

    void updateTile(int x, int y, int width, int height, const std::vector<float>& newData);

    static std::pair<std::string, std::string> split(const std::string& fullChannel);

    static std::string tail(const std::string& fullChannel);
    static std::string head(const std::string& fullChannel);

    static bool isTopmost(const std::string& fullChannel);

    static nanogui::Color color(std::string fullChannel);

private:
    std::string mName;
    int mCols, mRows;
    std::vector<float> mData;
};

TEV_NAMESPACE_END
