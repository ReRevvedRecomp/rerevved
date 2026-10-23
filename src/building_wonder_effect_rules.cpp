#include "building_effect_rules_registry.h"
#include "wonder_effect_rules_registry.h"

#include <building_effect_rules.h>
#include <wonder_effect_rules.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <limits>
#include <mutex>
#include <shared_mutex>
#include <vector>

namespace
{

template <size_t Size>
bool isZeroed(const int32_t (&values)[Size])
{
    return std::all_of(std::begin(values), std::end(values), [](int32_t value)
                       {
                           return value == 0;
                       });
}

template <size_t Size>
bool isRuleIdValid(const char (&value)[Size])
{
    if (!std::memchr(value, '\0', Size) || value[0] == '\0')
    {
        return false;
    }
    const auto isAlphanumeric = [](char c)
    {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
    };
    if (!isAlphanumeric(value[0]))
    {
        return false;
    }
    for (const char* current = value; *current; ++current)
    {
        if (!isAlphanumeric(*current) && *current != '.' && *current != '_' && *current != '-')
        {
            return false;
        }
    }
    return true;
}

bool isValid(const BuildingEffectRule* rule)
{
    return rule && rule->structSize >= sizeof(*rule) &&
           isRuleIdValid(rule->providerId) && isRuleIdValid(rule->ruleId) &&
           rule->effect == BUILDING_EFFECT_CHINESE_LIBRARY_FIXED_GOLD && isZeroed(rule->reserved);
}

bool isWonderEffect(WonderEffectId effect)
{
    return effect == WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE || effect == WONDER_EFFECT_STONEHENGE_RELIGION;
}

bool isValid(const WonderEffectRule* rule)
{
    return rule && rule->structSize >= sizeof(*rule) &&
           isRuleIdValid(rule->providerId) && isRuleIdValid(rule->ruleId) &&
           isWonderEffect(rule->effect) && isZeroed(rule->reserved);
}

bool isValid(const BuildingEffectRuleV2* rule)
{
    return rule && rule->structSize >= sizeof(*rule) &&
           isRuleIdValid(rule->providerId) && isRuleIdValid(rule->ruleId) &&
           rule->effect == BUILDING_EFFECT_CHINESE_LIBRARY_FIXED_GOLD && isZeroed(rule->reserved);
}

// The Religion grant has no amount; the Culture replacement accepts any amount.
bool isValid(const WonderEffectRuleV2* rule)
{
    return rule && rule->structSize >= sizeof(*rule) &&
           isRuleIdValid(rule->providerId) && isRuleIdValid(rule->ruleId) &&
           isWonderEffect(rule->effect) && (rule->effect != WONDER_EFFECT_STONEHENGE_RELIGION || rule->amount == 0) &&
           isZeroed(rule->reserved);
}

bool isYieldValid(int32_t value)
{
    return value >= std::numeric_limits<int16_t>::min() && value <= std::numeric_limits<int16_t>::max();
}

bool tryAddFixedYield(int32_t value, int32_t addition, int32_t& result)
{
    const int64_t sum = static_cast<int64_t>(value) + addition;
    if (sum < std::numeric_limits<int16_t>::min() || sum > std::numeric_limits<int16_t>::max())
    {
        return false;
    }
    result = static_cast<int32_t>(sum);
    return true;
}

template <typename Record>
int32_t prepareOutput(Record* out, uint32_t outSize)
{
    if (!out)
    {
        return -10;
    }
    std::memset(out, 0, std::min<size_t>(outSize, sizeof(Record)));
    return outSize < sizeof(Record) ? -11 : 0;
}

template <typename Record>
void copyOutput(Record* out, const Record& producer)
{
    std::memcpy(out, &producer, sizeof(producer));
}

template <size_t Size>
void normalizeRuleId(char (&value)[Size])
{
    const size_t length = std::strlen(value);
    std::memset(value + length + 1, 0, Size - length - 1);
}

template <typename Rule>
bool sameIdentity(const Rule& left, const Rule& right)
{
    return std::strcmp(left.providerId, right.providerId) == 0 &&
           std::strcmp(left.ruleId, right.ruleId) == 0;
}

template <typename Rule>
bool identityLess(const Rule& left, const Rule& right)
{
    const int providerOrder = std::strcmp(left.providerId, right.providerId);
    return providerOrder < 0 ||
           (providerOrder == 0 && std::strcmp(left.ruleId, right.ruleId) < 0);
}

} // namespace

