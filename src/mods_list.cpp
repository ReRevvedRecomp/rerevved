#include "mods_list.h"

#include <algorithm>
#include <utility>

namespace rerevved::mods_list
{

namespace
{

bool contains(const std::vector<std::string>& ids, const std::string& id)
{
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

} // namespace

void Model::Update(std::vector<Mod> mods, RuntimeState state)
{
    const bool  follows        = staged_ == state_.applied;
    const auto  before         = std::make_pair(rows_.size(), staged_);
    std::string followId       = highlightId_;
    const bool  appliedChanged = state.applied != state_.applied;
    mods_                      = std::move(mods);
    state_                     = std::move(state);
    if (follows)
        staged_ = state_.applied;
    if (grabbing_ && appliedChanged)
        Drop();
    disabledOrder_.clear();
    for (size_t i = 0; i < mods_.size(); ++i)
        disabledOrder_.push_back(i);
    std::stable_sort(disabledOrder_.begin(), disabledOrder_.end(), [this](size_t a, size_t b)
                     {
                         if (mods_[a].name != mods_[b].name)
                             return mods_[a].name < mods_[b].name;
                         return mods_[a].folder < mods_[b].folder;
                     });
    rebuild(followId);
    if (before.first != rows_.size() || before.second != staged_)
        touch();
}

void Model::SetStagedValidity(bool valid, std::string firstProblem)
{
    stagedValid_   = valid;
    stagedProblem_ = std::move(firstProblem);
}

void Model::rebuild(const std::string& followId)
{
    rows_.clear();
    for (size_t i = 0; i < staged_.size(); ++i)
    {
        const Mod* mod = nullptr;
        for (const auto& candidate : mods_)
        {
            if (candidate.id == staged_[i])
            {
                mod = &candidate;
                break;
            }
        }
        rows_.push_back({ mod ? RowKind::kEnabled : RowKind::kUnavailable, staged_[i], i + 1, mod });
    }
    for (const size_t index : disabledOrder_)
    {
        if (!contains(staged_, mods_[index].id))
            rows_.push_back({ RowKind::kDisabled, mods_[index].id, 0, &mods_[index] });
    }
    for (size_t i = 0; i < rows_.size(); ++i)
    {
        if (!followId.empty() && rows_[i].id == followId)
        {
            highlight_ = i;
            return;
        }
    }
    if (highlight_ >= rows_.size())
        highlight_ = rows_.empty() ? 0 : rows_.size() - 1;
    highlightId_ = rows_.empty() ? std::string() : rows_[highlight_].id;
}

void Model::touch()
{
    ++revision_;
}

void Model::clearStatus()
{
    status_.clear();
}

const Row* Model::Current() const
{
    return highlight_ < rows_.size() ? &rows_[highlight_] : nullptr;
}

bool Model::Select(size_t row)
{
    if (grabbing_ || confirm_ != Confirm::kNone || row >= rows_.size() || row == highlight_)
        return false;
    highlight_   = row;
    highlightId_ = rows_[row].id;
    return true;
}

bool Model::moveStaged(size_t index, int direction)
{
    if (index >= staged_.size())
        return false;
    if (direction < 0 ? index == 0 : index + 1 >= staged_.size())
        return false;
    const size_t other = direction < 0 ? index - 1 : index + 1;
    std::swap(staged_[index], staged_[other]);
    return true;
}

void Model::Move(int direction)
{
    if (direction == 0 || confirm_ != Confirm::kNone)
        return;
    if (grabbing_)
    {
        Shift(direction);
        return;
    }
    if (rows_.empty())
        return;
    if (direction < 0)
        highlight_ = highlight_ > 0 ? highlight_ - 1 : 0;
    else
        highlight_ = std::min(highlight_ + 1, rows_.size() - 1);
    highlightId_ = rows_[highlight_].id;
}

bool Model::Shift(int direction)
{
    const Row* row = Current();
    if (!row || row->kind != RowKind::kEnabled || Locked() || direction == 0)
        return false;
    const std::string id = row->id;
    if (!moveStaged(row->position - 1, direction))
        return false;
    clearStatus();
    rebuild(id);
    touch();
    return true;
}

Press Model::Activate()
{
    const Row* row = Current();
    if (!row || confirm_ != Confirm::kNone)
        return Press::kNone;
    if (grabbing_)
    {
        Drop();
        return Press::kDropped;
    }
    if (Locked())
        return Press::kNone;
    const std::string id   = row->id;
    const RowKind     kind = row->kind;
    clearStatus();
    if (kind == RowKind::kDisabled)
    {
        staged_.push_back(id);
    }
    else
    {
        staged_.erase(std::remove(staged_.begin(), staged_.end(), id), staged_.end());
    }
    rebuild(id);
    touch();
    return kind == RowKind::kDisabled ? Press::kEnabled : kind == RowKind::kEnabled ? Press::kDisabled
                                                                                    : Press::kRemoved;
}

bool Model::Grab()
{
    const Row* row = Current();
    if (!row || row->kind != RowKind::kEnabled || Locked() || grabbing_ || confirm_ != Confirm::kNone)
        return false;
    grabbing_     = true;
    grabSnapshot_ = staged_;
    return true;
}

void Model::Drop()
{
    grabbing_ = false;
    grabSnapshot_.clear();
}

void Model::CancelGrab()
{
    if (!grabbing_)
        return;
    const std::string id = highlightId_;
    staged_              = std::move(grabSnapshot_);
    Drop();
    clearStatus();
    rebuild(id);
    touch();
}

void Model::SetLock(std::optional<std::string> lock)
{
    state_.lock = std::move(lock);
    if (state_.lock && grabbing_)
        CancelGrab();
}

const std::string& Model::LockReason() const
{
    static const std::string none;
    return state_.lock ? *state_.lock : none;
}

bool Model::CanApply() const
{
    const bool persistable = Dirty() || !state_.orderFileExists || state_.orderFileInvalid;
    return (persistable || state_.liveApply) && stagedValid_ && !Locked();
}

ApplyStep Model::Apply()
{
    if (confirm_ != Confirm::kNone)
        return ApplyStep::kNone;
    if (grabbing_)
        Drop();
    if (!CanApply())
        return ApplyStep::kNone;
    if (state_.orderFileInvalid)
    {
        confirm_ = Confirm::kReplace;
        return ApplyStep::kAskReplace;
    }
    return ApplyStep::kApply;
}

void Model::ApplyFinished(ApplyResult result, std::string message)
{
    confirm_ = Confirm::kNone;
    status_.clear();
    if (result == ApplyResult::kInvalidCurrent)
        confirm_ = Confirm::kReplace;
    else if (result == ApplyResult::kFailed)
        status_ = std::move(message);
    touch();
}

BackResult Model::Back()
{
    if (confirm_ != Confirm::kNone)
    {
        confirm_ = Confirm::kNone;
        return BackResult::kHandled;
    }
    if (grabbing_)
    {
        CancelGrab();
        return BackResult::kHandled;
    }
    if (Dirty())
    {
        confirm_ = Confirm::kDiscard;
        return BackResult::kHandled;
    }
    return BackResult::kClose;
}

Accepted Model::Accept()
{
    const Confirm asked = confirm_;
    confirm_            = Confirm::kNone;
    if (asked == Confirm::kDiscard)
    {
        const std::string id = highlightId_;
        staged_              = state_.applied;
        clearStatus();
        rebuild(id);
        touch();
        return Accepted::kClose;
    }
    if (asked == Confirm::kReplace)
        return Accepted::kApplyReplacing;
    return Accepted::kNone;
}

Note Model::FooterNote() const
{
    if (Locked())
        return { NoteKind::kLocked, LockReason() };
    if (grabbing_)
        return { NoteKind::kMoving, {} };
    if (!status_.empty())
        return { NoteKind::kStatus, status_ };
    if (!stagedValid_)
        return { NoteKind::kSelectedInvalid, stagedProblem_ };
    if (state_.currentInvalid)
        return { NoteKind::kCurrentInvalid, state_.currentProblem };
    if (Dirty())
        return { NoteKind::kUnsaved, {} };
    if (state_.restartRequired)
        return { NoteKind::kRestart, {} };
    if (!state_.catalogMessages.empty())
        return { NoteKind::kCatalog, state_.catalogMessages.front().text };
    return {};
}

} // namespace rerevved::mods_list
