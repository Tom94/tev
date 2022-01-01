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

template <typename future_t, typename data_t>
struct TaskPromise : public TaskPromiseBase<data_t> {
    COROUTINE_NAMESPACE::coroutine_handle<> precursor;
    Latch latch{2};

    future_t get_return_object() noexcept {
        return {COROUTINE_NAMESPACE::coroutine_handle<TaskPromise<future_t, data_t>>::from_promise(*this), this->promise.get_future()};
    }

    COROUTINE_NAMESPACE::suspend_never initial_suspend() const noexcept { return {}; }

    void unhandled_exception() {
        this->promise.set_exception(std::current_exception());
    }

    // The coroutine is about to complete (via co_return, reaching the end of the coroutine body,
    // or an uncaught exception). The awaiter returned here defines what happens next
    auto final_suspend() const noexcept {
        struct Awaiter {
            bool await_ready() const noexcept { return false; }
            void await_resume() const noexcept {}

            // Returning the parent coroutine has the effect of continuing execution where the parent co_await'ed us.
            COROUTINE_NAMESPACE::coroutine_handle<> await_suspend(COROUTINE_NAMESPACE::coroutine_handle<TaskPromise<future_t, data_t>> h) const noexcept {
                bool isLast = h.promise().latch.countDown();
                auto precursor = h.promise().precursor;

                if (isLast) {
                    h.destroy();
                    if (precursor) {
                        return precursor;
                    }
                }

                return COROUTINE_NAMESPACE::noop_coroutine();
            }
        };

        return Awaiter{};
    }
};

template <typename T>
struct Task {
    using promise_type = TaskPromise<Task<T>, T>;

    // This handle is assigned to when the coroutine itself is suspended (see await_suspend above)
    COROUTINE_NAMESPACE::coroutine_handle<promise_type> handle;
    std::future<T> future;

    Task(COROUTINE_NAMESPACE::coroutine_handle<promise_type> handle, std::future<T>&& future) : handle{handle}, future{std::move(future)} {}

    // No copying allowed!
    Task(const Task& other) = delete;
    Task& operator=(const Task& other) = delete;

    Task& operator=(Task&& other) {
        handle = other.handle;
        future = std::move(other.future);
        other.detach();
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
        return false;
    }

    T await_resume() {
        TEV_ASSERT(handle, "Cannot resume a detached Task<T>.");

        // `handle` has already (or will be) destroyed by either
        // - TaskPromise::final_suspend::Awaiter::await_suspend, or
        // - Task::get,
        // one of which will have been called prior to await_resume.
        handle = nullptr;
        return future.get();
    }

    T get() {
        if (handle.promise().latch.countDown()) {
            handle.destroy();
        }
        return await_resume();
    }

    bool await_suspend(COROUTINE_NAMESPACE::coroutine_handle<> coroutine) noexcept {
        if (!handle) {
            tlog::error() << "Cannot co_await a detached Task<T>.";
            std::terminate();
        }

        // The coroutine itself is being suspended (async work can beget other async work)
        // Record the argument as the continuation point when this is resumed later. See
        // the final_suspend awaiter on the promise_type above for where this gets used
        handle.promise().precursor = coroutine;
        bool isLast = handle.promise().latch.countDown();
        if (isLast) {
            handle.destroy();
        }
        return !isLast;
    }

    COROUTINE_NAMESPACE::coroutine_handle<promise_type> detach() noexcept {
        auto tmp = handle;
        handle = nullptr;
        return tmp;
    }
};

TEV_NAMESPACE_END