namespace rerevved::building_effect_rules
{

namespace
{

std::shared_mutex                 registryMutex;
std::vector<BuildingEffectRuleV2> registry;

int32_t normalizeV2(const BuildingEffectRuleV2& rule, BuildingEffectRuleV2& normalized) noexcept
{
    normalized            = rule;
    normalized.structSize = sizeof(normalized);
    normalizeRuleId(normalized.providerId);
    normalizeRuleId(normalized.ruleId);
    return BUILDING_EFFECT_RULES_OK;
}

bool fitsInt32(int64_t value)
{
    return value >= std::numeric_limits<int32_t>::min() && value <= std::numeric_limits<int32_t>::max();
}

bool fitsInt16(int64_t value)
{
    return value >= std::numeric_limits<int16_t>::min() && value <= std::numeric_limits<int16_t>::max();
}

// Keys strictly decrease from the first entry to the last: the guest stack grows down.
struct PendingToken
{
    uint32_t stackPointer;
    uint32_t cityRecord;
    int32_t  ownerIndex;
    int32_t  amount;
};

struct PendingTokens
{
    PendingToken entries[kPendingTokenCapacity];
    size_t       count = 0;
};

thread_local PendingTokens pendingTokens;

// Drops tokens whose key is below stackPointer, or at it when inclusive.
void discardTokens(uint32_t stackPointer, bool inclusive)
{
    while (pendingTokens.count != 0)
    {
        const uint32_t key = pendingTokens.entries[pendingTokens.count - 1].stackPointer;
        if (key > stackPointer || (key == stackPointer && !inclusive))
        {
            break;
        }
        --pendingTokens.count;
    }
}

bool pushToken(const PendingToken& token)
{
    if (pendingTokens.count == kPendingTokenCapacity)
    {
        return false;
    }
    pendingTokens.entries[pendingTokens.count++] = token;
    return true;
}

// Both phases gate on identical live state.
bool isEligible(int32_t ownerIndex, int32_t cityOwner, int32_t civilization, bool hasLibrary, bool excludedPath)
{
    return !excludedPath && ownerIndex >= 0 && ownerIndex < kPlayerCount &&
           cityOwner == ownerIndex && civilization == CIVILIZATION_CHINESE && hasLibrary;
}

} // namespace

int32_t NormalizeV1Request(const BuildingEffectRule* rule, BuildingEffectRuleV2& normalized) noexcept
{
    if (!isValid(rule))
    {
        return BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }
    BuildingEffectRuleV2 widened{};
    widened.structSize = sizeof(widened);
    std::memcpy(widened.providerId, rule->providerId, sizeof(widened.providerId));
    std::memcpy(widened.ruleId, rule->ruleId, sizeof(widened.ruleId));
    widened.effect = rule->effect;
    widened.amount = 1;
    return normalizeV2(widened, normalized);
}

int32_t NormalizeRequest(const BuildingEffectRuleV2* rule, BuildingEffectRuleV2& normalized) noexcept
{
    return isValid(rule) ? normalizeV2(*rule, normalized) : BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT;
}

std::unique_lock<std::shared_mutex> AcquirePublicationLock()
{
    return std::unique_lock(registryMutex);
}

int32_t BuildCandidate(std::span<const BuildingEffectRuleV2> requests,
                       std::vector<BuildingEffectRuleV2>&    candidate,
                       size_t&                               failingOffset,
                       void (*checkpoint)())
{
    failingOffset = 0;
    if (checkpoint)
    {
        checkpoint();
    }
    candidate   = registry;
    int64_t sum = 0;
    for (const auto& rule : candidate)
    {
        sum += rule.amount;
    }
    for (size_t i = 0; i < requests.size(); ++i)
    {
        failingOffset          = i;
        const auto& normalized = requests[i];
        const auto  existing   = std::find_if(candidate.begin(), candidate.end(), [&](const auto& rule)
                                              {
                                               return sameIdentity(rule, normalized);
                                              });
        if (existing != candidate.end())
        {
            if (std::memcmp(&*existing, &normalized, sizeof(normalized)) != 0)
            {
                return BUILDING_EFFECT_RULES_ERR_CONFLICT;
            }
            continue;
        }
        sum += normalized.amount;
        if (!fitsInt32(sum))
        {
            return BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT;
        }
        if (checkpoint)
        {
            checkpoint();
        }
        candidate.push_back(normalized);
    }
    if (checkpoint)
    {
        checkpoint();
    }
    std::sort(candidate.begin(), candidate.end(), identityLess<BuildingEffectRuleV2>);
    return BUILDING_EFFECT_RULES_OK;
}

void CommitCandidate(std::vector<BuildingEffectRuleV2>& candidate) noexcept
{
    registry.swap(candidate);
}

bool TryGetEffectiveAmount(int32_t& amount)
{
    std::shared_lock lock(registryMutex);
    if (registry.empty())
    {
        return false;
    }
    int64_t sum = 0;
    for (const auto& rule : registry)
    {
        sum += rule.amount;
    }
    if (!fitsInt32(sum))
    {
        return false;
    }
    amount = static_cast<int32_t>(sum);
    return true;
}

size_t RuleCount()
{
    std::shared_lock lock(registryMutex);
    return registry.size();
}

void ResetForTests()
{
    {
        std::unique_lock lock(registryMutex);
        registry.clear();
    }
    pendingTokens.count = 0;
}

CacheDecision EvaluateCachePhase(const CacheQuery& query)
{
    discardTokens(query.stackPointer, true);

    const bool creditsReserve = query.mode == kModeCredit;
    int32_t    amount         = 0;
    if ((query.mode != kModeCacheOnly && !creditsReserve) ||
        !isEligible(query.ownerIndex, query.cityOwner, query.civilization, query.hasLibrary, query.excludedPath) ||
        !TryGetEffectiveAmount(amount) || amount == 0)
    {
        return {};
    }

    CacheDecision decision;
    decision.status     = HookStatus::kOverflow;
    const int64_t cache = static_cast<int64_t>(query.nativeCache) + amount;
    if (!fitsInt16(cache))
    {
        return decision;
    }
    if (creditsReserve)
    {
        const int64_t aggregate = static_cast<int64_t>(query.ownerAggregate) + amount;
        // The credit hook sees the native sum in a 32-bit register, so it must not wrap either.
        const int64_t nativeReserve = static_cast<int64_t>(query.ownerReserve) + query.nativeCredit;
        const int64_t reserve       = nativeReserve + amount;
        if (!fitsInt32(aggregate) || !fitsInt32(nativeReserve) || !fitsInt32(reserve) ||
            !pushToken({ query.stackPointer, query.cityRecord, query.ownerIndex, amount }))
        {
            return decision;
        }
        decision.applyAggregate = true;
        decision.newAggregate   = static_cast<int32_t>(aggregate);
    }
    decision.status     = HookStatus::kMatched;
    decision.applyCache = true;
    decision.newCache   = static_cast<int16_t>(cache);
    return decision;
}

CreditDecision DecideCredit(int32_t reserveWithCredit, int32_t amount)
{
    CreditDecision decision;
    const int64_t  reserve = static_cast<int64_t>(reserveWithCredit) + amount;
    if (!fitsInt32(reserve))
    {
        decision.status = HookStatus::kOverflow;
        return decision;
    }
    decision.status         = HookStatus::kMatched;
    decision.apply          = true;
    decision.newReserveGold = static_cast<int32_t>(reserve);
    return decision;
}

bool TakePendingCredit(uint32_t stackPointer, uint32_t cityRecord, int32_t ownerIndex, int32_t& amount)
{
    discardTokens(stackPointer, false);
    if (pendingTokens.count == 0 || pendingTokens.entries[pendingTokens.count - 1].stackPointer != stackPointer)
    {
        return false;
    }
    const PendingToken token = pendingTokens.entries[--pendingTokens.count];
    if (token.cityRecord != cityRecord || token.ownerIndex != ownerIndex)
    {
        return false;
    }
    amount = token.amount;
    return true;
}

CreditDecision EvaluateCreditPhase(const CreditQuery& query)
{
    int32_t amount = 0;
    if (!TakePendingCredit(query.stackPointer, query.cityRecord, query.ownerIndex, amount) ||
        !isEligible(query.ownerIndex, query.cityOwner, query.civilization, query.hasLibrary, query.excludedPath))
    {
        return {};
    }
    return DecideCredit(query.reserveWithCredit, amount);
}

} // namespace rerevved::building_effect_rules

