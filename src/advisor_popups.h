#pragma once

// Advisor call sites either ignore sub_82CE8338's result or treat -1 as
// declining.

#include <array>
#include <cstdint>
#include <string_view>

namespace rerevved::advisor_popups
{

inline constexpr std::array<std::string_view, 15> kGroupCvars{
    "hide_popup_rival_war_peace",
    "hide_popup_rival_government",
    "hide_popup_city_captures",
    "hide_popup_victory_warnings",
    "hide_popup_rival_great_people",
    "hide_popup_rankings",
    "hide_popup_artifacts",
    "hide_popup_combat_reports",
    "hide_popup_spy_reports",
    "hide_popup_city_status",
    "hide_popup_discoveries",
    "hide_popup_villages",
    "hide_popup_save_reminder",
    "hide_popup_trophy_room",
    "hide_popup_government_available",
};

struct Site
{
    uint32_t address;
    uint8_t  group;
};

inline constexpr Site kSites[] = {
    { 0x82CC89BC, 0 },
    { 0x82CC8A3C, 0 },
    { 0x82CDFF70, 1 },
    { 0x82D1CB44, 2 },
    { 0x82D1D0A8, 2 },
    { 0x82D1D18C, 2 },
    { 0x82D1CE5C, 2 },
    { 0x82D1C48C, 2 },
    { 0x82CE1640, 3 },
    { 0x82D0A938, 3 },
    { 0x82D0CDA4, 3 },
    { 0x82D0D3AC, 3 },
    { 0x82D0D5EC, 3 },
    { 0x82D0DAD0, 3 },
    { 0x82E66AE0, 3 },
    { 0x82D11370, 3 },
    { 0x82CE1244, 4 },
    { 0x82D1775C, 4 },
    { 0x82CC7414, 5 },
    { 0x82D85B14, 5 },
    { 0x82D1B374, 5 },
    { 0x82D18D7C, 6 },
    { 0x82D08E5C, 6 },
    { 0x82D17FD8, 6 },
    { 0x82D186E0, 6 },
    { 0x82D18B9C, 6 },
    { 0x82CD8F84, 7 },
    { 0x82CD9814, 7 },
    { 0x82CD91D0, 7 },
    { 0x82CD9294, 7 },
    { 0x82CD9558, 7 },
    { 0x82CD960C, 7 },
    { 0x82CD7A38, 7 },
    { 0x82CDCBE8, 7 },
    { 0x82CDCCD4, 7 },
    { 0x82CDCE24, 7 },
    { 0x82CDCF94, 7 },
    { 0x82CDD0AC, 7 },
    { 0x82CDF268, 7 },
    { 0x82CF6E6C, 8 },
    { 0x82CF7418, 8 },
    { 0x82D1DABC, 9 },
    { 0x82D1DBA0, 9 },
    { 0x82D1DCA4, 9 },
    { 0x82D1D9B0, 9 },
    { 0x82D1E3A8, 9 },
    { 0x82CF5134, 9 },
    { 0x82CEAFC4, 10 },
    { 0x82CEAE60, 10 },
    { 0x82D076C4, 10 },
    { 0x82CF5D20, 10 },
    { 0x82CF3DC4, 10 },
    { 0x82D06FCC, 10 },
    { 0x82D17470, 10 },
    { 0x82D1DEBC, 10 },
    { 0x82CED234, 10 },
    { 0x82D0AA70, 10 },
    { 0x82D0AD5C, 10 },
    { 0x82D09098, 11 },
    { 0x82D091E8, 11 },
    { 0x82D0EFB0, 11 },
    { 0x82D08448, 11 },
    { 0x82D1D3A0, 12 },
    { 0x82D083E0, 13 },
    { 0x82CEB298, 14 },
};

// The group whose call site returns to returnAddress (the show's entry LR,
// call site + 4), or -1 for any other caller.
inline int GroupForReturn(uint32_t returnAddress)
{
    for (const Site& site : kSites)
    {
        if (site.address + 4 == returnAddress)
        {
            return site.group;
        }
    }
    return -1;
}

} // namespace rerevved::advisor_popups
