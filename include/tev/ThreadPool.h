// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

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

    template<class F>
    auto enqueueTask(F&& f, int priority) {
        using return_type = std::invoke_result_t<decltype(f)>;

        ++mNumTasksInSystem;

        auto task = std::make_shared<std::packaged_task<return_type()>>(std::forward<F>(f));

        auto res = task->get_future();

        {
            std::lock_guard<std::mutex> lock{mTaskQueueMutex};
            mTaskQueue.push({priority, [task]() { (*task)(); }});
        }

        mWorkerCondition.notify_one();
        return res;
    }

    inline auto enqueueCoroutine(int priority) noexcept {
        class Awaiter {
        public:
            Awaiter(ThreadPool* pool, int priority)
            : mPool{pool}, mPriority{priority} {}

            bool await_ready() const noexcept { return false; }

            // Suspend and enqueue coroutine continuation onto the threadpool
            void await_suspend(COROUTINE_NAMESPACE::coroutine_handle<> coroutine) noexcept {
                mPool->enqueueTask(coroutine, mPriority);
            }

            void await_resume() const noexcept {}

        private:
            ThreadPool* mPool;
            int mPriority;
        };

        return Awaiter{this, priority};
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
    Task<void> parallelForAsync(Int start, Int end, F body, int priority) {
        Int range = end - start;
        Int nTasks = std::min((Int)mNumThreads, range);

        std::vector<Task<void>> tasks;
        for (Int i = 0; i < nTasks; ++i) {
            Int taskStart = start + (range * i / nTasks);
            Int taskEnd = start + (range * (i+1) / nTasks);
            TEV_ASSERT(taskStart != taskEnd, "Should not produce tasks with empty range.");

            tasks.emplace_back([](Int start, Int end, F body, int priority, ThreadPool* pool) -> Task<void> {
                co_await pool->enqueueCoroutine(priority);
                for (Int j = start; j < end; ++j) {
                    body(j);
                }
            }(taskStart, taskEnd, body, priority, this));
        }

        for (auto& task : tasks) {
            co_await task;
        }
    }

    template <typename Int, typename F>
    void parallelFor(Int start, Int end, F body, int priority) {
        parallelForAsync(start, end, body, priority).get();
    }

private:
    size_t mNumThreads = 0;
    std::vector<std::thread> mThreads;

    struct QueuedTask {
        int priority;
        std::function<void()> fun;

        struct Comparator {
            bool operator()(const QueuedTask& a, const QueuedTask& b) {
                return a.priority < b.priority;
            }
        };
    };

    std::priority_queue<QueuedTask, std::vector<QueuedTask>, QueuedTask::Comparator> mTaskQueue;
    std::mutex mTaskQueueMutex;
    std::condition_variable mWorkerCondition;

    std::atomic<size_t> mNumTasksInSystem;
    std::mutex mSystemBusyMutex;
    std::condition_variable mSystemBusyCondition;
};

}