namespace rerevved::wonder_effect_rules
{

namespace
{

std::shared_mutex               registryMutex;
std::vector<WonderEffectRuleV2> registry;
// Mirrors !registry.empty(); written only while the publication lock is held.
std::atomic<bool> religionActive{ false };

int32_t normalizeV2(const WonderEffectRuleV2& rule, WonderEffectRuleV2& normalized) noexcept
{
    if (rule.effect == WONDER_EFFECT_STONEHENGE_RELIGION_CULTURE)
    {
        return WONDER_EFFECT_RULES_ERR_UNAVAILABLE;
    }
    normalized            = rule;
    normalized.structSize = sizeof(normalized);
    normalizeRuleId(normalized.providerId);
    normalizeRuleId(normalized.ruleId);
    return WONDER_EFFECT_RULES_OK;
}

} // namespace

int32_t NormalizeV1Request(const WonderEffectRule* rule, WonderEffectRuleV2& normalized) noexcept
{
    if (!isValid(rule))
    {
        return WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }
    WonderEffectRuleV2 widened{};
    widened.structSize = sizeof(widened);
    std::memcpy(widened.providerId, rule->providerId, sizeof(widened.providerId));
    std::memcpy(widened.ruleId, rule->ruleId, sizeof(widened.ruleId));
    widened.effect = rule->effect;
    widened.amount = 0;
    return normalizeV2(widened, normalized);
}

int32_t NormalizeRequest(const WonderEffectRuleV2* rule, WonderEffectRuleV2& normalized) noexcept
{
    return isValid(rule) ? normalizeV2(*rule, normalized) : WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT;
}

std::unique_lock<std::shared_mutex> AcquirePublicationLock()
{
    return std::unique_lock(registryMutex);
}

int32_t BuildCandidate(std::span<const WonderEffectRuleV2> requests,
                       std::vector<WonderEffectRuleV2>&    candidate,
                       size_t&                             failingOffset,
                       void (*checkpoint)())
{
    failingOffset = 0;
    if (checkpoint)
    {
        checkpoint();
    }
    candidate = registry;
    for (size_t i = 0; i < requests.size(); ++i)
    {
        failingOffset          = i;
        const auto& normalized = requests[i];
        const auto  existing   = std::find_if(candidate.begin(), candidate.end(), [&](const auto& rule)
                                              {
                                               return sameIdentity(rule, normalized);
                                              });
        if (existing != candidate.end())
        {
            if (std::memcmp(&*existing, &normalized, sizeof(normalized)) != 0)
            {
                return WONDER_EFFECT_RULES_ERR_CONFLICT;
            }
            continue;
        }
        if (checkpoint)
        {
            checkpoint();
        }
        candidate.push_back(normalized);
    }
    if (checkpoint)
    {
        checkpoint();
    }
    std::sort(candidate.begin(), candidate.end(), identityLess<WonderEffectRuleV2>);
    return WONDER_EFFECT_RULES_OK;
}

void CommitCandidate(std::vector<WonderEffectRuleV2>& candidate) noexcept
{
    registry.swap(candidate);
    religionActive.store(!registry.empty(), std::memory_order_release);
}

bool IsStonehengeReligionActive() noexcept
{
    return religionActive.load(std::memory_order_acquire);
}

size_t RuleCount()
{
    std::shared_lock lock(registryMutex);
    return registry.size();
}

void ResetForTests()
{
    std::unique_lock lock(registryMutex);
    registry.clear();
    religionActive.store(false, std::memory_order_release);
}

bool ShouldGrantReligion(const GrantQuery& query) noexcept
{
    return query.ruleActive && query.readsOk && query.wonderIndex == kStonehengeWonderIndex &&
           query.cityIndex >= 0 && query.cityIndex < kCitySlots && query.claimCity == query.cityIndex &&
           query.claimOwner == query.liveOwner && query.claimOwner >= 0 && query.claimOwner < query.playerCount &&
           query.claimOwner < kOwnerLimit && query.claimOwner != query.excludedPlayer;
}

} // namespace rerevved::wonder_effect_rules

