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

#include <tev/ThreadPool.h>

#include <chrono>

using namespace std;

namespace tev {

ThreadPool::ThreadPool() : ThreadPool{thread::hardware_concurrency()} {}

ThreadPool::ThreadPool(size_t maxNumThreads, bool force) {
    if (!force) {
        maxNumThreads = min((size_t)thread::hardware_concurrency(), maxNumThreads);
    }

    startThreads(maxNumThreads);
    mNumTasksInSystem.store(0);
}

ThreadPool::~ThreadPool() {
    waitUntilFinished();
    shutdown();

    while (!mThreads.empty()) {
        this_thread::sleep_for(1ms);
    }
}

void ThreadPool::startThreads(size_t num) {
    const lock_guard lock{mTaskQueueMutex};
    if (mShuttingDown) {
        return;
    }

    mNumThreads += num;
    for (size_t i = mThreads.size(); i < mNumThreads; ++i) {
        mThreads.emplace_back([this] {
            const auto id = this_thread::get_id();
            tlog::debug() << "Spawning thread pool thread " << id;

            unique_lock lock{mTaskQueueMutex};
            while (true) {
                if (!lock) {
                    lock.lock();
                }

                // look for a work item
                while (mThreads.size() <= mNumThreads && mTaskQueue.empty()) {
                    // if there are none wait for notification
                    mWorkerCondition.wait(lock);
                }

                if (mThreads.size() > mNumThreads) {
                    break;
                }

                const function<void()> task{std::move(mTaskQueue.top().fun)};
                mTaskQueue.pop();

                // Unlock the lock, so we can process the task without blocking other threads
                lock.unlock();

                task();

                {
                    const unique_lock localLock{mTaskQueueMutex};

                    mNumTasksInSystem--;

                    if (mNumTasksInSystem == 0) {
                        mSystemBusyCondition.notify_all();
                    }
                }
            }

            // Remove oneself from the thread pool. NOTE: at this point, the lock is still held, so modifying mThreads is safe.
            tlog::debug() << "Shutting down thread pool thread " << id;

            const auto it = find_if(mThreads.begin(), mThreads.end(), [&id](const std::thread& t) { return t.get_id() == id; });
            TEV_ASSERT(it != mThreads.end(), "Thread not found in thread pool.");

            // Thread must be detached, otherwise running our own constructor while still running would result in errors.
            thread self = std::move(*it);
            mThreads.erase(it);

            self.detach();
        });
    }
}

void ThreadPool::shutdownThreads(size_t num) {
    {
        const lock_guard lock{mTaskQueueMutex};

        const auto numToClose = min(num, mNumThreads);
        mNumThreads -= numToClose;
    }

    // Wake up all the threads to have them quit
    mWorkerCondition.notify_all();
}

void ThreadPool::shutdown() {
    {
        const lock_guard lock{mTaskQueueMutex};

        mNumThreads = 0;
        mShuttingDown = true;
    }

    // Wake up all the threads to have them quit
    mWorkerCondition.notify_all();
}

void ThreadPool::waitUntilFinished() {
    unique_lock lock{mTaskQueueMutex};

    if (mNumTasksInSystem == 0) {
        return;
    }

    mSystemBusyCondition.wait(lock);
}

void ThreadPool::waitUntilFinishedFor(const chrono::microseconds Duration) {
    unique_lock lock{mTaskQueueMutex};

    if (mNumTasksInSystem == 0) {
        return;
    }

    mSystemBusyCondition.wait_for(lock, Duration);
}

void ThreadPool::flushQueue() {
    const lock_guard lock{mTaskQueueMutex};

    mNumTasksInSystem -= mTaskQueue.size();
    while (!mTaskQueue.empty()) {
        mTaskQueue.pop();
    }

    if (mNumTasksInSystem == 0) {
        mSystemBusyCondition.notify_all();
    }
}

} // namespace tev
