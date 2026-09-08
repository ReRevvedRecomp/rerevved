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
        std::unique_lock lock(mutex);
        completed.wait(lock, [this]()
                       {
                           return state != State::Running;
                       });
        if (state == State::Succeeded)
        {
            return true;
        }

        state = State::Running;
        lock.unlock();
        const bool succeeded = std::forward<Finalizer>(finalizer)();
        lock.lock();
        state = succeeded ? State::Succeeded : State::Idle;
        lock.unlock();
        completed.notify_all();
        return succeeded;
    }

private:
    enum class State
    {
        Idle,
        Running,
        Succeeded,
    };

    std::mutex              mutex;
    std::condition_variable completed;
    State                   state = State::Idle;
};

} // namespace rerevved::diagnostics
