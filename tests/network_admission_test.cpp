#include "join_refusal_hooks.h"
#include "network_admission.h"

#include <catch2/catch_test_macros.hpp>
#include <map>
#include <optional>

namespace
{

namespace admission = rerevved::network_admission;

constexpr uint32_t kApp        = 0x83100000;
constexpr uint32_t kCurrent    = 0x83200000;
constexpr uint32_t kSetup      = 0x83300000;
constexpr uint32_t kManager    = 0x83400000;
constexpr uint32_t kFrontend   = 0x83500000;
constexpr uint32_t kController = 0x83600000;
constexpr uint32_t kVtable     = 0x83700000;

class FakeMemory final : public admission::Memory
{
public:
    std::optional<uint32_t> Word(uint32_t address) override
    {
        const auto found = words.find(address);
        return found == words.end() ? std::nullopt : std::optional{ found->second };
    }

    std::optional<uint8_t> Byte(uint32_t address) override
    {
        const auto found = bytes.find(address);
        return found == bytes.end() ? std::nullopt : std::optional{ found->second };
    }

    std::map<uint32_t, uint32_t> words;
    std::map<uint32_t, uint8_t>  bytes;
};

FakeMemory networkStart(uint32_t mode = 6)
{
    FakeMemory memory;
    memory.words[admission::kAppGlobal]                = kApp;
    memory.words[kApp + admission::kCurrentField]      = kCurrent;
    memory.words[kApp + admission::kPendingField]      = 0;
    memory.words[kApp + admission::kRequestedField]    = admission::kGameKey;
    memory.words[kCurrent + admission::kStateKeyField] = admission::kFrontendKey;
    memory.words[admission::kSetupGlobal]              = kSetup;
    memory.words[kSetup + admission::kModeField]       = mode;
    memory.bytes[admission::kNetworkByte]              = 1;
    return memory;
}

bool starts(FakeMemory& memory, uint32_t updateResult = 1)
{
    const auto transition = admission::ReadTransition(memory, kApp, updateResult);
    return transition && admission::IsNetworkGameStart(*transition);
}

} // namespace

TEST_CASE("network admission reads the owner fields at their native offsets", "[ctest][network-admission]")
{
    auto       memory     = networkStart();
    const auto transition = admission::ReadTransition(memory, kApp, 0x101);
    REQUIRE(transition);
    CHECK(transition->app == kApp);
    CHECK(transition->root == kApp);
    CHECK(transition->current == kCurrent);
    CHECK(transition->currentKey == 1);
    CHECK(transition->pending == 0);
    CHECK(transition->requestedKey == 2);
    CHECK(transition->updateResult == 0x101);
    CHECK(transition->network == 1);
    CHECK(transition->setup == kSetup);
    CHECK(transition->mode == 6);
    CHECK(admission::IsNetworkGameStart(*transition));

    CHECK(admission::kCurrentField == 0x70);
    CHECK(admission::kPendingField == 0x74);
    CHECK(admission::kRequestedField == 0x78);
}

TEST_CASE("network admission applies only to a network game headed for key 2", "[ctest][network-admission]")
{
    for (const uint32_t mode : { 4u, 5u, 6u })
    {
        auto memory = networkStart(mode);
        CHECK(starts(memory));
    }
    for (const uint32_t mode : { 0u, 1u, 2u, 3u, 7u })
    {
        auto memory = networkStart(mode);
        CHECK_FALSE(starts(memory));
    }

    auto memory = networkStart();
    CHECK_FALSE(starts(memory, 0));
    CHECK_FALSE(starts(memory, 0x100));
    CHECK(starts(memory, 0xFF));

    memory.words[kApp + admission::kPendingField] = kCurrent;
    CHECK_FALSE(starts(memory));
    memory.words[kApp + admission::kPendingField] = 0;

    for (const uint32_t key : { 0u, 1u, 3u })
    {
        memory.words[kApp + admission::kRequestedField] = key;
        CHECK_FALSE(starts(memory));
    }
    memory.words[kApp + admission::kRequestedField] = 2;

    memory.words[kCurrent + admission::kStateKeyField] = 2;
    CHECK_FALSE(starts(memory));
    memory.words[kCurrent + admission::kStateKeyField] = 1;

    memory.bytes[admission::kNetworkByte] = 0;
    CHECK_FALSE(starts(memory));
    memory.bytes[admission::kNetworkByte] = 1;

    memory.words[admission::kAppGlobal] = kApp + 4;
    CHECK_FALSE(starts(memory));
    memory.words[admission::kAppGlobal] = kApp;

    memory.words[kApp + admission::kCurrentField] = 0;
    CHECK_FALSE(starts(memory));
    memory.words[kApp + admission::kCurrentField] = kCurrent;

    memory.words[admission::kSetupGlobal] = 0;
    CHECK_FALSE(starts(memory));
    CHECK(admission::ReadTransition(memory, 0, 1) == std::nullopt);
}

