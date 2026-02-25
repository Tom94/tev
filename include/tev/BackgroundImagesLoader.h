/*
 * tev -- the EDR viewer
 *
 * Copyright (C) 2026 Thomas Müller <contact@tom94.net>
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
#include <tev/Image.h>
#include <tev/SharedQueue.h>
#include <tev/imageio/ImageLoader.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace tev {

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

    ImageLoaderSettings& imageLoaderSettings() & { return mImageLoaderSettings; }
    const ImageLoaderSettings& imageLoaderSettings() const & { return mImageLoaderSettings; }

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

    ImageLoaderSettings mImageLoaderSettings;
    bool mGroupChannels = true;

    std::chrono::system_clock::time_point mLoadStartTime;
    int mLoadStartCounter = 0;
};

} // namespace tev
