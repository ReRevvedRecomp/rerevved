#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <list>
#include <map>
#include <memory>
#include <string>
#include <unordered_set>
#include <vector>

namespace rerevved::gpu
{

enum class NativeGameplayPreparationStatus
{
    Pending,
    Success,
    Failed,
    Stopped,
};

struct NativeGameplayPreparationReceipt
{
    bool          known = false;
    std::size_t   bytes = 0, callerBankBytes = 0;
    std::uint32_t basis = 0;

    void RetainCallerStorage(std::size_t capacityBytes) noexcept
    {
        constexpr auto unknown = std::numeric_limits<std::size_t>::max();
        callerBankBytes        = capacityBytes > unknown - callerBankBytes ? unknown : callerBankBytes + capacityBytes;
        if (known)
        {
            bytes = capacityBytes > unknown - bytes ? unknown : bytes + capacityBytes;
            if (bytes == unknown || callerBankBytes == unknown)
            {
                known = false;
                bytes = 0;
            }
        }
    }
};

// Successful Recording releases recipes; caller banks remain charged until
// their actual allocations are destroyed or transferred by the coordinator.
struct NativeGameplayPreparationContext
{
    NativeGameplayPreparationReceipt receipt;
    NativeGameplayPreparationStatus  state                                             = NativeGameplayPreparationStatus::Success;
    std::size_t                      callerBankBytes                                   = 0;
    void*                            coordinator                                       = nullptr;
    NativeGameplayPreparationStatus (*settleRecording)(void*, bool wait, std::string&) = nullptr;

