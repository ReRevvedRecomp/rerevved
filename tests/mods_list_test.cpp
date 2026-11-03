#include "mods_list.h"

#include <catch2/catch_test_macros.hpp>

#include <string>
#include <utility>
#include <vector>

namespace
{

namespace ml = rerevved::mods_list;

ml::Mod mod(const char* id, const char* name)
{
    ml::Mod m;
    m.id     = id;
    m.name   = name;
    m.folder = id;
    return m;
}

std::vector<ml::Mod> catalog()
{
    return { mod("zed", "Zed"), mod("alpha", "Alpha"), mod("mid", "Mid"), mod("beta", "Beta") };
}

ml::RuntimeState state(std::vector<std::string> applied)
{
    ml::RuntimeState s;
    s.applied         = std::move(applied);
    s.orderFileExists = true;
    return s;
}

std::vector<std::string> ids(const ml::Model& model)
{
    std::vector<std::string> out;
    for (const auto& row : model.Rows())
        out.push_back(row.id);
    return out;
}

using Ids = std::vector<std::string>;

} // namespace

TEST_CASE("mods_list_rows_are_enabled_in_order_then_disabled_by_name", "[ctest][mods-list]")
{
    ml::Model model;
    model.Update(catalog(), state({ "zed", "mid" }));
    CHECK(ids(model) == Ids{ "zed", "mid", "alpha", "beta" });
    CHECK(model.Rows()[0].kind == ml::RowKind::kEnabled);
    CHECK(model.Rows()[0].position == 1);
    CHECK(model.Rows()[1].position == 2);
    CHECK(model.Rows()[2].kind == ml::RowKind::kDisabled);
    CHECK(model.Rows()[2].position == 0);
    CHECK(model.InstalledCount() == 4);
    CHECK(model.EnabledCount() == 2);
    CHECK_FALSE(model.Dirty());
}

TEST_CASE("mods_list_unavailable_enabled_package_has_a_row", "[ctest][mods-list]")
{
    ml::Model model;
    model.Update(catalog(), state({ "gone", "mid" }));
    REQUIRE(model.Rows().size() == 5);
    CHECK(model.Rows()[0].kind == ml::RowKind::kUnavailable);
    CHECK(model.Rows()[0].mod == nullptr);
    CHECK(model.Activate() == ml::Press::kRemoved);
    CHECK(model.Staged() == Ids{ "mid" });
    CHECK(model.Dirty());
}

TEST_CASE("mods_list_activate_toggles_and_the_highlight_follows", "[ctest][mods-list]")
{
    ml::Model model;
    model.Update(catalog(), state({ "zed" }));
    model.Move(1);
    REQUIRE(model.Current()->id == "alpha");
    CHECK(model.Activate() == ml::Press::kEnabled);
    CHECK(model.Staged() == Ids{ "zed", "alpha" });
    CHECK(model.Current()->id == "alpha");
    CHECK(model.Current()->position == 2);
    CHECK(model.Dirty());
    CHECK(model.Activate() == ml::Press::kDisabled);
    CHECK(model.Staged() == Ids{ "zed" });
    CHECK(model.Current()->id == "alpha");
    CHECK_FALSE(model.Dirty());
}

TEST_CASE("mods_list_move_stops_at_the_ends", "[ctest][mods-list]")
{
    ml::Model model;
    model.Update(catalog(), state({}));
    model.Move(-1);
    CHECK(model.Highlight() == 0);
    for (int i = 0; i < 10; ++i)
        model.Move(1);
    CHECK(model.Highlight() == 3);
}

TEST_CASE("mods_list_grab_reorders_only_enabled_and_cancel_restores", "[ctest][mods-list]")
{
    ml::Model model;
    model.Update(catalog(), state({ "zed", "mid", "beta" }));
    model.Move(1);
    REQUIRE(model.Grab());
    model.Move(-1);
    CHECK(model.Staged() == Ids{ "mid", "zed", "beta" });
    CHECK(model.Current()->id == "mid");
    model.Move(-1);
    CHECK(model.Staged() == Ids{ "mid", "zed", "beta" });
    model.Move(1);
    model.Move(1);
    model.Move(1); // last enabled place; never past into the disabled rows
    CHECK(model.Staged() == Ids{ "zed", "beta", "mid" });
    CHECK(model.Current()->position == 3);
    CHECK(model.Back() == ml::BackResult::kHandled);
    CHECK_FALSE(model.Grabbing());
    CHECK(model.Staged() == Ids{ "zed", "mid", "beta" });
    CHECK_FALSE(model.Dirty());
}

TEST_CASE("mods_list_drop_keeps_the_order", "[ctest][mods-list]")
{
    ml::Model model;
    model.Update(catalog(), state({ "zed", "mid" }));
    REQUIRE(model.Grab());
    model.Move(1);
    CHECK(model.Activate() == ml::Press::kDropped);
    CHECK_FALSE(model.Grabbing());
    CHECK(model.Staged() == Ids{ "mid", "zed" });
    CHECK(model.Dirty());
}

