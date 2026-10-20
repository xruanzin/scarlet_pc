// Direct fast paths for tiny nn::os SDK helpers that are called hundreds of
// thousands of times in Scarlet's slow scenes. These functions intentionally
// mirror the AArch64 SDK loads instead of introducing host-side thread state.
//
// gen.py emits direct calls to the hle_<sanitized> symbols below for the SDK
// definitions and, for Scarlet 1.0.0, the matching main PLT thunks. HLE_FUNC
// wrappers keep ordinary unresolved imports correct too.
#include "../hle.h"

#define NN_OS_THREADTYPE_TLR_OFFSET 0x1F8ull
#define NN_OS_TLS_VALUES_OFFSET     0x80ull

static inline uint64_t nn_os_current_thread_type(Cpu *c) {
    return LD64(c->tpidrro + NN_OS_THREADTYPE_TLR_OFFSET);
}

void hle__ZN2nn2os16GetCurrentThreadEv(Cpu *c) {
    c->x[0] = nn_os_current_thread_type(c);
}

void hle__ZN2nn2os11GetTlsValueENS0_7TlsSlotE(Cpu *c) {
    uint64_t thread = nn_os_current_thread_type(c);
    uint32_t slot = (uint32_t)c->x[0];
    // Exact SDK semantics: ThreadType::tlsValue[slot]. Do not add a bounds
    // check here; the guest function does not have one either.
    c->x[0] = LD64(thread + NN_OS_TLS_VALUES_OFFSET + (uint64_t)slot * 8);
}

HLE_FUNC("_ZN2nn2os16GetCurrentThreadEv", hle_import_get_current_thread) {
    hle__ZN2nn2os16GetCurrentThreadEv(c);
}

HLE_FUNC("_ZN2nn2os11GetTlsValueENS0_7TlsSlotE", hle_import_get_tls_value) {
    hle__ZN2nn2os11GetTlsValueENS0_7TlsSlotE(c);
}
