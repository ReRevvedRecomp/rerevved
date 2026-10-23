#include "city_founding_unit_rules.h"
#include "registry_scope.hpp"

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <map>
#include <stdexcept>

using namespace rerevved::city_founding_unit;

namespace
{

Rule rule(bool enable, const char* provider = "test.founding")
{
    Rule result{};
    result.structSize = sizeof(result);
    std::strcpy(result.providerId, provider);
    std::strcpy(result.ruleId, "militia");
    result.enable = enable;
    return result;
}

int32_t publish(Rule request)
{
    Rule       normalized{};
    const auto status = NormalizeRequest(&request, normalized);
    if (status != 0)
    {
        return status;
    }
    auto              lock = AcquirePublicationLock();
    std::vector<Rule> candidate;
    size_t            offset = 0;
    const auto        result = BuildCandidate(std::span(&normalized, 1), candidate, offset);
    if (result == 0)
    {
        CommitCandidate(candidate);
    }
    return result;
}

class Fixture final : public World
{
public:
    Input                       input{ 0x400000, 0, 2 };
    std::map<uint32_t, uint8_t> memory;
    int                         calls               = 0;
    int                         writes              = 0;
    int32_t                     returnedSlot        = 0;
    bool                        retainedObject      = true;
    bool                        matchingBackPointer = true;
    bool                        reenter             = false;
    bool                        throwInCreator      = false;
    Outcome                     nested;
    std::array<uint8_t, 0x54>   before{};

    Fixture()
    {
        word(input.frame + 0x178, 0x82CE03AC);
        word(input.frame + 0x19C, 13);
        word(input.frame + 0x1A4, 17);
        word(0x82F700B0, 5);
        for (uint32_t i = 0; i < 0x34; ++i)
        {
            memory[city() + i] = 0;
        }
        memory[city() + 1] = static_cast<uint8_t>(input.owner);
        half(city() + 0x30, 13);
        half(city() + 0x32, 17);
    }

    uint32_t city() const
    {
        return 0x8314FE28 + 0xBC * static_cast<uint32_t>(input.citySlot);
    }

    uint32_t unit() const
    {
        return 0x830F2BF0 + 0x54 * (256 * static_cast<uint32_t>(input.owner) + static_cast<uint32_t>(returnedSlot));
    }

    void word(uint32_t address, uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
        {
            memory[address + i] = static_cast<uint8_t>(value >> (24 - 8 * i));
        }
    }

    void half(uint32_t address, uint16_t value)
    {
        memory[address]     = static_cast<uint8_t>(value >> 8);
        memory[address + 1] = static_cast<uint8_t>(value);
    }

    bool Read(uint32_t address, void* out, uint32_t size) override
    {
        auto* bytes = static_cast<uint8_t*>(out);
        for (uint32_t i = 0; i < size; ++i)
        {
            const auto found = memory.find(address + i);
            if (found == memory.end())
            {
                return false;
            }
            bytes[i] = found->second;
        }
        return true;
    }

    bool Write(uint32_t address, const void* bytes, uint32_t size) override
    {
        ++writes;
        UNSCOPED_INFO("only the created unit's upgrade word may change");
        REQUIRE((address == unit() + 0x10 && size == 4));
        const auto* source = static_cast<const uint8_t*>(bytes);
        for (uint32_t i = 0; i < size; ++i)
        {
            memory[address + i] = source[i];
        }
        return true;
    }