namespace
{

int32_t registerBuilding(const BuildingEffectRuleV2& normalized)
{
    using namespace rerevved::building_effect_rules;
    try
    {
        auto                              lock = AcquirePublicationLock();
        std::vector<BuildingEffectRuleV2> candidate;
        size_t                            failingOffset = 0;
        const int32_t                     status        = BuildCandidate(std::span(&normalized, 1), candidate, failingOffset);
        if (status != BUILDING_EFFECT_RULES_OK)
        {
            return status;
        }
        CommitCandidate(candidate);
    }
    catch (...)
    {
        return BUILDING_EFFECT_RULES_ERR_INTERNAL;
    }
    return BUILDING_EFFECT_RULES_OK;
}

int32_t registerWonder(const WonderEffectRuleV2& normalized)
{
    using namespace rerevved::wonder_effect_rules;
    try
    {
        auto                            lock = AcquirePublicationLock();
        std::vector<WonderEffectRuleV2> candidate;
        size_t                          failingOffset = 0;
        const int32_t                   status        = BuildCandidate(std::span(&normalized, 1), candidate, failingOffset);
        if (status != WONDER_EFFECT_RULES_OK)
        {
            return status;
        }
        CommitCandidate(candidate);
    }
    catch (...)
    {
        return WONDER_EFFECT_RULES_ERR_INTERNAL;
    }
    return WONDER_EFFECT_RULES_OK;
}

} // namespace

