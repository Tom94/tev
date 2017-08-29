// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include "../include/ThreadPool.h"

#include <chrono>

using namespace std;

ThreadPool::ThreadPool()
: ThreadPool{thread::hardware_concurrency()} {
}

ThreadPool::ThreadPool(size_t numThreads) {
    startThreads(numThreads);
    mNumTasksInSystem.store(0);
}

ThreadPool::~ThreadPool() {
    shutdownThreads(mThreads.size());
}

void ThreadPool::startThreads(size_t amount) {
    mNumThreads += amount;
    for (size_t i = mThreads.size(); i < mNumThreads; ++i) {
        mThreads.emplace_back([this, i] {
            while (true) {
                unique_lock<mutex> lock{mTaskQueueMutex};

                // look for a work item
                while (i < mNumThreads && mTaskQueue.empty()) {
                    // if there are none wait for notification
                    mWorkerCondition.wait(lock);
                }

                if (i >= mNumThreads) {
                    break;
                }

                function<void()> task{move(mTaskQueue.front())};
                mTaskQueue.pop_front();

                // Unlock the lock, so we can process the task without blocking other threads
                lock.unlock();

                task();

                mNumTasksInSystem--;

                {
                    unique_lock<mutex> localLock{mSystemBusyMutex};

                    if (mNumTasksInSystem == 0) {
                        mSystemBusyCondition.notify_all();
                    }
                }
            }
        });
    }
}

void ThreadPool::shutdownThreads(size_t amount) {
    auto amountToClose = min(amount, mNumThreads);

    {
        lock_guard<mutex> lock{mTaskQueueMutex};
        mNumThreads -= amountToClose;
    }

    // Wake up all the threads to have them quit
    mWorkerCondition.notify_all();
    for (auto i = 0u; i < amountToClose; ++i) {
        mThreads.back().join();
        mThreads.pop_back();
    }
}

void ThreadPool::waitUntilFinished() {
    unique_lock<mutex> lock{mSystemBusyMutex};

    if (mNumTasksInSystem == 0) {
        return;
    }

    mSystemBusyCondition.wait(lock);
}

void ThreadPool::waitUntilFinishedFor(const chrono::microseconds Duration) {
    unique_lock<mutex> lock{mSystemBusyMutex};

    if (mNumTasksInSystem == 0) {
        return;
    }

    mSystemBusyCondition.wait_for(lock, Duration);
}

void ThreadPool::flushQueue() {
    lock_guard<mutex> lock{mTaskQueueMutex};

    mNumTasksInSystem -= mTaskQueue.size();
    mTaskQueue.clear();
}

void ThreadPool::parallelForNoWait(size_t start, size_t end, std::function<void(size_t)> body) {
    size_t localNumThreads = mNumThreads;

    size_t range = end - start;
    size_t chunk = (range / localNumThreads) + 1;

    for (size_t i = 0; i < localNumThreads; ++i) {
        enqueueTask([i, chunk, start, end, body] {
            size_t innerStart = start + i * chunk;
            size_t innerEnd = min(end, start + (i + 1) * chunk);
            for (size_t j = innerStart; j < innerEnd; ++j) {
                body(j);
            }
        });
    }
}

void ThreadPool::parallelFor(size_t start, size_t end, std::function<void(size_t)> body) {
    parallelForNoWait(start, end, body);
    waitUntilFinished();
}
