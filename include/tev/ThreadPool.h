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

#pragma once

#include <tev/Common.h>
#include <tev/Task.h>

#include <atomic>
#include <functional>
#include <future>
#include <queue>
#include <thread>
#include <vector>

namespace tev {

class ThreadPool {
public:
    ThreadPool();
    ThreadPool(size_t maxNumThreads, bool force = false);
    virtual ~ThreadPool();

    static ThreadPool& global() {
        static ThreadPool pool;
        return pool;
    }

    template <class F> auto enqueueTask(F&& f, int priority) {
        using return_type = std::invoke_result_t<decltype(f)>;

        const auto task = std::make_shared<std::packaged_task<return_type()>>(std::forward<F>(f));
        auto res = task->get_future();

        {
            const std::lock_guard<std::mutex> lock{mTaskQueueMutex};

            if (!mShuttingDown) {
                mTaskQueue.push({priority, [task]() { (*task)(); }});
                ++mNumTasksInSystem;
            }
        }

        mWorkerCondition.notify_one();
        return res;
    }

    inline auto enqueueCoroutine(int priority) noexcept {
        class Awaiter {
        public:
            Awaiter(ThreadPool* pool, int priority) : mPool{pool}, mPriority{priority} {}

            bool await_ready() const noexcept { return false; }

            // Suspend and enqueue coroutine continuation onto the threadpool
            void await_suspend(std::coroutine_handle<> coroutine) noexcept { mPool->enqueueTask(coroutine, mPriority); }

            void await_resume() const noexcept {}

        private:
            ThreadPool* mPool;
            int mPriority;
        };

        return Awaiter{this, priority};
    }

    template <class F> auto enqueueCoroutine(F&& fun, int priority) -> Task<void> {
        return [](F&& fun, ThreadPool* pool, int tPriority) -> Task<void> {
            // Makes sure the function's captures have same lifetime as coroutine
            auto exec = std::move(fun);
            co_await pool->enqueueCoroutine(tPriority);
            co_await exec();
        }(std::forward<F>(fun), this, priority);
    }

    void startThreads(size_t num);
    void shutdownThreads(size_t num);
    void shutdown();

    size_t numTasksInSystem() const { return mNumTasksInSystem; }

    void waitUntilFinished();
    void waitUntilFinishedFor(const std::chrono::microseconds Duration);
    void flushQueue();

    template <typename Int, typename F> Task<void> parallelForAsync(Int start, Int end, F body, int priority) {
        Int range = end - start;
        Int nTasks = std::min({(Int)mNumThreads, (Int)mHardwareConcurrency, range});

        std::vector<Task<void>> tasks;
        for (Int i = 0; i < nTasks; ++i) {
            Int taskStart = start + (range * i / nTasks);
            Int taskEnd = start + (range * (i + 1) / nTasks);
            TEV_ASSERT(taskStart != taskEnd, "Should not produce tasks with empty range.");

            tasks.emplace_back([](Int tStart, Int tEnd, F tBody, int tPriority, ThreadPool* pool) -> Task<void> {
                co_await pool->enqueueCoroutine(tPriority);
                for (Int j = tStart; j < tEnd; ++j) {
                    tBody(j);
                }
            }(taskStart, taskEnd, body, priority, this));
        }

        for (auto&& task : tasks) {
            co_await task;
        }
    }

    template <typename Int, typename F> void parallelFor(Int start, Int end, F body, int priority) {
        parallelForAsync(start, end, body, priority).get();
    }

    size_t numThreads() const { return mNumThreads; }

private:
    size_t mNumThreads = 0;
    bool mShuttingDown = false;

    const size_t mHardwareConcurrency = std::thread::hardware_concurrency();
    std::vector<std::thread> mThreads;

    struct QueuedTask {
        int priority;
        std::function<void()> fun;

        struct Comparator {
            bool operator()(const QueuedTask& a, const QueuedTask& b) { return a.priority < b.priority; }
        };
    };

    std::priority_queue<QueuedTask, std::vector<QueuedTask>, QueuedTask::Comparator> mTaskQueue;
    std::mutex mTaskQueueMutex;
    std::condition_variable mWorkerCondition;
    std::condition_variable mSystemBusyCondition;

    std::atomic<size_t> mNumTasksInSystem;
};

} // namespace tev
