#include "capital_grace_special.h"

#include <cstring>
#include <map>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>

namespace
{
namespace grace = rerevved::capital_grace;

struct Profile
{
    static constexpr uint32_t                root       = 0x40000000;
    static constexpr uint32_t                child      = 0x40001000;
    static constexpr uint32_t                list       = 0x40002000;
    static constexpr uint32_t                resources  = 0x40003000;
    static constexpr uint32_t                resource   = 0x40004000;
    static constexpr uint32_t                component  = 0x40005000;
    static constexpr uint32_t                node       = 0x40006000;
    static constexpr uint32_t                collection = 0x40007000;
    std::map<uint32_t, std::vector<uint8_t>> regions{
        { 0x40000000, std::vector<uint8_t>(0x10000) },
        { 0x83140000, std::vector<uint8_t>(0x20000) }
    };
    grace::SpecialMemory memory{ this, Read };

    static bool Read(void* context, uint32_t address, void* output, uint32_t size) noexcept
    {
        for (const auto& [base, data] : static_cast<Profile*>(context)->regions)
        {
            if (address >= base && uint64_t{ address } + size <= uint64_t{ base } + data.size())
            {
                std::memcpy(output, data.data() + address - base, size);
                return true;
            }
        }
        return false;
    }

    void Word(uint32_t address, uint32_t value)
    {
        for (auto& [base, data] : regions)
        {
            if (address >= base && uint64_t{ address } + 4 <= uint64_t{ base } + data.size())
            {
                for (uint32_t i = 0; i < 4; ++i)
                {
                    data[address - base + i] = static_cast<uint8_t>(value >> (24 - 8 * i));
                }
                return;
            }
        }
        regions[address] = { static_cast<uint8_t>(value >> 24), static_cast<uint8_t>(value >> 16), static_cast<uint8_t>(value >> 8), static_cast<uint8_t>(value) };
    }

    Profile()
    {
        Word(0x8314F76C, 0x82138768);
        Word(0x8314F2A4, collection);
        Word(0x8314EFA0, root);
        Word(root, 0x82126A64);
        Word(root + 24, child);
        Word(root + 8, list);
        Word(root + 12, list + 4);
        Word(list, child);
        Word(0x8314F76C + 0x8C, child);
        Word(child, 0x82128740);
        Word(0x82128748, 0x82C7BCC0);
        Word(0x8212874C, 0x82C7BD50);
        for (const auto [offset, target] : std::array<std::pair<uint32_t, uint32_t>, 7>{ { { 0x178, 0x822DF8A0 }, { 0x17C, 0x822DF8A0 }, { 0x1F4, 0x82D82370 }, { 0xC4, 0x82D7D578 }, { 0x1F0, 0x82D7E910 }, { 0x1EC, 0x82D7CEE0 }, { 4, 0x82D80DB0 } } })
        {
            Word(0x82138768 + offset, target);
        }
    }

    void Component()
    {
        Word(0x8314F76C + 0x88, resources);
        Word(resources + 24, list + 16);
        Word(resources + 28, list + 20);
        Word(list + 16, resource);
        Word(resource + 44, component);
        Word(component, 0x821654FC);
        Word(0x821654FC, 0x822FD528);
        Word(0x822FD528, 0x3D608315);
        Word(0x822FD52C, 0x386B61E0);
        Word(0x822FD530, 0x4E800020);
    }
};

} // namespace

TEST_CASE("capital_grace_type24_refuses_before_collection_changes", "[ctest][capital-grace]")
{
    Profile profile;
    profile.Component();
    const grace::SpecialInput input{ grace::SpecialRoute::Type24, { 17, 5, 9, 0xFFFFFFFF, 0x80000000 }, { 5, 24, 0, { 10, 10 }, 0 }, -3001, 5 };
    const auto                before   = profile.regions;
    const auto                decision = grace::AdmitSpecial(profile.memory, input);
    CHECK(decision == grace::SpecialDecision::RefuseUnsupported);
    CHECK(profile.regions == before);
    CHECK(input.words[3] == 0xFFFFFFFF);

    // A callback between the admission decision and the lifecycle seam can
    // populate the previously empty nodes or replace the reached collection.
    SECTION("populate an accepted component")
    {
        profile.Word(Profile::component + 20, Profile::list + 32);
        profile.Word(Profile::component + 24, Profile::list + 36);
        profile.Word(Profile::list + 32, Profile::node);
        profile.Word(Profile::node, 0xDEADBEEF);
    }
    SECTION("replace the receiver collection")
    {
        profile.Word(0x8314F76C + 0x88, Profile::node);
        profile.Word(Profile::node, 0xDEADBEEF);
    }
    bool lifecycleCalled = false;
    if (decision == grace::SpecialDecision::Native)
    {
        lifecycleCalled = true;
    }
    CHECK_FALSE(lifecycleCalled);
    CHECK(grace::AdmitSpecial(profile.memory, input) == grace::SpecialDecision::RefuseUnsupported);
}