static_assert(sizeof(BuildingEffectRule) == 160);
static_assert(sizeof(BuildingEffectSupport) == 32);
static_assert(sizeof(BuildingEffectPreviewQuery) == 32);
static_assert(sizeof(BuildingEffectPreview) == 32);
static_assert(sizeof(WonderEffectRule) == 160);
static_assert(sizeof(WonderEffectSupport) == 32);
static_assert(sizeof(WonderEffectPreviewQuery) == 32);
static_assert(sizeof(WonderEffectPreview) == 32);
static_assert(sizeof(BuildingEffectRuleV2) == 160);
static_assert(sizeof(WonderEffectRuleV2) == 160);
static_assert(sizeof(BuildingEffectSupportV2) == 32);
static_assert(sizeof(WonderEffectSupportV2) == 32);

extern "C" uint32_t BuildingEffectRulesAbiVersion(void)
{
    return BUILDING_EFFECT_RULES_ABI_VERSION;
}

extern "C" uint32_t BuildingEffectRulesV2AbiVersion(void)
{
    return BUILDING_EFFECT_RULES_V2_ABI_VERSION;
}

extern "C" int32_t GetBuildingEffectSupportV2(BuildingEffectSupportV2* out, uint32_t outSize)
{
    const int32_t status = prepareOutput(out, outSize);
    if (status != BUILDING_EFFECT_RULES_OK)
    {
        return status;
    }
    const BuildingEffectSupportV2 result{ sizeof(BuildingEffectSupportV2), 1, INT32_MIN, INT32_MAX, {} };
    copyOutput(out, result);
    return BUILDING_EFFECT_RULES_OK;
}

