// Public C ABI for ReRevved's conservative gameplay-state snapshot.
//
// The guest frame thread publishes immutable snapshots for host-side readers.
// Mods do not link against the ReRevved executable; they resolve the two entry
// points from the host process and check ReRevvedGameplayAbiVersion first.
//
// ABI 1 evolves additively by consuming reserved fields or adding validity
// bits without changing existing offsets. An incompatible change requires a
// new ABI version.

#pragma once

#include <stdint.h>

#if defined(REREVVED_GAMEPLAY_API_EXPORTS)
#if defined(_WIN32)
#define REREVVED_GAMEPLAY_API __declspec(dllexport)
#else
#define REREVVED_GAMEPLAY_API __attribute__((visibility("default")))
#endif
#else
#define REREVVED_GAMEPLAY_API
#endif

#define REREVVED_GAMEPLAY_ABI_VERSION 1u

enum
{
    REREVVED_GAMEPLAY_OK                   = 0,
    REREVVED_GAMEPLAY_ERR_UNAVAILABLE      = -1,
    REREVVED_GAMEPLAY_ERR_INVALID_ARGUMENT = -10,
    REREVVED_GAMEPLAY_ERR_BUFFER_TOO_SMALL = -11,
};

enum
{
    REREVVED_GAMEPLAY_VALID_FRONTEND  = 1u << 0,
    REREVVED_GAMEPLAY_VALID_TURN      = 1u << 1,
    REREVVED_GAMEPLAY_VALID_INTERFACE = 1u << 2,
};

#define REREVVED_GAMEPLAY_PLAYER_UNKNOWN (-1)

typedef struct ReRevvedGameplayState
{
    // Size written when out_size can hold this structure, including when the
    // snapshot is unavailable. Smaller buffers are cleared as far as possible
    // and rejected.
    uint32_t struct_size;

    // A validity bit means that the corresponding source fields were read
    // safely. available is stricter: every conservative playable-turn gate is
    // satisfied. Valid fields may still be returned while available is zero.
    uint32_t valid_fields;
    uint64_t frame_sequence;
    int32_t  gameplay_active;
    int32_t  interface_update;
    int32_t  active_player;
    uint32_t human_player_mask;
    int32_t  turn_owner_known;
    int32_t  human_turn;
    int32_t  available;
    int32_t  reserved[8];
} ReRevvedGameplayState;

typedef uint32_t (*ReRevvedGameplayAbiVersionFn)(void);
typedef int (*ReRevvedGetGameplayStateFn)(ReRevvedGameplayState* out,
                                          uint32_t               out_size);

#ifdef __cplusplus
extern "C"
{
#endif

    REREVVED_GAMEPLAY_API uint32_t ReRevvedGameplayAbiVersion(void);
    REREVVED_GAMEPLAY_API int      ReRevvedGetGameplayState(
        ReRevvedGameplayState* out,
        uint32_t               out_size);

#ifdef __cplusplus
} // extern "C"
#endif

#undef REREVVED_GAMEPLAY_API
