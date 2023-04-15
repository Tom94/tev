// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>
#include <tev/ThreadPool.h>

#include <chrono>
#include <functional>
#include <future>

namespace tev {

// Encapsulates a lazy, potentially asynchronous computation
// of some value. The public interface of this object is not
// thread-safe, i.e. it is expected to never be used from
// multiple threads at once.
template <typename T>
class Lazy {
public:
    Lazy(std::function<T(void)> compute)
    : Lazy{compute, nullptr} {
    }

    Lazy(std::function<T(void)> compute, ThreadPool* threadPool)
    : mThreadPool{threadPool}, mCompute{compute} {
    }

    Lazy(std::future<T>&& future)
    : mAsyncValue{std::move(future)} {
    }

    T get() {
        if (mIsComputed) {
            return mValue;
        }

        if (mAsyncValue.valid()) {
            mValue = mAsyncValue.get();
        } else {
            mValue = compute();
        }

        mIsComputed = true;
        mBecameReadyAt = std::chrono::steady_clock::now();
        return mValue;
    }

    bool isReady() const {
        if (mIsComputed) {
            TEV_ASSERT(
                !mAsyncValue.valid(),
                "There should never be a background computation while the result is already available."
            );

            return true;
        }

        if (!mAsyncValue.valid()) {
            return false;
        }

        return mAsyncValue.wait_for(std::chrono::seconds{0}) == std::future_status::ready;
    }

    std::chrono::steady_clock::time_point becameReadyAt() const {
        if (!isReady()) {
            return std::chrono::steady_clock::now();
        } else {
            return mBecameReadyAt;
        }
    }

    void computeAsync(int priority) {
        // No need to perform an async computation if we
        // already computed the value before or if one is
        // already running.
        if (mAsyncValue.valid() || mIsComputed) {
            return;
        }

        if (mThreadPool) {
            mAsyncValue = mThreadPool->enqueueTask([this]() { return compute(); }, priority);
        } else {
            mAsyncValue = std::async(std::launch::async, [this]() { return compute(); });
        }
    }

private:
    T compute() {
        T result = mCompute();
        mCompute = std::function<T(void)>{};
        return result;
    }

    // If this thread pool is present, use it to run tasks
    // instead of std::async.
    ThreadPool* mThreadPool = nullptr;

    std::function<T(void)> mCompute;
    std::future<T> mAsyncValue;
    T mValue;
    bool mIsComputed = false;
    std::chrono::steady_clock::time_point mBecameReadyAt;
};

}
