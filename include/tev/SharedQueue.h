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

#include <tev/Common.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <optional>

namespace tev {

template <typename T> class SharedQueue {
public:
    bool empty() const {
        std::lock_guard lock{mMutex};
        return mRawQueue.empty();
    }

    size_t size() const {
        std::lock_guard lock{mMutex};
        return mRawQueue.size();
    }

    void push(T newElem) {
        std::lock_guard lock{mMutex};
        mRawQueue.push_back(newElem);
        mDataCondition.notify_one();
    }

    T waitAndPop() {
        std::unique_lock lock{mMutex};

        while (mRawQueue.empty()) {
            mDataCondition.wait(lock);
        }

        T result = std::move(mRawQueue.front());
        mRawQueue.pop_front();

        return result;
    }

    std::optional<T> tryPop() {
        std::unique_lock lock{mMutex};

        if (mRawQueue.empty()) {
            return {};
        }

        T result = std::move(mRawQueue.front());
        mRawQueue.pop_front();

        return result;
    }

private:
    std::deque<T> mRawQueue;
    mutable std::mutex mMutex;
    std::condition_variable mDataCondition;
};

} // namespace tev
