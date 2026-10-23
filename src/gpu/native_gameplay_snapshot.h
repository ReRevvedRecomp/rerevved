#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include <rex/graphics/draw_capture_mailbox.h>

namespace rerevved::gpu
{

inline constexpr std::size_t NativeGameplayBatchEvents   = 4096;
inline constexpr std::size_t NativeGameplayRegisterWords = 0x5003;

// Checkpoints bound random access to one batch. Sequential consumers own only
// their current reconstructed event; stored records never borrow guest memory.
class NativeGameplayEvents
{
    using Event = rex::graphics::diagnostic::DrawCaptureFrameEvent;

    struct Record
    {
        Event                      metadata;
        std::vector<std::uint32_t> registers;
        bool                       delta = false;
    };

    std::vector<std::vector<Record>> batches;
    std::vector<std::uint32_t>       previous;
    std::size_t                      count = 0;
    std::size_t                      bytes = 0;

    const Record& record(std::size_t index) const
    {
        return batches.at(index / NativeGameplayBatchEvents).at(index % NativeGameplayBatchEvents);
    }

    void read(std::size_t index, Event& event) const
    {
        const auto& source    = record(index);
        auto        registers = std::move(event.draw.draw.registers);
        event                 = source.metadata;
        if (source.delta)
        {
            if (registers.size() != NativeGameplayRegisterWords)
            {
                throw std::runtime_error("native input delta has no preceding register state");
            }
            for (std::size_t i = 0; i < source.registers.size(); i += 2)
            {
                registers[source.registers[i]] = source.registers[i + 1];
            }
        }
        else
        {
            registers = source.registers;
        }
        event.draw.draw.registers = std::move(registers);
    }

public:
    class Iterator
    {
        const NativeGameplayEvents* owner = nullptr;
        std::size_t                 index = 0;
        Event                       current;

    public:
        using iterator_category = std::input_iterator_tag;
        using value_type        = Event;
        using difference_type   = std::ptrdiff_t;
        using pointer           = const Event*;
        using reference         = const Event&;
        Iterator()              = default;

        Iterator(const NativeGameplayEvents* owner, std::size_t index)
        : owner(owner)
        , index(index)
        {
            if (index < owner->size())
            {
                owner->read(index, current);
            }
        }

        const Event& operator*() const
        {
            return current;
        }

        const Event* operator->() const
        {
            return &current;
        }

        Iterator& operator++()
        {
            if (++index < owner->size())
            {
                owner->read(index, current);
            }
            return *this;
        }

        void operator++(int)
        {
            ++*this;
        }

        bool operator==(const Iterator& other) const
        {
            return owner == other.owner && index == other.index;
        }
    };

    std::size_t size() const
    {
        return count;
    }

    bool empty() const
    {
        return count == 0;
    }

    std::size_t BatchCount() const
    {
        return batches.size();
    }

    std::size_t StoredBytes() const
    {
        return bytes + previous.size() * sizeof(std::uint32_t);
    }

    std::uint64_t LastId() const
    {
        return empty() ? 0 : record(count - 1).metadata.id;
    }

    Iterator begin() const
    {
        return { this, 0 };
    }

    Iterator end() const
    {
        return { this, count };
    }

    Event at(std::size_t index) const
    {
        if (index >= count)
        {
            throw std::out_of_range("native input event index");
        }
        Event event;
        // Successful appends retain the final owned bank. Boundary reads need
        // not reconstruct every preceding event in its checkpoint batch.
        if (index == count - 1)
        {
            event                     = record(index).metadata;
            event.draw.draw.registers = previous;
            return event;
        }
        for (std::size_t i = index - index % NativeGameplayBatchEvents; i <= index; ++i)
        {
            read(i, event);
        }
        return event;
    }

    Event operator[](std::size_t index) const
    {
        return at(index);
    }

    Event front() const
    {
        return at(0);
    }

    Event back() const
    {
        return at(count - 1);
    }

