// Entry of the game code library (runtime/gamelib.h), compiled into it with the generated code: the function
// tables, found by name when the app loads the library. Everything else in it stays hidden.
#include "gamelib.h"

#define MOD(n) extern const uint64_t n##_fn_addrs[], n##_fn_count; extern const GuestFn n##_fn_ptrs[];
MOD(main) MOD(sdk) MOD(subsdk0)
#undef MOD
#define TAB(n) {#n, n##_fn_addrs, n##_fn_ptrs, &n##_fn_count, NULL, NULL}
// Both main and SDK contain switches that enter the middle of another emitted
// fragment. subsdk0 can retain its existing generated chunks and table.
extern const GameLibResumeRange main_resume_ranges[];
extern const uint64_t main_resume_count;
extern const GameLibResumeRange sdk_resume_ranges[];
extern const uint64_t sdk_resume_count;
#define MAIN_TAB {"main", main_fn_addrs, main_fn_ptrs, &main_fn_count, main_resume_ranges, &main_resume_count}
#define SDK_TAB {"sdk", sdk_fn_addrs, sdk_fn_ptrs, &sdk_fn_count, sdk_resume_ranges, &sdk_resume_count}

GAMELIB_EXPORT const GameLibTables *lasgame_tables(void) {
    static const GameLibTables t = {GAMELIB_ABI, GAMELIB_ID, 3, {MAIN_TAB, SDK_TAB, TAB(subsdk0)}};
    return &t;
}

