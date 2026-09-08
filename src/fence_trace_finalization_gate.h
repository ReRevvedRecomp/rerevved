#pragma once

#include <condition_variable>
#include <mutex>
#include <utility>

namespace rerevved::diagnostics
{

class FenceTraceFinalizationGate final
{
public:
    template <typename Finalizer>
    bool Run(Finalizer&& finalizer)
    {
        std::unique_lock lock(mutex_);
        completed_.wait(lock, [this]()
                        {
                            return state_ != State::Running;
                        });
        if (state_ == State::Succeeded)
        {
            return true;
        }

        state_ = State::Running;
        lock.unlock();
        const bool succeeded = std::forward<Finalizer>(finalizer)();
        lock.lock();
        state_ = succeeded ? State::Succeeded : State::Idle;
        lock.unlock();
        completed_.notify_all();
        return succeeded;
    }

private:
    enum class State
    {
        Idle,
        Running,
        Succeeded,
    };

    std::mutex              mutex_;
    std::condition_variable completed_;
    State                   state_ = State::Idle;
};

} // namespace rerevved::diagnostics
