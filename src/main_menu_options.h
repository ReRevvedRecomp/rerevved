#pragma once

// The main menu's Options row opens options_page.h in place of the retail Options screen.
// The row is OnAccept descriptor "4": callback 0x82E51560 calls parent R's virtual +0x30 (0x82D6BFD8),
// which requests frontend transition 36 and plays cue 1001. Skipping the transition preserves the menu's focus and legend;
// pad capture blocks menu input while the frontend state machine runs behind the page.

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

namespace rerevved::main_menu_options
{

constexpr uint32_t kMenuVtable  = 0x82139CBC;
constexpr uint32_t kParentField = 4;

// R+0x10 is the parent's own transition word; the handler only admits while it is zero. Every exit path goes through 0x82D704F0(R, state),
// which sets byte R+0xC and the manager's pending word (rerevved_recomp.29.cpp:29598), leaving R+0x10 zero.
constexpr uint32_t kParentVtable  = 0x8213B90C;
constexpr uint32_t kParentSlot    = 0x30;
constexpr uint32_t kParentHandler = 0x82D6BFD8;
constexpr uint32_t kPendingField  = 0x10;
constexpr uint32_t kExitField     = 0x0C;

// Frontend manager F = U32[0x8314F2E4]; update 0x82C568C8 calls the current state's virtual +8, so F+4 == R while R is current.
// F+0x10 is the pending transition word; F+0x2C the guest prompt events 25 and 28 install (0x82D70880, rerevved_recomp.217.cpp:29292).
constexpr uint32_t kManagerGlobal       = 0x8314F2E4;
constexpr uint32_t kManagerUpdate       = 0x82C568C8;
constexpr uint32_t kManagerStateField   = 0x04;
constexpr uint32_t kManagerPendingField = 0x10;
constexpr uint32_t kManagerPromptField  = 0x2C;

constexpr uint32_t kCueFunction = 0x82E4E680;
constexpr uint32_t kCue         = 1001;

// readByte(i) gives the descriptor byte or nothing; retail stops at NUL, so read the second byte only after a 4.
template <typename ReadByte>
constexpr bool IsOptionsDescriptor(ReadByte&& readByte)
{
    const auto first = readByte(0);
    if (!first || *first != '4')
        return false;
    const auto second = readByte(1);
    return second && *second == 0;
}

struct Entry
{
    bool     hostBound           = false;
    uint32_t menuVtable          = 0;
    uint32_t parent              = 0;
    uint32_t parentVtable        = 0;
    uint32_t parentHandler       = 0; // U32[U32[R] + 0x30]
    uint32_t pending             = 0; // U32[R + 0x10]
    uint32_t exiting             = 0; // byte R + 0xC
    uint32_t currentState        = 0; // U32[F + 4]
    uint32_t prompt              = 0; // U32[F + 0x2C]
    bool     descriptorIsOptions = false;
    bool     pageOpen            = false;
};

enum class Route
{
    kPage,
    kIgnore,
    kRetail,
};

// The page route needs a validated descriptor, current parent R, no transition or prompt, and host bindings; otherwise retail.
Route ChooseRoute(const Entry& entry);

// Opened and checked on the guest thread; no guest state survives a call.
class Session
{
public:
    // A change from the opening managerPending value signals a transition.
    void Begin(uint64_t page, uint32_t parent, uint32_t managerPending);
    void End(uint64_t page);

    struct State
    {
        uint64_t page           = 0;
        uint32_t parent         = 0;
        uint32_t managerPending = 0;
    };

    State Current() const;

    // Lock-free per-frame session check.
    bool Active() const
    {
        return active_.load(std::memory_order_acquire) != 0;
    }

private:
    mutable std::mutex    mutex_;
    State                 state_;
    std::atomic<uint64_t> active_{ 0 };
};

// Read parent fields only while R is current; otherwise leave them zero.
struct Frame
{
    uint32_t currentState     = 0;
    uint32_t parentTransition = 0;
    uint32_t parentExiting    = 0;
    uint32_t managerPending   = 0;
    uint32_t prompt           = 0;
};

// The page's menu is leaving or covered: R is no longer current, R requested a transition, or a guest prompt took over.
constexpr bool ShouldDismiss(const Session::State& session, const Frame& frame)
{
    if (session.page == 0 || session.parent == 0)
        return false;
    return frame.currentState != session.parent || frame.prompt != 0 || frame.parentTransition != 0 ||
           frame.parentExiting != 0 || frame.managerPending != session.managerPending;
}

} // namespace rerevved::main_menu_options
