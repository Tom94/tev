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
#include <mutex>
#include <semaphore>

TEV_NAMESPACE_BEGIN

class Latch {
public:
    Latch(int val) : mCounter{val} {}
    bool countDown() noexcept {
        std::unique_lock lock{mMutex};
        int val = --mCounter;
        if (val <= 0) {
            mCv.notify_all();
            return true;
        }

        if (val < 0) {
            tlog::warning() << "Latch should never count below zero.";
        }

        return false;
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

// The task implementation is inspired by a sketch from the following blog post:
// https://www.jeremyong.com/cpp/2021/01/04/cpp20-coroutines-a-minimal-async-framework/
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
    COROUTINE_NAMESPACE::coroutine_handle<> precursor;
    Latch latch{2};
    std::atomic<bool> done = false; // TODO: use a std::binary_semaphore once that's available on macOS
    std::exception_ptr eptr;

    future_t get_return_object() noexcept {
        return {COROUTINE_NAMESPACE::coroutine_handle<TaskPromise<future_t, data_t>>::from_promise(*this)};
    }

    COROUTINE_NAMESPACE::suspend_never initial_suspend() const noexcept { return {}; }

    void unhandled_exception() {
        eptr = std::current_exception();
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
                h.promise().done = true; // Allow destroying this coroutine's handle
                if (isLast && precursor) {
                    return precursor;
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

    Task(COROUTINE_NAMESPACE::coroutine_handle<promise_type> handle) : handle{handle} {}

    // No copying allowed!
    Task(const Task& other) = delete;
    Task& operator=(const Task& other) = delete;

    Task(Task&& other) {
        handle = other.handle;
        other.detach();
    }
    Task& operator=(const Task&& other) {
        handle = other.handle;
        other.detach();
    }

    ~Task() {
        // Make sure the coroutine finished and is cleaned up
        if (handle) {
            tlog::warning() << "~Task<T> was waiting for completion. This was likely not intended.";
            wait();
            clear();
        }
    }

    bool await_ready() const noexcept {
        // No need to suspend if this task has no outstanding work
        return handle.done();
    }

    T await_resume() {
        TEV_ASSERT(handle, "Should not have been able to co_await a detached Task<T>.");

        ScopeGuard guard{[this] {
            // Spinlock is fine since this will always
            // be set in the next couple of instructions
            // of the task's executing thread.
            while (!handle.promise().done) {}
            clear();
        }};

        auto eptr = handle.promise().eptr;
        if (eptr) {
            std::rethrow_exception(eptr);
        }

        if constexpr (!std::is_void_v<T>) {
            // The returned value here is what `co_await our_task` evaluates to
            T tmp = std::move(handle.promise().data);
            return tmp;
        }
    }

    bool await_suspend(COROUTINE_NAMESPACE::coroutine_handle<> coroutine) const noexcept {
        if (!handle) {
            tlog::error() << "Cannot co_await a detached Task<T>.";
            std::terminate();
        }

        // The coroutine itself is being suspended (async work can beget other async work)
        // Record the argument as the continuation point when this is resumed later. See
        // the final_suspend awaiter on the promise_type above for where this gets used
        handle.promise().precursor = coroutine;
        return !handle.promise().latch.countDown();
    }

    bool wait() const {
        if (!handle) {
            throw std::runtime_error{"Cannot wait for a detached Task<T>."};
        }

        if (!handle.promise().latch.countDown()) {
            handle.promise().latch.wait();
            return true;
        }
        return false;
    }

    T get() {
        wait();
        return await_resume();
    }

    COROUTINE_NAMESPACE::coroutine_handle<promise_type> detach() noexcept {
        auto tmp = handle;
        handle = nullptr;
        return tmp;
    }

private:
    void clear() noexcept {
        if (handle) {
            // The state is automatically destroyed/freed when the coroutine
            // finishes via co_return or an exception. No need to do it
            // manually here. (Further: manual destruction appears to lead
            // wo mysterious crashed on Windows. Warrants investigation.)
            // handle.destroy();
            handle = nullptr;
        }
    }
};

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
            } catch(const std::exception& e) {
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

TEV_NAMESPACE_END
