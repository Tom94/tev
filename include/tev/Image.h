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

#include <tev/Box.h>
#include <tev/Channel.h>
#include <tev/Common.h>
#include <tev/SharedQueue.h>
#include <tev/ThreadPool.h>
#include <tev/VectorGraphics.h>

#include <nanogui/texture.h>

#include <array>
#include <atomic>
#include <istream>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace tev {

struct AttributeNode {
    std::string name;
    std::string value;
    std::string type;
    std::vector<AttributeNode> children;
};

struct HdrMetadata {
    float maxCLL = 0.0f;
    float maxFALL = 0.0f;
    float whiteLevel = 80.0f;
};

struct ImageData {
    ImageData() = default;
    ImageData(const ImageData&) = delete;
    ImageData(ImageData&&) = default;
    ImageData& operator=(const ImageData&) = delete;
    ImageData& operator=(ImageData&&) = default;

    std::vector<Channel> channels;
    std::vector<std::string> layers;
    nanogui::Matrix3f toRec709 = nanogui::Matrix3f{1.0f}; // Identity by default
    bool hasPremultipliedAlpha = false;
    EOrientation orientation = EOrientation::TopLeft;
    std::vector<AttributeNode> attributes;

    HdrMetadata hdrMetadata;

    Box2i dataWindow;
    Box2i displayWindow;

    std::string partName;

    nanogui::Vector2i size() const { return dataWindow.size(); }

    nanogui::Vector2i displaySize() const { return displayWindow.size(); }

    size_t numPixels() const { return channels.front().numPixels(); }

    std::vector<std::string> channelsInLayer(std::string_view layerName) const;

    Task<void> convertToRec709(int priority);
    Task<void> convertToDesiredPixelFormat(int priority);

    void alphaOperation(const std::function<void(Channel&, const Channel&)>& func);

    Task<void> multiplyAlpha(int priority);
    Task<void> unmultiplyAlpha(int priority);

    Task<void> orientToTopLeft(int priority);

    Task<void> ensureValid(std::string_view channelSelector, int taskPriority);

    bool hasChannel(std::string_view channelName) const { return channel(channelName) != nullptr; }

    const Channel* channel(std::string_view channelName) const {
        auto it = std::find_if(std::begin(channels), std::end(channels), [&channelName](const Channel& c) { return c.name() == channelName; });
        if (it != std::end(channels)) {
            return &(*it);
        } else {
            return nullptr;
        }
    }

    Channel* mutableChannel(std::string_view channelName) {
        auto it = std::find_if(std::begin(channels), std::end(channels), [&channelName](const Channel& c) { return c.name() == channelName; });
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

    bool operator==(const ChannelGroup& other) const { return name == other.name; }
};

struct ImageTexture {
    nanogui::ref<nanogui::Texture> nanoguiTexture;
    std::vector<std::string> channels;
    bool mipmapDirty;
};

class Image {
public:
    Image(const fs::path& path, fs::file_time_type fileLastModified, ImageData&& data, std::string_view channelSelector, bool groupChannels);
    virtual ~Image();

    const fs::path& path() const { return mPath; }

    fs::file_time_type fileLastModified() const { return mFileLastModified; }

    void setFileLastModified(fs::file_time_type value) { mFileLastModified = value; }

    std::string_view channelSelector() const { return mChannelSelector; }

    std::string_view name() const { return mName; }

    std::string shortName() const;

    bool hasChannel(std::string_view channelName) const { return mData.hasChannel(channelName); }

    const Channel* channel(std::string_view channelName) const { return mData.channel(channelName); }
    std::vector<const Channel*> channels(std::span<const std::string> channelNames) const {
        std::vector<const Channel*> result;
        for (const auto& channelName : channelNames) {
            result.push_back(channel(channelName));
        }

        return result;
    }

    bool isInterleaved(std::span<const std::string> channelNames, size_t desiredBytesPerSample, size_t desiredStride) const;

    nanogui::Texture* texture(std::span<const std::string> channelNames, EInterpolationMode minFilter, EInterpolationMode magFilter);

    std::vector<std::string> channelsInGroup(std::string_view groupName) const;
    void decomposeChannelGroup(std::string_view groupName);

    std::vector<std::string> getExistingChannels(std::span<const std::string> requestedChannels) const;

    nanogui::Vector2i size() const { return mData.size(); }

    nanogui::Vector2i displaySize() const { return mData.displaySize(); }

    bool contains(const nanogui::Vector2i& pos) const {
        return pos.x() >= 0 && pos.y() >= 0 && pos.x() < mData.size().x() && pos.y() < mData.size().y();
    }

    const Box2i& dataWindow() const { return mData.dataWindow; }

    const Box2i& displayWindow() const { return mData.displayWindow; }

    nanogui::Vector2f centerDisplayOffset(const Box2i& displayWindow) const {
        return Box2f{dataWindow()}.middle() - Box2f{displayWindow}.middle();
    }

