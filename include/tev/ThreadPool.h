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

#include <experimental/coroutine>

TEV_NAMESPACE_BEGIN

class Latch {
public:
    Latch(int val) : mCounter{val} {}
    bool countDown() noexcept {
        std::unique_lock lock{mMutex};
        bool result = (--mCounter == 0);
        if (result) {
            mCv.notify_all();
        }
        return result;
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

template <typename T>
void waitAll(std::vector<T>& futures) {
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
    std::exception_ptr eptr;
    std::vector<std::function<void()>> onFinalSuspend;

    future_t get_return_object() noexcept {
        return {std::experimental::coroutine_handle<TaskPromise<future_t, data_t>>::from_promise(*this)};
    }

    std::experimental::suspend_never initial_suspend() const noexcept { return {}; }

    void unhandled_exception() {
        eptr = std::current_exception();
    }

    template <typename F>
    void finally(F&& fun) {
        onFinalSuspend.emplace_back(std::forward<F>(fun));
    }

    // The coroutine is about to complete (via co_return or reaching the end of the coroutine body).
    // The awaiter returned here defines what happens next
    auto final_suspend() const noexcept {
        for (auto& f : onFinalSuspend) {
            f();
        }

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
                if (isLast && h.promise().precursor) {
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

    template <typename F>
    void finally(F&& fun) {
        handle.promise().finally(std::forward<F>(fun));
    }

    T await_resume() const {
        if (handle.promise().eptr) {
            std::rethrow_exception(handle.promise().eptr);
        }

        if constexpr (!std::is_void_v<T>) {
            // The returned value here is what `co_await our_task` evaluates to
            return std::move(handle.promise().data);
        }
    }

    bool await_suspend(std::experimental::coroutine_handle<> coroutine) const noexcept {
        // The coroutine itself is being suspended (async work can beget other async work)
        // Record the argument as the continuation point when this is resumed later. See
        // the final_suspend awaiter on the promise_type above for where this gets used
        handle.promise().precursor = coroutine;
        return !handle.promise().latch.countDown();
    }

    void wait() const {
        handle.promise().latch.countDown();
        handle.promise().latch.wait();
    }

    T get() const {
        wait();
        if (handle.promise().eptr) {
            std::rethrow_exception(handle.promise().eptr);
        }
        if constexpr (!std::is_void_v<T>) {
            return std::move(handle.promise().data);
        }
    }
};

template<typename T>
void coCaptureVar(Task<T>& task, auto* var) {
    task.finally([var]() {
        delete var;
    });
}

inline auto coLambda(auto&& executor) {
    return [executor=std::move(executor)]<typename ...Args>(Args&&... args) {
        using ReturnType = decltype(executor(args...));
        // copy the lambda into a new std::function pointer
        auto exec = new std::function<ReturnType(Args...)>(executor);
        // execute the lambda and save the result
        auto result = (*exec)(args...);
        // call custom method to save lambda until task ends
        coCaptureVar(result, exec);
        return result;
    };
}

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
            TEV_ASSERT(taskStart != taskEnd, "Shouldn't not produce tasks with empty range.");

            tasks.emplace_back([this](Int start, Int end, F body, int priority) -> Task<void> {
                co_await schedule(priority);
                for (Int j = start; j < end; ++j) {
                    body(j);
                }
            }(taskStart, taskEnd, body, priority));
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

TEV_NAMESPACE_END
