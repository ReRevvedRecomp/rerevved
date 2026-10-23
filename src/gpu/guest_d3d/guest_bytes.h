#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace rerevved::gpu::guest_d3d
{

// Bytes a guest memory read returned: an owned vector, or a shared immutable
// buffer that the read path retains for repeated reads of the same range.
// Mutation copies a shared buffer first, so holders never see a change.
class GuestBytes
{
public:
    GuestBytes() = default;

    GuestBytes(std::vector<std::uint8_t> bytes) noexcept
    : owned_(std::move(bytes))
    {
    }

    explicit GuestBytes(std::size_t size)
    : owned_(size)
    {
    }

    static GuestBytes Shared(std::shared_ptr<const std::vector<std::uint8_t>> bytes) noexcept
    {
        GuestBytes result;
        result.shared_ = std::move(bytes);
        return result;
    }

    const std::uint8_t* data() const noexcept
    {
        return bytes().data();
    }

    // Copies a shared buffer first.
    std::uint8_t* MutableData()
    {
        return own().data();
    }

    std::size_t size() const noexcept
    {
        return bytes().size();
    }

    bool empty() const noexcept
    {
        return bytes().empty();
    }

    auto begin() const noexcept
    {
        return bytes().begin();
    }

    auto end() const noexcept
    {
        return bytes().end();
    }

    std::uint8_t operator[](std::size_t i) const noexcept
    {
        return bytes()[i];
    }

    void clear() noexcept
    {
        shared_.reset();
        owned_.clear();
    }

    void resize(std::size_t size)
    {
        own().resize(size);
    }

    void assign(std::size_t size, std::uint8_t value)
    {
        own().assign(size, value);
    }

    // Whether this names a retained buffer, which copies share.
    bool IsShared() const noexcept
    {
        return bool(shared_);
    }

    // The retained buffer, or null for owned bytes.
    const std::shared_ptr<const std::vector<std::uint8_t>>& Buffer() const noexcept
    {
        return shared_;
    }

    // Whether both name one retained buffer, whose bytes are then equal.
    bool SharesBuffer(const GuestBytes& other) const noexcept
    {
        return shared_ && shared_ == other.shared_;
    }

    operator std::span<const std::uint8_t>() const noexcept
    {
        return bytes();
    }

    operator std::vector<std::uint8_t>() const&
    {
        return bytes();
    }

    operator std::vector<std::uint8_t>() &&
    {
        own();
        return std::move(owned_);
    }

    friend bool operator==(const GuestBytes& left, const GuestBytes& right) noexcept
    {
        return std::ranges::equal(left.bytes(), right.bytes());
    }

    friend bool operator==(const GuestBytes& left, const std::vector<std::uint8_t>& right) noexcept
    {
        return std::ranges::equal(left.bytes(), right);
    }

private:
    const std::vector<std::uint8_t>& bytes() const noexcept
    {
        return shared_ ? *shared_ : owned_;
    }

    std::vector<std::uint8_t>& own()
    {
        if (shared_)
        {
            owned_ = *shared_;
            shared_.reset();
        }
        return owned_;
    }

    std::vector<std::uint8_t>                        owned_;
    std::shared_ptr<const std::vector<std::uint8_t>> shared_;
};

} // namespace rerevved::gpu::guest_d3d
