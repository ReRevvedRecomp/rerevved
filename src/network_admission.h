#pragma once

#include <cstdint>
#include <optional>

namespace rerevved::network_admission
{

constexpr uint32_t kAppGlobal      = 0x82FFD624;
constexpr uint32_t kCurrentField   = 0x70;
constexpr uint32_t kPendingField   = 0x74;
constexpr uint32_t kRequestedField = 0x78;
constexpr uint32_t kStateKeyField  = 0x04;
constexpr uint32_t kFrontendKey    = 1;
constexpr uint32_t kGameKey        = 2;

constexpr uint32_t kSetupGlobal = 0x8314EFC8;
constexpr uint32_t kModeField   = 0x04;
constexpr uint32_t kNetworkByte = 0x8314F10B;

// kFrontendReturn cleans up the multiplayer manager and returns to the selector.
constexpr uint32_t kManagerGlobal         = 0x8314EFE0;
constexpr uint32_t kFrontendManagerGlobal = 0x8314F2E4;
constexpr uint32_t kControllerField       = 0x04;
constexpr uint32_t kRequestFrontend       = 0x82E54FD8;
constexpr uint32_t kFrontendReturn        = 0x82E6AFF8;
constexpr uint32_t kMaxVtableSlots        = 64;

class Memory
{
public:
    virtual ~Memory()                                      = default;
    virtual std::optional<uint32_t> Word(uint32_t address) = 0;
    virtual std::optional<uint8_t>  Byte(uint32_t address) = 0;
};

struct Transition
{
    uint32_t app          = 0;
    uint32_t root         = 0;
    uint32_t current      = 0;
    uint32_t currentKey   = 0;
    uint32_t pending      = 0;
    uint32_t requestedKey = 0;
    uint32_t updateResult = 0;
    uint8_t  network      = 0;
    uint32_t setup        = 0;
    int32_t  mode         = 0;

    friend bool operator==(const Transition&, const Transition&) = default;
};

std::optional<Transition> ReadTransition(Memory& memory, uint32_t app, uint32_t updateResult);

bool IsNetworkGameStart(const Transition& transition) noexcept;

// Retry admits the new network start again; the hook allows one retry.
enum class Recheck
{
    Apply,
    Retry,
    Refuse,
    NoAction,
};

Recheck DecideRecheck(const Transition& current, const std::optional<Transition>& after, bool retried) noexcept;

struct ReturnRoute
{
    uint32_t controller = 0;
    uint32_t slot       = 0;
};

std::optional<ReturnRoute> MatchFrontendReturn(Memory& memory);

// Key 2 leads to the returning-player checks; refusing there takes the policy's own refusal return.
constexpr uint32_t kJoinPolicyKeyCompare = 0x82D89740;
constexpr uint32_t kJoinRefusalReturn    = 0x82D897B8;
constexpr uint32_t kRefusedResult        = 0;

bool IsGameInProgress(uint32_t currentStateKey) noexcept;

} // namespace rerevved::network_admission
