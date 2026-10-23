#include "gameplay_state.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>

#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <gameplay_state.h>

namespace rerevved::gameplay
{

struct Snapshot
{
    uint64_t frameSequence     = 0;
    uint32_t frontendRoot      = 0;
    uint32_t frontendState     = 0;
    uint32_t frontendKey       = UINT32_MAX;
    uint32_t activePlayer      = UINT32_MAX;
    uint32_t humanPlayerMask   = 0;
    uint32_t interfaceGate     = 0;
    uint32_t civilization      = UINT32_MAX;
    uint32_t era               = UINT32_MAX;
    int32_t  year              = GAMEPLAY_YEAR_UNKNOWN;
    uint32_t turn              = UINT32_MAX;
    bool     frontendKnown     = false;
    bool     gameplayActive    = false;
    bool     turnOwnerKnown    = false;
    bool     humanTurn         = false;
    bool     interfaceKnown    = false;
    bool     interfaceUpdate   = false;
    bool     civilizationKnown = false;
    bool     eraKnown          = false;
    bool     yearKnown         = false;
    bool     turnNumberKnown   = false;
    bool     available         = false;
};

namespace
{

constexpr uint32_t kInterfaceGateGlobal     = 0x8314F28C;
constexpr uint32_t kFrontendRootGlobal      = 0x82FFD624;
constexpr uint32_t kPlayerEraArray          = 0x830ECD08;
constexpr uint32_t kPlayerCivilizationArray = 0x830ECD28;
constexpr uint32_t kCurrentTurnGlobal       = 0x8312B8DC;
constexpr uint32_t kCurrentYearGlobal       = 0x8312B8E0;
constexpr uint32_t kActivePlayerGlobal      = 0x8312B8E8;
constexpr uint32_t kHumanPlayerMaskGlobal   = 0x8312E608;
constexpr uint32_t kPlayerCount             = 6;

struct PublishedSlot
{
    std::atomic<int32_t> users{ 0 };
    Snapshot             snapshot;
};

// UI readers may make the inactive slot temporarily unavailable, but the guest
// frame writer never waits for them. Skipping one publication is harmless.
std::array<PublishedSlot, 2> gPublishedSlots;
std::atomic<uint32_t>        gActiveSlot{ 0 };
std::atomic<bool>            gSnapshotPublished{ false };
uint32_t                     gWriterSlot    = 0;
uint64_t                     gFrameSequence = 0;

bool isGuestPointer(uint32_t address)
{
    return address >= 0x10000 && address < 0xFFFFF000;
}

bool isGuestReadableRange(rex::memory::Memory* memory,
                          uint32_t             address,
                          uint32_t             extent)
{
    if (!memory || extent == 0 || address > UINT32_MAX - extent)
    {
        return false;
    }

    const uint32_t end = address + extent;
    if (!isGuestPointer(address) || !isGuestPointer(end - 1))
    {
        return false;
    }

    auto* heap = memory->LookupHeap(address);
    return heap && memory->LookupHeap(end - 1) == heap &&
           heap->QueryRangeAccess(address, end - 1) !=
               rex::memory::PageAccess::kNoAccess;
}

bool tryReadU8(rex::memory::Memory* memory, uint32_t address, uint8_t& value)
{
    if (!isGuestReadableRange(memory, address, sizeof(value)))
    {
        return false;
    }
    value = *memory->TranslateVirtual<const uint8_t*>(address);
    return true;
}

bool tryReadU32(rex::memory::Memory* memory, uint32_t address, uint32_t& value)
{
    if (!isGuestReadableRange(memory, address, sizeof(value)))
    {
        return false;
    }

    const auto* source = memory->TranslateVirtual<const uint8_t*>(address);
    value              = (uint32_t{ source[0] } << 24) |
                         (uint32_t{ source[1] } << 16) |
                         (uint32_t{ source[2] } << 8) | uint32_t{ source[3] };
    return true;
}

} // namespace

static Snapshot readGuestSnapshot()
{
    Snapshot state{};
    auto*    runtime = rex::Runtime::instance();
    auto*    memory  = runtime ? runtime->memory() : nullptr;
    if (!memory)
    {
        return state;
    }

    if (tryReadU32(memory, kFrontendRootGlobal, state.frontendRoot) &&
        state.frontendRoot != 0 &&
        tryReadU32(memory,
                   state.frontendRoot + 0x70,
                   state.frontendState) &&
        state.frontendState != 0 &&
        tryReadU32(memory,
                   state.frontendState + 0x4,
                   state.frontendKey))
    {
        state.frontendKnown  = true;
        state.gameplayActive = state.frontendKey == 2;
    }

    if (tryReadU32(memory, kActivePlayerGlobal, state.activePlayer) &&
        tryReadU32(memory,
                   kHumanPlayerMaskGlobal,
                   state.humanPlayerMask) &&
        state.activePlayer < 32 && state.humanPlayerMask != 0)
    {
        state.turnOwnerKnown = true;
        state.humanTurn =
            (state.humanPlayerMask &
             (uint32_t{ 1 } << state.activePlayer)) != 0;
    }

    if (state.gameplayActive && state.humanTurn &&
        state.activePlayer < kPlayerCount)
    {
        state.civilizationKnown =
            tryReadU32(memory,
                       kPlayerCivilizationArray +
                           state.activePlayer * sizeof(uint32_t),
                       state.civilization);
        state.eraKnown =
            tryReadU32(memory,
                       kPlayerEraArray +
                           state.activePlayer * sizeof(uint32_t),
                       state.era);
        state.turnNumberKnown =
            tryReadU32(memory, kCurrentTurnGlobal, state.turn);
        uint32_t year   = 0;
        state.yearKnown = tryReadU32(memory, kCurrentYearGlobal, year);
        state.year      = static_cast<int32_t>(year);
    }

    uint8_t interfaceByte = 0;
    if (tryReadU32(memory,
                   kInterfaceGateGlobal,
                   state.interfaceGate) &&
        state.interfaceGate != 0 &&
        tryReadU8(memory,
                  state.interfaceGate + 0x5,
                  interfaceByte))
    {
        state.interfaceKnown  = true;
        state.interfaceUpdate = interfaceByte != 0;
    }

    state.available = state.frontendKnown && state.gameplayActive &&
                      state.interfaceKnown && state.interfaceUpdate &&
                      state.turnOwnerKnown && state.humanTurn;
    return state;
}

void PublishFrameSnapshot()
{
    const uint32_t nextSlot = gWriterSlot ^ 1u;
    auto&          slot     = gPublishedSlots[nextSlot];
    int32_t        expected = 0;
    if (!slot.users.compare_exchange_strong(expected,
                                            -1,
                                            std::memory_order_acquire,
                                            std::memory_order_relaxed))
    {
        return;
    }

    Snapshot state      = readGuestSnapshot();
    state.frameSequence = ++gFrameSequence;
    slot.snapshot       = state;
    slot.users.store(0, std::memory_order_release);
    gWriterSlot = nextSlot;
    gActiveSlot.store(nextSlot, std::memory_order_release);
    gSnapshotPublished.store(true, std::memory_order_release);
}

static bool getPublishedSnapshot(Snapshot& out)
{
    if (!gSnapshotPublished.load(std::memory_order_acquire))
    {
        return false;
    }

    for (;;)
    {
        const uint32_t slotIndex =
            gActiveSlot.load(std::memory_order_acquire);
        auto&   slot     = gPublishedSlots[slotIndex];
        int32_t expected = slot.users.load(std::memory_order_relaxed);
        if (expected < 0 ||
            !slot.users.compare_exchange_weak(expected,
                                              expected + 1,
                                              std::memory_order_acquire,
                                              std::memory_order_relaxed))
        {
            continue;
        }

        if (slotIndex != gActiveSlot.load(std::memory_order_acquire))
        {
            slot.users.fetch_sub(1, std::memory_order_release);
            continue;
        }

        out = slot.snapshot;
        slot.users.fetch_sub(1, std::memory_order_release);
        return true;
    }
}

} // namespace rerevved::gameplay

