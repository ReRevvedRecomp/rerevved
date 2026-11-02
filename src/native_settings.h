#pragma once

// Options model over registry services; host glue is in native_settings_host.cpp.

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/ui/overlay/settings_layout.h>

#include "text_entry.h"
#include "title_text.h"

namespace rerevved::native_settings
{

constexpr std::string_view kNoteRestart   = "Takes effect after restart.";
constexpr std::string_view kNoteNextGame  = "Takes effect in the next game.";
constexpr std::string_view kNoteUnsaved   = "Changed for this session, but it could not be saved.";
constexpr std::string_view kNoteRejected  = "That value was not accepted.";
constexpr std::string_view kNoteNoMonitor = "Monitors are unavailable.";
constexpr std::string_view kNoteNoAudio   = "Available once a game profile is loaded.";

constexpr int64_t kWeekLast      = 2147483647;
constexpr size_t  kWeekMaxDigits = 10;
// Keep an 11-digit entry so range refusal does not silently truncate it.
constexpr size_t kWeekEntryLength = kWeekMaxDigits + 1;

std::optional<std::string> NormalizeWeek(std::string_view text);

enum class FlagKind
{
    kBool,
    kInteger,
    kDouble,
    kString,
    kOther,
};

struct FlagState
{
    FlagKind                 kind = FlagKind::kOther;
    std::string              value;
    std::vector<std::string> allowed;
    std::optional<double>    min;
    std::optional<double>    max;
    bool                     requiresRestart = false;
    std::string              category;
    std::string              description;
    std::string              defaultValue;
    int                      conflicts = 0;
};

struct Choice
{
    std::string raw;
    std::string caption;
};

enum class ApplyStatus
{
    kRejected,
    kSaved,
    kUnsaved,
};

struct LiveVolume
{
    int  position = 0;
    bool exact    = true;
    int  below    = 0;
    int  above    = 0;
};

struct LiveVolumes
{
    uint64_t                                 revision = 0;
    std::array<std::optional<LiveVolume>, 4> volume{};
};

// All host calls run on the UI thread; none may queue and wait.
class Services
{
public:
    virtual ~Services() = default;

    virtual const rex::ui::SettingsLayout&     Layout() const                                    = 0;
    virtual std::optional<FlagState>           Read(std::string_view key)                        = 0;
    virtual std::optional<std::vector<Choice>> MonitorChoices()                                  = 0;
    virtual ApplyStatus                        Apply(std::string_view key, std::string_view raw) = 0;

    // F1 order: controller keys, then overlay hotkeys.
    virtual std::vector<std::string> BindKeys()
    {
        return {};
    }

    virtual bool Invoke(std::string_view key)
    {
        (void)key;
        return false;
    }

    // The retail audio rows (retail_audio.h), used by the Options page only.
    // The defaults suit a host that has none.
    // The live game values, a channel being empty while the game has no entry.
    virtual LiveVolumes ReadLiveVolumes()
    {
        return {};
    }

    virtual uint64_t LiveRevision()
    {
        return 0;
    }

    virtual void RequestLiveRefresh()
    {
    }
};

std::vector<std::string_view> CuratedKeys();

std::string SanitizeCaption(std::string_view text, size_t maxLength);

constexpr size_t kPickerThreshold = 10;

enum class Timing
{
    kLive,
    kNextGame,
    kRestart,
};

// English, as F1 words it; the model translates it for the page.
std::string_view TimingNote(Timing timing);
std::string_view OutcomeNote(ApplyStatus status);

enum class RowKind
{
    kChoice,
    kAction,
    kStatus,
    kText,
    kBind,
};

struct OptionRow
{
    std::string         key;
    std::string         label;
    std::string         description;
    std::string         group;
    std::vector<Choice> choices;
    size_t              current     = 0;
    Timing              timing      = Timing::kLive;
    bool                toggle      = false;
    bool                monitor     = false;
    bool                stepper     = false;
    bool                unavailable = false;
    // Left, Right and A go to below and above (indices into choices).
    bool    offGrid = false;
    size_t  below   = 0;
    size_t  above   = 0;
    RowKind kind    = RowKind::kChoice;
    // Secret rows keep no value here, only Set or Not set.
    std::string       text;
    bool              secret    = false;
    bool              secretSet = false;
    bool              numeric   = false;
    std::string       bindValue;
    std::string       bindDefault;
    bool              bindController = false;
    int               conflicts      = 0;
    text_entry::Rules rules;

