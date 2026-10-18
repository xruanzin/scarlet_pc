// Explicit graphics diagnostics, disabled during ordinary runs. These capture
// shader control metadata missing from F9/SASS dumps; they do not execute TF.
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "nvn.h"

static pthread_once_t diag_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t diag_lock = PTHREAD_MUTEX_INITIALIZER;
static char diag_dir[2048];
static struct { uint64_t code; const uint8_t *control; uint32_t stage; } diag_seen[128];
static struct { uint64_t code; uint32_t stage; } diag_tf_seen[128];
static unsigned diag_nseen, diag_tf_nseen, diag_calls;
static size_t diag_snapshot_bytes;

typedef struct {
    uint64_t cb, addr, size;
    uint32_t slot;
} DiagTfRange;
static DiagTfRange diag_tf_ranges[512];
static unsigned diag_tf_nranges, diag_tf_range_next, diag_tf_vertex_links;
#define DIAG_CONTROL_PREFIX 0x704u
#define DIAG_CONTROL_FULL   0x880u
#define DIAG_CODE_LIMIT     (8u << 20)
typedef struct {
    uint8_t control[DIAG_CONTROL_FULL], header[0x80];
    uint32_t control_bytes;
} DiagShaderSnapshot;

static uint32_t diag_u32(const uint8_t *p) {
    uint32_t v;
    memcpy(&v, p, sizeof v);
    return v;
}

// Scarlet's NVN controls advertise the standard 0x880-byte control size at
// +0x6f0. Older diagnostics knew only the 0x704-byte prefix. Never extend a
// read unless the known prefix itself identifies the full control layout.
static uint32_t diag_control_bytes(const uint8_t *control) {
    if (!control || diag_u32(control) != 0x98761234u) return DIAG_CONTROL_PREFIX;
    uint32_t advertised = diag_u32(control + 0x6f0);
    return advertised == DIAG_CONTROL_FULL ? DIAG_CONTROL_FULL : DIAG_CONTROL_PREFIX;
}

static bool diag_snapshot_live(const NvnShaderStage *s, DiagShaderSnapshot *copy) {
    if (!s || !s->control || !s->code_addr) return false;
    memset(copy, 0, sizeof *copy);
    copy->control_bytes = diag_control_bytes(s->control);
    memcpy(copy->control, s->control, copy->control_bytes);
    memcpy(copy->header, (const void *)(uintptr_t)s->code_addr, sizeof copy->header);
    return true;
}

static void diag_init(void) {
    const char *dir = getenv("LARECOMP_TF_DIAG_DIR");
    if (!dir || !*dir) return;
    if (strlen(dir) >= sizeof diag_dir) {
        LOG("[tf-diag] output path too long; collection disabled\n");
        return;
    }
    strcpy(diag_dir, dir);
    // The launcher creates the parent and passes an absolute, per-run path.
    mkdir(diag_dir, 0755);
    LOG("[tf-diag] local output: %s (TF programs captured on use; TF remains unimplemented)\n", diag_dir);
}

static bool diag_write(const char *leaf, const void *data, size_t size) {
    char path[2304];
    int n = snprintf(path, sizeof path, "%s/%s", diag_dir, leaf);
    if (n < 0 || (size_t)n >= sizeof path) return false;
    FILE *file = fopen(path, "wb");
    if (!file) { LOG("[tf-diag] cannot create %s\n", path); return false; }
    bool ok = fwrite(data, 1, size, file) == size;
    if (fclose(file)) ok = false;
    if (!ok) { remove(path); LOG("[tf-diag] incomplete write: %s\n", path); }
    return ok;
}

