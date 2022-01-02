// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Box.h>
#include <tev/Channel.h>
#include <tev/SharedQueue.h>
#include <tev/ThreadPool.h>

#include <nanogui/texture.h>

#include <atomic>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

TEV_NAMESPACE_BEGIN

class ImageLoader;

struct ImageData {
    ImageData() = default;
    ImageData(const ImageData&) = delete;
    ImageData(ImageData&&) = default;

    std::vector<Channel> channels;
    std::vector<std::string> layers;
    nanogui::Matrix4f toRec709 = nanogui::Matrix4f{1.0f}; // Identity by default
    bool hasPremultipliedAlpha;

    Box2i dataWindow;
    Box2i displayWindow;

    std::string partName;

    nanogui::Vector2i size() const {
        return dataWindow.size();
    }

    nanogui::Vector2i displaySize() const {
        return displayWindow.size();
    }

    size_t numPixels() const {
        return channels.front().numPixels();
    }

    std::vector<std::string> channelsInLayer(std::string layerName) const;

    Task<void> convertToRec709(int priority);

    void alphaOperation(const std::function<void(Channel&, const Channel&)>& func);

    Task<void> multiplyAlpha(int priority);
    Task<void> unmultiplyAlpha(int priority);

    Task<void> ensureValid(const std::string& channelSelector, int taskPriority);

    bool hasChannel(const std::string& channelName) const {
        return channel(channelName) != nullptr;
    }

    const Channel* channel(const std::string& channelName) const {
        auto it = std::find_if(
            std::begin(channels),
            std::end(channels),
            [&channelName](const Channel& c) { return c.name() == channelName; }
        );

        if (it != std::end(channels)) {
            return &(*it);
        } else {
            return nullptr;
        }
    }

    Channel* mutableChannel(const std::string& channelName) {
        auto it = std::find_if(
            std::begin(channels),
            std::end(channels),
            [&channelName](const Channel& c) { return c.name() == channelName; }
        );

        if (it != std::end(channels)) {
            return &(*it);
        } else {
            return nullptr;
        }
    }
};

struct ChannelGroup {
    std::string name;
    std::vector<std::string> channels;
};

struct ImageTexture {
    nanogui::ref<nanogui::Texture> nanoguiTexture;
    std::vector<std::string> channels;
    bool mipmapDirty;
};

class Image {
public:
    Image(const fs::path& path, ImageData&& data, const std::string& channelSelector);
    virtual ~Image();

    const fs::path& path() const {
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
        return mData.hasChannel(channelName);
    }

    const Channel* channel(const std::string& channelName) const {
        return mData.channel(channelName);
    }

    nanogui::Texture* texture(const std::string& channelGroupName);
    nanogui::Texture* texture(const std::vector<std::string>& channelNames);

    std::vector<std::string> channelsInGroup(const std::string& groupName) const;
    std::vector<std::string> getSortedChannels(const std::string& layerName) const;

    nanogui::Vector2i size() const {
        return mData.size();
    }

    const Box2i& dataWindow() const {
        return mData.dataWindow;
    }

    const Box2i& displayWindow() const {
        return mData.displayWindow;
    }

    nanogui::Vector2f centerDisplayOffset(const Box2i& displayWindow) const {
        return Box2f{dataWindow()}.middle() - Box2f{displayWindow}.middle();
    }

    size_t numPixels() const {
        return mData.numPixels();
    }

    const std::vector<ChannelGroup>& channelGroups() const {
        return mChannelGroups;
    }

    int id() const {
        return mId;
    }

    void bumpId() {
        int oldId = mId;
        mId = sId++;

        if (mStaleIdCallback) {
            mStaleIdCallback(oldId);
        }
    }

    static int drawId() {
        return sId++;
    }

    void updateChannel(const std::string& channelName, int x, int y, int width, int height, const std::vector<float>& data);

    void setStaleIdCallback(const std::function<void(int)>& callback) {
        mStaleIdCallback = callback;
    }

    std::string toString() const;

private:
    static std::atomic<int> sId;

    Channel* mutableChannel(const std::string& channelName) {
        return mData.mutableChannel(channelName);
    }

    std::vector<ChannelGroup> getGroupedChannels(const std::string& layerName) const;

    fs::path mPath;
    std::string mChannelSelector;

    std::string mName;

    std::map<std::string, ImageTexture> mTextures;

    ImageData mData;

    std::vector<ChannelGroup> mChannelGroups;

    std::function<void(int)> mStaleIdCallback;

    int mId;
};

Task<std::vector<std::shared_ptr<Image>>> tryLoadImage(int imageId, fs::path path, std::istream& iStream, std::string channelSelector);
Task<std::vector<std::shared_ptr<Image>>> tryLoadImage(fs::path path, std::istream& iStream, std::string channelSelector);
Task<std::vector<std::shared_ptr<Image>>> tryLoadImage(int imageId, fs::path path, std::string channelSelector);
Task<std::vector<std::shared_ptr<Image>>> tryLoadImage(fs::path path, std::string channelSelector);

struct ImageAddition {
    int loadId;
    bool shallSelect;
    std::vector<std::shared_ptr<Image>> images;

    struct Comparator {
        bool operator()(const ImageAddition& a, const ImageAddition& b) {
            return a.loadId > b.loadId;
        }
    };
};

class BackgroundImagesLoader {
public:
    void enqueue(const fs::path& path, const std::string& channelSelector, bool shallSelect);
    std::optional<ImageAddition> tryPop() { return mLoadedImages.tryPop(); }

    bool publishSortedLoads();
    bool hasPendingLoads() const {
        return mLoadCounter != mUnsortedLoadCounter;
    }

private:
    SharedQueue<ImageAddition> mLoadedImages;

    std::priority_queue<
        ImageAddition,
        std::vector<ImageAddition>,
        ImageAddition::Comparator
    > mPendingLoadedImages;
    std::mutex mPendingLoadedImagesMutex;

    std::atomic<int> mLoadCounter{0};
    std::atomic<int> mUnsortedLoadCounter{0};
};

TEV_NAMESPACE_END
