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
#include <string>
#include <vector>

namespace tev {

class ImageLoader;

struct AttributeNode {
    std::string name;
    std::string value;
    std::string type;
    std::vector<AttributeNode> children;
};

struct ImageData {
    ImageData() = default;
    ImageData(const ImageData&) = delete;
    ImageData(ImageData&&) = default;
    ImageData& operator=(const ImageData&) = delete;
    ImageData& operator=(ImageData&&) = default;

    std::vector<Channel> channels;
    std::vector<std::string> layers;
    nanogui::Matrix4f toRec709 = nanogui::Matrix4f{1.0f}; // Identity by default
    bool hasPremultipliedAlpha = false;
    EOrientation orientation = EOrientation::TopLeft;
    AttributeNode attributes;

    Box2i dataWindow;
    Box2i displayWindow;

    std::string partName;

    nanogui::Vector2i size() const { return dataWindow.size(); }

    nanogui::Vector2i displaySize() const { return displayWindow.size(); }

    size_t numPixels() const { return channels.front().numPixels(); }

    std::vector<std::string> channelsInLayer(std::string layerName) const;

    Task<void> convertToRec709(int priority);

    void alphaOperation(const std::function<void(Channel&, const Channel&)>& func);

    Task<void> multiplyAlpha(int priority);
    Task<void> unmultiplyAlpha(int priority);

    Task<void> orientToTopLeft(int priority);

    Task<void> ensureValid(const std::string& channelSelector, int taskPriority);

    bool hasChannel(const std::string& channelName) const { return channel(channelName) != nullptr; }

    const Channel* channel(const std::string& channelName) const {
        auto it = std::find_if(std::begin(channels), std::end(channels), [&channelName](const Channel& c) { return c.name() == channelName; });

        if (it != std::end(channels)) {
            return &(*it);
        } else {
            return nullptr;
        }
    }

    Channel* mutableChannel(const std::string& channelName) {
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
    static const size_t DITHER_MATRIX_SIZE = 8;
    using dither_matrix_t = std::array<std::array<float, DITHER_MATRIX_SIZE>, DITHER_MATRIX_SIZE>;
    static dither_matrix_t ditherMatrix();

    Image(const fs::path& path, fs::file_time_type fileLastModified, ImageData&& data, const std::string& channelSelector);
    virtual ~Image();

    const fs::path& path() const { return mPath; }

    fs::file_time_type fileLastModified() const { return mFileLastModified; }

    void setFileLastModified(fs::file_time_type value) { mFileLastModified = value; }

    const std::string& channelSelector() const { return mChannelSelector; }

    const std::string& name() const { return mName; }

    std::string shortName() const;

    bool hasChannel(const std::string& channelName) const { return mData.hasChannel(channelName); }

    const Channel* channel(const std::string& channelName) const { return mData.channel(channelName); }
    std::vector<const Channel*> channels(const std::vector<std::string>& channelNames) const {
        std::vector<const Channel*> result;
        for (const auto& channelName : channelNames) {
            result.push_back(channel(channelName));
        }

        return result;
    }

    bool isInterleavedRgba(const std::vector<std::string>& channelNames) const;

    nanogui::Texture* texture(const std::vector<std::string>& channelNames, EInterpolationMode minFilter, EInterpolationMode magFilter);

    std::vector<std::string> channelsInGroup(const std::string& groupName) const;
    void decomposeChannelGroup(const std::string& groupName);

    std::vector<std::string> getSortedChannels(const std::string& layerName) const;
    std::vector<std::string> getExistingChannels(const std::vector<std::string>& requestedChannels) const;

    nanogui::Vector2i size() const { return mData.size(); }

    bool contains(const nanogui::Vector2i& pos) const {
        return pos.x() >= 0 && pos.y() >= 0 && pos.x() < mData.size().x() && pos.y() < mData.size().y();
    }

    const Box2i& dataWindow() const { return mData.dataWindow; }

    const Box2i& displayWindow() const { return mData.displayWindow; }

    nanogui::Vector2f centerDisplayOffset(const Box2i& displayWindow) const {
        return Box2f{dataWindow()}.middle() - Box2f{displayWindow}.middle();
    }

    size_t numPixels() const { return mData.numPixels(); }

    const std::vector<ChannelGroup>& channelGroups() const { return mChannelGroups; }

    int id() const { return mId; }

    void bumpId() {
        int oldId = mId;
        mId = sId++;

        if (mStaleIdCallback) {
            mStaleIdCallback(oldId);
        }
    }

    static int drawId() { return sId++; }

    void updateChannel(const std::string& channelName, int x, int y, int width, int height, const std::vector<float>& data);

    void updateVectorGraphics(bool append, const std::vector<VgCommand>& commands);

    const std::vector<VgCommand>& vgCommands() const { return mVgCommands; }

    void setStaleIdCallback(const std::function<void(int)>& callback) { mStaleIdCallback = callback; }

    std::string toString() const;

    const AttributeNode& attributes() const { return mData.attributes; }

private:
    static std::atomic<int> sId;

    Channel* mutableChannel(const std::string& channelName) { return mData.mutableChannel(channelName); }

    std::vector<ChannelGroup> getGroupedChannels(const std::string& layerName) const;

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

Task<std::vector<std::shared_ptr<Image>>>
    tryLoadImage(int imageId, fs::path path, std::istream& iStream, std::string channelSelector, bool applyGainmaps);
Task<std::vector<std::shared_ptr<Image>>> tryLoadImage(fs::path path, std::istream& iStream, std::string channelSelector, bool applyGainmaps);
Task<std::vector<std::shared_ptr<Image>>> tryLoadImage(int imageId, fs::path path, std::string channelSelector, bool applyGainmaps);
Task<std::vector<std::shared_ptr<Image>>> tryLoadImage(fs::path path, std::string channelSelector, bool applyGainmaps);

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
    void enqueue(const fs::path& path, const std::string& channelSelector, bool shallSelect, const std::shared_ptr<Image>& toReplace = nullptr);
    void checkDirectoriesForNewFilesAndLoadThose();

    std::optional<ImageAddition> tryPop() { return mLoadedImages.tryPop(); }

    bool publishSortedLoads();
    bool hasPendingLoads() const { return mLoadCounter != mUnsortedLoadCounter; }

    bool recursiveDirectories() const { return mRecursiveDirectories; }
    void setRecursiveDirectories(bool value) { mRecursiveDirectories = value; }

    bool applyGainmaps() const { return mApplyGainmaps; }
    void setApplyGainmaps(bool value) { mApplyGainmaps = value; }

private:
    SharedQueue<ImageAddition> mLoadedImages;

    std::priority_queue<ImageAddition, std::vector<ImageAddition>, ImageAddition::Comparator> mPendingLoadedImages;
    std::mutex mPendingLoadedImagesMutex;

    std::atomic<int> mLoadCounter{0};
    std::atomic<int> mUnsortedLoadCounter{0};

    bool mRecursiveDirectories = false;
    std::map<fs::path, std::set<std::string>> mDirectories;
    std::set<PathAndChannelSelector> mFilesFoundInDirectories;

    bool mApplyGainmaps = true;

    std::chrono::system_clock::time_point mLoadStartTime;
    int mLoadStartCounter = 0;
};

} // namespace tev

namespace std {

template <> struct hash<tev::ChannelGroup> {
    size_t operator()(const tev::ChannelGroup& g) const { return hash<string>()(g.name); }
};

} // namespace std
