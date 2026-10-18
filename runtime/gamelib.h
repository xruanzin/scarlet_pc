// The recompiled game code: a library built from the player's own game files (`make gen` + `make` in a source
// checkout, the start screen's setup otherwise; runtime/gamebuild.c) and loaded at startup (runtime/gamelib.c).
// Nothing from the game ships with the app.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"

#if defined(_WIN32)
#define GAMELIB_NAME "lasgame.dll"
#define GAMELIB_EXPORT __declspec(dllexport)
#elif defined(__APPLE__)
#define GAMELIB_NAME "lasgame.dylib"
#define GAMELIB_EXPORT __attribute__((visibility("default")))
#else
#define GAMELIB_NAME "lasgame.so"
#define GAMELIB_EXPORT __attribute__((visibility("default")))
#endif

// GAMELIB_ID (-D, from the Makefile): a hash of the generator and the headers the game code is compiled against. A
// library built with another one is rebuilt, not loaded.
#ifndef GAMELIB_ID
#define GAMELIB_ID "dev"
#endif
#define GAMELIB_ABI 4  // (4: static resume entries for indirect branches inside a function)
#define GAMELIB_MAX_MODULES 4

// A generated resume function enters at c->pc without repeating the guest prologue.
// Ranges are sorted, non-overlapping, half-open, and only accept aligned instructions.
typedef struct {
    uint64_t lo, hi;
    GuestFn fn;
} GameLibResumeRange;

// One table per recompiled module, in load order (Scarlet base: main, sdk, subsdk0; runtime/loader.c).
typedef struct {
    const char *name;
    const uint64_t *addrs;  // sorted guest addresses
    const GuestFn *fns;     // the functions at them
    const uint64_t *count;
    const GameLibResumeRange *resumes;
    const uint64_t *resume_count;
} GameLibModule;

typedef struct {
    uint32_t abi;    // GAMELIB_ABI
    const char *id;  // GAMELIB_ID
    uint32_t nmods;
    GameLibModule mods[GAMELIB_MAX_MODULES];
} GameLibTables;

// The library's one exported function (tools/gamelib_entry.c).
typedef const GameLibTables *(*GameLibEntry)(void);
#define GAMELIB_ENTRY "lasgame_tables"

#ifdef __cplusplus
extern "C" {
#endif

// Loads the game code for the game files in game_dir (once; later calls return the first result). Looks at
// LARECOMP_GAME_LIB, next to the executable (a source checkout's build/), then game_dir. false: why says what's wrong.
bool gamelib_load(const char *game_dir, char *why, size_t why_size);
// The tables of the loaded library (NULL before gamelib_load succeeds).
const GameLibTables *gamelib_tables(void);
// True when a library usable with this build is in one of those places (without loading it).
bool gamelib_present(const char *game_dir);

#ifdef __cplusplus
}
#endif

