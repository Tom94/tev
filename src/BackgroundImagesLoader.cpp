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

#include <tev/BackgroundImagesLoader.h>
#include <tev/Common.h>
#include <tev/Image.h>
#include <tev/ThreadPool.h>
#include <tev/imageio/Colors.h>
#include <tev/imageio/ImageLoader.h>
#include <tev/imageio/ImageSaver.h>

#include <algorithm>
#include <chrono>
#include <map>
#include <unordered_set>
#include <vector>

using namespace nanogui;
using namespace std;

namespace tev {

void BackgroundImagesLoader::enqueue(const fs::path& path, string_view channelSelector, bool shallSelect, const shared_ptr<Image>& toReplace) {
    // If we're trying to open a directory, try loading all the images inside of that directory
    if (fs::exists(path) && fs::is_directory(path)) {
        tlog::info() << fmt::format("Loading images {}from directory {}", mRecursiveDirectories ? "recursively " : "", path);

        const fs::path canonicalPath = fs::canonical(path);
        mDirectories[canonicalPath].emplace(channelSelector);

        vector<fs::directory_entry> entries;
        forEachFileInDir(mRecursiveDirectories, canonicalPath, [&](const auto& entry) {
            if (!entry.is_directory()) {
                mFilesFoundInDirectories.emplace(PathAndChannelSelector{entry, string{channelSelector}});
                entries.emplace_back(entry);
            }
        });

        // Open directory entries in natural order (e.g. "file1.exr", "file2.exr", "file10.exr" instead of "file1.exr", "file10.exr"),
        // selecting the first one.
        sort(begin(entries), end(entries), [](const auto& a, const auto& b) { return naturalCompare(a.path().string(), b.path().string()); });

        for (size_t i = 0; i < entries.size(); ++i) {
            enqueue(entries[i], channelSelector, i == 0 ? shallSelect : false);
        }

        return;
    }

    // We want to measure the time it takes to load a whole batch of images. Start measuring when the loader queue goes from empty to
    // non-empty and stop measuring when the queue goes from non-empty to empty again.
    if (mUnsortedLoadCounter == mLoadCounter) {
        mLoadStartTime = chrono::system_clock::now();
        mLoadStartCounter = mUnsortedLoadCounter;
    }

    const int loadId = mUnsortedLoadCounter++;
    invokeTaskDetached([loadId, path, channelSelector = string{channelSelector}, shallSelect, toReplace, this]() -> Task<void> {
        const int taskPriority = -Image::drawId();

        co_await ThreadPool::global().enqueueCoroutine(taskPriority);
        const auto images = co_await tryLoadImage(taskPriority, path, channelSelector, mImageLoaderSettings, mGroupChannels);

        {
            const lock_guard lock{mPendingLoadedImagesMutex};
            mPendingLoadedImages.push({loadId, shallSelect, images, toReplace});
        }

        if (publishSortedLoads()) {
            redrawWindow();
        }
    });
}

void BackgroundImagesLoader::checkDirectoriesForNewFilesAndLoadThose() {
    for (const auto& dir : mDirectories) {
        forEachFileInDir(mRecursiveDirectories, dir.first, [&](const auto& entry) {
            if (!entry.is_directory()) {
                for (const auto& channelSelector : dir.second) {
                    const PathAndChannelSelector p = {entry, channelSelector};
                    if (!mFilesFoundInDirectories.contains(p)) {
                        mFilesFoundInDirectories.emplace(p);
                        enqueue(entry, channelSelector, false);
                    }
                }
            }
        });
    }
}

optional<ImageAddition> BackgroundImagesLoader::tryPop() {
    const lock_guard lock{mPendingLoadedImagesMutex};
    return mLoadedImages.tryPop();
}

optional<nanogui::Vector2i> BackgroundImagesLoader::firstImageSize() const {
    const lock_guard lock{mPendingLoadedImagesMutex};
    if (mLoadedImages.empty()) {
        return nullopt;
    }

    const ImageAddition& firstImage = mLoadedImages.front();
    if (firstImage.images.empty()) {
        return nullopt;
    }

    return firstImage.images.front()->size();
}

bool BackgroundImagesLoader::publishSortedLoads() {
    const lock_guard lock{mPendingLoadedImagesMutex};
    bool pushed = false;
    while (!mPendingLoadedImages.empty() && mPendingLoadedImages.top().loadId == mLoadCounter) {
        // null image pointers indicate failed loads. These shouldn't be pushed.
        if (!mPendingLoadedImages.top().images.empty()) {
            mLoadedImages.push(mPendingLoadedImages.top());
        }

        mPendingLoadedImages.pop();
        pushed = true;

        ++mLoadCounter;
    }

    if (mLoadCounter == mUnsortedLoadCounter && mLoadCounter - mLoadStartCounter > 1) {
        const auto end = chrono::system_clock::now();
        const chrono::duration<double> elapsedSeconds = end - mLoadStartTime;
        tlog::success() << fmt::format("Loaded {} images in {:.3f} seconds.", mLoadCounter - mLoadStartCounter, elapsedSeconds.count());
    }

    return pushed;
}

bool BackgroundImagesLoader::hasPendingLoads() const {
    const lock_guard lock{mPendingLoadedImagesMutex};
    return mLoadCounter != mUnsortedLoadCounter;
}

} // namespace tev