extern "C" int32_t RegisterBuildingEffectRuleV2(const BuildingEffectRuleV2* rule)
{
    BuildingEffectRuleV2 normalized{};
    const int32_t        status = rerevved::building_effect_rules::NormalizeRequest(rule, normalized);
    return status == BUILDING_EFFECT_RULES_OK ? registerBuilding(normalized) : status;
}

extern "C" uint32_t WonderEffectRulesAbiVersion(void)
{
    return WONDER_EFFECT_RULES_ABI_VERSION;
}

extern "C" uint32_t WonderEffectRulesV2AbiVersion(void)
{
    return WONDER_EFFECT_RULES_V2_ABI_VERSION;
}

extern "C" int32_t GetWonderEffectSupportV2(WonderEffectSupportV2* out, uint32_t outSize)
{
    const int32_t status = prepareOutput(out, outSize);
    if (status != WONDER_EFFECT_RULES_OK)
    {
        return status;
    }
    const WonderEffectSupportV2 result{ sizeof(WonderEffectSupportV2), 1, 0, 0, {} };
    copyOutput(out, result);
    return WONDER_EFFECT_RULES_OK;
}

extern "C" int32_t RegisterWonderEffectRuleV2(const WonderEffectRuleV2* rule)
{
    WonderEffectRuleV2 normalized{};
    const int32_t      status = rerevved::wonder_effect_rules::NormalizeRequest(rule, normalized);
    return status == WONDER_EFFECT_RULES_OK ? registerWonder(normalized) : status;
}

extern "C" int32_t GetBuildingEffectSupport(BuildingEffectSupport* out, uint32_t outSize)
{
    const int32_t status = prepareOutput(out, outSize);
    if (status != BUILDING_EFFECT_RULES_OK)
    {
        return status;
    }
    const BuildingEffectSupport result{ sizeof(BuildingEffectSupport), 1, {} };
    copyOutput(out, result);
    return BUILDING_EFFECT_RULES_OK;
}

extern "C" int32_t GetWonderEffectSupport(WonderEffectSupport* out, uint32_t outSize)
{
    const int32_t status = prepareOutput(out, outSize);
    if (status != WONDER_EFFECT_RULES_OK)
    {
        return status;
    }
    const WonderEffectSupport result{ sizeof(WonderEffectSupport), 1, {} };
    copyOutput(out, result);
    return WONDER_EFFECT_RULES_OK;
}

extern "C" int32_t RegisterBuildingEffectRule(const BuildingEffectRule* rule)
{
    BuildingEffectRuleV2 normalized{};
    const int32_t        status = rerevved::building_effect_rules::NormalizeV1Request(rule, normalized);
    return status == BUILDING_EFFECT_RULES_OK ? registerBuilding(normalized) : status;
}

extern "C" int32_t RegisterWonderEffectRule(const WonderEffectRule* rule)
{
    WonderEffectRuleV2 normalized{};
    const int32_t      status = rerevved::wonder_effect_rules::NormalizeV1Request(rule, normalized);
    return status == WONDER_EFFECT_RULES_OK ? registerWonder(normalized) : status;
}