    int32_t Create(int32_t owner, int32_t type, int32_t x, int32_t y) override
    {
        ++calls;
        UNSCOPED_INFO("creator must receive actual owner and city coordinates with ordinary type 7");
        REQUIRE((owner == input.owner && type == 7 && x == 13 && y == 17));
        if (reenter)
        {
            reenter = false;
            nested  = Apply(*this, input);
        }
        if (throwInCreator)
        {
            throw std::runtime_error("creator interrupted");
        }
        if (returnedSlot < 0 || returnedSlot >= 256)
        {
            return returnedSlot;
        }
        for (uint32_t i = 0; i < before.size(); ++i)
        {
            memory[unit() + i] = 0;
        }
        memory[unit()]     = static_cast<uint8_t>(owner);
        memory[unit() + 1] = 7;
        memory[unit() + 2] = 3;
        memory[unit() + 5] = 1;
        word(unit() + 0x10, 0x80000040);
        half(unit() + 0x1C, static_cast<uint16_t>(x));
        half(unit() + 0x1E, static_cast<uint16_t>(y));
        half(unit() + 0x30, static_cast<uint16_t>(returnedSlot));
        word(unit() + 0x3C, retainedObject ? 0x500000 : 0);
        word(0x500084, matchingBackPointer ? unit() : unit() + 0x54);
        UNSCOPED_INFO("fixture unit");
        REQUIRE(Read(unit(), before.data(), static_cast<uint32_t>(before.size())));
        return returnedSlot;
    }
};

void TestRules()
{
    ResetForTests();
    UNSCOPED_INFO("default off");
    REQUIRE((!IsEnabled() && RuleCount() == 0));
    UNSCOPED_INFO("explicit disabled rule");
    REQUIRE((publish(rule(false)) == 0 && !IsEnabled()));
    UNSCOPED_INFO("changed replay rejected");
    REQUIRE((publish(rule(true)) == kConflict && !IsEnabled()));
    ResetForTests();
    UNSCOPED_INFO("enabled rule");
    REQUIRE((publish(rule(true)) == 0 && IsEnabled()));
    UNSCOPED_INFO("identical replay");
    REQUIRE((publish(rule(true)) == 0 && RuleCount() == 1));
    UNSCOPED_INFO("conflicting provider rejected");
    REQUIRE((publish(rule(true, "other.provider")) == kConflict && RuleCount() == 1));
    auto invalid   = rule(true);
    invalid.enable = 2;
    UNSCOPED_INFO("boolean domain");
    REQUIRE(publish(invalid) == kInvalidArgument);
    invalid            = rule(true);
    invalid.structSize = sizeof(invalid) - 1;
    UNSCOPED_INFO("undersized request rejected");
    REQUIRE(publish(invalid) == kInvalidArgument);
    invalid.structSize = sizeof(invalid) + sizeof(uint32_t);
    Rule normalized{};
    UNSCOPED_INFO("larger request normalized to current size");
    REQUIRE((NormalizeRequest(&invalid, normalized) == 0 && normalized.structSize == sizeof(Rule)));
    invalid.structSize = sizeof(invalid);
    for (auto& reserved : invalid.reserved)
    {
        reserved = 1;
        UNSCOPED_INFO("nonzero reserved field rejected without changing registration");
        REQUIRE((publish(invalid) == kInvalidArgument && IsEnabled() && RuleCount() == 1));
        reserved = 0;
    }
}

void TestGates()
{
    ResetForTests();
    Fixture    disabled;
    const auto unchanged = disabled.memory;
    UNSCOPED_INFO("disabled path is native");
    REQUIRE((Apply(disabled, disabled.input).decision == Decision::Disabled && disabled.calls == 0 && disabled.memory == unchanged));
    UNSCOPED_INFO("enable gates");
    REQUIRE(publish(rule(true)) == 0);
    Fixture frame;
    frame.input.frame = 1;
    UNSCOPED_INFO("invalid frame is a no-op");
    REQUIRE((Apply(frame, frame.input).decision == Decision::InvalidFrame && frame.calls == 0));
    for (const uint32_t pc : { 0u, 0x82D1B788u, 0x82D2334Cu, 0x82D23790u, 0x82D23908u, 0x82D24410u })
    {
        Fixture f;
        f.word(f.input.frame + 0x178, pc);
        UNSCOPED_INFO("capture/setup/unknown origin rejected");
        REQUIRE((Apply(f, f.input).decision == Decision::OriginMismatch && f.calls == 0 && f.writes == 0));
    }
    for (const uint32_t pc : { 0x82CE03ACu, 0x82CBC040u })
    {
        Fixture f;
        f.word(f.input.frame + 0x178, pc);
        UNSCOPED_INFO("command and AI gate accepted");
        REQUIRE((Apply(f, f.input).loyaltyWritten && f.calls == 1));
    }
    for (int32_t owner : { 0, 1, 2, 3, 4, 6, 7 })
    {
        Fixture f;
        f.input.owner          = owner;
        f.memory[f.city() + 1] = static_cast<uint8_t>(owner);
        UNSCOPED_INFO("all nonbarbarian owners use the actual city owner");
        REQUIRE((Apply(f, f.input).loyaltyWritten && f.calls == 1));
    }
    Fixture barbarian;
    barbarian.word(0x82F700B0, static_cast<uint32_t>(barbarian.input.owner));
    UNSCOPED_INFO("barbarian owner rejected");
    REQUIRE((Apply(barbarian, barbarian.input).decision == Decision::InvalidOwner && barbarian.calls == 0));
    Fixture city;
    city.memory[city.city()] = 0xFF;
    UNSCOPED_INFO("dead city rejected");
    REQUIRE((Apply(city, city.input).decision == Decision::InvalidCity && city.calls == 0));
    city.memory[city.city()]     = 0;
    city.memory[city.city() + 1] = 3;
    UNSCOPED_INFO("city owner mismatch rejected");
    REQUIRE((Apply(city, city.input).decision == Decision::InvalidCity && city.calls == 0));
    city.memory[city.city() + 1] = 2;
    city.half(city.city() + 0x30, 14);
    UNSCOPED_INFO("city coordinate mismatch rejected");
    REQUIRE((Apply(city, city.input).decision == Decision::InvalidCity && city.calls == 0));
    for (int32_t slot : { -1, 128 })
    {
        city.input.citySlot = slot;
        UNSCOPED_INFO("city slot range");
        REQUIRE(Apply(city, city.input).decision == Decision::InvalidCity);
    }
}

void TestCreation()
{
    for (int32_t slot : { -1, 0, 255, 256 })
    {
        Fixture f;
        f.returnedSlot    = slot;
        const auto result = Apply(f, f.input);
        UNSCOPED_INFO("one native creator call, no retry");
        REQUIRE((result.creatorCalled && result.unitSlot == slot && f.calls == 1));
        UNSCOPED_INFO("slot zero valid; minus one fails");
        REQUIRE(result.loyaltyWritten == (slot >= 0 && slot < 256));
        if (result.loyaltyWritten)
        {
            auto expected = f.before;
            expected[0x13] |= 0x8;
            std::array<uint8_t, 0x54> after{};
            UNSCOPED_INFO("big-endian Loyalty only; rank, movement and all other bits unchanged");
            REQUIRE((f.Read(f.unit(), after.data(), static_cast<uint32_t>(after.size())) && after == expected));
            UNSCOPED_INFO("Loyalty is not Engineer");
            REQUIRE((f.writes == 1 && result.upgradesAfter == 0x80000048));
        }
    }
    Fixture rejectedObject;
    rejectedObject.retainedObject = false;
    UNSCOPED_INFO("normal creator return with rejected object gets no grant or retry");
    REQUIRE((!Apply(rejectedObject, rejectedObject.input).loyaltyWritten && rejectedObject.calls == 1 && rejectedObject.writes == 0));
    Fixture wrongObject;
    wrongObject.matchingBackPointer = false;
    UNSCOPED_INFO("object must point back to this unit");
    REQUIRE((!Apply(wrongObject, wrongObject.input).loyaltyWritten && wrongObject.writes == 0));
    Fixture        defender;
    const uint32_t nativeDefender       = defender.unit() + 0x54;
    defender.memory[nativeDefender]     = static_cast<uint8_t>(defender.input.owner);
    defender.memory[nativeDefender + 1] = 7;
    defender.half(nativeDefender + 0x1C, 13);
    defender.half(nativeDefender + 0x1E, 17);
    defender.word(nativeDefender + 0x10, 0x40);
    const auto nativeBytes = defender.memory;
    UNSCOPED_INFO("native or scenario Militia does not suppress the extra grant");
    REQUIRE((Apply(defender, defender.input).loyaltyWritten && defender.calls == 1));
    for (const auto& [address, byte] : nativeBytes)
    {
        UNSCOPED_INFO("existing defender and founding inputs stay unchanged");
        REQUIRE(defender.memory.at(address) == byte);
    }
    Fixture recursive;
    recursive.reenter = true;
    UNSCOPED_INFO("same founding cannot allocate twice through reentry");
    REQUIRE((Apply(recursive, recursive.input).loyaltyWritten && recursive.calls == 1 && recursive.writes == 1 && recursive.nested.decision == Decision::Reentry));
    Fixture interrupted;
    interrupted.throwInCreator = true;
    try
    {
        Apply(interrupted, interrupted.input);
        UNSCOPED_INFO("creator exception fixture");
        REQUIRE(false);
    }
    catch (const std::runtime_error&)
    {
    }
    Fixture later;
    UNSCOPED_INFO("guard releases after interrupted invocation; no durable city-slot latch");
    REQUIRE(Apply(later, later.input).loyaltyWritten);
}

} // namespace

TEST_CASE("city_founding_unit_rules", "[ctest][city-founding-unit-rules][local-fast]")
{
    const rerevved::tests::ScopedRegistryReset registryScope;
    TestRules();
    TestGates();
    TestCreation();
}
