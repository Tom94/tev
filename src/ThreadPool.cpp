// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#include <tev/ThreadPool.h>

#include <chrono>

using namespace std;

namespace tev {

ThreadPool::ThreadPool()
: ThreadPool{thread::hardware_concurrency()} {
}

ThreadPool::ThreadPool(size_t maxNumThreads, bool force) {
    if (!force) {
        maxNumThreads = min((size_t)thread::hardware_concurrency(), maxNumThreads);
    }
    startThreads(maxNumThreads);
    mNumTasksInSystem.store(0);
}

ThreadPool::~ThreadPool() {
    waitUntilFinished();
    shutdownThreads(mThreads.size());
}

void ThreadPool::startThreads(size_t num) {
    mNumThreads += num;
    for (size_t i = mThreads.size(); i < mNumThreads; ++i) {
        mThreads.emplace_back([this, i] {
            while (true) {
                unique_lock lock{mTaskQueueMutex};

                // look for a work item
                while (i < mNumThreads && mTaskQueue.empty()) {
                    // if there are none wait for notification
                    mWorkerCondition.wait(lock);
                }

                if (i >= mNumThreads) {
                    break;
                }

                function<void()> task{move(mTaskQueue.top().fun)};
                mTaskQueue.pop();

                // Unlock the lock, so we can process the task without blocking other threads
                lock.unlock();

                task();

                mNumTasksInSystem--;

                {
                    unique_lock localLock{mSystemBusyMutex};

                    if (mNumTasksInSystem == 0) {
                        mSystemBusyCondition.notify_all();
                    }
                }
            }
        });
    }
}

void ThreadPool::shutdownThreads(size_t num) {
    auto numToClose = min(num, mNumThreads);

    {
        lock_guard lock{mTaskQueueMutex};
        mNumThreads -= numToClose;
    }

    // Wake up all the threads to have them quit
    mWorkerCondition.notify_all();
    for (auto i = 0u; i < numToClose; ++i) {
        mThreads.back().join();
        mThreads.pop_back();
    }
}

void ThreadPool::waitUntilFinished() {
    unique_lock lock{mSystemBusyMutex};

    if (mNumTasksInSystem == 0) {
        return;
    }

    mSystemBusyCondition.wait(lock);
}

void ThreadPool::waitUntilFinishedFor(const chrono::microseconds Duration) {
    unique_lock lock{mSystemBusyMutex};

    if (mNumTasksInSystem == 0) {
        return;
    }

    mSystemBusyCondition.wait_for(lock, Duration);
}

void ThreadPool::flushQueue() {
    lock_guard lock{mTaskQueueMutex};

    mNumTasksInSystem -= mTaskQueue.size();
    while (!mTaskQueue.empty()) {
        mTaskQueue.pop();
    }
}

}
