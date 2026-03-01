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

#include <concurrentqueue/concurrentqueue.h> // Needs to be included before lightweightsemaphore.h
#include <concurrentqueue/lightweightsemaphore.h>

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

    template <class F> auto enqueueTask(F&& f, int priority, bool stopToken = false) {
        using return_type = std::invoke_result_t<decltype(f)>;

        const auto task = std::make_shared<std::packaged_task<return_type()>>(std::forward<F>(f));
        auto res = task->get_future();

        if (mShuttingDown) {
            return res;
        }

        {
            const std::scoped_lock lock{mTaskQueueMutex};
            mTaskQueue.push({
                .fun = [task]() { (*task)(); },
                .priority = priority,
                .stopToken = stopToken,
            });
        }

        ++mNumTasksInSystem;
        mTaskQueueSemaphore.signal();

        return res;
    }

    auto enqueueStopToken() {
        return enqueueTask([]() {}, std::numeric_limits<int>::max(), true);
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

    template <class F> auto enqueueCoroutine(F&& fun, int priority) -> Task<typename std::invoke_result_t<decltype(fun)>::value_type> {
        using return_type = std::invoke_result_t<decltype(fun)>::value_type;
        co_return co_await [](F&& fun, ThreadPool* pool, int tPriority) -> Task<return_type> {
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

    template <typename Int>
    Int nTasks(
        Int start,
        Int end,
        // In ~number of samples (== pixels * channels) processed with a few operations (e.g. toSrgb, color transform, alpha mult/div) each.
        // If the per-sample cost is particularly high, an arbitrary factor can be applied to increase this value.
        size_t approxCost
    ) const {
        const size_t targetCostPerTask = 64 * 1024;
        const size_t maxNTasks = (approxCost + targetCostPerTask - 1) / targetCostPerTask;

        const Int range = end - start;
        const Int nTasks = std::min({(Int)mNumThreads, (Int)mHardwareConcurrency, (Int)maxNTasks, range});

        return nTasks;
    }

    template <typename Int, typename F> Task<void> parallelForAsync(Int start, Int end, size_t approxCost, F body, int priority) {
        const Int range = end - start;
        const Int n = nTasks(start, end, approxCost);

        std::vector<Task<void>> tasks;

        {
            // The task queue is already thread-safe due to the lock in enqueueTask, but we can optimize a bit by locking only once for all
            // tasks.
            const std::scoped_lock lock{mTaskQueueMutex};

            for (Int i = 0; i < n; ++i) {
                Int taskStart = start + (range * i / n);
                Int taskEnd = start + (range * (i + 1) / n);
                TEV_ASSERT(taskStart != taskEnd, "Should not produce tasks with empty range.");

                tasks.emplace_back([](Int tStart, Int tEnd, F& tBody, int tPriority, ThreadPool* pool) -> Task<void> {
                    co_await pool->enqueueCoroutine(tPriority);
                    for (Int j = tStart; j < tEnd; ++j) {
                        if constexpr (is_coroutine_callable_v<F, Int>) {
                            co_await tBody(j);
                        } else {
                            tBody(j);
                        }
                    }
                }(taskStart, taskEnd, body, priority, this));
            }
        }

        co_await awaitAll(tasks);
    }

    size_t numThreads() const { return mNumThreads; }

private:
    bool mShuttingDown = false;

    const size_t mHardwareConcurrency = std::thread::hardware_concurrency();
    std::vector<std::thread> mThreads;

    struct QueuedTask {
        std::function<void()> fun;
        int priority;
        bool stopToken;

        struct Comparator {
            bool operator()(const QueuedTask& a, const QueuedTask& b) { return a.priority < b.priority; }
        };
    };

    std::priority_queue<QueuedTask, std::vector<QueuedTask>, QueuedTask::Comparator> mTaskQueue;
    std::recursive_mutex mTaskQueueMutex;
    moodycamel::LightweightSemaphore mTaskQueueSemaphore;

    std::recursive_mutex mThreadsMutex;

    std::atomic<size_t> mNumThreads = 0;
    std::atomic<size_t> mNumTasksInSystem;
};

} // namespace tev
