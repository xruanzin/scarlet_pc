#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../cpu.h"

#ifdef __cplusplus
extern "C" {
#endif

// Load and validate scarlet-rtld/main/multimedia/sdk AOT DLLs without running
// guest code. This is idempotent across in-process game reboots.
bool scarlet_aot_load(const char *game_root, char *why, size_t why_size);

// Bind the already-loaded AOT images to the native NSO loader's module bases.
// loader_load() must have run first. rtld intentionally remains resident but
// unbound because the native loader applies relocations itself.
bool scarlet_aot_bind_loaded_modules(char *why, size_t why_size);

bool scarlet_aot_ready(void);
void scarlet_aot_log_status(void);

// Implemented by aot_execute.c. Installed as the guest execution backend when
// SCARLET_RUNTIME=native.
void scarlet_aot_guest_call(Cpu *cpu, uint64_t address);
void scarlet_aot_profile_report(void);

#ifdef __cplusplus
}
#endif
