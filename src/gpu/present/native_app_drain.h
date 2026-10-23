#pragma once

#include <functional>
#include <future>
#include <mutex>
#include <optional>
#include <vector>

#include "native_output_lease.h"

namespace rerevved::gpu
{

// One bounded drain and one immutable result, shared by every close caller.
class NativeAppDrain
{
public:
    struct Completion
    {
        std::shared_future<bool> result;
        OperationDeadline        deadline;
    };

    template <typename CloseAdmission, typename RequestStop>
    bool Start(CloseAdmission closeAdmission, RequestStop requestStop)
    {
        std::lock_guard lock(mutex_);
        if (started_)
            return false;
        started_  = true;
        deadline_ = OperationDeadline::After(std::chrono::seconds(5));
        closeAdmission(deadline_);
        requestStop();
        return true;
    }

    Completion GetCompletion()
    {
        std::lock_guard lock(mutex_);
        return { future_, deadline_ };
    }

    template <typename StopProducer, typename Shutdown>
    bool Run(StopProducer stopProducer, Shutdown shutdown)
    {
        auto deadline = GetCompletion().deadline;
        stopProducer();
        return shutdown(deadline) && !deadline.Expired();
    }

    void Subscribe(std::function<void(bool)> callback)
    {
        std::unique_lock lock(mutex_);
        if (!result_)
        {
            callbacks_.push_back(std::move(callback));
            return;
        }
        const bool result = *result_;
        lock.unlock();
        callback(result);
    }

    void Complete(bool result)
    {
        std::vector<std::function<void(bool)>> callbacks;
        {
            std::lock_guard lock(mutex_);
            if (result_)
                return;
            result_ = result;
            callbacks.swap(callbacks_);
            promise_.set_value(result);
        }
        for (const auto& callback : callbacks)
            callback(result);
    }

private:
    std::mutex                             mutex_;
    bool                                   started_  = false;
    OperationDeadline                      deadline_ = OperationDeadline::At(OperationDeadline::Clock::time_point::max());
    std::promise<bool>                     promise_;
    std::shared_future<bool>               future_ = promise_.get_future().share();
    std::optional<bool>                    result_;
    std::vector<std::function<void(bool)>> callbacks_;
};

} // namespace rerevved::gpu
