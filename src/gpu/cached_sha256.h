#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/crypto/sha256.h>
#include <rex/hash.h>

namespace rerevved::gpu
{

// Cache SHA-256 by size plus 128-bit XXH3; repeated shader content avoids a
// second digest pass.
class CachedSha256
{
public:
    // SHA-256 of `bytes`.
    static std::string Of(std::span<const std::uint8_t> bytes)
    {
        return Lookup(bytes, false);
    }

    // SHA-256 of `bytes` with each 4-byte group reversed (big-endian guest
    // words as little-endian words); the size must be a multiple of 4.
    static std::string OfSwappedWords(std::span<const std::uint8_t> bytes)
    {
        return Lookup(bytes, true);
    }

private:
    struct Key
    {
        std::uint64_t size = 0, low = 0, high = 0;
        bool          swapped                      = false;
        bool          operator==(const Key&) const = default;
    };

    struct KeyHash
    {
        std::size_t operator()(const Key& key) const noexcept
        {
            return std::size_t(key.low ^ (key.high * 0x9E3779B97F4A7C15ULL) ^ (key.size << 1) ^ std::uint64_t(key.swapped));
        }
    };

    // Distinct shaders are few; a runaway producer clears the cache instead of
    // growing it.
    static constexpr std::size_t kMaxEntries = 8192;

    static std::string Compute(std::span<const std::uint8_t> bytes, bool swapped)
    {
        if (!swapped)
            return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
        std::vector<std::uint8_t> reversed(bytes.begin(), bytes.end());
        for (std::size_t i = 0; i + 4 <= reversed.size(); i += 4)
        {
            std::swap(reversed[i], reversed[i + 3]);
            std::swap(reversed[i + 1], reversed[i + 2]);
        }
        return rex::crypto::sha256(std::string_view(reinterpret_cast<const char*>(reversed.data()), reversed.size()));
    }

    static std::string Lookup(std::span<const std::uint8_t> bytes, bool swapped)
    {
        const auto                                           hash = XXH3_128bits(bytes.data(), bytes.size());
        const Key                                            key{ bytes.size(), hash.low64, hash.high64, swapped };
        static std::mutex                                    mutex;
        static std::unordered_map<Key, std::string, KeyHash> digests;
        {
            std::lock_guard lock(mutex);
            if (const auto found = digests.find(key); found != digests.end())
                return found->second;
        }
        auto digest = Compute(bytes, swapped);
        {
            std::lock_guard lock(mutex);
            if (digests.size() >= kMaxEntries)
                digests.clear();
            digests.emplace(key, digest);
        }
        return digest;
    }
};

} // namespace rerevved::gpu
