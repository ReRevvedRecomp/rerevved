#include "save_sidecar.h"

#include "custom_achievements.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdio>
#include <fstream>

#include <rex/crypto/sha256.h>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace rerevved::save_sidecar
{
namespace
{

constexpr std::array<uint8_t, 4> kMagic{ 'R', 'R', 'G', 'S' };
constexpr size_t                 kMaximumPayload = 4 + 1 + 1 + 42 + 4 + 64 + 18 + 3 + 32 * 32 * 3;

bool validIdentity(const Identity& identity)
{
    return !identity.slot.empty() && identity.slot.size() <= 42 &&
           std::all_of(identity.slot.begin(), identity.slot.end(), [](unsigned char c)
                       {
                           return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
                       }) &&
           identity.slot != "." && identity.slot != ".." && identity.digest.size() == 64 && std::all_of(identity.digest.begin(), identity.digest.end(), [](char c)
                                                                                                        {
                                                                                                            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                                                                                                        });
}

bool validState(const State& state)
{
    return state.occPopulation > 31 && state.occPopulation <= 127 && state.resources >= 0 && state.resources <= 3 &&
           (state.resources == 1 || state.resources == 3 || state.overrides.empty()) && ai_economy::IsValid(state.aiEconomy) &&
           state.barbarianDefeats >= 0 && state.barbarianDefeats <= 50 && (!state.occFromSetup || state.occEnabled) &&
           state.firstFoundingTurn >= -1 && state.firstFoundingTurn <= custom_achievements::kMaximumRecordedTurn &&
           state.initialAiOpponents >= -1 && state.initialAiOpponents <= custom_achievements::kMaximumAiOpponents;
}

} // namespace

std::vector<uint8_t> Serialize(const State& state, const Identity& identity)
{
    const auto records = resource_identity_overrides::Serialize(state.overrides);
    if (!validIdentity(identity) || !validState(state) || records.empty())
    {
        return {};
    }
    std::vector<uint8_t> bytes(kMagic.begin(), kMagic.end());
    bytes.push_back(kSchemaVersion);
    bytes.push_back(static_cast<uint8_t>(identity.slot.size()));
    bytes.insert(bytes.end(), identity.slot.begin(), identity.slot.end());
    for (int shift : { 24, 16, 8, 0 })
    {
        bytes.push_back(static_cast<uint8_t>(identity.device >> shift));
    }
    bytes.insert(bytes.end(), identity.digest.begin(), identity.digest.end());
    bytes.push_back(state.ragingBarbarians);
    bytes.push_back(state.occEnabled);
    bytes.push_back(static_cast<uint8_t>(state.occPopulation));
    bytes.push_back(static_cast<uint8_t>(state.resources));
    for (int32_t bonus : { state.aiEconomy.researchBonusPercent, state.aiEconomy.productionBonusPercent, state.aiEconomy.goldBonusPercent, state.aiEconomy.foodBonusPercent, state.aiEconomy.cultureBonusPercent })
    {
        bytes.push_back(static_cast<uint8_t>(bonus));
    }
    bytes.push_back(state.occFromSetup);
    bytes.push_back(static_cast<uint8_t>(state.barbarianDefeats));
    bytes.push_back(state.historyKnown);
    bytes.push_back(state.unitBuilt);
    bytes.push_back(state.battleFought);
    bytes.push_back(state.deepOceanEntered);
    // Founding turn is a big-endian halfword and the AI count a byte, 0xFFFF and 0xFF for -1.
    const auto turn = static_cast<uint16_t>(state.firstFoundingTurn);
    bytes.push_back(static_cast<uint8_t>(turn >> 8));
    bytes.push_back(static_cast<uint8_t>(turn));
    bytes.push_back(static_cast<uint8_t>(state.initialAiOpponents));
    bytes.insert(bytes.end(), records.begin(), records.end());
    return bytes;
}

std::optional<State> Deserialize(std::span<const uint8_t> bytes, const Identity& identity)
{
    if (!validIdentity(identity) || bytes.size() > kMaximumPayload || bytes.size() < 6 ||
        !std::equal(kMagic.begin(), kMagic.end(), bytes.begin()) || (bytes[4] < 1 || bytes[4] > kSchemaVersion))
    {
        return std::nullopt;
    }
    const size_t slotSize = bytes[5];
    // Schema 1 has only the four setting bytes. Schema 2 adds five AI bonuses, the
    // occ-from-setup flag, Barbarian defeat count, whole-game history flags, first
    // founding turn and initial AI count.
    const size_t stateSize = bytes[4] == 1 ? 4 : 18;
    if (slotSize != identity.slot.size() || bytes.size() < 6 + slotSize + 4 + 64 + stateSize + 3 ||
        !std::equal(identity.slot.begin(), identity.slot.end(), bytes.begin() + 6))
    {
        return std::nullopt;
    }
    size_t   offset = 6 + slotSize;
    uint32_t device = 0;
    for (size_t i = 0; i < 4; ++i)
    {
        device = (device << 8) | bytes[offset++];
    }
    if (device != identity.device || !std::equal(identity.digest.begin(), identity.digest.end(), bytes.begin() + offset))
    {
        return std::nullopt;
    }
    offset += 64;
    if (bytes[offset] > 1 || bytes[offset + 1] > 1)
    {
        return std::nullopt;
    }
    State state{ bytes[offset] != 0, bytes[offset + 1] != 0, bytes[offset + 2], bytes[offset + 3], {} };
    if (bytes[4] >= 2)
    {
        const uint16_t turn = static_cast<uint16_t>(uint16_t{ bytes[offset + 15] } << 8 | bytes[offset + 16]);
        if (bytes[offset + 9] > 1 || bytes[offset + 10] > 50 || bytes[offset + 11] > 1 || bytes[offset + 12] > 1 || bytes[offset + 13] > 1 ||
            bytes[offset + 14] > 1)
        {
            return std::nullopt;
        }
        state.aiEconomy          = { bytes[offset + 4], bytes[offset + 5], bytes[offset + 6], bytes[offset + 7], bytes[offset + 8] };
        state.occFromSetup       = bytes[offset + 9] != 0;
        state.barbarianDefeats   = bytes[offset + 10];
        state.historyKnown       = bytes[offset + 11] != 0;
        state.unitBuilt          = bytes[offset + 12] != 0;
        state.battleFought       = bytes[offset + 13] != 0;
        state.deepOceanEntered   = bytes[offset + 14] != 0;
        state.firstFoundingTurn  = turn == 0xFFFF ? -1 : turn;
        state.initialAiOpponents = bytes[offset + 17] == 0xFF ? -1 : bytes[offset + 17];
    }
    const auto records = resource_identity_overrides::Deserialize(bytes.subspan(offset + stateSize));
    if (!records)
    {
        return std::nullopt;
    }
    state.overrides = *records;
    return validState(state) ? std::optional<State>(std::move(state)) : std::nullopt;
}

std::filesystem::path SidecarPath(const std::filesystem::path& nativeFile)
{
    auto path = nativeFile;
    path += ".rerevved-gameplay";
    return path;
}

std::optional<State> Read(const std::filesystem::path& nativeFile, std::string_view slot, uint32_t device, ReadFailure* failure)
{
    const auto fail = [failure](ReadFailure reason) -> std::optional<State>
    {
        if (failure)
        {
            *failure = reason;
        }
        return std::nullopt;
    };
    const auto    path = SidecarPath(nativeFile);
    std::ifstream file(path, std::ios::binary);
    if (!file)
    {
        std::error_code error;
        const bool      exists = std::filesystem::exists(path, error);
        return fail(!error && !exists ? ReadFailure::Missing : ReadFailure::Unreadable);
    }
    std::vector<uint8_t> bytes;
    for (char byte; bytes.size() <= kMaximumPayload && file.get(byte);)
    {
        bytes.push_back(static_cast<uint8_t>(byte));
    }
    if (file.bad() || (!file.eof() && bytes.size() <= kMaximumPayload))
    {
        return fail(ReadFailure::Unreadable);
    }
    const size_t digestOffset = 6 + slot.size() + 4;
    if (slot.size() > 42 || bytes.size() < digestOffset + 64)
    {
        return fail(ReadFailure::Rejected);
    }
    // Validate the payload against its stored digest before hashing native bytes.
    const Identity identity{ std::string(slot), device, std::string(bytes.begin() + digestOffset, bytes.begin() + digestOffset + 64) };
    auto           state = Deserialize(bytes, identity);
    if (!state)
    {
        return fail(ReadFailure::Rejected);
    }
    std::string digest;
    try
    {
        digest = rex::crypto::sha256_file(nativeFile);
    }
    catch (...)
    {
        return fail(ReadFailure::DigestUnreadable);
    }
    if (digest.empty())
    {
        return fail(ReadFailure::DigestUnreadable);
    }
    if (digest != identity.digest)
    {
        return fail(ReadFailure::DigestMismatch);
    }
    if (failure)
    {
        *failure = ReadFailure::None;
    }
    return state;
}

bool Save(bool nativeSuccess, const std::filesystem::path& nativeFile, std::string_view slot, uint32_t device, const State& state)
{
    if (!nativeSuccess)
    {
        return false;
    }
    const auto bytes = Serialize(state, { std::string(slot), device, rex::crypto::sha256_file(nativeFile) });
    if (bytes.empty())
    {
        return false;
    }
    const auto                   path = SidecarPath(nativeFile);
    static std::atomic<uint64_t> counter{ 0 };
#ifdef _WIN32
    const auto process = GetCurrentProcessId();
#else
    const auto process = getpid();
#endif
    std::filesystem::path temporary;
    // Exclusive creation never truncates another operation's temporary file.
    // The temporary and destination share a directory for atomic replacement.
    FILE* file = nullptr;
    for (;;)
    {
        temporary = path;
        temporary += ".tmp." + std::to_string(process) + "." + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));
#ifdef _WIN32
        const int error = _wfopen_s(&file, temporary.c_str(), L"wbx");
#else
        file            = std::fopen(temporary.c_str(), "wbx");
        const int error = file ? 0 : errno;
#endif
        if (file)
        {
            break;
        }
        if (error != EEXIST)
        {
            return false;
        }
    }
    bool success = std::fwrite(bytes.data(), 1, bytes.size(), file) == bytes.size();
    success      = std::fflush(file) == 0 && success;
    success      = std::fclose(file) == 0 && success;
    if (success)
    {
#ifdef _WIN32
        success = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
#else
        success = std::rename(temporary.c_str(), path.c_str()) == 0;
#endif
    }
    if (!success)
    {
        std::error_code error;
        std::filesystem::remove(temporary, error);
    }
    return success;
}

} // namespace rerevved::save_sidecar
