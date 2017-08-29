// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include "../include/Channel.h"
#include "../include/GlTexture.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

TEV_NAMESPACE_BEGIN

class Image {
public:
    Image(const std::string& filename);

    const auto& name() {
        return mName;
    }

    std::string shortName();

    bool hasChannel(const std::string& channelName) const {
        return mChannels.count(channelName) != 0;
    }

    const Channel* channel(const std::string& channelName) const {
        if (hasChannel(channelName)) {
            return &mChannels.at(channelName);
        } else {
            return nullptr;
        }
    }

    std::vector<std::string> channelsInLayer(std::string layerName) const;

    const GlTexture* texture(const std::string& channelName);

    const auto& size() const {
        return mSize;
    }

    const auto& layers() const {
        return mLayers;
    }

private:
    void readStbi(const std::string& filename);
    void readExr(const std::string& filename);

    std::string mName;
    Eigen::Vector2i mSize;

    size_t mNumChannels;
    std::map<std::string, Channel> mChannels;
    std::map<std::string, GlTexture> mTextures;

    std::vector<std::string> mLayers;
};

std::shared_ptr<Image> tryLoadImage(std::string filename);

TEV_NAMESPACE_END