TEST_CASE("mods_list_grab_needs_an_enabled_row", "[ctest][mods-list]")
{
    ml::Model model;
    model.Update(catalog(), state({ "zed" }));
    model.Move(1);
    CHECK_FALSE(model.Grab());
    CHECK_FALSE(model.Shift(1));
}

TEST_CASE("mods_list_shift_moves_one_place", "[ctest][mods-list]")
{
    ml::Model model;
    model.Update(catalog(), state({ "zed", "mid" }));
    CHECK(model.Shift(1));
    CHECK(model.Staged() == Ids{ "mid", "zed" });
    CHECK(model.Current()->id == "zed");
    CHECK_FALSE(model.Shift(1));
}

TEST_CASE("mods_list_lock_makes_rows_read_only", "[ctest][mods-list]")
{
    ml::Model model;
    auto      locked = state({ "zed", "mid" });
    locked.lock      = "Mods are locked.";
    model.Update(catalog(), locked);
    CHECK(model.Locked());
    CHECK(model.Activate() == ml::Press::kNone);
    CHECK_FALSE(model.Grab());
    CHECK_FALSE(model.Shift(1));
    CHECK(model.Staged() == Ids{ "zed", "mid" });
    CHECK(model.Apply() == ml::ApplyStep::kNone);
    CHECK_FALSE(model.CanApply());
    const auto note = model.FooterNote();
    CHECK(note.kind == ml::NoteKind::kLocked);
    CHECK(note.text == "Mods are locked.");
    model.Move(1);
    CHECK(model.Highlight() == 1);
}

TEST_CASE("mods_list_apply_follows_f2_rules", "[ctest][mods-list]")
{
    ml::Model model;
    model.Update(catalog(), state({ "zed" }));
    CHECK_FALSE(model.CanApply());
    CHECK(model.Apply() == ml::ApplyStep::kNone);
    auto live      = state({ "zed" });
    live.liveApply = true;
    model.Update(catalog(), live);
    CHECK(model.CanApply());
    auto fresh            = state({});
    fresh.orderFileExists = false;
    model.Update(catalog(), fresh);
    CHECK(model.CanApply());
    model.Update(catalog(), state({ "zed" }));
    model.Move(1);
    model.Activate();
    CHECK(model.CanApply());
    CHECK(model.Apply() == ml::ApplyStep::kApply);
    model.SetStagedValidity(false, "bad");
    CHECK_FALSE(model.CanApply());
    const auto note = model.FooterNote();
    CHECK(note.kind == ml::NoteKind::kSelectedInvalid);
    CHECK(note.text == "bad");
}

TEST_CASE("mods_list_apply_result_updates_pending_state", "[ctest][mods-list]")
{
    ml::Model model;
    model.Update(catalog(), state({ "zed" }));
    model.Move(1);
    model.Activate();
    REQUIRE(model.Apply() == ml::ApplyStep::kApply);
    model.ApplyFinished(ml::ApplyResult::kFailed, "disk full");
    CHECK(model.FooterNote().kind == ml::NoteKind::kStatus);
    CHECK(model.FooterNote().text == "disk full");
    CHECK(model.Dirty());
    model.Activate();
    CHECK(model.FooterNote().kind != ml::NoteKind::kStatus);
    model.Activate();
    auto saved            = state({ "zed", "alpha" });
    saved.restartRequired = true;
    model.Update(catalog(), saved);
    model.ApplyFinished(ml::ApplyResult::kSaved, {});
    CHECK_FALSE(model.Dirty());
    CHECK(model.FooterNote().kind == ml::NoteKind::kRestart);
}

TEST_CASE("mods_list_update_keeps_pending_edits_and_follows_otherwise", "[ctest][mods-list]")
{
    ml::Model model;
    model.Update(catalog(), state({ "zed" }));
    model.Update(catalog(), state({ "mid" }));
    CHECK(model.Staged() == Ids{ "mid" });
    model.Move(1);
    model.Activate();
    const Ids pending = model.Staged();
    model.Update(catalog(), state({ "mid" }));
    CHECK(model.Staged() == pending);
    CHECK(model.Dirty());
}

TEST_CASE("mods_list_back_asks_before_discarding", "[ctest][mods-list]")
{
    ml::Model model;
    model.Update(catalog(), state({ "zed" }));
    CHECK(model.Back() == ml::BackResult::kClose);
    model.Move(1);
    model.Activate();
    CHECK(model.Back() == ml::BackResult::kHandled);
    CHECK(model.Confirmation() == ml::Confirm::kDiscard);
    CHECK(model.Back() == ml::BackResult::kHandled);
    CHECK(model.Confirmation() == ml::Confirm::kNone);
    CHECK(model.Dirty());
    CHECK(model.Back() == ml::BackResult::kHandled);
    CHECK(model.Accept() == ml::Accepted::kClose);
    CHECK_FALSE(model.Dirty());
    CHECK(model.Staged() == Ids{ "zed" });
}