// Called with diag_lock held. Initial shaders use a small retained snapshot;
// TF shaders are captured from the program that is actually bound at Begin/Resume.
static void diag_save_shader(const NvnShaderStage *s, const DiagShaderSnapshot *copy, const char *reason) {
    if (!copy) return;
    char leaf[160], meta[896];
    snprintf(leaf, sizeof leaf, "shader_%llx_s%u.control.bin", (unsigned long long)s->code_addr, s->stage);
    bool control_ok = diag_write(leaf, copy->control, copy->control_bytes);
    snprintf(leaf, sizeof leaf, "shader_%llx_s%u.header.bin", (unsigned long long)s->code_addr, s->stage);
    bool header_ok = diag_write(leaf, copy->header, sizeof copy->header);

    uint32_t code_size = 0, program_start = 0, program_size = 0, control_stage = ~0u;
    bool code_ok = false;
    if (copy->control_bytes >= DIAG_CONTROL_FULL) {
        code_size = diag_u32(copy->control + 0x704);
        program_start = diag_u32(copy->control + 0x708);
        control_stage = diag_u32(copy->control + 0x714);
        program_size = diag_u32(copy->control + 0x6f8);
        if (!strcmp(reason, "transform-feedback") && code_size >= 0x80 && code_size <= DIAG_CODE_LIMIT) {
            snprintf(leaf, sizeof leaf, "shader_%llx_s%u.code.bin", (unsigned long long)s->code_addr, s->stage);
            code_ok = diag_write(leaf, (const void *)(uintptr_t)s->code_addr, code_size);
        }
    }

    int length = snprintf(meta, sizeof meta,
        "code=%llx\nstage=%u\ncontrol=%llx\ncontrol_bytes=0x%x\nheader_bytes=0x80\n"
        "program_start=0x%x\nprogram_size=0x%x\ncode_size=0x%x\ncontrol_stage=0x%x\n"
        "const_offset=%x\nconst_size=%x\ncontrol_saved=%d\nheader_saved=%d\ncode_saved=%d\nreason=%s\n",
        (unsigned long long)s->code_addr, s->stage, (unsigned long long)(uintptr_t)s->control,
        copy->control_bytes, program_start, program_size, code_size, control_stage,
        s->const_offset, s->const_size, control_ok, header_ok, code_ok, reason);
    snprintf(leaf, sizeof leaf, "shader_%llx_s%u.txt", (unsigned long long)s->code_addr, s->stage);
    diag_write(leaf, meta, (size_t)length);
}

void nvn_diag_shader_release(NvnShaderStage *s) {
    if (!s->diag_snapshot) return;
    pthread_mutex_lock(&diag_lock);
    free(s->diag_snapshot);
    s->diag_snapshot = NULL;
    diag_snapshot_bytes -= sizeof(DiagShaderSnapshot);
    pthread_mutex_unlock(&diag_lock);
}

void nvn_diag_shader(NvnShaderStage *s) {
    pthread_once(&diag_once, diag_init);
    if (!diag_dir[0] || !s || !s->control || !s->code_addr || s->stage == 1 || s->stage == 5) return;
    pthread_mutex_lock(&diag_lock);
    for (unsigned i = 0; i < diag_nseen; i++)
        if (diag_seen[i].code == s->code_addr && diag_seen[i].control == s->control &&
            diag_seen[i].stage == s->stage) { pthread_mutex_unlock(&diag_lock); return; }

    // Keep snapshots only for the ordinary first-128 inventory. The previous
    // implementation retained every live program hoping it might later be used
    // for TF and exhausted 16 MiB before Scarlet reached customization.
    if (diag_nseen == 128) { pthread_mutex_unlock(&diag_lock); return; }
    if (!s->diag_snapshot) {
        DiagShaderSnapshot *copy = malloc(sizeof *copy);
        if (copy && diag_snapshot_live(s, copy)) {
            s->diag_snapshot = copy;
            diag_snapshot_bytes += sizeof *copy;
        } else free(copy);
    }
    if (!s->diag_snapshot) { pthread_mutex_unlock(&diag_lock); return; }

    unsigned id = diag_nseen++;
    diag_seen[id].code = s->code_addr; diag_seen[id].control = s->control; diag_seen[id].stage = s->stage;
    diag_save_shader(s, (const DiagShaderSnapshot *)s->diag_snapshot, "initial");
    pthread_mutex_unlock(&diag_lock);
}

