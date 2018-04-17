// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Channel.h>
#include <tev/GlTexture.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

TEV_NAMESPACE_BEGIN

class Image {
public:
    Image(const filesystem::path& path, const std::string& channelSelector);

    const filesystem::path& path() const {
        return mPath;
    }

    const std::string& channelSelector() const {
        return mChannelSelector;
    }

    const std::string& name() const {
        return mName;
    }

    std::string shortName() const;

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

    const GlTexture* texture(const std::vector<std::string>& channelNames);

    std::vector<std::string> channelsInLayer(std::string layerName) const;

    Eigen::Vector2i size() const {
        return mChannels.begin()->second.size();
    }

    Eigen::DenseIndex count() const {
        return mChannels.begin()->second.count();
    }

    const std::vector<std::string>& layers() const {
        return mLayers;
    }

    int id() const {
        return mId;
    }

    std::string toString() const;

private:
    static std::atomic<int> sId;

    void ensureValid();

    void readStbi(std::ifstream& f);
    void readExr(std::ifstream& f);

    filesystem::path mPath;
    std::string mChannelSelector;

    std::string mName;

    std::map<std::string, Channel> mChannels;
    std::map<std::string, GlTexture> mTextures;

    std::vector<std::string> mLayers;

    const int mId;
};

std::shared_ptr<Image> tryLoadImage(filesystem::path path, std::string channelSelector);

TEV_NAMESPACE_END
