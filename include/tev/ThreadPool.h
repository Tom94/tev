// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>

#include <atomic>
#include <functional>
#include <future>
// #include <latch>
#include <queue>
#include <thread>
#include <vector>

#include <experimental/coroutine>

class Latch {
public:
    Latch(int val) : mCounter{val} {}
    bool countDown() noexcept {
        bool result = (--mCounter == 0);
        if (result) {
            std::unique_lock lock{mMutex};
            mCv.notify_all();
        }
        return result;
    }

    bool tryWait() {
        return mCounter == 0;
    }

    void wait() {
        if (mCounter <= 0) {
            return;
        }

        std::unique_lock lock{mMutex};
        if (mCounter > 0) {
            mCv.wait(lock);
        }
    }
private:
    std::atomic<int> mCounter;
    std::mutex mMutex;
    std::condition_variable mCv;
};

TEV_NAMESPACE_BEGIN

template <typename T>
void waitAll(std::vector<std::future<T>>& futures) {
    for (auto& f : futures) {
        f.get();
    }
}

template <typename data_t>
struct TaskPromiseBase {
    data_t data;

    // When the coroutine co_returns a value, this method is used to publish the result
    void return_value(data_t value) noexcept {
        data = std::move(value);
    }
};

template <>
struct TaskPromiseBase<void> {
    void return_void() noexcept {}
};

template <typename future_t, typename data_t>
struct TaskPromise : public TaskPromiseBase<data_t> {
    std::experimental::coroutine_handle<> precursor;
    Latch latch{2};

    future_t get_return_object() noexcept {
        return {std::experimental::coroutine_handle<TaskPromise<future_t, data_t>>::from_promise(*this)};
    }

    std::experimental::suspend_never initial_suspend() const noexcept { return {}; }

    void unhandled_exception() {
        tlog::error() << "Unhandled exception in Task<T>";
    }

    // The coroutine is about to complete (via co_return or reaching the end of the coroutine body).
    // The awaiter returned here defines what happens next
    auto final_suspend() const noexcept {
        struct awaiter {
            // Return false here to return control to the thread's event loop. Remember that we're
            // running on some async thread at this point.
            bool await_ready() const noexcept { return false; }

            void await_resume() const noexcept {}

            // Returning a coroutine handle here resumes the coroutine it refers to (needed for
            // continuation handling). If we wanted, we could instead enqueue that coroutine handle
            // instead of immediately resuming it by enqueuing it and returning void.
            std::experimental::coroutine_handle<> await_suspend(std::experimental::coroutine_handle<TaskPromise<future_t, data_t>> h) const noexcept {
                bool isLast = h.promise().latch.countDown();
                if (isLast) {
                    if (!h.promise().precursor) {
                        tlog::error() << "Precursor must be defined when being last.";
                    }
                    return h.promise().precursor;
                }

                return std::experimental::noop_coroutine();
            }
        };

        return awaiter{};
    }
};

template <typename T>
struct Task {
    using promise_type = TaskPromise<Task<T>, T>;

    // This handle is assigned to when the coroutine itself is suspended (see await_suspend above)
    std::experimental::coroutine_handle<promise_type> handle;

    // The following methods make our task type conform to the awaitable concept, so we can
    // co_await for a task to complete
    bool await_ready() const noexcept {
        // No need to suspend if this task has no outstanding work
        return handle.done();
    }

    T await_resume() const noexcept {
        // The returned value here is what `co_await our_task` evaluates to
        return std::move(handle.promise().data);
    }

    bool await_suspend(std::experimental::coroutine_handle<> coroutine) const noexcept {
        // The coroutine itself is being suspended (async work can beget other async work)
        // Record the argument as the continuation point when this is resumed later. See
        // the final_suspend awaiter on the promise_type above for where this gets used
        handle.promise().precursor = coroutine;

        return !handle.promise().latch.countDown();
    }

    void wait() const {
        handle.promise().latch.wait();
    }

    T get() const {
        wait();
        if constexpr (!std::is_void_v<T>) {
            return std::move(handle.promise().data);
        }
    }
};

class ThreadPool {
public:
    ThreadPool();
    ThreadPool(size_t maxNumThreads, bool force = false);
    virtual ~ThreadPool();

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

    inline auto schedule(int priority) noexcept {
        class Awaiter {
        public:
            Awaiter(ThreadPool* pool, int priority)
            : mPool{pool}, mPriority{priority} {}

            // Unlike the OS event case, there's no case where we suspend and the work
            // is immediately ready
            bool await_ready() const noexcept { return false; }

            // Since await_ready() always returns false, when suspend is called, we will
            // always immediately suspend and call this function (which enqueues the coroutine
            // for immediate reactivation on a different thread)
            void await_suspend(std::experimental::coroutine_handle<> coroutine) noexcept {
                mPool->enqueueTask(coroutine, mPriority);
            }

            void await_resume() const noexcept {
                tlog::info() << "Suspended task now running on thread " << std::this_thread::get_id();
            }

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
