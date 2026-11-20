#include "support_log.h"

#include "build_info.h"

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>
#include <rex/runtime.h>

#include <array>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/utsname.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <mach/mach.h>
#endif
#endif

namespace rerevved::support_log
{
namespace
{

std::mutex              contextMutex;
std::string             currentPhase;
std::optional<uint32_t> currentTurn;

void PublishContextLocked()
{
    rex::SetCrashContext(detail::ContextText(REREVVED_VERSION, REREVVED_GIT_COMMIT, currentPhase, currentTurn));
}

std::string SystemText()
{
    const unsigned threads = std::thread::hardware_concurrency();
#if defined(_WIN32)
    std::string os = "Windows";
    // GetVersionEx reports the manifest's compatibility version, not the real one.
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    if (const HMODULE ntdll = GetModuleHandleW(L"ntdll.dll"))
    {
        if (const auto get = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion")))
        {
            OSVERSIONINFOW info{};
            info.dwOSVersionInfoSize = sizeof(info);
            if (get(&info) == 0)
                os = fmt::format("Windows {}.{} build {}", info.dwMajorVersion, info.dwMinorVersion, info.dwBuildNumber);
        }
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength        = sizeof(memory);
    const uint64_t totalMb = GlobalMemoryStatusEx(&memory) ? memory.ullTotalPhys >> 20 : 0;
    const uint64_t freeMb  = memory.ullAvailPhys >> 20;
#else
    std::string os = "unknown OS";
    utsname     name{};
    if (uname(&name) == 0)
        os = fmt::format("{} {} {}", name.sysname, name.release, name.machine);
    const long pages    = sysconf(_SC_PHYS_PAGES);
    const long pageSize = sysconf(_SC_PAGESIZE);
#if defined(__APPLE__)
    // macOS has no _SC_AVPHYS_PAGES; inactive pages are reclaimable like free ones.
    long                   avail = 0;
    vm_statistics64_data_t vm{};
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&vm), &count) == KERN_SUCCESS)
        avail = static_cast<long>(vm.free_count + vm.inactive_count);
#else
    const long avail = sysconf(_SC_AVPHYS_PAGES);
#endif
    const uint64_t totalMb = pages > 0 && pageSize > 0 ? static_cast<uint64_t>(pages) * pageSize >> 20 : 0;
    const uint64_t freeMb  = avail > 0 && pageSize > 0 ? static_cast<uint64_t>(avail) * pageSize >> 20 : 0;
#endif
    return fmt::format("System: {}, {} CPU threads, {} MB RAM ({} MB free)", os, threads, totalMb, freeMb);
}

std::string SettingsText()
{
    static constexpr std::array kNames{ "fullscreen", "window_size", "monitor", "resolution_scale", "vsync", "user_language", "skip_intro_movie", "live_sessions" };
    std::string                 text = "Settings:";
    for (const char* name : kNames)
    {
        const std::string value = rex::cvar::GetFlagByName(name);
        text += fmt::format(" {}={}", name, value.empty() ? "\"\"" : value);
    }
    return text;
}

} // namespace

void LogStartup(const std::filesystem::path& userDataRoot)
{
    REXLOG_INFO("ReRevved {} {}", REREVVED_VERSION, REREVVED_BUILD_INFO);
    REXLOG_INFO("{}", SystemText());
    REXLOG_INFO("Paths: game={} user_data={} log={}",
                rex::path_to_utf8(rex::filesystem::GetExecutableFolder()),
                rex::path_to_utf8(userDataRoot),
                rex::path_to_utf8(rex::GetLogFilePath()));
    REXLOG_INFO("{}", SettingsText());
    std::lock_guard lock(contextMutex);
    PublishContextLocked();
}

void LogLoadout(const rex::Runtime& runtime)
{
    std::vector<detail::Package> mods;
    for (const auto& mod : runtime.active_mods_info())
        mods.push_back({ mod.id, mod.version });
    std::vector<detail::Package> assets;
    for (const auto& asset : runtime.active_asset_overlays())
        assets.push_back({ asset.id, asset.version });
    REXLOG_INFO("Mods: {} active: {}", mods.size(), detail::PackageList(mods));
    REXLOG_INFO("Asset packages: {} active: {}", assets.size(), detail::PackageList(assets));
}

void EnterPhase(std::string_view phase)
{
    REXLOG_INFO("Phase: {}", phase);
    std::lock_guard lock(contextMutex);
    currentPhase = phase;
    currentTurn.reset();
    PublishContextLocked();
}

void NoteTurn(uint32_t turn)
{
    std::lock_guard lock(contextMutex);
    if (currentTurn == turn)
        return;
    currentTurn = turn;
    REXLOG_INFO("Turn {}", turn);
    PublishContextLocked();
}

} // namespace rerevved::support_log