    bool HasCopyPaste() const
    {
        return kind == RowKind::kText && secret;
    }

    bool HasOpenButton() const
    {
        return kind == RowKind::kAction;
    }

    bool HasBindButtons() const
    {
        return kind == RowKind::kBind;
    }

    bool CanClearBind() const
    {
        return kind == RowKind::kBind && !bindValue.empty();
    }

    bool CanResetBind() const
    {
        return kind == RowKind::kBind && bindValue != bindDefault;
    }

    bool CanCopy() const
    {
        return HasCopyPaste() && secretSet;
    }

    bool UsesPicker() const
    {
        return !stepper && choices.size() > kPickerThreshold;
    }

    const std::string& Caption() const
    {
        return choices[current].caption;
    }
};

struct OptionSection
{
    std::string            title;
    std::vector<OptionRow> rows;
};

// Page-thread calls only; services must not queue and wait.
class OptionsModel
{
public:
    explicit OptionsModel(Services& services, title_text::Language language = title_text::Language::kEnglish);

    const std::vector<OptionSection>& Sections() const
    {
        return sections_;
    }

    size_t RowCount() const
    {
        return rowCount_;
    }

    const OptionRow* Row(size_t index) const;
    size_t           SectionOf(size_t index) const;
    size_t           SectionStart(size_t section) const;

    size_t Tab() const
    {
        return SectionOf(highlight_);
    }

    size_t Highlight() const
    {
        return highlight_;
    }

    void Select(size_t index);
    void Move(int delta);
    // False for an invalid or current tab, or while the picker is open.
    bool SelectTab(size_t tab);
    // Wraps between the last and first tab; false when it did not move.
    bool StepTab(int direction);

    // No wrap; false when there is no such choice or nothing was applied.
    bool Step(int direction);

    enum class Press
    {
        kNone,
        kApplied,
        kOpenPicker,
        kRan,
        kOpenText,
        kCapture,
    };
    Press Activate();

    bool PickerOpen() const
    {
        return pickerOpen_;
    }

    bool ModalOpen() const
    {
        return pickerOpen_ || editor_.has_value() || !captureKey_.empty();
    }

    bool Capturing() const
    {
        return !captureKey_.empty();
    }

    bool IsCapturing(std::string_view key) const
    {
        return !key.empty() && captureKey_ == key;
    }

    ApplyStatus CommitBinding(std::string_view value);
    void        CancelCapture();
    void        RefuseCapture();
    // False when there is nothing to change or the row is not a bind.
    bool ClearBind();
    bool ResetBind();

    bool TextOpen() const
    {
        return editor_.has_value();
    }

    text_entry::Editor& TextEditor()
    {
        return *editor_;
    }

    text_entry::Keyboard& TextKeyboard()
    {
        return keyboard_;
    }

    const OptionRow* TextRow() const;
    // Refusal keeps the entry open with a note.
    ApplyStatus CommitText();
    void        CancelText();

    void SetTextRefused(bool refused)
    {
        outcome_ = refused ? ApplyStatus::kRejected : ApplyStatus::kSaved;
    }

    // Needs a highlighted secret row and no picker or entry. The caller wipes clipboard text.
    ApplyStatus PasteSecret(std::string_view text);
    // Passes the stored value to put, then wipes the local copy; false if empty.
    bool CopySecret(const std::function<void(const char*)>& put);

    size_t PickerCursor() const
    {
        return pickerCursor_;
    }

    void MovePicker(int delta);
    void SetPickerCursor(size_t index);
    bool CommitPicker();
    void ClosePicker();

    void Reload();
    void Poll();