    bool Settle(bool wait, std::string& error)
    {
        if (state == NativeGameplayPreparationStatus::Pending)
        {
            state = settleRecording ? settleRecording(coordinator, wait, error) : NativeGameplayPreparationStatus::Failed;
            if (state == NativeGameplayPreparationStatus::Success)
            {
                if (receipt.callerBankBytes > std::numeric_limits<std::size_t>::max() - callerBankBytes)
                    callerBankBytes = std::numeric_limits<std::size_t>::max();
                else
                    callerBankBytes += receipt.callerBankBytes;
                receipt = {};
            }
        }
        if (state == NativeGameplayPreparationStatus::Failed || state == NativeGameplayPreparationStatus::Stopped)
        {
            if (error.empty())
                error = state == NativeGameplayPreparationStatus::Stopped ? "native preparation stopped" : "native predecessor recording failed";
            return false;
        }
        return !wait || state == NativeGameplayPreparationStatus::Success;
    }
};

namespace preparation
{

inline constexpr std::size_t   Unknown = std::numeric_limits<std::size_t>::max();
inline constexpr std::size_t   Budget  = 512U * 1024U * 1024U;
inline constexpr std::uint32_t Basis   = 202503;
#if defined(_WIN64) && defined(_M_X64) && defined(_MSC_VER) && defined(__clang__) && __clang_major__ == 22 && __clang_minor__ == 1 && __clang_patchlevel__ == 4 && defined(_MSVC_STL_VERSION) && _MSVC_STL_VERSION == 143 && _MSVC_STL_UPDATE == 202503L && _ITERATOR_DEBUG_LEVEL == 0 && defined(NDEBUG) && !defined(_DEBUG) && !defined(__SANITIZE_ADDRESS__)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer) || __has_feature(memory_sanitizer) || __has_feature(undefined_behavior_sanitizer)
inline constexpr bool SupportedAbi = false;
#else
inline constexpr bool SupportedAbi = sizeof(void*) == 8;
#endif
#else
inline constexpr bool SupportedAbi = false;
#endif

inline std::size_t Add(std::size_t a, std::size_t b) noexcept
{
    return b > Unknown - a ? Unknown : a + b;
}

inline std::size_t Multiply(std::size_t a, std::size_t b) noexcept
{
    return b && a > Unknown / b ? Unknown : a * b;
}

template <class T>
std::size_t Allocation(std::size_t count) noexcept
{
    if (!count)
        return 0;
    if constexpr (alignof(T) > __STDCPP_DEFAULT_NEW_ALIGNMENT__)
        return Unknown;
    if (count > std::numeric_limits<std::ptrdiff_t>::max() / sizeof(T))
        return Unknown;
    const auto bytes = Multiply(count, sizeof(T));
    // MSVC default allocator's manual alignment requests 32 - 1 + 8 bytes.
    return Add(bytes, bytes >= 4096 ? 32 - 1 + sizeof(void*) : 0);
}

template <class T>
std::size_t Vector(const std::vector<T>& value) noexcept
{
    return Allocation<T>(value.capacity());
}

inline std::size_t Grow(std::size_t capacity, std::size_t size) noexcept
{
    return size <= capacity ? capacity : std::max(size, Add(capacity, capacity / 2));
}

template <class T>
std::size_t Pushes(std::size_t capacity, std::size_t size, std::size_t count) noexcept
{
    std::size_t bytes = 0;
    for (std::size_t i = 0; i < count; ++i)
    {
        size = Add(size, 1);
        if (size > capacity)
        {
            capacity = Grow(capacity, size);
            bytes    = Add(bytes, Allocation<T>(capacity));
        }
    }
    return bytes;
}

template <class T>
std::size_t Pushes(const std::vector<T>& value, std::size_t count) noexcept
{
    return Pushes<T>(value.capacity(), value.size(), count);
}

template <class C>
std::size_t String(const std::basic_string<C>& value) noexcept
{
    return value.capacity() < 16 / sizeof(C) ? 0 : Allocation<C>(Add(value.capacity(), 1));
}

template <class C>
std::size_t StringGrowth(std::size_t capacity, std::size_t length) noexcept
{
    if (length <= capacity)
        return 0;
    const auto grown = std::max(length | (16 / sizeof(C) - 1), Add(capacity, capacity / 2));
    return Allocation<C>(Add(grown, 1));
}

template <class T>
std::size_t Separate() noexcept
{
#if defined(_MSVC_STL_VERSION) && _MSVC_STL_VERSION == 143 && _MSVC_STL_UPDATE == 202503L
    return Add(sizeof(T), sizeof(std::_Ref_count<T>));
#else
    return Unknown;
#endif
}

inline std::size_t SharedBytes(std::size_t capacity) noexcept
{
#if defined(_MSVC_STL_VERSION) && _MSVC_STL_VERSION == 143 && _MSVC_STL_UPDATE == 202503L
    return Add(sizeof(std::_Ref_count_obj2<const std::vector<std::uint8_t>>), Allocation<std::uint8_t>(capacity));
#else
    return Unknown;
#endif
}

template <class Pair>
std::size_t Tree(std::size_t size) noexcept
{
#if defined(_MSVC_STL_VERSION) && _MSVC_STL_VERSION == 143 && _MSVC_STL_UPDATE == 202503L
    return Multiply(Add(size, 1), Allocation<std::_Tree_node<Pair, void*>>(1));
#else
    return Unknown;
#endif
}

template <class Map>
std::size_t Tree(const Map& value) noexcept
{
    return Tree<typename Map::value_type>(value.size());
}

template <class T>
std::size_t Hash(std::size_t size, std::size_t buckets) noexcept
{
#if defined(_MSVC_STL_VERSION) && _MSVC_STL_VERSION == 143 && _MSVC_STL_UPDATE == 202503L
    return Add(Multiply(Add(size, 1), Allocation<std::_List_node<T, void*>>(1)), Allocation<void*>(Multiply(2, buckets)));
#else
    return Unknown;
#endif
}

template <class Set>
std::size_t Hash(const Set& value) noexcept
{
    return Hash<typename Set::value_type>(value.size(), value.bucket_count());
}

template <class T>
std::size_t HashInsertions(std::size_t size, std::size_t buckets, std::size_t count) noexcept
{
#if defined(_MSVC_STL_VERSION) && _MSVC_STL_VERSION == 143 && _MSVC_STL_UPDATE == 202503L
    std::size_t bytes = Multiply(count, Allocation<std::_List_node<T, void*>>(1));
    for (std::size_t i = 0; i < count; ++i)
    {
        size = Add(size, 1);
        if (size > buckets)
        {
            buckets = Multiply(buckets, buckets < 512 ? 8 : 2);
            if (buckets == Unknown)
                return Unknown;
            bytes = Add(bytes, Allocation<void*>(Multiply(2, buckets)));
        }
    }
    return bytes;
#else
    return Unknown;
#endif
}

template <class Set>
std::size_t HashInsertions(const Set& value, std::size_t count) noexcept
{
    return HashInsertions<typename Set::value_type>(value.size(), value.bucket_count(), count);
}

struct Forecast
{
    bool        known   = false;
    std::size_t logical = 0, allocations = 0, cacheSupplements = 0;

    void Allocate(std::size_t bytes) noexcept
    {
        allocations = Add(allocations, bytes);
    }

    void Charge(std::size_t bytes) noexcept
    {
        logical = Add(logical, bytes);
    }
};

} // namespace preparation
} // namespace rerevved::gpu
