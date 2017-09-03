// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include "../include/GlTexture.h"

#include <vector>
#include <string>

TEV_NAMESPACE_BEGIN

class Channel {
public:
    Channel(const std::string& name, Eigen::Vector2i size);

    const auto& name() const {
        return mName;
    }

    auto& data() {
        return mData;
    }

    const auto& data() const {
        return mData;
    }

    float eval(size_t index) const {
        if (index >= mData.size()) {
            return 0;
        }
        return mData[index];
    }

    float eval(Eigen::Vector2i index) const {
        if (index.x() < 0 || index.x() >= mSize.x() ||
            index.y() < 0 || index.y() >= mSize.y()) {
            return 0;
        }

        size_t i = index.x() + index.y() * mSize.x();
        return mData[i];
    }

    size_t count() const {
        return mData.size();
    }

    Eigen::Vector2i size() const {
        return mSize;
    }

    static std::pair<std::string, std::string> split(const std::string& fullChannel);

    static std::string tail(const std::string& fullChannel);
    static std::string head(const std::string& fullChannel);

    static bool isTopmost(const std::string& fullChannel);

    static nanogui::Color color(std::string fullChannel);

private:
    std::string mName;
    std::vector<float> mData;
    Eigen::Vector2i mSize;
};

TEV_NAMESPACE_END
