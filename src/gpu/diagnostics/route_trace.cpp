#include "route_trace.h"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>

#include <rex/logging.h>

namespace rerevved::gpu::diagnostics
{

static_assert(std::atomic<std::uint64_t>::is_always_lock_free);
static_assert(std::atomic<unsigned>::is_always_lock_free);
static_assert(std::atomic<RouteRecorder*>::is_always_lock_free);
static_assert(std::size(kRouteEntries) == static_cast<std::size_t>(RouteFunction::Count));

std::atomic<unsigned>       routeTraceEnabled{ 0 };
std::atomic<RouteRecorder*> routeTraceRecorder{ nullptr };

void RouteRecorder::Record(RouteFunction function, std::uint64_t caller) noexcept
{
    const auto storage = kRouteStorageIndices[static_cast<std::size_t>(function)];
    if (storage.countOnly)
    {
        countOnly[storage.index].fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto&    target = functions[storage.index];
    unsigned epoch;
    for (;;)
    {
        epoch = segment.load(std::memory_order_seq_cst);
        if (epoch >= kRouteSegmentCapacity)
            return;
        target.writers[epoch].fetch_add(1, std::memory_order_seq_cst);
        if (epoch == segment.load(std::memory_order_seq_cst))
            break;
        target.writers[epoch].fetch_sub(1, std::memory_order_relaxed);
    }
    const auto frame    = frames.load(std::memory_order_relaxed);
    bool       recorded = false;
    // The empty sentinel cannot be a valid 32-bit guest return address.
    if (caller != kRouteNoFrame)
    {
        const auto start = (caller >> 2) % kRouteCallerCapacity;
        for (std::size_t probe = 0; probe < kRouteCallerCapacity; ++probe)
        {
            auto& slot = target.slots[(start + probe) % kRouteCallerCapacity];
            auto  key  = slot.caller.load(std::memory_order_relaxed);
            if (key == kRouteNoFrame)
            {
                slot.caller.compare_exchange_strong(key, caller, std::memory_order_relaxed);
                if (key == kRouteNoFrame)
                    key = caller;
            }
            if (key != caller)
                continue;
            auto& bank  = slot.banks[epoch];
            auto  first = bank.firstFrame.load(std::memory_order_relaxed);
            while (frame < first && !bank.firstFrame.compare_exchange_weak(first, frame, std::memory_order_relaxed))
            {
            }
            bank.count.fetch_add(1, std::memory_order_relaxed);
            recorded = true;
            break;
        }
    }
    if (!recorded)
        target.overflow[epoch].fetch_add(1, std::memory_order_relaxed);
    // Registration/recheck and the close/drain stay SC; release publishes bank writes.
    target.writers[epoch].fetch_sub(1, std::memory_order_release);
}

void RouteRecorder::FrameBoundary() noexcept
{
    frames.fetch_add(1, std::memory_order_relaxed);
}

RouteSnapshot RouteRecorder::CloseSegment()
{
    const auto closed = segment.load(std::memory_order_seq_cst);
    if (closed >= kRouteSegmentCapacity - 1)
        throw std::runtime_error("route trace segment capacity exhausted");
    segment.store(closed + 1, std::memory_order_seq_cst);
    RouteSnapshot result;
    result.beginFrame = beginFrame;
    result.frameCount = frames.load(std::memory_order_relaxed);
    beginFrame        = result.frameCount;
    for (std::size_t index = 0; index < std::size(kRouteEntries); ++index)
    {
        const auto storage = kRouteStorageIndices[index];
        if (storage.countOnly)
        {
            // One relaxed counter has a sampled boundary, without guest writer registration.
            const auto count = countOnly[storage.index].load(std::memory_order_relaxed);
            result.countOnly.push_back({ index, count, count - previousCountOnly[storage.index] });
            previousCountOnly[storage.index] = count;
            result.overflow.push_back({ index, 0, 0 });
            continue;
        }
        auto& function = functions[storage.index];
        while (function.writers[closed].load(std::memory_order_seq_cst))
            std::this_thread::yield();
        RouteOverflowCount overflow{ index, 0, function.overflow[closed].load(std::memory_order_relaxed) };
        for (unsigned bank = 0; bank <= closed; ++bank)
            overflow.count += function.overflow[bank].load(std::memory_order_relaxed);
        result.overflow.push_back(overflow);
        for (const auto& slot : function.slots)
        {
            RouteCallerCount row{ index, slot.caller.load(std::memory_order_relaxed), 0, slot.banks[closed].count.load(std::memory_order_relaxed), kRouteNoFrame, slot.banks[closed].firstFrame.load(std::memory_order_relaxed) };
            for (unsigned bank = 0; bank <= closed; ++bank)
            {
                row.count += slot.banks[bank].count.load(std::memory_order_relaxed);
                row.firstFrame = std::min(row.firstFrame, slot.banks[bank].firstFrame.load(std::memory_order_relaxed));
            }
            if (row.count)
                result.callers.push_back(row);
        }
    }
    return result;
}

bool WriteRouteSnapshot(const std::filesystem::path& directory, std::string_view segment, const RouteSnapshot& snapshot, std::string_view titleRevision, std::string_view sdkRevision, std::string& error)
{
    try
    {
        if (segment != "menu" && segment != "gameplay" && segment != "shutdown")
            throw std::runtime_error("invalid route trace segment name");
        const auto    path    = directory / ("route-" + std::string(segment) + ".toml");
        const auto    pending = directory / ("route-" + std::string(segment) + ".pending");
        std::ofstream file(pending, std::ios::out | std::ios::noreplace);
        file << "schema = 1\nsegment = " << std::quoted(std::string(segment))
             << "\ntitle_revision = " << std::quoted(std::string(titleRevision))
             << "\nsdk_revision = " << std::quoted(std::string(sdkRevision))
             << "\nentry_count = " << std::size(kRouteEntries)
             << "\ncaller_capacity = " << kRouteCallerCapacity
             << "\nrecorder_bytes = " << sizeof(RouteRecorder)
             << "\nbegin_frame = " << snapshot.beginFrame << "\nframe_count = " << snapshot.frameCount
             << "\nframe_boundary = \"before VdSwap\"\n";
        for (const auto& row : snapshot.callers)
        {
            const auto& entry = kRouteEntries[row.function];
            file << "\n[[callers]]\nfunction = " << entry.address
                 << "\ngroup = " << std::quoted(std::string(entry.groups))
                 << "\ncaller_lr = \"0x" << std::hex << row.caller << std::dec << "\"\ncount = " << row.count
                 << "\nsegment_count = " << row.segmentCount << "\nfirst_frame = " << row.firstFrame
                 << "\nsegment_first_frame = " << (row.segmentCount ? static_cast<std::int64_t>(row.segmentFirstFrame) : -1) << '\n';
        }
        for (const auto& row : snapshot.overflow)
            file << "\n[[overflow]]\nfunction = " << kRouteEntries[row.function].address
                 << "\ncount = " << row.count << "\nsegment_count = " << row.segmentCount << '\n';
        for (const auto& row : snapshot.countOnly)
            file << "\n[[count_only]]\nfunction = " << kRouteEntries[row.function].address
                 << "\ncount = " << row.count << "\nsegment_count = " << row.segmentCount
                 << "\nboundary = \"sampled relaxed counter\"\n";
        file.close();
        if (!file)
            throw std::runtime_error("could not write route trace snapshot");
        if (std::filesystem::exists(path))
            throw std::runtime_error("route trace snapshot already exists");
        std::filesystem::rename(pending, path);
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

namespace
{

struct Session
{
    std::filesystem::path          directory;
    std::string                    titleRevision, sdkRevision;
    std::unique_ptr<RouteRecorder> recorder = std::make_unique<RouteRecorder>();
    bool                           menu = false, gameplay = false;
    std::jthread                   worker;

    void Snapshot(std::string_view name)
    {
        std::string error;
        if (!WriteRouteSnapshot(directory, name, recorder->CloseSegment(), titleRevision, sdkRevision, error))
            throw std::runtime_error(error);
        REXLOG_INFO("Route trace wrote route-{}.toml", name);
    }

    void Poll()
    {
        if (!menu && std::filesystem::is_regular_file(directory / "menu"))
        {
            menu = true;
            Snapshot("menu");
        }
        if (menu && !gameplay && std::filesystem::is_regular_file(directory / "gameplay"))
        {
            gameplay = true;
            Snapshot("gameplay");
        }
    }
};

std::mutex lifecycle;
// Process-lifetime allocation also survives missing OnShutdown and CRT teardown.
Session* session = nullptr;

} // namespace

bool StartRouteTrace(const std::filesystem::path& directory, std::string_view titleRevision, std::string_view sdkRevision, std::string& error)
{
    std::lock_guard lock(lifecycle);
    try
    {
        if (session || !std::filesystem::create_directories(directory))
            throw std::runtime_error("route trace requires a fresh directory and one process session");
        session                = new Session();
        session->directory     = directory;
        session->titleRevision = titleRevision;
        session->sdkRevision   = sdkRevision;
        routeTraceRecorder.store(session->recorder.get(), std::memory_order_release);
        session->worker = std::jthread([](std::stop_token stop)
                                       {
                                           try
                                           {
                                               while (!stop.stop_requested())
                                               {
                                                   session->Poll();
                                                   std::this_thread::sleep_for(std::chrono::milliseconds(50));
                                               }
                                           }
                                           catch (const std::exception& exception)
                                           {
                                               REXLOG_ERROR("Route trace marker snapshot failed: {}", exception.what());
                                           }
                                       });
        routeTraceEnabled.store(true, std::memory_order_release);
        REXLOG_INFO("Route trace recording cold boot in {}", directory.string());
        return true;
    }
    catch (const std::exception& exception)
    {
        error = exception.what();
        return false;
    }
}

void NotifyRouteTraceFrameBoundary() noexcept
{
    if (routeTraceEnabled.load(std::memory_order_relaxed))
    {
        std::atomic_thread_fence(std::memory_order_acquire);
        routeTraceRecorder.load(std::memory_order_acquire)->FrameBoundary();
    }
}

void StopRouteTrace() noexcept
{
    std::lock_guard lock(lifecycle);
    if (!session || !session->worker.joinable())
        return;
    session->worker.request_stop();
    session->worker.join();
    try
    {
        session->Poll();
        routeTraceEnabled.store(false, std::memory_order_relaxed);
        session->Snapshot("shutdown");
    }
    catch (const std::exception& exception)
    {
        routeTraceEnabled.store(false, std::memory_order_relaxed);
        REXLOG_ERROR("Route trace shutdown snapshot failed: {}", exception.what());
    }
}

} // namespace rerevved::gpu::diagnostics