namespace
{

template <typename Rule>
int32_t previewBuilding(const Rule* rule, const BuildingEffectPreviewQuery* query, BuildingEffectPreview* out, uint32_t outSize)
{
    const int32_t status = prepareOutput(out, outSize);
    if (status != BUILDING_EFFECT_RULES_OK)
    {
        return status;
    }
    if (!isValid(rule) || !query || query->structSize < sizeof(*query) ||
        query->civilization < CIVILIZATION_ROMAN || query->civilization >= CIVILIZATION_COUNT ||
        query->hasLibrary > 1 || !isYieldValid(query->nativeGold) || !isYieldValid(query->nativeScience) || !isZeroed(query->reserved))
    {
        return BUILDING_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }

    BuildingEffectPreview result{ sizeof(BuildingEffectPreview), query->nativeGold, query->nativeGold, query->nativeScience, query->nativeScience, 0, {} };
    if (query->civilization == CIVILIZATION_CHINESE && query->hasLibrary)
    {
        int32_t amount = 1;
        if constexpr (requires { rule->amount; })
        {
            amount = rule->amount;
        }
        result.statusFlags = BUILDING_EFFECT_PREVIEW_MATCHED;
        if (!tryAddFixedYield(query->nativeGold, amount, result.finalGold))
        {
            result.statusFlags |= BUILDING_EFFECT_PREVIEW_OVERFLOW;
        }
    }
    copyOutput(out, result);
    return BUILDING_EFFECT_RULES_OK;
}

template <typename Rule>
int32_t previewWonder(const Rule* rule, const WonderEffectPreviewQuery* query, WonderEffectPreview* out, uint32_t outSize)
{
    const int32_t status = prepareOutput(out, outSize);
    if (status != WONDER_EFFECT_RULES_OK)
    {
        return status;
    }
    if (!isValid(rule) || !query || query->structSize < sizeof(*query) ||
        !isYieldValid(query->cultureBeforeFixedAddition) || query->hasStonehenge > 1 ||
        query->isNewCompletion > 1 || query->religionOwned > 1 ||
        (query->isNewCompletion && !query->hasStonehenge) || !isZeroed(query->reserved))
    {
        return WONDER_EFFECT_RULES_ERR_INVALID_ARGUMENT;
    }

    WonderEffectPreview result{ sizeof(WonderEffectPreview), query->cultureBeforeFixedAddition, query->cultureBeforeFixedAddition, 0, 0, 0, {} };
    if (query->hasStonehenge && rule->effect == WONDER_EFFECT_STONEHENGE_RELIGION)
    {
        // Native effects, including Culture, are unchanged.
        result.statusFlags   = WONDER_EFFECT_PREVIEW_MATCHED;
        result.grantReligion = query->isNewCompletion && !query->religionOwned;
    }
    else if (query->hasStonehenge)
    {
        int32_t amount = 5;
        if constexpr (requires { rule->amount; })
        {
            amount = rule->amount;
        }
        result.statusFlags = WONDER_EFFECT_PREVIEW_MATCHED;
        if (!tryAddFixedYield(query->cultureBeforeFixedAddition, amount, result.finalCulture))
        {
            result.statusFlags |= WONDER_EFFECT_PREVIEW_OVERFLOW;
        }
        else
        {
            result.replaceNativeEffects = 1;
            result.grantReligion        = query->isNewCompletion && !query->religionOwned;
        }
    }
    copyOutput(out, result);
    return WONDER_EFFECT_RULES_OK;
}

} // namespace

extern "C" int32_t PreviewBuildingEffect(const BuildingEffectRule* rule, const BuildingEffectPreviewQuery* query, BuildingEffectPreview* out, uint32_t outSize)
{
    return previewBuilding(rule, query, out, outSize);
}

extern "C" int32_t PreviewWonderEffect(const WonderEffectRule* rule, const WonderEffectPreviewQuery* query, WonderEffectPreview* out, uint32_t outSize)
{
    return previewWonder(rule, query, out, outSize);
}

extern "C" int32_t PreviewBuildingEffectV2(const BuildingEffectRuleV2* rule, const BuildingEffectPreviewQuery* query, BuildingEffectPreview* out, uint32_t outSize)
{
    return previewBuilding(rule, query, out, outSize);
}

extern "C" int32_t PreviewWonderEffectV2(const WonderEffectRuleV2* rule, const WonderEffectPreviewQuery* query, WonderEffectPreview* out, uint32_t outSize)
{
    return previewWonder(rule, query, out, outSize);
}
