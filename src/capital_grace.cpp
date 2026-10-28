#include "capital_grace.h"

#include <bit>
#include <limits>

namespace rerevved::capital_grace
{

bool Active(bool enabled, int32_t year) noexcept
{
    return enabled && year < -3000;
}

bool Player(int32_t player) noexcept
{
    return player >= 0 && player <= 7;
}

std::optional<uint32_t> UnitAddress(int32_t player, int32_t slot) noexcept
{
    if (!Player(player) || slot < 0 || slot > 255)
    {
        return std::nullopt;
    }
    return kUnits + 84 * (256 * player + slot);
}

std::optional<uint32_t> CityAddress(int32_t city) noexcept
{
    if (city < 0 || city > 127)
    {
        return std::nullopt;
    }
    return kCities + 188 * city;
}

std::optional<int32_t> ActorSlot(const Words& words) noexcept
{
    if (words[0] == 1)
    {
        if (words[2] > 65535)
        {
            return std::nullopt;
        }
        const auto composite = static_cast<int32_t>(words[2]);
        return composite - 256 * (composite / 256);
    }
    if ((words[0] == 0 || words[0] == 17) && words[2] <= 255)
    {
        return static_cast<int32_t>(words[2]);
    }
    return std::nullopt;
}

std::optional<int32_t> Direction(const Words& words, int16_t currentDirection) noexcept
{
    int32_t direction = -1;
    if (words[0] == 0)
    {
        direction = std::bit_cast<int16_t>(static_cast<uint16_t>(words[3]));
    }
    else if (words[0] == 1)
    {
        direction = currentDirection;
    }
    else if (words[0] == 17 && words[3] <= 7)
    {
        direction = static_cast<int32_t>(words[3]);
    }
    if (direction < 0 || direction > 7)
    {
        return std::nullopt;
    }
    return direction;
}

std::optional<Point> Adjacent(Point source, int32_t width, int32_t height, int32_t dx, int32_t dy) noexcept
{
    if (width < 1 || width > 40 || height < 1 || height > 40 || source.x < 0 || source.x >= width || source.y < 0 || source.y >= height)
    {
        return std::nullopt;
    }
    const int64_t x = (int64_t{ source.x } + dx + width) % width;
    const int64_t y = int64_t{ source.y } + dy;
    if (x < 0 || x >= width || y < 0 || y >= height || 40 * x + y >= 1600)
    {
        return std::nullopt;
    }
    return Point{ static_cast<int32_t>(x), static_cast<int32_t>(y) };
}

bool LiveActor(const Actor& actor, int32_t player, int32_t barbarian) noexcept
{
    return Player(player) && player == barbarian && actor.owner == player && actor.type >= 0 && actor.type <= 37 && (actor.flags & 0x80000000u) == 0;
}

bool ProtectedCity(const City& city, int32_t barbarian, std::optional<Point> target) noexcept
{
    return Player(barbarian) && Player(city.owner) && city.owner != barbarian && city.population > 0 && (city.buildings & 1) && (!target || city.position == *target);
}

bool SpecialArrival(uint32_t command, int32_t type, uint16_t tileFlags, int32_t tileOwner, int32_t player) noexcept
{
    // sub_82CD69B8 0x82CD6B3C..0x82CD6BA4 takes this arm before ordinary movement.
    return command == 17 && (type == 28 || type == 29) && (tileFlags & 1) && Player(tileOwner) && tileOwner != player;
}

Retirement PlanRetirement(uint32_t flags, int32_t player, int32_t slot, int32_t currentPlayer, int32_t selected, int32_t cached) noexcept
{
    if ((flags & 1) || !UnitAddress(player, slot))
    {
        return {};
    }
    return { true, currentPlayer == player && selected == slot, cached == slot };
}

void Tokens::Reset() noexcept
{
    producerCount_ = submitCount_ = parentCount_ = 0;
}

bool Tokens::PushProducer(uint32_t frame, Callsite site, const Words& words) noexcept
{
    if (frame < 176 || frame > std::numeric_limits<uint32_t>::max() - 0x390)
    {
        ++imbalances_;
        return false;
    }
    // Re-entering a live frame is imbalance, not evidence for a prior escape.
    for (size_t i = 0; i < producerCount_; ++i)
    {
        if (producers_[i].frame == frame)
        {
            producerCount_ = i;
            ++imbalances_;
            break;
        }
    }
    if (producerCount_ == kCapacity)
    {
        // The excess invocation is untracked. Older refusals retain their escapes.
        ++capacityFailures_;
        ++imbalances_;
        return false;
    }
    Producer producer{};
    producer.frame   = frame;
    producer.site    = site;
    producer.words   = words;
    producer.wrapper = frame - 256 + 80;
    if (site == Callsite::Linked)
    {
        for (size_t i = parentCount_; i > 0; --i)
        {
            if (parents_[i - 1].frame == frame && !parents_[i - 1].armed)
            {
                producer.parentPlayer = parents_[i - 1].player;
                producer.parentSlot   = parents_[i - 1].slot;
                break;
            }
        }
    }
    producers_[producerCount_++] = producer;
    return true;
}

Submission Tokens::PopProducer(uint32_t frame, Callsite site) noexcept
{
    for (size_t i = producerCount_; i > 0; --i)
    {
        const auto& producer = producers_[i - 1];
        if (producer.frame == frame && producer.site == site)
        {
            imbalances_ += producerCount_ != i;
            producerCount_ = i - 1;
            return { producer.stage == Stage::Refused, producer.parentPlayer, producer.parentSlot, ActorSlot(producer.words).value_or(-1) };
        }
    }
    if (producerCount_)
    {
        ++imbalances_;
    }
    return {};
}

void Tokens::BeginSubmit(uint32_t frame, uint32_t queue) noexcept
{
    for (size_t i = 0; i < submitCount_; ++i)
    {
        if (submits_[i].frame == frame)
        {
            for (size_t j = i; j < submitCount_; ++j)
            {
                FinishDrain(submits_[j].frame);
            }
            submitCount_ = i;
            ++imbalances_;
            break;
        }
    }
    if (submitCount_ == kCapacity)
    {
        ++capacityFailures_;
        ++imbalances_;
        return;
    }
    submits_[submitCount_++] = { frame, queue };
}

Tokens::Submit* Tokens::FindSubmit(uint32_t frame) noexcept
{
    for (size_t i = submitCount_; i > 0; --i)
    {
        if (submits_[i - 1].frame == frame)
        {
            if (submitCount_ != i)
            {
                for (size_t j = i; j < submitCount_; ++j)
                {
                    FinishDrain(submits_[j].frame);
                }
                submitCount_ = i;
                ++imbalances_;
            }
            return &submits_[i - 1];
        }
    }
    return nullptr;
}

void Tokens::FinishDrain(uint32_t frame) noexcept
{
    for (size_t i = 0; i < producerCount_; ++i)
    {
        auto& producer = producers_[i];
        if (producer.stage == Stage::Draining && producer.drainRecord == frame + 80)
        {
            producer.stage = Stage::Done;
        }
    }
}

void Tokens::EndSubmit(uint32_t frame) noexcept
{
    if (FindSubmit(frame))
    {
        FinishDrain(frame);
        --submitCount_;
    }
    else if (submitCount_)
    {
        ++imbalances_;
    }
}

bool Tokens::Append(uint32_t frame, uint32_t record, const Words& words, uint32_t write, uint32_t read) noexcept
{
    const auto* submit = FindSubmit(frame);
    // Appending adds one entry. Native unsigned cursors must not wrap here.
    if (!submit || write < read || write - read >= 32 || write == UINT32_MAX)
    {
        return false;
    }
    for (size_t i = producerCount_; i > 0; --i)
    {
        auto& producer = producers_[i - 1];
        if (producer.stage == Stage::Open && producer.wrapper == record && producer.words == words)
        {
            producer.queue  = submit->queue;
            producer.serial = write;
            producer.stage  = Stage::Appending;
            return true;
        }
    }
    return false;
}

void Tokens::CompleteAppend(uint32_t frame) noexcept
{
    const auto* submit = FindSubmit(frame);
    if (!submit)
    {
        return;
    }
    // The append is call-free. The wrapper is the queue caller's frame+0x50.
    for (size_t i = producerCount_; i > 0; --i)
    {
        auto& producer = producers_[i - 1];
        if (producer.stage == Stage::Appending && producer.queue == submit->queue && producer.wrapper == frame + 144 + 80)
        {
            producer.stage = Stage::Pending;
            return;
        }
    }
}

void Tokens::DrainSerial(uint32_t frame, uint32_t read, uint32_t write) noexcept
{
    if (auto* submit = FindSubmit(frame))
    {
        FinishDrain(frame);
        submit->read      = read;
        submit->validRead = write > read && write - read <= 32;
    }
}

bool Tokens::DrainRecord(uint32_t frame, uint32_t record, const Words& words) noexcept
{
    auto* submit = FindSubmit(frame);
    if (!submit || !submit->validRead || record != frame + 80)
    {
        return false;
    }
    submit->validRead = false;
    for (size_t i = producerCount_; i > 0; --i)
    {
        auto& producer = producers_[i - 1];
        if (producer.stage == Stage::Pending && producer.queue == submit->queue && producer.serial == submit->read && producer.words == words)
        {
            producer.stage       = Stage::Draining;
            producer.drainRecord = record;
            return true;
        }
    }
    return false;
}

bool Tokens::Refuse(uint32_t record, const Words& words) noexcept
{
    for (size_t i = producerCount_; i > 0; --i)
    {
        auto& producer = producers_[i - 1];
        if (producer.stage == Stage::Draining && producer.drainRecord == record && producer.words == words)
        {
            producer.stage = Stage::Refused;
            return true;
        }
    }
    return false;
}

void Tokens::PrepareParent(uint32_t frame, int32_t player, int32_t slot) noexcept
{
    for (size_t i = 0; i < parentCount_; ++i)
    {
        if (parents_[i].frame == frame)
        {
            parentCount_ = i;
            ++imbalances_;
            break;
        }
    }
    if (parentCount_ == kCapacity || !UnitAddress(player, slot))
    {
        capacityFailures_ += parentCount_ == kCapacity;
        ++imbalances_;
        return;
    }
    parents_[parentCount_++] = { frame, player, slot, false };
}

bool Tokens::ArmCleanup(uint32_t frame, const Submission& submission) noexcept
{
    if (!submission.refused || submission.childSlot == submission.parentSlot)
    {
        return false;
    }
    for (size_t i = parentCount_; i > 0; --i)
    {
        auto& parent = parents_[i - 1];
        if (parent.frame == frame && parent.player == submission.parentPlayer && parent.slot == submission.parentSlot && !parent.armed)
        {
            imbalances_ += parentCount_ != i;
            parentCount_ = i;
            parent.armed = true;
            return true;
        }
    }
    return false;
}

bool Tokens::FinishCleanup(uint32_t frame) noexcept
{
    for (size_t i = parentCount_; i > 0; --i)
    {
        if (parents_[i - 1].frame == frame)
        {
            imbalances_ += parentCount_ != i;
            parentCount_ = i - 1;
            return parents_[i - 1].armed;
        }
    }
    if (parentCount_)
    {
        ++imbalances_;
    }
    return false;
}

uint64_t Tokens::Imbalances() const noexcept
{
    return imbalances_;
}

uint64_t Tokens::CapacityFailures() const noexcept
{
    return capacityFailures_;
}

} // namespace rerevved::capital_grace
