// Public C ABI for ReRevved's conservative gameplay-state snapshot.
//
// The guest frame thread publishes immutable snapshots for host-side readers.
// Mods do not link against the ReRevved executable; they resolve the two entry
// points from the host process and check GameplayAbiVersion first.
//
// ABI 2 evolves additively by consuming reserved fields or adding validity
// bits without changing existing offsets. An incompatible change requires a
// new ABI version.

#pragma once

#include <stdint.h>

#include <game_ids.h>

#if defined(GAMEPLAY_API_EXPORTS)
#if defined(_WIN32)
#define GAMEPLAY_API __declspec(dllexport)
#else
#define GAMEPLAY_API __attribute__((visibility("default")))
#endif
#else
#define GAMEPLAY_API
#endif

#define GAMEPLAY_ABI_VERSION 2u

enum
{
    GAMEPLAY_OK                   = 0,
    GAMEPLAY_ERR_UNAVAILABLE      = -1,
    GAMEPLAY_ERR_INVALID_ARGUMENT = -10,
    GAMEPLAY_ERR_BUFFER_TOO_SMALL = -11,
};

enum
{
    GAMEPLAY_VALID_FRONTEND     = 1u << 0,
    GAMEPLAY_VALID_TURN         = 1u << 1,
    GAMEPLAY_VALID_INTERFACE    = 1u << 2,
    GAMEPLAY_VALID_CIVILIZATION = 1u << 3,
    GAMEPLAY_VALID_ERA          = 1u << 4,
    GAMEPLAY_VALID_YEAR         = 1u << 5,
    GAMEPLAY_VALID_TURN_NUMBER  = 1u << 6,
};

#define GAMEPLAY_PLAYER_UNKNOWN       (-1)
#define GAMEPLAY_CIVILIZATION_UNKNOWN (-1)
#define GAMEPLAY_ERA_UNKNOWN          (-1)
#define GAMEPLAY_YEAR_UNKNOWN         (-2147483647 - 1)
#define GAMEPLAY_TURN_UNKNOWN         (-1)

typedef struct GameplayState
{
    // Size written when outSize can hold this structure, including when the
    // snapshot is unavailable. Smaller buffers are cleared as far as possible
    // and rejected.
    uint32_t structSize;

    // A validity bit means that the corresponding source fields were read
    // safely. available is stricter: every conservative playable-turn gate is
    // satisfied. Valid fields may still be returned while available is zero.
    uint32_t validFields;
    uint64_t frameSequence;
    int32_t  gameplayActive;
    int32_t  interfaceUpdate;
    int32_t  activePlayer;
    uint32_t humanPlayerMask;
    int32_t  turnOwnerKnown;
    int32_t  humanTurn;
    int32_t  available;
    // These fields describe the active human player. Their validity bits are
    // clear during AI turns, menu/loading transitions, or failed guest reads.
    CivilizationId civilization;
    int32_t        era;
    int32_t        year;
    int32_t        turn;
    int32_t        reserved[4];
} GameplayState;

typedef uint32_t (*GameplayAbiVersionFn)(void);
typedef int (*GetGameplayStateFn)(GameplayState* out,
                                  uint32_t       outSize);

#ifdef __cplusplus
extern "C"
{
#endif

    GAMEPLAY_API uint32_t GameplayAbiVersion(void);
    GAMEPLAY_API int      GetGameplayState(
        GameplayState* out,
        uint32_t       outSize);

#ifdef __cplusplus
} // extern "C"
#endif

#undef GAMEPLAY_API
