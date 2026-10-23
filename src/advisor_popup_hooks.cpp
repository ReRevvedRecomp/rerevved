// Hidden advisor shows use 0x82CE87A8 to clear buffers and return -1; call-site
// safety is cataloged in advisor_popups.h.

#include "advisor_popups.h"

#include <rex/cvar.h>
#include <rex/ppc.h>

REXCVAR_DECLARE(bool, hide_popup_rival_war_peace);
REXCVAR_DECLARE(bool, hide_popup_rival_government);
REXCVAR_DECLARE(bool, hide_popup_city_captures);
REXCVAR_DECLARE(bool, hide_popup_victory_warnings);
REXCVAR_DECLARE(bool, hide_popup_rival_great_people);
REXCVAR_DECLARE(bool, hide_popup_rankings);
REXCVAR_DECLARE(bool, hide_popup_artifacts);
REXCVAR_DECLARE(bool, hide_popup_combat_reports);
REXCVAR_DECLARE(bool, hide_popup_spy_reports);
REXCVAR_DECLARE(bool, hide_popup_city_status);
REXCVAR_DECLARE(bool, hide_popup_discoveries);
REXCVAR_DECLARE(bool, hide_popup_villages);
REXCVAR_DECLARE(bool, hide_popup_save_reminder);
REXCVAR_DECLARE(bool, hide_popup_trophy_room);
REXCVAR_DECLARE(bool, hide_popup_government_available);

namespace
{

// Same order as advisor_popups::kGroupCvars.
bool GroupHidden(int group)
{
    switch (group)
    {
        case 0:
            return REXCVAR_GET(hide_popup_rival_war_peace);
        case 1:
            return REXCVAR_GET(hide_popup_rival_government);
        case 2:
            return REXCVAR_GET(hide_popup_city_captures);
        case 3:
            return REXCVAR_GET(hide_popup_victory_warnings);
        case 4:
            return REXCVAR_GET(hide_popup_rival_great_people);
        case 5:
            return REXCVAR_GET(hide_popup_rankings);
        case 6:
            return REXCVAR_GET(hide_popup_artifacts);
        case 7:
            return REXCVAR_GET(hide_popup_combat_reports);
        case 8:
            return REXCVAR_GET(hide_popup_spy_reports);
        case 9:
            return REXCVAR_GET(hide_popup_city_status);
        case 10:
            return REXCVAR_GET(hide_popup_discoveries);
        case 11:
            return REXCVAR_GET(hide_popup_villages);
        case 12:
            return REXCVAR_GET(hide_popup_save_reminder);
        case 13:
            return REXCVAR_GET(hide_popup_trophy_room);
        case 14:
            return REXCVAR_GET(hide_popup_government_available);
        default:
            return false;
    }
}

} // namespace

// In sub_82CE8338 after the nesting check. r12 still holds the entry LR that
// the prologue saved with mflr r12, so it names the calling site.
bool SuppressAdvisorPopupShow(PPCRegister& entryLr)
{
    return GroupHidden(rerevved::advisor_popups::GroupForReturn(entryLr.u32));
}