static void diag_save_tf_programs(NvnCmdBuf *cb) {
    for (unsigned stage = 0; stage < 5; stage++) {
        if (stage == 1 || !cb->diag_programs[stage]) continue;
        const NvnShaderStage *s = &cb->diag_programs[stage]->stages[stage];
        if (!s->control || !s->code_addr) continue;
        bool seen = false;
        for (unsigned i = 0; i < diag_tf_nseen; i++)
            if (diag_tf_seen[i].code == s->code_addr && diag_tf_seen[i].stage == stage) seen = true;
        if (seen || diag_tf_nseen == 128) continue;

        // Capture on use. This is the program actually bound when TF begins,
        // so early UI shaders cannot consume the late-scene capture budget.
        DiagShaderSnapshot live;
        const DiagShaderSnapshot *copy = s->diag_snapshot;
        if (!copy) {
            if (!diag_snapshot_live(s, &live)) continue;
            copy = &live;
        }
        diag_tf_seen[diag_tf_nseen].code = s->code_addr;
        diag_tf_seen[diag_tf_nseen++].stage = stage;
        diag_save_shader(s, copy, "transform-feedback");
        LOG("[tf-diag] TF program cb=%llx code=%llx stage=%u control=0x%x\n",
            (unsigned long long)cb->guest, (unsigned long long)s->code_addr, stage, copy->control_bytes);
    }
}

void nvn_diag_bind_program(NvnCmdBuf *cb, NvnProgram *program, uint32_t mask) {
    pthread_once(&diag_once, diag_init);
    if (!diag_dir[0] || !cb) return;
    pthread_mutex_lock(&diag_lock);
    for (unsigned i = 0; i < 6; i++)
        if (mask & (1u << i)) cb->diag_programs[i] = program;
    if (cb->diag_tf_active) diag_save_tf_programs(cb);
    pthread_mutex_unlock(&diag_lock);
}

void nvn_diag_tf_state(NvnCmdBuf *cb, const char *name) {
    pthread_once(&diag_once, diag_init);
    if (!diag_dir[0] || !cb) return;
    pthread_mutex_lock(&diag_lock);
    if (!strcmp(name, "nvnCommandBufferBeginTransformFeedback") ||
        !strcmp(name, "nvnCommandBufferResumeTransformFeedback")) {
        cb->diag_tf_active = true;
        diag_save_tf_programs(cb);
    } else if (!strcmp(name, "nvnCommandBufferEndTransformFeedback") ||
               !strcmp(name, "nvnCommandBufferPauseTransformFeedback")) cb->diag_tf_active = false;
    pthread_mutex_unlock(&diag_lock);
}


static void diag_remember_tf_range(uint64_t cb, uint32_t slot, uint64_t addr, uint64_t size) {
    if (!addr || !size || addr > UINT64_MAX - size) return;
    for (unsigned i = 0; i < diag_tf_nranges; i++) {
        DiagTfRange *r = &diag_tf_ranges[i];
        if (r->addr == addr && r->size == size) {
            r->cb = cb;
            r->slot = slot;
            return;
        }
    }
    unsigned id;
    if (diag_tf_nranges < (unsigned)(sizeof diag_tf_ranges / sizeof diag_tf_ranges[0]))
        id = diag_tf_nranges++;
    else
        id = diag_tf_range_next++ % (unsigned)(sizeof diag_tf_ranges / sizeof diag_tf_ranges[0]);
    diag_tf_ranges[id] = (DiagTfRange){.cb = cb, .addr = addr, .size = size, .slot = slot};
}

static bool diag_ranges_overlap(uint64_t a, uint64_t asz, uint64_t b, uint64_t bsz) {
    if (!a || !asz || !b || !bsz || a > UINT64_MAX - asz || b > UINT64_MAX - bsz) return false;
    return a < b + bsz && b < a + asz;
}