    size_t numPixels() const { return mData.numPixels(); }

    std::span<const ChannelGroup> channelGroups() const { return mChannelGroups; }

    int id() const { return mId; }

    void bumpId() {
        int oldId = mId;
        mId = sId++;

        if (mStaleIdCallback) {
            mStaleIdCallback(oldId);
        }
    }

    static int drawId() { return sId++; }

    void updateChannel(std::string_view channelName, int x, int y, int width, int height, std::span<const float> data);

    void updateVectorGraphics(bool append, std::span<const VgCommand> commands);

    std::span<const VgCommand> vgCommands() const { return mVgCommands; }

    void setStaleIdCallback(const std::function<void(int)>& callback) { mStaleIdCallback = callback; }

    std::string toString() const;

    std::span<const AttributeNode> attributes() const { return mData.attributes; }

private:
    static std::atomic<int> sId;

    Channel* mutableChannel(std::string_view channelName) { return mData.mutableChannel(channelName); }

    std::vector<ChannelGroup> getGroupedChannels(std::string_view layerName) const;

    fs::path mPath;
    fs::file_time_type mFileLastModified;

    std::string mChannelSelector;

    std::string mName;

    std::map<std::string, ImageTexture> mTextures;

    ImageData mData;

    std::vector<ChannelGroup> mChannelGroups;

    std::vector<VgCommand> mVgCommands;

    std::function<void(int)> mStaleIdCallback;

    int mId;
};

// Modifies `data` and returns the new size of the data after reorientation.
Task<nanogui::Vector2i>
    orientToTopLeft(EPixelFormat format, std::vector<uint8_t>& data, nanogui::Vector2i size, EOrientation orientation, int priority);

Task<std::vector<std::shared_ptr<Image>>>
    tryLoadImage(int imageId, fs::path path, std::istream& iStream, std::string_view channelSelector, bool applyGainmaps, bool groupChannels);
Task<std::vector<std::shared_ptr<Image>>>
    tryLoadImage(fs::path path, std::istream& iStream, std::string_view channelSelector, bool applyGainmaps, bool groupChannels);
Task<std::vector<std::shared_ptr<Image>>>
    tryLoadImage(int imageId, fs::path path, std::string_view channelSelector, bool applyGainmaps, bool groupChannels);
Task<std::vector<std::shared_ptr<Image>>> tryLoadImage(fs::path path, std::string_view channelSelector, bool applyGainmaps, bool groupChannels);

struct ImageAddition {
    int loadId;
    bool shallSelect;
    std::vector<std::shared_ptr<Image>> images;
    std::shared_ptr<Image> toReplace;

    struct Comparator {
        bool operator()(const ImageAddition& a, const ImageAddition& b) { return a.loadId > b.loadId; }
    };
};

struct PathAndChannelSelector {
    fs::path path;
    std::string channelSelector;

    bool operator<(const PathAndChannelSelector& other) const {
        return path == other.path ? (channelSelector < other.channelSelector) : (path < other.path);
    }
};

class BackgroundImagesLoader {
public:
    void enqueue(const fs::path& path, std::string_view channelSelector, bool shallSelect, const std::shared_ptr<Image>& toReplace = nullptr);
    void checkDirectoriesForNewFilesAndLoadThose();

    std::optional<ImageAddition> tryPop();
    std::optional<nanogui::Vector2i> firstImageSize() const;

    bool publishSortedLoads();
    bool hasPendingLoads() const;

    bool recursiveDirectories() const { return mRecursiveDirectories; }
    void setRecursiveDirectories(bool value) { mRecursiveDirectories = value; }

    bool applyGainmaps() const { return mApplyGainmaps; }
    void setApplyGainmaps(bool value) { mApplyGainmaps = value; }

    bool groupChannels() const { return mGroupChannels; }
    void setGroupChannels(bool value) { mGroupChannels = value; }

private:
    SharedQueue<ImageAddition> mLoadedImages;

    std::priority_queue<ImageAddition, std::vector<ImageAddition>, ImageAddition::Comparator> mPendingLoadedImages;
    mutable std::mutex mPendingLoadedImagesMutex;

    std::atomic<int> mLoadCounter{0};
    std::atomic<int> mUnsortedLoadCounter{0};

    bool mRecursiveDirectories = false;
    std::map<fs::path, std::set<std::string>> mDirectories;
    std::set<PathAndChannelSelector> mFilesFoundInDirectories;

    bool mApplyGainmaps = true;
    bool mGroupChannels = true;

    std::chrono::system_clock::time_point mLoadStartTime;
    int mLoadStartCounter = 0;
};

} // namespace tev

namespace std {

template <> struct hash<tev::ChannelGroup> {
    size_t operator()(const tev::ChannelGroup& g) const { return hash<string>()(g.name); }
};

} // namespace std
