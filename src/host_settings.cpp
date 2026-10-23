#include "host_settings.h"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <mutex>

namespace rerevved::host_settings
{

namespace
{

constexpr uint8_t kFormatVersion   = 1;
constexpr uint8_t kTruncatedFlag   = 0x01;
constexpr size_t  kHeaderSize      = 4;
constexpr size_t  kMaxFieldLength  = 255;
constexpr size_t  kMaxModCount     = 255;
constexpr size_t  kMaxListedIssues = 4;

std::mutex                                 localModsMutex;
std::shared_ptr<const std::vector<ModRef>> localMods;

int32_t clampAi(int32_t value)
{
    return std::clamp(value, int32_t{ 0 }, kMaxAiCivilizations);
}

size_t encodedSize(const ModRef& mod)
{
    return 3 + mod.id.size() + mod.version.size() + mod.displayName.size();
}

bool fitsFields(const ModRef& mod)
{
    return mod.id.size() <= kMaxFieldLength && mod.version.size() <= kMaxFieldLength &&
           mod.displayName.size() <= kMaxFieldLength;
}

void appendField(std::vector<uint8_t>& out, const std::string& text)
{
    out.push_back(static_cast<uint8_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
}

bool readField(std::span<const uint8_t> bytes, size_t& offset, std::string& text)
{
    if (offset >= bytes.size())
    {
        return false;
    }
    const size_t length = bytes[offset++];
    if (bytes.size() - offset < length)
    {
        return false;
    }
    text.assign(reinterpret_cast<const char*>(bytes.data() + offset), length);
    offset += length;
    return true;
}

const ModRef* findMod(const std::vector<ModRef>& mods, const std::string& id)
{
    const auto it = std::ranges::find(mods, id, &ModRef::id);
    return it == mods.end() ? nullptr : &*it;
}

const std::string& shownName(const ModRef* local, const ModRef* host, const std::string& id)
{
    if (local != nullptr && !local->displayName.empty())
    {
        return local->displayName;
    }
    if (host != nullptr && !host->displayName.empty())
    {
        return host->displayName;
    }
    return id;
}

} // namespace

void PublishLocalMods(std::vector<ModRef> mods)
{
    auto             snapshot = std::make_shared<const std::vector<ModRef>>(std::move(mods));
    std::scoped_lock lock(localModsMutex);
    localMods = std::move(snapshot);
}

std::vector<ModRef> LocalMods()
{
    std::shared_ptr<const std::vector<ModRef>> snapshot;
    {
        std::scoped_lock lock(localModsMutex);
        snapshot = localMods;
    }
    return snapshot ? *snapshot : std::vector<ModRef>{};
}

std::vector<uint8_t> Encode(const HostSettings& settings)
{
    std::vector<ModRef> mods = settings.mods;
    std::ranges::stable_sort(mods, {}, &ModRef::id);
    const auto duplicates = std::ranges::unique(mods, {}, &ModRef::id);
    mods.erase(duplicates.begin(), duplicates.end());

    bool   truncated = settings.modsTruncated;
    size_t size      = kHeaderSize;
    size_t kept      = 0;
    for (const ModRef& mod : mods)
    {
        if (kept == kMaxModCount || !fitsFields(mod) || size + encodedSize(mod) > rex::session_data::kMaxBytes)
        {
            truncated = true;
            break;
        }
        size += encodedSize(mod);
        ++kept;
    }

    std::vector<uint8_t> out;
    out.reserve(size);
    out.push_back(kFormatVersion);
    out.push_back(static_cast<uint8_t>(clampAi(settings.aiCivilizations)));
    out.push_back(truncated ? kTruncatedFlag : uint8_t{ 0 });
    out.push_back(static_cast<uint8_t>(kept));
    for (size_t index = 0; index < kept; ++index)
    {
        appendField(out, mods[index].id);
        appendField(out, mods[index].version);
        appendField(out, mods[index].displayName);
    }
    return out;
}

std::optional<HostSettings> Decode(std::span<const uint8_t> bytes)
{
    if (bytes.size() < kHeaderSize || bytes[0] != kFormatVersion || (bytes[2] & ~kTruncatedFlag) != 0)
    {
        return std::nullopt;
    }

    HostSettings settings;
    settings.aiCivilizations = clampAi(bytes[1]);
    settings.modsTruncated   = (bytes[2] & kTruncatedFlag) != 0;

    const size_t count  = bytes[3];
    size_t       offset = kHeaderSize;
    for (size_t index = 0; index < count; ++index)
    {
        ModRef mod;
        if (!readField(bytes, offset, mod.id) || !readField(bytes, offset, mod.version) ||
            !readField(bytes, offset, mod.displayName))
        {
            return std::nullopt;
        }
        if (!settings.mods.empty() && settings.mods.back().id >= mod.id)
        {
            return std::nullopt;
        }
        settings.mods.push_back(std::move(mod));
    }
    if (offset != bytes.size())
    {
        return std::nullopt;
    }
    return settings;
}

std::string DescribeModMismatch(const std::vector<ModRef>& host, const std::vector<ModRef>& local, bool hostTruncated)
{
    std::vector<std::string> ids;
    for (const ModRef& mod : host)
    {
        ids.push_back(mod.id);
    }
    for (const ModRef& mod : local)
    {
        ids.push_back(mod.id);
    }
    std::ranges::sort(ids);
    const auto duplicates = std::ranges::unique(ids);
    ids.erase(duplicates.begin(), duplicates.end());

    std::vector<std::string> issues;
    for (const std::string& id : ids)
    {
        const ModRef* hostMod  = findMod(host, id);
        const ModRef* localMod = findMod(local, id);
        const char*   issue    = nullptr;
        if (localMod == nullptr)
        {
            issue = "host on, you off";
        }
        else if (hostMod == nullptr)
        {
            issue = hostTruncated ? nullptr : "host off, you on";
        }
        else if (hostMod->version != localMod->version)
        {
            issue = "different version";
        }
        if (issue != nullptr)
        {
            issues.push_back(shownName(localMod, hostMod, id) + ": " + issue);
        }
    }

    std::string text;
    for (size_t index = 0; index < issues.size() && index < kMaxListedIssues; ++index)
    {
        text += index == 0 ? "" : "; ";
        text += issues[index];
    }
    if (issues.size() > kMaxListedIssues)
    {
        text += "; and " + std::to_string(issues.size() - kMaxListedIssues) + " more";
    }
    if (hostTruncated)
    {
        text += text.empty() ? "The host's full mod list could not be checked"
                             : "; the host's full mod list could not be checked";
    }
    return text;
}

int32_t ResolveAiLimit(bool                             networkGame,
                       const rex::session_data::Status& status,
                       int32_t                          local,
                       std::string*                     source)
{
    const auto choose = [&](int32_t value, const char* from)
    {
        if (source != nullptr)
        {
            *source = from;
        }
        return value;
    };

    if (!networkGame)
    {
        return choose(local, "local");
    }

    using State = rex::session_data::State;
    if (status.state == State::kHost || status.state == State::kReceived)
    {
        if (const auto settings = Decode(status.bytes))
        {
            return choose(settings->aiCivilizations, status.state == State::kHost ? "host" : "received");
        }
    }
    return choose(local, "fallback");
}

} // namespace rerevved::host_settings