    const std::string& Description() const;
    std::string        Note() const;

private:
    void rebuild();
    void refreshStatus();
    bool applyChoice(size_t index);
    void remember();

    Services&                                    services_;
    title_text::Language                         language_;
    std::vector<Choice>                          monitors_;
    std::vector<OptionSection>                   sections_;
    size_t                                       rowCount_     = 0;
    size_t                                       highlight_    = 0;
    bool                                         pickerOpen_   = false;
    size_t                                       pickerCursor_ = 0;
    std::optional<text_entry::Editor>            editor_;
    text_entry::Keyboard                         keyboard_;
    std::string                                  textKey_;
    std::string                                  captureKey_;
    bool                                         captureRefused_ = false;
    bool                                         copied_         = false;
    ApplyStatus                                  outcome_        = ApplyStatus::kSaved;
    bool                                         noMonitor_      = false;
    uint64_t                                     liveRevision_   = 0;
    uint32_t                                     polls_          = 0;
    std::unordered_map<std::string, std::string> remembered_;
};

using Poster = std::function<bool(std::function<void()>)>;

// post queues job on the UI thread; the caller waits without holding its locks.
// Timeout abandons an unstarted job; alive() returning false abandons either.
template <typename Result>
std::optional<Result> RunOnUi(const Poster&                post,
                              const std::function<bool()>& alive,
                              std::chrono::milliseconds    timeout,
                              std::function<Result()>      job)
{
    struct Ticket
    {
        std::mutex              mutex;
        std::condition_variable done;
        bool                    started   = false;
        bool                    finished  = false;
        bool                    abandoned = false;
        std::optional<Result>   result;
    };

    auto ticket = std::make_shared<Ticket>();
    if (!post([ticket, job = std::move(job)]
              {
                  {
                      std::lock_guard lock(ticket->mutex);
                      if (ticket->abandoned)
                          return;
                      ticket->started = true;
                  }
                  Result result = job();
                  {
                      std::lock_guard lock(ticket->mutex);
                      ticket->result   = std::move(result);
                      ticket->finished = true;
                  }
                  ticket->done.notify_all();
              }))
    {
        return std::nullopt;
    }

    constexpr auto   kSlice   = std::chrono::milliseconds(20);
    const auto       deadline = std::chrono::steady_clock::now() + timeout;
    std::unique_lock lock(ticket->mutex);
    while (!ticket->finished)
    {
        const auto now = std::chrono::steady_clock::now();
        if (!alive() || (now >= deadline && !ticket->started))
        {
            ticket->abandoned = true;
            return std::nullopt;
        }
        const auto remaining = now < deadline ? deadline - now : std::chrono::steady_clock::duration(kSlice);
        ticket->done.wait_for(lock, std::min<std::chrono::steady_clock::duration>(kSlice, remaining));
    }
    return std::move(ticket->result);
}

struct HostBindings
{
    Poster                                                        post;
    std::function<bool()>                                         alive;
    std::filesystem::path                                         configPath;
    std::function<std::unordered_map<std::string, std::string>()> monitorChoices;
    // UI-thread open; the guest waits so pad capture precedes its loop. Returns page id, or zero.
    std::function<uint64_t()>                      openOptionsPage;
    std::function<bool(uint64_t)>                  optionsPageOpen;
    std::function<void(uint64_t)>                  closeOptionsPage;
    std::shared_ptr<const rex::ui::SettingsLayout> layout;
    std::chrono::milliseconds                      timeout{ 5000 };
};

void                        SetHost(HostBindings host);
std::optional<HostBindings> Host();

std::optional<FlagState> ReadFlag(std::string_view name);
ApplyStatus              ApplyAndSave(std::string_view name, std::string_view raw, const std::filesystem::path& configPath);
// ApplyAndSave for a retail volume cvar: once the value is accepted, posts the
// live apply for the guest thread. Other names behave as ApplyAndSave.
ApplyStatus ApplyAndSaveVolume(std::string_view name, std::string_view raw, const std::filesystem::path& configPath);

std::unique_ptr<Services> MakeUiServices(HostBindings host);

} // namespace rerevved::native_settings
