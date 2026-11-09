// Registry reads and the UI-thread apply and save behind the Options page.

#include "native_settings.h"

#include <algorithm>
#include <charconv>
#include <mutex>
#include <utility>

#include <rex/cvar.h>

#include "retail_audio.h"

REXCVAR_DECLARE(double, mnk_sensitivity);

namespace rerevved::native_settings
{

namespace
{

std::mutex                  gHostMutex;
std::optional<HostBindings> gHost;

std::vector<Choice> MonitorChoicesFrom(const std::unordered_map<std::string, std::string>& labels)
{
    std::vector<std::pair<int, Choice>> ordered;
    for (const auto& [raw, caption] : labels)
    {
        int         ordinal = 0;
        const char* end     = raw.data() + raw.size();
        const auto  r       = std::from_chars(raw.data(), end, ordinal);
        if (raw.empty() || r.ec != std::errc{} || r.ptr != end || ordinal < 0)
            continue;
        ordered.push_back({ ordinal, { raw, SanitizeCaption(caption, 44) } });
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto& a, const auto& b)
              {
                  return a.first < b.first;
              });
    std::vector<Choice> result;
    for (auto& entry : ordered)
        result.push_back(std::move(entry.second));
    return result;
}

bool IsBindCategory(std::string_view category)
{
    return category == "Input/Keybinds" || category.starts_with("Input/Keybinds/");
}

// Match F1 selectors: exact name, prefix ending in *, or @Category with subcategories.
bool Selects(std::string_view match, const rex::cvar::FlagEntry& entry)
{
    if (match.starts_with('@'))
    {
        const std::string_view category = match.substr(1);
        return entry.category == category ||
               (entry.category.size() > category.size() && entry.category.starts_with(category) && entry.category[category.size()] == '/');
    }
    if (match.ends_with('*'))
        return std::string_view(entry.name).starts_with(match.substr(0, match.size() - 1));
    return entry.name == match;
}

// Definition-hidden rows stay hidden; otherwise the last matching layout rule wins.
rex::cvar::Audience EffectiveAudience(const rex::cvar::FlagEntry& entry, const rex::ui::SettingsLayout& layout)
{
    if (entry.audience == rex::cvar::Audience::kHidden)
        return entry.audience;
    rex::cvar::Audience audience = entry.audience;
    for (const auto& rule : layout.audience)
    {
        if (Selects(rule.match, entry))
            audience = rule.audience;
    }
    return audience;
}

// Options runs on the UI thread, so reads and applies need no queue.
class UiServices final : public Services
{
public:
    explicit UiServices(HostBindings host)
    : host_(std::move(host))
    {
    }

    const rex::ui::SettingsLayout& Layout() const override
    {
        return *host_.layout;
    }

    std::optional<FlagState> Read(std::string_view key) override
    {
        auto state = ReadFlag(key);
        if (state && host_.layout->row_lock)
        {
            if (const auto lock = host_.layout->row_lock(key))
                state->value = lock->value;
        }
        return state;
    }

    std::optional<std::vector<Choice>> MonitorChoices() override
    {
        if (!host_.monitorChoices)
            return std::nullopt;
        return MonitorChoicesFrom(host_.monitorChoices());
    }

    ApplyStatus Apply(std::string_view key, std::string_view raw) override
    {
        if (host_.layout->row_lock && host_.layout->row_lock(key))
            return ApplyStatus::kRejected;
        return ApplyAndSave(key, raw, host_.configPath);
    }

    // F1 order: layout Keybinds controller keys, then other Input/Keybinds; player-visible rows only.
    std::vector<std::string> BindKeys() override
    {
        std::vector<std::string> controller;
        std::vector<std::string> hotkeys;
        for (const auto& entry : rex::cvar::GetRegistry())
        {
            if (!IsBindCategory(entry.category) || entry.type != rex::cvar::FlagType::String)
                continue;
            if (EffectiveAudience(entry, *host_.layout) != rex::cvar::Audience::kPlayer)
                continue;
            const bool isController = entry.category == "Input/Keybinds/Controller" || entry.category.starts_with("Input/Keybinds/Controller/");
            (isController ? controller : hotkeys).push_back(entry.name);
        }
        controller.insert(controller.end(), hotkeys.begin(), hotkeys.end());
        return controller;
    }