    // Consume the final boundary when a completed frame is retired. Moving the
    // owned working bank avoids borrowing callback state or adding another bank.
    Event ReleaseFinalEvent()
    {
        if (empty())
            throw std::runtime_error("native input events are empty");
        auto event                = record(count - 1).metadata;
        event.draw.draw.registers = std::move(previous);
        batches.clear();
        count = bytes = 0;
        return event;
    }

private:
    void append(Event event, std::span<const std::uint32_t> values, std::size_t maximumBytes, std::vector<std::uint32_t>* ownedValues)
    {
        if (values.size() != NativeGameplayRegisterWords)
        {
            throw std::runtime_error("native input registers are incomplete");
        }
        if (event.id <= LastId())
        {
            throw std::runtime_error("native input event identities are not ordered");
        }
        Record stored;
        stored.metadata = std::move(event);
        if (count % NativeGameplayBatchEvents != 0)
        {
            std::size_t index = 0;
            while (index < values.size())
            {
                const auto changed = std::mismatch(values.begin() + index, values.end(), previous.begin() + index);
                if (changed.first == values.end())
                {
                    break;
                }
                index = static_cast<std::size_t>(changed.first - values.begin());
                stored.registers.push_back(static_cast<std::uint32_t>(index));
                stored.registers.push_back(values[index]);
                ++index;
                if (stored.registers.size() >= values.size())
                {
                    break;
                }
            }
            stored.delta = stored.registers.capacity() < values.size();
        }
        if (!stored.delta)
        {
            std::vector<std::uint32_t>(values.begin(), values.end()).swap(stored.registers);
        }
        const auto added        = sizeof(Record) + stored.registers.capacity() * sizeof(std::uint32_t) +
                                  stored.metadata.draw.draw.vertex_fetches.capacity() * sizeof(rex::graphics::diagnostic::DrawCaptureVertexFetch) +
                                  stored.metadata.draw.texture_uses.capacity() * sizeof(rex::graphics::diagnostic::DrawCaptureTextureUse) +
                                  stored.metadata.copy_mode.size();
        const auto workingBytes = values.size() * sizeof(std::uint32_t);
        if (workingBytes > maximumBytes || bytes > maximumBytes - workingBytes ||
            added > maximumBytes - workingBytes - bytes)
        {
            throw std::runtime_error("native input records exceed the owned byte bound");
        }
        // Prepare the first owned bank before publishing. Later sparse appends
        // update it from the owned delta, without copying the whole callback bank.
        std::vector<std::uint32_t> initial;
        if (!ownedValues && previous.empty())
        {
            initial.assign(values.begin(), values.end());
        }
        if (count % NativeGameplayBatchEvents == 0)
        {
            std::vector<Record> batch;
            batch.push_back(std::move(stored));
            batches.push_back(std::move(batch));
        }
        else
        {
            batches.back().push_back(std::move(stored));
        }
        if (ownedValues)
        {
            previous = std::move(*ownedValues);
        }
        else if (!initial.empty())
        {
            previous = std::move(initial);
        }
        else if (const auto& committed = record(count); committed.delta)
        {
            for (std::size_t i = 0; i < committed.registers.size(); i += 2)
            {
                previous[committed.registers[i]] = committed.registers[i + 1];
            }
        }
        else
        {
            std::copy(values.begin(), values.end(), previous.begin());
        }
        bytes += added;
        ++count;
    }

public:
    void push_back(Event event, std::size_t maximumBytes = 512U * 1024U * 1024U)
    {
        auto values = std::move(event.draw.draw.registers);
        append(std::move(event), values, maximumBytes, &values);
    }

    void push_back(Event event, std::span<const std::uint32_t> registers, std::size_t maximumBytes = 512U * 1024U * 1024U)
    {
        if (!event.draw.draw.registers.empty())
        {
            throw std::runtime_error("native input registers have two sources");
        }
        append(std::move(event), registers, maximumBytes, nullptr);
    }
};

// Title-owned compact counterpart of the SDK diagnostic DTO. The SDK observer
// and existing saved schema remain independent of this live storage choice.
struct NativeGameplaySnapshot
{
    std::uint64_t                                              start_frame      = 0;
    std::uint64_t                                              start_submission = 0;
    std::uint64_t                                              end_frame        = 0;
    std::uint64_t                                              end_submission   = 0;
    NativeGameplayEvents                                       events;
    std::vector<rex::graphics::diagnostic::DrawCaptureTexture> textures;
    rex::graphics::diagnostic::DrawCaptureFinalSwap            final_swap;
    std::uint32_t                                              schema_version     = rex::graphics::diagnostic::kDrawCaptureFrameSchemaVersion;
    rex::graphics::diagnostic::DrawCaptureAvailability         acquisition        = rex::graphics::diagnostic::DrawCaptureAvailability::kComplete;
    rex::graphics::diagnostic::DrawCaptureReason               acquisition_reason = rex::graphics::diagnostic::DrawCaptureReason::kNone;
};

} // namespace rerevved::gpu
