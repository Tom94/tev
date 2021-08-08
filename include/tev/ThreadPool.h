// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>

#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <thread>
#include <vector>

TEV_NAMESPACE_BEGIN

template <typename T>
void waitAll(const std::vector<std::future<T>>& futures) {
    for (auto& f : futures) {
        f.wait();
    }
}

class ThreadPool {
public:
    ThreadPool();
    ThreadPool(size_t maxNumThreads, bool force = false);
    virtual ~ThreadPool();

    template<class F>
    auto enqueueTask(F&& f, bool highPriority = false) -> std::future<std::invoke_result_t<decltype(f)>> {
        using return_type = std::invoke_result_t<decltype(f)>;

        ++mNumTasksInSystem;

        auto task = std::make_shared<std::packaged_task<return_type()>>(std::forward<F>(f));

        auto res = task->get_future();

        {
            std::lock_guard<std::mutex> lock{mTaskQueueMutex};

            if (highPriority) {
                mTaskQueue.emplace_front([task]() { (*task)(); });
            } else {
                mTaskQueue.emplace_back([task]() { (*task)(); });
            }
        }

        mWorkerCondition.notify_one();
        return res;
    }

    void startThreads(size_t num);
    void shutdownThreads(size_t num);

    size_t numTasksInSystem() const {
        return mNumTasksInSystem;
    }

    void waitUntilFinished();
    void waitUntilFinishedFor(const std::chrono::microseconds Duration);
    void flushQueue();

    template <typename Int, typename F>
    void parallelForAsync(Int start, Int end, F body, std::vector<std::future<void>>& futures) {
        Int localNumThreads = (Int)mNumThreads;

        Int range = end - start;
        Int chunk = (range / localNumThreads) + 1;

        for (Int i = 0; i < localNumThreads; ++i) {
            futures.emplace_back(enqueueTask([i, chunk, start, end, body] {
                Int innerStart = start + i * chunk;
                Int innerEnd = std::min(end, start + (i + 1) * chunk);
                for (Int j = innerStart; j < innerEnd; ++j) {
                    body(j);
                }
            }));
        }
    }

    template <typename Int, typename F>
    std::vector<std::future<void>> parallelForAsync(Int start, Int end, F body) {
        std::vector<std::future<void>> futures;
        parallelForAsync(start, end, body, futures);
        return futures;
    }

    template <typename Int, typename F>
    void parallelFor(Int start, Int end, F body) {
        waitAll(parallelForAsync(start, end, body));
    }

private:
    size_t mNumThreads = 0;
    std::vector<std::thread> mThreads;

    std::deque<std::function<void()>> mTaskQueue;
    std::mutex mTaskQueueMutex;
    std::condition_variable mWorkerCondition;

    std::atomic<size_t> mNumTasksInSystem;
    std::mutex mSystemBusyMutex;
    std::condition_variable mSystemBusyCondition;
};

TEV_NAMESPACE_END
