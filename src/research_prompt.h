#pragma once

// Gating for the hide_research_prompt hooks in research_prompt_hooks.cpp,
// kept free of guest memory so it can be tested on the host.

#include <cstdint>

namespace rerevved::research_prompt
{

// Stack frames of sub_82CECCD8 (stwu r1,-336(r1)) and the advisor show
// sub_82CE8338 (stwu r1,-1280(r1)); a callee's r1 plus its size is the
// caller's r1.
constexpr uint32_t kPromptFrameSize = 336;
constexpr uint32_t kShowFrameSize   = 1280;

// Prompt hooks arm per-thread caller r1 only for automatic turn, village, and
// technology-reward calls.
class AutomaticPromptGate
{
public:
    void Arm(uint32_t callerFrame)
    {
        callerFrame_ = callerFrame;
        promptFrame_ = 0;
    }

    void Disarm()
    {
        callerFrame_ = 0;
        promptFrame_ = 0;
    }

    // At the prompt's show call: remember the prompt frame when it is hidden.
    void BeginShow(bool enabled, uint32_t promptFrame)
    {
        if (enabled && callerFrame_ != 0 && promptFrame + kPromptFrameSize == callerFrame_)
        {
            promptFrame_ = promptFrame;
        }
    }

    // Inside the show: true once, for the show called directly by that prompt.
    bool SuppressShow(uint32_t showFrame)
    {
        if (promptFrame_ == 0 || showFrame + kShowFrameSize != promptFrame_)
        {
            return false;
        }
        promptFrame_ = 0;
        return true;
    }

private:
    uint32_t callerFrame_ = 0;
    uint32_t promptFrame_ = 0;
};

} // namespace rerevved::research_prompt
