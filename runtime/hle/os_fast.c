// Direct fast paths for tiny nn::os SDK helpers that are called hundreds of
// thousands of times in Scarlet's slow scenes. These functions intentionally
// mirror the AArch64 SDK loads instead of introducing host-side thread state.
//
// gen.py emits direct calls to the hle_<sanitized> symbols below for the SDK
// definitions and, for Scarlet 1.0.0, the matching main PLT thunks. HLE_FUNC
// wrappers keep ordinary unresolved imports correct too.
#include "../hle.h"

uint64_t loader_find_symbol(const char *name);
void guest_call(Cpu *c, uint64_t addr);

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

// Scarlet's hottest nn::os helper. The SDK implementation at sdk+0x3720d0
// stores the signal state at LightEventType+4:
//   0 = unsignaled/no waiter, 1 = waiter present, 2 = signaled.
// Both clear modes share the same uncontended path: 2 returns immediately and
// 0 is atomically promoted to 2. Only state 1 needs the SDK's process-wide-key
// wake/arbiter path. Keep that exact slow path by calling the original guest
// implementation, so waiter semantics are unchanged.
void hle__ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE(Cpu *c) {
    uint64_t ev = c->x[0];
    uint32_t *state = (uint32_t *)(uintptr_t)(ev + 4);
    uint32_t cur = __atomic_load_n(state, __ATOMIC_ACQUIRE);

    for (;;) {
        if (cur == 2) return;
        if (cur != 0) break;

        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(state, &expected, 2, false,
                                        __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE))
            return;
        cur = expected;
    }

    static uint64_t original;
    if (!original)
        original = loader_find_symbol("_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE");
    if (!original)
        FATAL("os-fast: original nn::os::SignalLightEvent not found\n");
    guest_call(c, original);
}

HLE_FUNC("_ZN2nn2os16GetCurrentThreadEv", hle_import_get_current_thread) {
    hle__ZN2nn2os16GetCurrentThreadEv(c);
}

HLE_FUNC("_ZN2nn2os11GetTlsValueENS0_7TlsSlotE", hle_import_get_tls_value) {
    hle__ZN2nn2os11GetTlsValueENS0_7TlsSlotE(c);
}

HLE_FUNC("_ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE", hle_import_signal_light_event) {
    hle__ZN2nn2os16SignalLightEventEPNS0_14LightEventTypeE(c);
}
