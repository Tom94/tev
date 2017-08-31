// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include "../include/Common.h"

#include <atomic>
#include <deque>
#include <functional>
#include <future>
#include <thread>
#include <vector>

class ThreadPool {
public:

    ThreadPool();
    ThreadPool(size_t numThreads);
    virtual ~ThreadPool();

    template<class F, class... Args>
    std::future<typename std::result_of<F(Args...)>::type> enqueueTask(F && f, Args && ... args) {
        typedef typename std::result_of<F(Args...)>::type return_type;

        ++mNumTasksInSystem;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        auto res = task->get_future();

        {
            std::lock_guard<std::mutex> lock{mTaskQueueMutex};
            mTaskQueue.emplace_back([task]() { (*task)(); });
        }

        mWorkerCondition.notify_one();
        return res;
    }

    void startThreads(size_t Amount);
    void shutdownThreads(size_t Amount);

    size_t numTasksInSystem() const {
        return mNumTasksInSystem;
    }

    void waitUntilFinished();
    void waitUntilFinishedFor(const std::chrono::microseconds Duration);
    void flushQueue();

    void parallelForNoWait(size_t start, size_t end, std::function<void(size_t)> body);
    void parallelFor(size_t start, size_t end, std::function<void(size_t)> body);

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