static_assert(sizeof(GameplayState) == 80);

extern "C" uint32_t GameplayAbiVersion(void)
{
    return GAMEPLAY_ABI_VERSION;
}

extern "C" int GetGameplayState(
    GameplayState* out,
    uint32_t       outSize)
{
    if (!out)
    {
        return GAMEPLAY_ERR_INVALID_ARGUMENT;
    }

    std::memset(out, 0, std::min<size_t>(outSize, sizeof(*out)));
    if (outSize < sizeof(*out))
    {
        return GAMEPLAY_ERR_BUFFER_TOO_SMALL;
    }

    out->structSize   = sizeof(*out);
    out->activePlayer = GAMEPLAY_PLAYER_UNKNOWN;
    out->civilization = GAMEPLAY_CIVILIZATION_UNKNOWN;
    out->era          = GAMEPLAY_ERA_UNKNOWN;
    out->year         = GAMEPLAY_YEAR_UNKNOWN;
    out->turn         = GAMEPLAY_TURN_UNKNOWN;

    rerevved::gameplay::Snapshot snapshot{};
    if (!rerevved::gameplay::getPublishedSnapshot(snapshot))
    {
        return GAMEPLAY_ERR_UNAVAILABLE;
    }

    if (snapshot.frontendKnown)
    {
        out->validFields |= GAMEPLAY_VALID_FRONTEND;
    }
    if (snapshot.turnOwnerKnown)
    {
        out->validFields |= GAMEPLAY_VALID_TURN;
        out->activePlayer    = static_cast<int32_t>(snapshot.activePlayer);
        out->humanPlayerMask = snapshot.humanPlayerMask;
    }
    if (snapshot.interfaceKnown)
    {
        out->validFields |= GAMEPLAY_VALID_INTERFACE;
    }
    if (snapshot.civilizationKnown)
    {
        out->validFields |= GAMEPLAY_VALID_CIVILIZATION;
        out->civilization = static_cast<int32_t>(snapshot.civilization);
    }
    if (snapshot.eraKnown)
    {
        out->validFields |= GAMEPLAY_VALID_ERA;
        out->era = static_cast<int32_t>(snapshot.era);
    }
    if (snapshot.yearKnown)
    {
        out->validFields |= GAMEPLAY_VALID_YEAR;
        out->year = snapshot.year;
    }
    if (snapshot.turnNumberKnown)
    {
        out->validFields |= GAMEPLAY_VALID_TURN_NUMBER;
        out->turn = static_cast<int32_t>(snapshot.turn);
    }

    out->frameSequence   = snapshot.frameSequence;
    out->gameplayActive  = snapshot.gameplayActive ? 1 : 0;
    out->interfaceUpdate = snapshot.interfaceUpdate ? 1 : 0;
    out->turnOwnerKnown  = snapshot.turnOwnerKnown ? 1 : 0;
    out->humanTurn       = snapshot.humanTurn ? 1 : 0;
    out->available       = snapshot.available ? 1 : 0;
    return GAMEPLAY_OK;
}