// NVNbufferRange is {u64 address, u64 size}. When a TF output later becomes a
// vertex source, record the exact overlap so stretched geometry can be tied to
// the currently unimplemented stream-output path rather than inferred visually.
void nvn_diag_vertex_buffers(uint64_t cb, uint32_t first, uint32_t count, const uint64_t *ranges) {
    pthread_once(&diag_once, diag_init);
    if (!diag_dir[0] || !ranges || !count) return;
    if (count > 32) count = 32;
    pthread_mutex_lock(&diag_lock);
    for (uint32_t i = 0; i < count; i++) {
        uint64_t addr = ranges[i * 2], size = ranges[i * 2 + 1];
        if (!addr || !size) continue;
        for (unsigned j = 0; j < diag_tf_nranges; j++) {
            const DiagTfRange *tf = &diag_tf_ranges[j];
            if (!diag_ranges_overlap(addr, size, tf->addr, tf->size)) continue;
            if (diag_tf_vertex_links++ >= 256) continue;
            char path[2304];
            snprintf(path, sizeof path, "%s/tf_vertex_links.txt", diag_dir);
            FILE *file = fopen(path, "a");
            if (file) {
                fprintf(file,
                    "%u vb_cb=%llx vb_slot=%u vb=%llx+%llx tf_cb=%llx tf_slot=%u tf=%llx+%llx\n",
                    diag_tf_vertex_links,
                    (unsigned long long)cb, first + i,
                    (unsigned long long)addr, (unsigned long long)size,
                    (unsigned long long)tf->cb, tf->slot,
                    (unsigned long long)tf->addr, (unsigned long long)tf->size);
                fclose(file);
            }
            if (diag_tf_vertex_links <= 16)
                LOG("[tf-diag] TF output reused as vertex buffer: vb=%llx+%llx tf=%llx+%llx slot=%u\n",
                    (unsigned long long)addr, (unsigned long long)size,
                    (unsigned long long)tf->addr, (unsigned long long)tf->size, tf->slot);
        }
    }
    pthread_mutex_unlock(&diag_lock);
}

void nvn_diag_tf_call(const char *name, const Cpu *c) {
    pthread_once(&diag_once, diag_init);
    if (!diag_dir[0]) return;
    pthread_mutex_lock(&diag_lock);

    if (!strcmp(name, "nvnCommandBufferBindTransformFeedbackBuffer")) {
        diag_remember_tf_range(c->x[0], (uint32_t)c->x[1], c->x[2], c->x[3]);
    } else if (!strcmp(name, "nvnCommandBufferBindTransformFeedbackBuffers")) {
        uint32_t first = (uint32_t)c->x[1], count = (uint32_t)c->x[2];
        uint64_t ptr = c->x[3];
        if (count > 32) count = 32;
        uint64_t bytes = (uint64_t)count * 16;
        if (ptr >= GUEST_LO && bytes <= GUEST_HI - GUEST_LO &&
            ptr <= GUEST_HI - bytes) {
            const uint64_t *ranges = (const uint64_t *)(uintptr_t)ptr;
            for (uint32_t i = 0; i < count; i++)
                diag_remember_tf_range(c->x[0], first + i, ranges[i * 2], ranges[i * 2 + 1]);
        }
    }

    if (diag_calls++ < 128) {
        char path[2304];
        snprintf(path, sizeof path, "%s/tf_calls.txt", diag_dir);
        FILE *file = fopen(path, "a");
        if (file) {
            // Raw argument registers: only the API's declared arguments are meaningful.
            fprintf(file, "%u %s cb=%llx x1=%llx x2=%llx x3=%llx lr=%llx\n", diag_calls, name,
                (unsigned long long)c->x[0], (unsigned long long)c->x[1], (unsigned long long)c->x[2],
                (unsigned long long)c->x[3], (unsigned long long)c->x[30]);
            fclose(file);
        }
    }
    pthread_mutex_unlock(&diag_lock);
}
