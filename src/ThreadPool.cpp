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

#include <tev/ThreadPool.h>

#include <chrono>

using namespace std;

namespace tev {

ThreadPool::ThreadPool() : ThreadPool{thread::hardware_concurrency()} {}

ThreadPool::ThreadPool(size_t maxNumThreads, bool force) {
    if (!force) {
        maxNumThreads = min((size_t)thread::hardware_concurrency(), maxNumThreads);
    }

    mNumTasksInSystem.store(0);
    startThreads(maxNumThreads);
}

ThreadPool::~ThreadPool() {
    waitUntilFinished();
    shutdown();

    while (!mThreads.empty()) {
        this_thread::sleep_for(1ms);
    }
}

void ThreadPool::startThreads(size_t num) {
    const scoped_lock lock{mThreadsMutex};
    if (mShuttingDown) {
        return;
    }

    mNumThreads += num;
    for (size_t i = mThreads.size(); i < mNumThreads; ++i) {
        mThreads.emplace_back([this] {
            const auto id = this_thread::get_id();
            // tlog::debug() << "Spawning thread pool thread " << id;

            while (true) {
                QueuedTask task = {};
                while (!mTaskQueueSemaphore.wait()) {}

                {
                    const scoped_lock taskQueueLock{mTaskQueueMutex};
                    TEV_ASSERT(!mTaskQueue.empty(), "Task queue was empty after semaphore wait.");

                    task = std::move(mTaskQueue.top());
                    mTaskQueue.pop();
                }

                task.fun();
                mNumTasksInSystem--;

                if (task.stopToken) {
                    break;
                }
            }

            const scoped_lock threadsLock{mThreadsMutex};

            // Remove oneself from the thread pool. NOTE: at this point, the lock is still held, so modifying mThreads is safe.
            // tlog::debug() << "Shutting down thread pool thread " << id;

            const auto it = ranges::find(mThreads, id, [](const auto& t) { return t.get_id(); });
            TEV_ASSERT(it != mThreads.end(), "Thread not found in thread pool.");

            // Thread must be detached, otherwise running our own constructor while still running would result in errors.
            thread self = std::move(*it);
            mThreads.erase(it);

            self.detach();
        });
    }
}

void ThreadPool::shutdownThreads(size_t num) {
    mNumThreads -= num;
    for (size_t i = 0; i < num; ++i) {
        enqueueStopToken();
    }
}

void ThreadPool::shutdown() {
    shutdownThreads(mNumThreads);
    mShuttingDown = true;
}

void ThreadPool::waitUntilFinished() {
    while (mThreads.size() > 0 && mNumTasksInSystem > 0) {
        this_thread::sleep_for(1ms);
    }
}

void ThreadPool::waitUntilFinishedFor(const chrono::microseconds duration) {
    const auto now = chrono::steady_clock::now();
    while (mThreads.size() > 0 && mNumTasksInSystem > 0 && chrono::steady_clock::now() - now < duration) {
        this_thread::sleep_for(1ms);
    }
}

} // namespace tev
