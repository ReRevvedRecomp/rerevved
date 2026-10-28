#include "host_settings.h"

#include "ai_economy_settings.h"
#include "gotw_settings.h"

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <memory>
#include <mutex>

namespace rerevved::host_settings
{

namespace
{

// Gate built-in gameplay semantics as well as the serialized field layout.
constexpr uint8_t kFormatVersion    = 2;
constexpr uint8_t kTruncatedFlag    = 0x01;
constexpr uint8_t kCapitalGraceFlag = 0x20;
constexpr size_t  kHeaderSize       = 21;
constexpr size_t  kMaxFieldLength   = 255;
constexpr size_t  kMaxModCount      = 255;
constexpr size_t  kMaxListedIssues  = 4;

std::mutex                              localModsMutex;
std::shared_ptr<const LocalModSnapshot> localModSnapshot;
std::atomic<bool>                       localModSnapshotInvalid{ false };

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
    return !mod.id.empty() && mod.id.size() <= kMaxFieldLength && mod.version.size() <= kMaxFieldLength &&
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

HostSettings Normalize(HostSettings settings, bool networkGame)
{
    settings.aiCivilizations = std::clamp(settings.aiCivilizations, int32_t{ 0 }, networkGame ? kMaxAiCivilizations : kMaxLocalAiCivilizations);
    if (settings.resources != 0 && settings.resources != 1 && settings.resources != 3)
    {
        settings.resources = 0;
    }
    for (int32_t* bonus : { &settings.aiResearchBonusPercent, &settings.aiProductionBonusPercent, &settings.aiGoldBonusPercent, &settings.aiFoodBonusPercent, &settings.aiCultureBonusPercent })
    {
        *bonus = std::clamp(*bonus, 0, ai_economy::kMaxBonusPercent);
    }
    if (settings.dominationMinimumTurns != 5 && settings.dominationMinimumTurns != 10)
    {
        settings.dominationMinimumTurns = 10;
    }
    settings.combatSpeed = settings.combatSpeed == 1 ? 1 : 0;
    if (!settings.gotwCustomWeek || !gotw::IsValidWeek(settings.gotwWeek))
    {
        settings.gotwCustomWeek = false;
        settings.gotwWeek       = 0;
        settings.gotwSeed       = 0;
    }
    else
    {
        settings.gotwSeed = gotw::SeedForWeek(settings.gotwWeek);
    }
    // The civilization is local-only and has no wire field, so network games drop it.
    if (networkGame || !settings.gotwCustomCivilization || !gotw::IsValidCivilization(settings.gotwCivilization))
    {
        settings.gotwCustomCivilization = false;
        settings.gotwCivilization       = 0;
    }
    if (!settings.rulesDigestPresent)
    {
        settings.rulesDigest.fill(0);
    }
    return settings;
}

void PublishLocalModSnapshot(LocalModSnapshot snapshot)
{
    if (!snapshot.rulesDigestPresent)
    {
        snapshot.rulesDigest.fill(0);
    }
    auto             published = std::make_shared<const LocalModSnapshot>(std::move(snapshot));
    std::scoped_lock lock(localModsMutex);
    localModSnapshot = std::move(published);
    localModSnapshotInvalid.store(false);
}

void InvalidateLocalModSnapshot() noexcept
{
    localModSnapshotInvalid.store(true);
}

LocalModSnapshot LocalModSnapshotForSettings()
{
    std::shared_ptr<const LocalModSnapshot> snapshot;
    {
        std::scoped_lock lock(localModsMutex);
        snapshot = localModSnapshot;
    }
    auto result     = snapshot ? *snapshot : LocalModSnapshot{};
    result.complete = result.complete && !localModSnapshotInvalid.load();
    return result;
}

void PublishLocalMods(std::vector<ModRef> mods)
{
    LocalModSnapshot snapshot;
    snapshot.mods = std::move(mods);
    PublishLocalModSnapshot(std::move(snapshot));
}

std::vector<ModRef> LocalMods()
{
    return LocalModSnapshotForSettings().mods;
}

std::vector<uint8_t> Encode(const HostSettings& settings)
{
    const auto          normalized = Normalize(settings);
    std::vector<ModRef> mods       = settings.mods;
    std::ranges::stable_sort(mods, {}, &ModRef::id);
    const auto duplicates = std::ranges::unique(mods, {}, &ModRef::id);
    mods.erase(duplicates.begin(), duplicates.end());

    bool   truncated = settings.modsTruncated;
    size_t size      = kHeaderSize + (normalized.rulesDigestPresent ? normalized.rulesDigest.size() : 0);
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
    out.push_back(static_cast<uint8_t>(normalized.aiCivilizations));
    out.push_back(truncated ? kTruncatedFlag : uint8_t{ 0 });
    out.push_back(static_cast<uint8_t>(kept));
    out.push_back(static_cast<uint8_t>((normalized.ragingBarbarians ? 1 : 0) |
                                       (normalized.oneCityChallenge ? 2 : 0) |
                                       (normalized.gotwCustomWeek ? 4 : 0) |
                                       (normalized.gotwStandardRules ? 8 : 0) |
                                       (normalized.rulesDigestPresent ? 16 : 0) |
                                       (normalized.ragingBarbarians ? kCapitalGraceFlag : 0)));
    out.push_back(static_cast<uint8_t>(normalized.resources));
    out.push_back(static_cast<uint8_t>(normalized.aiResearchBonusPercent));
    out.push_back(static_cast<uint8_t>(normalized.aiProductionBonusPercent));
    out.push_back(static_cast<uint8_t>(normalized.aiGoldBonusPercent));
    out.push_back(static_cast<uint8_t>(normalized.aiFoodBonusPercent));
    out.push_back(static_cast<uint8_t>(normalized.aiCultureBonusPercent));
    out.push_back(static_cast<uint8_t>(normalized.dominationMinimumTurns));
    out.push_back(static_cast<uint8_t>(normalized.combatSpeed));
    for (uint32_t value : { normalized.gotwWeek, normalized.gotwSeed })
    {
        for (unsigned shift = 0; shift < 32; shift += 8)
        {
            out.push_back(static_cast<uint8_t>(value >> shift));
        }
    }
    if (normalized.rulesDigestPresent)
    {
        out.insert(out.end(), normalized.rulesDigest.begin(), normalized.rulesDigest.end());
    }
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
    if (bytes.size() < kHeaderSize || bytes.size() > rex::session_data::kMaxBytes || bytes[0] != kFormatVersion ||
        bytes[1] > kMaxAiCivilizations || (bytes[2] & ~kTruncatedFlag) != 0 || (bytes[4] & ~0x3Fu) != 0 ||
        ((bytes[4] & kCapitalGraceFlag) != 0) != ((bytes[4] & 1) != 0) ||
        (bytes[5] != 0 && bytes[5] != 1 && bytes[5] != 3) || bytes[6] > ai_economy::kMaxBonusPercent || bytes[7] > ai_economy::kMaxBonusPercent ||
        bytes[8] > ai_economy::kMaxBonusPercent || bytes[9] > ai_economy::kMaxBonusPercent || bytes[10] > ai_economy::kMaxBonusPercent ||
        (bytes[11] != 5 && bytes[11] != 10) || bytes[12] > 1)
    {
        return std::nullopt;
    }

    HostSettings settings;
    settings.aiCivilizations          = clampAi(bytes[1]);
    settings.modsTruncated            = (bytes[2] & kTruncatedFlag) != 0;
    settings.ragingBarbarians         = (bytes[4] & 1) != 0;
    settings.oneCityChallenge         = (bytes[4] & 2) != 0;
    settings.gotwCustomWeek           = (bytes[4] & 4) != 0;
    settings.gotwStandardRules        = (bytes[4] & 8) != 0;
    settings.rulesDigestPresent       = (bytes[4] & 16) != 0;
    settings.resources                = bytes[5];
    settings.aiResearchBonusPercent   = bytes[6];
    settings.aiProductionBonusPercent = bytes[7];
    settings.aiGoldBonusPercent       = bytes[8];
    settings.aiFoodBonusPercent       = bytes[9];
    settings.aiCultureBonusPercent    = bytes[10];
    settings.dominationMinimumTurns   = bytes[11];
    settings.combatSpeed              = bytes[12];
    for (unsigned index = 0; index < 4; ++index)
    {
        settings.gotwWeek |= uint32_t{ bytes[13 + index] } << (index * 8);
        settings.gotwSeed |= uint32_t{ bytes[17 + index] } << (index * 8);
    }
    if (settings.gotwCustomWeek ? (!gotw::IsValidWeek(settings.gotwWeek) || settings.gotwSeed != gotw::SeedForWeek(settings.gotwWeek))
                                : (settings.gotwWeek != 0 || settings.gotwSeed != 0))
    {
        return std::nullopt;
    }

    const size_t count  = bytes[3];
    size_t       offset = kHeaderSize;
    if (settings.rulesDigestPresent)
    {
        if (bytes.size() - offset < settings.rulesDigest.size())
        {
            return std::nullopt;
        }
        std::copy_n(bytes.begin() + offset, settings.rulesDigest.size(), settings.rulesDigest.begin());
        offset += settings.rulesDigest.size();
    }
    for (size_t index = 0; index < count; ++index)
    {
        ModRef mod;
        if (!readField(bytes, offset, mod.id) || !readField(bytes, offset, mod.version) ||
            !readField(bytes, offset, mod.displayName))
        {
            return std::nullopt;
        }
        if (mod.id.empty() || (!settings.mods.empty() && settings.mods.back().id >= mod.id))
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

GameSnapshot ResolveForGame(bool networkGame, const rex::session_data::Status& status, const HostSettings& local)
{
    if (!networkGame)
    {
        return { SnapshotState::Local, Normalize(local, false), {} };
    }
    using State = rex::session_data::State;
    if (status.state == State::kPending)
    {
        return { SnapshotState::Pending, {}, "Waiting for the host's game settings" };
    }
    if (status.state != State::kHost && status.state != State::kReceived)
    {
        return { SnapshotState::Missing, {}, "The host's game settings are unavailable" };
    }
    if (!status.game_ready)
    {
        return { SnapshotState::Pending, {}, "The host has not prepared this game's settings" };
    }
    const auto host = Decode(status.bytes);
    if (!host)
    {
        return { SnapshotState::Incompatible, {}, "The host's gameplay snapshot is missing, old or invalid" };
    }
    if (host->modsTruncated || local.modsTruncated || !DescribeModMismatch(host->mods, local.mods).empty())
    {
        return { SnapshotState::Incompatible, {}, "The host's complete mod loadout does not match" };
    }
    if ((!host->mods.empty() || !local.mods.empty() || host->rulesDigestPresent || local.rulesDigestPresent) &&
        (!host->rulesDigestPresent || !local.rulesDigestPresent || host->rulesDigest != local.rulesDigest))
    {
        return { SnapshotState::Incompatible, {}, "The host's rule content is different or unverified" };
    }
    return { status.state == State::kHost ? SnapshotState::Host : SnapshotState::Received, *host, {} };
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
        return choose(std::clamp(local, int32_t{ 0 }, kMaxLocalAiCivilizations), "local");
    }

    using State = rex::session_data::State;
    if (status.game_ready && (status.state == State::kHost || status.state == State::kReceived))
    {
        if (const auto settings = Decode(status.bytes))
        {
            return choose(settings->aiCivilizations, status.state == State::kHost ? "host" : "received");
        }
    }
    return choose(-1, "unavailable");
}

} // namespace rerevved::host_settings
