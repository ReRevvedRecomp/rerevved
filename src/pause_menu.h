#pragma once

// Append Exit Game last; retail pause code must see only its own row indices.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace rerevved::pause_menu
{

// Return addresses of the advisor list 0x82CE8338 calls from 0x82D06398 (r8=0 full, r8=1 alternate).
constexpr uint32_t kFullShowReturn      = 0x82D06538;
constexpr uint32_t kAlternateShowReturn = 0x82D06614;

constexpr uint32_t kTextManagerGlobal = 0x8314F2C8;

// 0x82D064E4..0x82D06510 passes the header to virtual +56 here; a list without it shows the advisor's name.
constexpr uint32_t kHeaderObject     = 0x8314F76C;
constexpr uint32_t kHeaderSetterSlot = 56;
constexpr uint32_t kHeaderText       = 0x8214DC68;

// 0x82D85340..0x82D85384 picks the quit question: mode word 2, then the ranked byte, else unsaved progress.
constexpr uint32_t kQuitSessionGlobal = 0x8314EFC8;
constexpr uint32_t kQuitRankedByte    = 0x8314F10B;
constexpr uint32_t kQuitTextSession   = 0x821377C0;
constexpr uint32_t kQuitTextRanked    = 0x82137828;
constexpr uint32_t kQuitTextUnsaved   = 0x821378E0;
constexpr int32_t  kQuitConfirmYes    = 1;

// Templates formatted at 0x82D064F0..0x82D0651C and 0x82D065D4..0x82D065F8; keyed by whole-template CRC-32.
constexpr uint32_t kRetailFullTemplate      = 0x8214DC80;
constexpr uint32_t kRetailAlternateTemplate = 0x8214DCF8;

enum class Variant
{
    kFull,
    kAlternate,
};

struct Choice
{
    enum class Kind
    {
        // Return retailRow (-1 cancels) to retail code.
        kRetail,
        kExitGame,
    };
    Kind    kind      = Kind::kRetail;
    int32_t retailRow = -1;
};

std::optional<Variant> VariantForReturn(uint32_t returnAddress);

// Map extended rows; unknown indices cancel to protect retail jump tables.
Choice ResolveRow(Variant variant, int32_t row);

// Disc lists have five full or three alternate rows after the header.
size_t RetailRowCount(Variant variant);

// Windows-1252 list, one newline-terminated row per line; nullopt keeps unexpected retail rows.
std::optional<std::string> BuildList(Variant variant, std::string_view formattedRetail, uint32_t languageId);

uint32_t QuitText(uint32_t sessionMode, bool sessionPresent, bool ranked);

// Called on the guest thread; the action must only queue UI-thread work. An
// empty function disables the row's effect but still closes the menu.
struct HostActions
{
    std::function<void()> quit;
};

void SetHostActions(HostActions actions);
void RequestQuit();

} // namespace rerevved::pause_menu
