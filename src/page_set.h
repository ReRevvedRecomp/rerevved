#pragma once

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace rerevved
{

class PageSet
{
public:
    size_t Add(std::string title)
    {
        titles_.push_back(std::move(title));
        return titles_.size() - 1;
    }

    size_t Count() const
    {
        return titles_.size();
    }

    size_t Current() const
    {
        return current_;
    }

    const std::string& Title(size_t index) const
    {
        return titles_[index];
    }

    bool Switchable() const
    {
        return titles_.size() > 1;
    }

    // Select a page; false for out-of-range or current index.
    bool Select(size_t index)
    {
        if (index >= titles_.size() || index == current_)
            return false;
        current_ = index;
        return true;
    }

    bool Step(int direction)
    {
        if (!Switchable() || direction == 0)
            return false;
        const size_t count = titles_.size();
        return Select(direction < 0 ? (current_ + count - 1) % count : (current_ + 1) % count);
    }

private:
    std::vector<std::string> titles_;
    size_t                   current_ = 0;
};

// Trigger range 0..255: press above kOn, release below kOff to avoid jitter.
class TriggerLatch
{
public:
    static constexpr int kOn  = 100;
    static constexpr int kOff = 40;

    // Feeds one sample; true on the sample that starts a press.
    bool Update(int value)
    {
        if (down_)
        {
            if (value < kOff)
                down_ = false;
            return false;
        }
        if (value > kOn)
        {
            down_ = true;
            return true;
        }
        return false;
    }

    bool Down() const
    {
        return down_;
    }

private:
    bool down_ = false;
};

// A tilt past kOn steps immediately, then every kInterval after kDelay, until below kOff.
// Steps are -1 up and +1 down; the pad axis is positive up.
class StickRepeat
{
public:
    static constexpr int   kOn       = 16000;
    static constexpr int   kOff      = 8000;
    static constexpr float kDelay    = 0.35f;
    static constexpr float kInterval = 0.08f;

    int Update(int axis, float seconds)
    {
        if (direction_ != 0)
        {
            const int held = direction_ < 0 ? axis : -axis;
            if (held >= kOff)
            {
                wait_ -= seconds;
                if (wait_ > 0.0f)
                    return 0;
                wait_ = kInterval;
                return direction_;
            }
            direction_ = 0;
        }
        if (axis > kOn)
            direction_ = -1;
        else if (axis < -kOn)
            direction_ = 1;
        else
            return 0;
        wait_ = kDelay;
        return direction_;
    }

    bool Held() const
    {
        return direction_ != 0;
    }

private:
    int   direction_ = 0;
    float wait_      = 0.0f;
};

} // namespace rerevved
