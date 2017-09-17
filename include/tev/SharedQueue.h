// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>

#include <deque>
#include <mutex>
#include <condition_variable>

template <typename T>
class SharedQueue {
public:
    bool empty() const {
        std::lock_guard<std::mutex> lock{mMutex};
        return mRawQueue.empty();
    }

    size_t size() const {
        std::lock_guard<std::mutex> lock{mMutex};
        return mRawQueue.size();
    }

    void push(T newElem) {
        std::lock_guard<std::mutex> lock{mMutex};
        mRawQueue.push_back(newElem);
        mDataCondition.notify_one();
    }

    T waitAndPop() {
        std::unique_lock<std::mutex> lock{mMutex};

        while (mRawQueue.empty()) {
            mDataCondition.wait(lock);
        }

        T result = std::move(mRawQueue.front());
        mRawQueue.pop_front();

        return result;
    }

    T tryPop() {
        std::unique_lock<std::mutex> lock{mMutex};

        if (mRawQueue.empty()) {
            throw std::runtime_error{"Could not pop."};
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
