// This file was developed by Thomas Müller <thomas94@gmx.net>.
// It is published under the BSD 3-Clause License within the LICENSE file.

#pragma once

#include <tev/Common.h>

#ifdef __clang__
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

namespace tev {

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
            return {};
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
};

template <typename F, typename ...Args>
DetachedTask invokeTaskDetached(F&& executor, Args&&... args) {
    auto exec = std::move(executor);
    co_await exec(args...);
}

template <typename data_t>
class TaskPromiseBase {
public:
    void return_value(data_t&& value) noexcept { mPromise.set_value(std::move(value)); }
    void return_value(const data_t& value) noexcept { mPromise.set_value(value); }

protected:
    std::promise<data_t> mPromise;
};

template <>
class TaskPromiseBase<void> {
public:
    void return_void() noexcept { mPromise.set_value(); }

protected:
    std::promise<void> mPromise;
};

struct TaskSharedState {
    COROUTINE_NAMESPACE::coroutine_handle<> continuation = nullptr;
    Latch latch{2};
};

template <typename future_t, typename data_t>
class TaskPromise : public TaskPromiseBase<data_t> {
public:
    future_t get_return_object() noexcept {
        return {COROUTINE_NAMESPACE::coroutine_handle<TaskPromise<future_t, data_t>>::from_promise(*this), this->mPromise.get_future(), mState};
    }

    COROUTINE_NAMESPACE::suspend_never initial_suspend() const noexcept { return {}; }

    void unhandled_exception() {
        this->mPromise.set_exception(std::current_exception());
    }

    // The coroutine is about to complete (via co_return, reaching the end of the coroutine body,
    // or an uncaught exception). The awaiter returned here defines what happens next
    auto final_suspend() noexcept {
        class Awaiter {
        public:
            Awaiter(COROUTINE_NAMESPACE::coroutine_handle<> continuation) : mContinuation{continuation} {}

            bool await_ready() noexcept { return !mContinuation; }
            void await_resume() noexcept {}

            // Returning the continuation has the effect of continuing execution where the parent co_await'ed us.
            // It's the parent's job to call destroy on this coroutine's handle.
            COROUTINE_NAMESPACE::coroutine_handle<> await_suspend(COROUTINE_NAMESPACE::coroutine_handle<TaskPromise<future_t, data_t>>) noexcept {
                return mContinuation;
            }

        private:
            COROUTINE_NAMESPACE::coroutine_handle<> mContinuation;
        };

        bool isLast = mState->latch.countDown();
        return Awaiter{isLast ? mState->continuation : nullptr};
    }

private:
    std::shared_ptr<TaskSharedState> mState = std::make_shared<TaskSharedState>();
};

template <typename T>
class Task {
public:
    using promise_type = TaskPromise<Task<T>, T>;

    Task(COROUTINE_NAMESPACE::coroutine_handle<promise_type> handle, std::future<T>&& future, const std::shared_ptr<TaskSharedState>& state)
    : mHandle{handle}, mFuture{std::move(future)}, mState{state} {}

    // No copying allowed!
    Task(const Task& other) = delete;
    Task& operator=(const Task& other) = delete;

    Task& operator=(Task&& other) {
        mHandle = other.mHandle;
        other.mHandle = nullptr;
        mFuture = std::move(other.mFuture);
        mState = std::move(other.mState);
        return *this;
    }
    Task(Task&& other) {
        *this = std::move(other);
    }

    ~Task() {
        // Make sure the coroutine finished and is cleaned up
        if (mHandle) {
            tlog::warning() << "~Task<T> was invoked before completion.";
        }
    }

    bool await_ready() noexcept {
        // If the latch has already been passed by final_suspend()
        // above, the task has already completed and we can skip
        // suspension of the coroutine.
        if (mState->latch.value() <= 1) {
            mState->latch.countDown();
            mHandle = nullptr;
            return true;
        }

        return false;
    }

    T await_resume() {
        // If (and only if) a previously suspended coroutine is resumed here,
        // this task's own coroutine handle has not been cleaned up (for
        // implementation reasons) and needs to be destroyed here.
        // (See the behavior of final_suspend() above.)
        if (mHandle) {
            mHandle.destroy();
            mHandle = nullptr;
        }

        // Note: if there occurred an uncaught exception while executing
        // this task, it'll get rethrown in the following call.
        return mFuture.get();
    }

    T get() {
        if (!mHandle) {
            tlog::error() << "Cannot get()/co_await a task multiple times.";
        }

        mHandle = nullptr;
        return await_resume();
    }

    bool await_suspend(COROUTINE_NAMESPACE::coroutine_handle<> coroutine) noexcept {
        if (!mHandle) {
            tlog::error() << "Cannot co_await/get() a task multiple times.";
            std::terminate();
        }

        // If the task is still running (checked by arriving at the latch),
        // mark this coroutine as the task's continuation and suspend it until then.
        // The task's coroutine `mHandle` remains valid if suspended, implying that it
        // needs to be manually cleaned up on resumption of this coroutine (see await_resume()).
        mState->continuation = coroutine;
        bool shallSuspend = !mState->latch.countDown();
        if (!shallSuspend) {
            mHandle = nullptr;
        }
        return shallSuspend;
    }

private:
    COROUTINE_NAMESPACE::coroutine_handle<promise_type> mHandle;
    std::future<T> mFuture;
    std::shared_ptr<TaskSharedState> mState;
};

}