    bool Invoke(std::string_view key) override
    {
        return rex::cvar::InvokeCommand(key, {});
    }

    LiveVolumes ReadLiveVolumes() override
    {
        LiveVolumes result;
        const auto  snapshot = retail_audio::Live().Read(result.revision);
        for (size_t channel = 0; channel < result.volume.size() && channel < snapshot.volume.size(); ++channel)
        {
            if (!snapshot.volume[channel].available)
                continue;
            const auto reading     = retail_audio::ReadGain(snapshot.volume[channel].gain);
            result.volume[channel] = LiveVolume{ reading.position, reading.exact, reading.below, reading.above };
        }
        return result;
    }

    uint64_t LiveRevision() override
    {
        return retail_audio::Live().Revision();
    }

    void RequestLiveRefresh() override
    {
        retail_audio::Live().RequestRefresh();
    }

private:
    HostBindings host_;
};

} // namespace

void SetHost(HostBindings host)
{
    std::lock_guard lock(gHostMutex);
    if (host.post)
        gHost = std::move(host);
    else
        gHost.reset();
}

std::optional<HostBindings> Host()
{
    std::lock_guard lock(gHostMutex);
    return gHost;
}

std::optional<FlagState> ReadFlag(std::string_view name)
{
    const auto* info = rex::cvar::GetFlagInfo(name);
    if (!info)
        return std::nullopt;
    FlagState state;
    switch (info->type)
    {
        case rex::cvar::FlagType::Boolean:
            state.kind = FlagKind::kBool;
            break;
        case rex::cvar::FlagType::Int32:
        case rex::cvar::FlagType::Int64:
        case rex::cvar::FlagType::Uint32:
        case rex::cvar::FlagType::Uint64:
            state.kind = FlagKind::kInteger;
            break;
        case rex::cvar::FlagType::Double:
            state.kind = FlagKind::kDouble;
            break;
        case rex::cvar::FlagType::String:
            state.kind = FlagKind::kString;
            break;
        case rex::cvar::FlagType::Command:
            state.kind = FlagKind::kOther;
            break;
    }
    state.value = rex::cvar::GetFlagByName(name);
    // Registry text has six decimals; full precision writes the same number back.
    if (state.kind == FlagKind::kDouble && name == "mnk_sensitivity")
    {
        char       text[64];
        const auto r = std::to_chars(text, text + sizeof text, REXCVAR_GET(mnk_sensitivity));
        if (r.ec == std::errc{})
            state.value.assign(text, r.ptr);
    }
    state.allowed         = info->constraints.allowed_values;
    state.min             = info->constraints.min;
    state.max             = info->constraints.max;
    state.requiresRestart = info->lifecycle == rex::cvar::Lifecycle::kRequiresRestart;
    state.category        = info->category;
    state.description     = info->description;
    state.defaultValue    = info->default_value;
    if (IsBindCategory(info->category) && !state.value.empty())
    {
        for (const auto& other : rex::cvar::GetRegistry())
        {
            if (IsBindCategory(other.category) && other.name != info->name && other.getter() == state.value)
                ++state.conflicts;
        }
    }
    return state;
}

ApplyStatus ApplyAndSave(std::string_view name, std::string_view raw, const std::filesystem::path& configPath)
{
    if (!rex::cvar::SetFlagByName(name, raw))
        return ApplyStatus::kRejected;
    return rex::cvar::TrySaveConfigSubset(configPath, { std::string(name) }) ? ApplyStatus::kSaved : ApplyStatus::kUnsaved;
}

std::unique_ptr<Services> MakeUiServices(HostBindings host)
{
    if (!host.layout)
        return nullptr;
    return std::make_unique<UiServices>(std::move(host));
}

} // namespace rerevved::native_settings
