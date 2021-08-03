// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>

#include <atomic>
#include <functional>
#include <future>
#include <queue>
#include <thread>
#include <vector>

TEV_NAMESPACE_BEGIN

template <typename T>
void waitAll(std::vector<std::future<T>>& futures) {
    for (auto& f : futures) {
        f.get();
    }
}

class ThreadPool {
public:
    ThreadPool();
    ThreadPool(size_t maxNumThreads, bool force = false);
    virtual ~ThreadPool();

    template<class F>
    auto enqueueTask(F&& f, int priority) -> std::future<std::invoke_result_t<decltype(f)>> {
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

    void startThreads(size_t num);
    void shutdownThreads(size_t num);

    size_t numTasksInSystem() const {
        return mNumTasksInSystem;
    }

    void waitUntilFinished();
    void waitUntilFinishedFor(const std::chrono::microseconds Duration);
    void flushQueue();

    template <typename Int, typename F>
    auto parallelForAsync(Int start, Int end, F body, int priority) {
        Int range = end - start;
        Int nTasks = std::min((Int)mNumThreads, range);

        std::promise<void> promise;
        auto future = promise.get_future();

        auto callbackGuard = SharedScopeGuard{[p = std::move(promise)] () mutable {
            p.set_value();
        }};

        for (Int i = 0; i < nTasks; ++i) {
            Int taskStart = start + (range * i / nTasks);
            Int taskEnd = start + (range * (i+1) / nTasks);
            TEV_ASSERT(taskStart != taskEnd, "Shouldn't not produce tasks with empty range.");
            enqueueTask([callbackGuard, taskStart, taskEnd, body] {
                for (Int j = taskStart; j < taskEnd; ++j) {
                    body(j);
                }
            }, priority);
        }

        return future;
    }

    template <typename Int, typename F>
    void parallelFor(Int start, Int end, F body, int priority) {
        parallelForAsync(start, end, body, priority).get();
    }

private:
    size_t mNumThreads = 0;
    std::vector<std::thread> mThreads;

    struct Task {
        int priority;
        std::function<void()> fun;

        struct Comparator {
            bool operator()(const Task& a, const Task& b) {
                return a.priority < b.priority;
            }
        };
    };

    std::priority_queue<Task, std::vector<Task>, Task::Comparator> mTaskQueue;
    std::mutex mTaskQueueMutex;
    std::condition_variable mWorkerCondition;

    std::atomic<size_t> mNumTasksInSystem;
    std::mutex mSystemBusyMutex;
    std::condition_variable mSystemBusyCondition;
};

TEV_NAMESPACE_END
