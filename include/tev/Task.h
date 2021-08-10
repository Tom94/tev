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

#include <mutex>

TEV_NAMESPACE_BEGIN

// TODO: replace with std::latch when it's supported everywhere
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
    std::exception_ptr eptr;
    std::vector<std::function<void()>> onFinalSuspend;

    future_t get_return_object() noexcept {
        return {COROUTINE_NAMESPACE::coroutine_handle<TaskPromise<future_t, data_t>>::from_promise(*this)};
    }

    COROUTINE_NAMESPACE::suspend_never initial_suspend() const noexcept { return {}; }

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

        struct Awaiter {
            bool await_ready() const noexcept { return false; }
            void await_resume() const noexcept {}

            // Returning the parent coroutine has the effect of destroying this coroutine handle
            // and continuing execution where the parent co_await'ed us.
            COROUTINE_NAMESPACE::coroutine_handle<> await_suspend(COROUTINE_NAMESPACE::coroutine_handle<TaskPromise<future_t, data_t>> h) const noexcept {
                bool isLast = h.promise().latch.countDown();
                if (isLast && h.promise().precursor) {
                    return h.promise().precursor;
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

    bool await_suspend(COROUTINE_NAMESPACE::coroutine_handle<> coroutine) const noexcept {
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

// Ties the lifetime of a lambda coroutine's captures
// to that of the coroutine.
// Taken from https://stackoverflow.com/a/68630143
auto coLambda(auto&& executor) {
    return [executor=std::move(executor)]<typename ...Args>(Args&&... args) {
        using ReturnType = decltype(executor(args...));
        // copy the lambda into a new std::function pointer
        auto exec = new std::function<ReturnType(Args...)>(executor);
        // execute the lambda and save the result
        auto result = (*exec)(args...);
        // call custom method to save lambda until task ends
        result.finally([exec]() { delete exec; });
        return result;
    };
}

TEV_NAMESPACE_END