TEST_CASE("network admission reads nothing it cannot see", "[ctest][network-admission]")
{
    const auto base = networkStart();
    for (const auto& [address, value] : base.words)
    {
        auto memory = base;
        memory.words.erase(address);
        CAPTURE(address);
        CHECK_FALSE(admission::ReadTransition(memory, kApp, 1));
    }
    auto noByte = base;
    noByte.bytes.clear();
    CHECK_FALSE(admission::ReadTransition(noByte, kApp, 1));
}

TEST_CASE("network admission returns only through a controller that owns the native method", "[ctest][network-admission]")
{
    FakeMemory memory;
    memory.words[admission::kManagerGlobal]               = kManager;
    memory.words[admission::kFrontendManagerGlobal]       = kFrontend;
    memory.words[kFrontend + admission::kControllerField] = kController;
    memory.words[kController]                             = kVtable;
    for (uint32_t slot = 0; slot < admission::kMaxVtableSlots; ++slot)
    {
        memory.words[kVtable + 4 * slot] = 0x82000000 + slot;
    }
    CHECK_FALSE(admission::MatchFrontendReturn(memory));

    memory.words[kVtable + 4 * 7] = admission::kFrontendReturn;
    auto route                    = admission::MatchFrontendReturn(memory);
    REQUIRE(route);
    CHECK(route->controller == kController);
    CHECK(route->slot == 7);

    memory.words[kVtable + 4 * 7]  = 0x82000007;
    memory.words[kVtable + 4 * 63] = admission::kFrontendReturn;
    route                          = admission::MatchFrontendReturn(memory);
    REQUIRE(route);
    CHECK(route->slot == 63);

    memory.words[kVtable + 4 * 63] = 0x8200003F;
    memory.words[kVtable + 4 * 64] = admission::kFrontendReturn;
    CHECK_FALSE(admission::MatchFrontendReturn(memory));

    memory.words[kVtable + 4 * 5] = admission::kFrontendReturn;
    for (const uint32_t address : { admission::kManagerGlobal, admission::kFrontendManagerGlobal, kFrontend + admission::kControllerField, kController })
    {
        auto broken           = memory;
        broken.words[address] = 0;
        CAPTURE(address);
        CHECK_FALSE(admission::MatchFrontendReturn(broken));
        broken.words.erase(address);
        CHECK_FALSE(admission::MatchFrontendReturn(broken));
    }
}

TEST_CASE("a join is refused only while the gameplay state is current", "[ctest][network-admission]")
{
    CHECK(admission::kGameKey == 2);
    CHECK(admission::IsGameInProgress(2));
    for (const uint32_t key : { 0u, admission::kFrontendKey, 3u, 0x102u, 0xFFFFFFFFu })
    {
        CHECK_FALSE(admission::IsGameInProgress(key));
    }
}

TEST_CASE("the join refusal takes the policy's refusal return only for a game in progress", "[ctest][network-admission]")
{
    CHECK(admission::kJoinPolicyKeyCompare == 0x82D89740);
    CHECK(admission::kJoinRefusalReturn == 0x82D897B8);
    CHECK(admission::kRefusedResult == 0);

    PPCRegister result{};
    PPCRegister key{};
    result.u64 = 0xFFFFFFFF00000001ull;
    key.u64    = 2;
    CHECK(RefuseJoinInProgress(result, key));
    CHECK(result.u64 == 0);
    CHECK(key.u64 == 2);

    result.u64 = 0x1234567800000001ull;
    key.u64    = admission::kFrontendKey;
    CHECK_FALSE(RefuseJoinInProgress(result, key));
    CHECK(result.u64 == 0x1234567800000001ull);
    CHECK(key.u64 == 1);
}

TEST_CASE("network admission decides what to do after an admission wait", "[ctest][network-admission]")
{
    auto       memory  = networkStart(5);
    const auto current = admission::ReadTransition(memory, kApp, 0x101);
    REQUIRE(current);

    CHECK(admission::DecideRecheck(*current, current, false) == admission::Recheck::Apply);
    CHECK(admission::DecideRecheck(*current, current, true) == admission::Recheck::Apply);

    memory.words[kSetup + admission::kModeField] = 6;
    const auto changed                           = admission::ReadTransition(memory, kApp, 0x101);
    REQUIRE(changed);
    REQUIRE(admission::IsNetworkGameStart(*changed));
    CHECK(admission::DecideRecheck(*current, changed, false) == admission::Recheck::Retry);
    CHECK(admission::DecideRecheck(*current, changed, true) == admission::Recheck::Refuse);

    memory.words[kApp + admission::kRequestedField] = 0;
    const auto left                                 = admission::ReadTransition(memory, kApp, 0x101);
    REQUIRE(left);
    CHECK(admission::DecideRecheck(*current, left, false) == admission::Recheck::NoAction);
    CHECK(admission::DecideRecheck(*current, left, true) == admission::Recheck::NoAction);

    CHECK(admission::DecideRecheck(*current, std::nullopt, false) == admission::Recheck::NoAction);
    CHECK(admission::DecideRecheck(*current, std::nullopt, true) == admission::Recheck::Refuse);
}
