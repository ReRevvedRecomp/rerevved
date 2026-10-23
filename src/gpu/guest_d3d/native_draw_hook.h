#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <optional>

struct PPCContext;

namespace rerevved::gpu::guest_d3d
{

// Draw hooks run Original except consume roots from indexed_buffer_consume.h or
// user_pointer_consume.h; consumed wrappers skip nested lowerer/sink.
enum class NativeDrawHook : std::uint8_t
{
    IndexedBuffer,    // 0x826A39F8
    IndexedUp,        // 0x826A3568; calls IndexedUpLowerer at 0x826A35AC
    NonindexedUp,     // 0x826A3000; calls NonindexedSink at 0x826A3020
    IndexedUpLowerer, // 0x826A3048; also called directly by guest code
    NonindexedSink,   // 0x826A2B58; also called directly by guest code
    AutomaticIndex,   // 0x826A3608
};
inline constexpr std::size_t kNativeDrawHookCount = 6;
static_assert(static_cast<std::size_t>(NativeDrawHook::AutomaticIndex) + 1 == kNativeDrawHookCount);

inline constexpr std::array<std::uint32_t, kNativeDrawHookCount> kNativeDrawHookAddresses{
    0x826A39F8, 0x826A3568, 0x826A3000, 0x826A3048, 0x826A2B58, 0x826A3608
};

// Return addresses of the static child calls inside the two outer UP wrappers.
inline constexpr std::uint32_t kIndexedUpLowererReturn = 0x826A35B0;
inline constexpr std::uint32_t kNonindexedSinkReturn   = 0x826A3024;

// 0x826A2FF0 publishes a pending sink draw (G+0x30 = G+0x3484). A direct
// IndexedUpLowerer caller fills the reserved vertices and indices, then calls it.
inline constexpr std::uint32_t kNativeDrawCommitAddress = 0x826A2FF0;

constexpr std::uint32_t NativeDrawHookAddress(NativeDrawHook hook) noexcept
{
    return kNativeDrawHookAddresses[static_cast<std::size_t>(hook)];
}

constexpr std::optional<NativeDrawHook> FindNativeDrawHook(std::uint32_t address) noexcept
{
    for (std::size_t i = 0; i < kNativeDrawHookCount; ++i)
        if (kNativeDrawHookAddresses[i] == address)
            return static_cast<NativeDrawHook>(i);
    return std::nullopt;
}

class NativeDrawDispatcher;

// Null keeps every hook on its unchanged Original path. Owners publish and
// withdraw it only through Install/RemoveNativeDrawDispatcher.
extern std::atomic<NativeDrawDispatcher*> nativeDrawDispatcher;
// Root draws still holding a dispatcher; nonzero whenever any thread has one in flight.
extern std::atomic<std::uint64_t> nativeDrawInFlight;

// Set while this thread runs an observed draw, so its children fold into it
// even after the dispatcher was withdrawn.
inline thread_local bool nativeDrawThreadActive = false;

struct NativeDrawToken
{
    std::uint64_t generation = 0; // zero: nothing observed
    bool          root       = false;
    int           exceptions = 0;
};

NativeDrawToken BeginNativeDraw(NativeDrawHook hook, const PPCContext& ctx, const std::uint8_t* base) noexcept;
void            EndNativeDraw(const NativeDrawToken& token, const PPCContext& ctx) noexcept;
// Finishes this thread's held direct-lowerer root at its commit entry.
void ObserveNativeDrawCommit(const PPCContext& ctx, const std::uint8_t* base) noexcept;

// A held root keeps the in-flight count and thread flag set, so other threads
// and idle commits pay only these two loads.
inline void NoteNativeDrawCommit(const PPCContext& ctx, const std::uint8_t* base) noexcept
{
    if (nativeDrawInFlight.load(std::memory_order_relaxed) && nativeDrawThreadActive) [[unlikely]]
        ObserveNativeDrawCommit(ctx, base);
}

class NativeDrawScope
{
public:
    NativeDrawScope(NativeDrawHook hook, const PPCContext& ctx, const std::uint8_t* base) noexcept
    {
        // A thread's own in-flight root keeps the counter nonzero, so the
        // thread-local flag is read only while some draw is in flight.
        if (nativeDrawDispatcher.load(std::memory_order_relaxed) ||
            (nativeDrawInFlight.load(std::memory_order_relaxed) && nativeDrawThreadActive)) [[unlikely]]
        {
            token   = BeginNativeDraw(hook, ctx, base);
            context = &ctx;
        }
    }

    ~NativeDrawScope()
    {
        if (token.generation)
            EndNativeDraw(token, *context);
    }

    NativeDrawScope(const NativeDrawScope&)            = delete;
    NativeDrawScope& operator=(const NativeDrawScope&) = delete;

    // This scope observes a call: a root with a live dispatcher, or a call nested in one.
    bool Observed() const noexcept
    {
        return token.generation != 0;
    }

    // Names the root this scope belongs to; zero when it observes nothing.
    std::uint64_t Generation() const noexcept
    {
        return token.generation;
    }

    // This scope is the outermost observed draw of its thread, holding a dispatcher.
    bool Root() const noexcept
    {
        return token.generation != 0 && token.root;
    }

    // r1 of the observed call at its entry; zero when the scope observes nothing.
    std::uint32_t EntryStack() const noexcept;
    // r13 (the guest KPCR) of the observed call at its entry; zero when the scope observes nothing.
    std::uint32_t EntryPcr() const noexcept;

private:
    NativeDrawToken   token;
    const PPCContext* context = nullptr;
};

struct NativeDrawUnobserved
{
};

// Expands to a scope only for the six draw entries so shared hook macros cost
// nothing for every other entry.
template <std::uint32_t Address>
auto ObserveNativeDraw(const PPCContext& ctx, const std::uint8_t* base) noexcept
{
    if constexpr (constexpr auto hook = FindNativeDrawHook(Address); hook.has_value())
        return NativeDrawScope(*hook, ctx, base);
    else
        return NativeDrawUnobserved{};
}

} // namespace rerevved::gpu::guest_d3d
