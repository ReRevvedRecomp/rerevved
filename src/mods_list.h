#pragma once

// Pure model; the page owns SDK reads, Update, drawing and applying.

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace rerevved::mods_list
{

struct Message
{
    bool        error = false;
    std::string text;
};

struct Mod
{
    std::string           id;
    std::string           name;
    std::string           folder;
    std::string           version;
    std::string           author;
    std::string           description;
    std::filesystem::path iconPath;
    bool                  native = false;
    std::vector<Message>  messages;
};

enum class RowKind
{
    kEnabled,
    kDisabled,
    kUnavailable,
};

struct Row
{
    RowKind     kind = RowKind::kDisabled;
    std::string id;
    // 1-based load order; 0 for a disabled row.
    size_t position = 0;
    // Null for an unavailable row; valid until the next Update.
    const Mod* mod = nullptr;
};

struct RuntimeState
{
    std::vector<std::string>   applied;
    std::optional<std::string> lock;
    bool                       orderFileExists  = false;
    bool                       orderFileInvalid = false;
    // Live Lua replacement makes Apply meaningful even with unchanged order.
    bool                 restartRequired = false;
    bool                 liveApply       = false;
    bool                 currentInvalid  = false;
    std::string          currentProblem;
    std::vector<Message> catalogMessages;
};

enum class Confirm
{
    kNone,
    kDiscard,
    kReplace,
};

enum class Press
{
    kNone,
    kEnabled,
    kDisabled,
    kRemoved,
    kDropped,
};

enum class BackResult
{
    kClose,
    kHandled,
};

enum class Accepted
{
    kNone,
    kClose,
    kApplyReplacing,
};

enum class ApplyStep
{
    kNone,
    kApply,
    kAskReplace,
};

enum class ApplyResult
{
    kSaved,
    kInvalidCurrent,
    kFailed,
};

enum class NoteKind
{
    kNone,
    kLocked,
    kMoving,
    kStatus,
    kSelectedInvalid,
    kCurrentInvalid,
    kUnsaved,
    kRestart,
    kCatalog,
};

struct Note
{
    NoteKind    kind = NoteKind::kNone;
    std::string text;
};

class Model
{
public:
    // Replaces catalog/runtime state; pending edits survive.
    void Update(std::vector<Mod> mods, RuntimeState state);

    void SetStagedValidity(bool valid, std::string firstProblem);

    uint64_t Revision() const
    {
        return revision_;
    }

    const std::vector<Row>& Rows() const
    {
        return rows_;
    }

    const std::vector<std::string>& Staged() const
    {
        return staged_;
    }

    size_t InstalledCount() const
    {
        return mods_.size();
    }

    size_t EnabledCount() const
    {
        return staged_.size();
    }

    size_t Highlight() const
    {
        return highlight_;
    }

    const Row* Current() const;

    // Highlights a row (the mouse); ignored while a mod is grabbed.
    bool Select(size_t row);

    // Up (-1) or Down (+1): the highlight, or the grabbed mod's place.
    void Move(int direction);

    Press Activate();

    bool Grab();

    bool Grabbing() const
    {
        return grabbing_;
    }

    void Drop();
    void CancelGrab();

    bool Shift(int direction);

    void SetLock(std::optional<std::string> lock);

    bool Locked() const
    {
        return state_.lock.has_value();
    }

    const std::string& LockReason() const;

    bool Dirty() const
    {
        return staged_ != state_.applied;
    }

    // Requires changes or live Lua, a valid list, and no lock.
    bool CanApply() const;

    // The page runs the SDK call on kApply and reports with ApplyFinished.
    ApplyStep Apply();
    void      ApplyFinished(ApplyResult result, std::string message);

    Confirm Confirmation() const
    {
        return confirm_;
    }

    BackResult Back();

    Accepted Accept();

    Note FooterNote() const;

private:
    void rebuild(const std::string& followId);
    void touch();
    void clearStatus();
    bool moveStaged(size_t index, int direction);

    std::vector<Mod>         mods_;
    std::vector<size_t>      disabledOrder_;
    RuntimeState             state_;
    std::vector<std::string> staged_;
    std::vector<Row>         rows_;
    size_t                   highlight_ = 0;
    std::string              highlightId_;
    bool                     grabbing_ = false;
    std::vector<std::string> grabSnapshot_;
    Confirm                  confirm_     = Confirm::kNone;
    bool                     stagedValid_ = true;
    std::string              stagedProblem_;
    std::string              status_;
    uint64_t                 revision_ = 0;
};

} // namespace rerevved::mods_list
