// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>

#ifdef __APPLE__
#include <experimental/coroutine>
#define COROUTINE_NAMESPACE std::experimental
#else
#include <coroutine>
#define COROUTINE_NAMESPACE std
#endif

#include <condition_variable>
#include <future>
#include <mutex>
#include <semaphore>

TEV_NAMESPACE_BEGIN

class Latch {
public:
    Latch(int val) : mCounter{val} {}
    bool countDown() noexcept {
        int val = --mCounter;
        if (val < 0) {
            tlog::warning() << "Latch should never count below zero.";
        }

        return val <= 0;
    }

    int value() const {
        return mCounter;
    }

private:
    std::atomic<int> mCounter;
};

template <typename T>
void waitAll(std::vector<T>& futures) {
    for (auto& f : futures) {
        f.get();
    }
}

struct DetachedTask {
    struct promise_type {
        DetachedTask get_return_object() noexcept {
            return {COROUTINE_NAMESPACE::coroutine_handle<promise_type>::from_promise(*this)};
        }

        COROUTINE_NAMESPACE::suspend_never initial_suspend() const noexcept { return {}; }
        COROUTINE_NAMESPACE::suspend_never final_suspend() const noexcept { return {}; }

        void return_void() {}
        void unhandled_exception() {
            try {
                std::rethrow_exception(std::current_exception());
            } catch (const std::exception& e) {
                tlog::error() << "Unhandled exception in DetachedTask: " << e.what();
                std::terminate();
            }
        }
    };

    COROUTINE_NAMESPACE::coroutine_handle<promise_type> handle;
};

template <typename F, typename ...Args>
DetachedTask invokeTaskDetached(F&& executor, Args&&... args) {
    auto exec = std::move(executor);
    co_await exec(args...);
}

// The task implementation is inspired by a sketch from the following blog post:
// https://www.jeremyong.com/cpp/2021/01/04/cpp20-coroutines-a-minimal-async-framework/
template <typename data_t>
struct TaskPromiseBase {
    std::promise<data_t> promise;

    // When the coroutine co_returns a value, this method is used to publish the result
    void return_value(data_t&& value) noexcept {
        promise.set_value(std::move(value));
    }

    void return_value(const data_t& value) noexcept {
        promise.set_value(value);
    }
};

template <>
struct TaskPromiseBase<void> {
    std::promise<void> promise;

    void return_void() noexcept {
        promise.set_value();
    }
};

struct TaskSharedState {
    COROUTINE_NAMESPACE::coroutine_handle<> continuation = nullptr;
    Latch latch{2};
};

template <typename future_t, typename data_t>
struct TaskPromise : public TaskPromiseBase<data_t> {
    std::shared_ptr<TaskSharedState> state = std::make_shared<TaskSharedState>();

    future_t get_return_object() noexcept {
        return {COROUTINE_NAMESPACE::coroutine_handle<TaskPromise<future_t, data_t>>::from_promise(*this), this->promise.get_future(), state};
    }

    COROUTINE_NAMESPACE::suspend_never initial_suspend() const noexcept { return {}; }

    void unhandled_exception() {
        this->promise.set_exception(std::current_exception());
    }

    // The coroutine is about to complete (via co_return, reaching the end of the coroutine body,
    // or an uncaught exception). The awaiter returned here defines what happens next
    auto final_suspend() noexcept {
        struct Awaiter {
            COROUTINE_NAMESPACE::coroutine_handle<> continuation;

            bool await_ready() noexcept { return !continuation; }
            void await_resume() noexcept {}

            // Returning the continuation has the effect of continuing execution where the parent co_await'ed us.
            // It's the parent's job to call destroy on this coroutine's handle.
            COROUTINE_NAMESPACE::coroutine_handle<> await_suspend(COROUTINE_NAMESPACE::coroutine_handle<TaskPromise<future_t, data_t>>) noexcept {
                return continuation;
            }
        };

        bool isLast = state->latch.countDown();
        return Awaiter{isLast ? state->continuation : nullptr};
    }
};

template <typename T>
struct Task {
    using promise_type = TaskPromise<Task<T>, T>;

    // This handle is assigned to when the coroutine itself is suspended (see await_suspend above)
    COROUTINE_NAMESPACE::coroutine_handle<promise_type> handle;
    std::future<T> future;
    std::shared_ptr<TaskSharedState> state;
    bool wasSuspended = false;

    Task(COROUTINE_NAMESPACE::coroutine_handle<promise_type> handle, std::future<T>&& future, const std::shared_ptr<TaskSharedState>& state)
    : handle{handle}, future{std::move(future)}, state{state} {}

    // No copying allowed!
    Task(const Task& other) = delete;
    Task& operator=(const Task& other) = delete;

    Task& operator=(Task&& other) {
        handle = other.handle;
        other.handle = nullptr;
        future = std::move(other.future);
        state = std::move(other.state);
        wasSuspended = other.wasSuspended;
        return *this;
    }
    Task(Task&& other) {
        *this = std::move(other);
    }

    ~Task() {
        // Make sure the coroutine finished and is cleaned up
        if (handle) {
            tlog::warning() << "~Task<T> was invoked before completion.";
        }
    }

    bool await_ready() const noexcept {
        // If the latch has already been passed by final_suspend()
        // above, the task has already completed and we can skip
        // suspension of the coroutine.
        if (state->latch.value() <= 1) {
            state->latch.countDown();
            return true;
        }

        return false;
    }

    T await_resume() {
        TEV_ASSERT(handle, "Cannot resume a detached Task<T>.");

        // If (and only if) a previously suspended coroutine is resumed here,
        // this task's own coroutine handle has not been cleaned up (for
        // implementation reasons) and needs to be destroyed here.
        // (See the behavior of final_suspend() above.)
        if (wasSuspended) {
            handle.destroy();
        }

        // This task's coroutine has definitely been destoyed by now.
        // Mark this by setting its handle to null.
        handle = nullptr;

        // Note: if there occurred an uncaught exception while executing
        // this task, it'll get rethrown in the following call.
        return future.get();
    }

    T get() {
        return await_resume();
    }

    bool await_suspend(COROUTINE_NAMESPACE::coroutine_handle<> coroutine) noexcept {
        if (!handle) {
            tlog::error() << "Cannot co_await a detached Task<T>.";
            std::terminate();
        }

        // If the task is still running (checked by arriving at the latch),
        // mark this coroutine as the task's continuation and suspend it until then.
        // The member variable `wasSuspended` indicates this suspension and implies
        // that the state state of this task needs to be manually cleaned up on
        // resumption of this coroutine (see await_resume()).
        state->continuation = coroutine;
        wasSuspended = !state->latch.countDown();
        return wasSuspended;
    }
};

TEV_NAMESPACE_END