TEST_CASE("mods_list_input_is_ignored_while_confirming", "[ctest][mods-list]")
{
    ml::Model model;
    model.Update(catalog(), state({ "zed" }));
    model.Move(1);
    model.Activate();
    model.Back();
    const size_t at = model.Highlight();
    model.Move(1);
    CHECK(model.Highlight() == at);
    CHECK(model.Activate() == ml::Press::kNone);
    CHECK_FALSE(model.Select(0));
    CHECK(model.Apply() == ml::ApplyStep::kNone);
}

TEST_CASE("mods_list_invalid_saved_file_asks_before_replacing", "[ctest][mods-list]")
{
    ml::Model model;
    auto      bad        = state({});
    bad.orderFileInvalid = true;
    bad.currentInvalid   = true;
    bad.currentProblem   = "line 2";
    model.Update(catalog(), bad);
    CHECK(model.FooterNote().kind == ml::NoteKind::kCurrentInvalid);
    CHECK(model.Apply() == ml::ApplyStep::kAskReplace);
    CHECK(model.Confirmation() == ml::Confirm::kReplace);
    CHECK(model.Back() == ml::BackResult::kHandled);
    CHECK(model.Confirmation() == ml::Confirm::kNone);
    ml::Model other;
    other.Update(catalog(), state({}));
    other.Move(1);
    other.Activate();
    other.ApplyFinished(ml::ApplyResult::kInvalidCurrent, {});
    CHECK(other.Confirmation() == ml::Confirm::kReplace);
    CHECK(other.Accept() == ml::Accepted::kApplyReplacing);
    CHECK(other.Confirmation() == ml::Confirm::kNone);
}

TEST_CASE("mods_list_revision_changes_with_the_staged_list", "[ctest][mods-list]")
{
    ml::Model model;
    model.Update(catalog(), state({ "zed" }));
    const auto before = model.Revision();
    model.Update(catalog(), state({ "zed" }));
    CHECK(model.Revision() == before);
    model.Move(1);
    CHECK(model.Revision() == before);
    model.Activate();
    CHECK(model.Revision() != before);
}

TEST_CASE("mods_list_highlight_stays_in_range_when_rows_shrink", "[ctest][mods-list]")
{
    ml::Model model;
    model.Update(catalog(), state({}));
    for (int i = 0; i < 3; ++i)
        model.Move(1);
    model.Update({ mod("alpha", "Alpha") }, state({}));
    CHECK(model.Highlight() == 0);
    model.Update({}, state({}));
    CHECK(model.Current() == nullptr);
}

TEST_CASE("mods_list_catalog_message_is_the_lowest_note", "[ctest][mods-list]")
{
    ml::Model model;
    auto      s = state({});
    s.catalogMessages.push_back({ false, "note" });
    model.Update(catalog(), s);
    CHECK(model.FooterNote().kind == ml::NoteKind::kCatalog);
    model.Move(1);
    model.Activate();
    CHECK(model.FooterNote().kind == ml::NoteKind::kUnsaved);
}

TEST_CASE("mods_list_grab_ends_when_the_applied_loadout_changes", "[ctest][mods-list]")
{
    // An F2 save invalidates an unmoved grab; B must not restore stale order.
    ml::Model model;
    model.Update(catalog(), state({ "zed", "mid" }));
    REQUIRE(model.Grab());
    model.Update(catalog(), state({ "mid", "alpha" }));
    CHECK_FALSE(model.Grabbing());
    CHECK(model.Staged() == Ids{ "mid", "alpha" });
    CHECK_FALSE(model.Dirty());
    CHECK(model.Back() == ml::BackResult::kClose);
    CHECK(model.Staged() == Ids{ "mid", "alpha" });

    ml::Model moved;
    moved.Update(catalog(), state({ "zed", "mid" }));
    REQUIRE(moved.Grab());
    moved.Move(1);
    moved.Update(catalog(), state({ "alpha" }));
    CHECK_FALSE(moved.Grabbing());
    CHECK(moved.Staged() == Ids{ "mid", "zed" });
    CHECK(moved.Dirty());
    CHECK(moved.Back() == ml::BackResult::kHandled);
    CHECK(moved.Confirmation() == ml::Confirm::kDiscard);
}

TEST_CASE("mods_list_a_new_lock_blocks_edits_and_ends_a_grab", "[ctest][mods-list]")
{
    ml::Model model;
    model.Update(catalog(), state({ "zed", "mid" }));
    REQUIRE(model.Grab());
    model.Move(1);
    model.SetLock("locked");
    CHECK(model.Locked());
    CHECK_FALSE(model.Grabbing());
    CHECK(model.Staged() == Ids{ "zed", "mid" });
    CHECK(model.Activate() == ml::Press::kNone);
    CHECK(model.Apply() == ml::ApplyStep::kNone);
    model.SetLock(std::nullopt);
    CHECK_FALSE(model.Locked());
    CHECK(model.Activate() != ml::Press::kNone);
}
