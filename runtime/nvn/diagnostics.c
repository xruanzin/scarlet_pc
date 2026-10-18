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
static bool diag_snapshot_warning;
static size_t diag_snapshot_bytes;
#define DIAG_SNAPSHOT_LIMIT (16u << 20)
typedef struct {
    uint8_t control[0x704], header[0x80];
} DiagShaderSnapshot;

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
    LOG("[tf-diag] local output: %s (controls only; TF remains unimplemented)\n", diag_dir);
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

// Called with diag_lock held; bytes come from SetShaders, never a potentially
// released guest control pointer when a command buffer later binds the program.
static void diag_save_shader(const NvnShaderStage *s, const char *reason) {
    const DiagShaderSnapshot *copy = s->diag_snapshot;
    if (!copy) return;
    char leaf[160], meta[640];
    snprintf(leaf, sizeof leaf, "shader_%llx_s%u.control.bin", (unsigned long long)s->code_addr, s->stage);
    bool control_ok = diag_write(leaf, copy->control, sizeof copy->control);
    snprintf(leaf, sizeof leaf, "shader_%llx_s%u.header.bin", (unsigned long long)s->code_addr, s->stage);
    bool header_ok = diag_write(leaf, copy->header, sizeof copy->header);
    int length = snprintf(meta, sizeof meta,
        "code=%llx\nstage=%u\ncontrol=%llx\ncontrol_bytes=0x704\nheader_bytes=0x80\nconst_offset=%x\nconst_size=%x\ncontrol_saved=%d\nheader_saved=%d\nreason=%s\n",
        (unsigned long long)s->code_addr, s->stage, (unsigned long long)(uintptr_t)s->control,
        s->const_offset, s->const_size, control_ok, header_ok, reason);
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
    // Retain a bounded immutable copy for late TF selection even after the
    // ordinary first-128 quota is full. Program replacement/finalize releases it.
    if (!s->diag_snapshot && diag_snapshot_bytes + sizeof(DiagShaderSnapshot) <= DIAG_SNAPSHOT_LIMIT) {
        DiagShaderSnapshot *copy = malloc(sizeof *copy);
        if (copy) {
            memcpy(copy->control, s->control, sizeof copy->control);
            memcpy(copy->header, (const void *)(uintptr_t)s->code_addr, sizeof copy->header);
            s->diag_snapshot = copy;
            diag_snapshot_bytes += sizeof *copy;
        }
    }
    if (!s->diag_snapshot && !diag_snapshot_warning) {
        diag_snapshot_warning = true;
        LOG("[tf-diag] snapshot allocation/budget exhausted; some late TF controls may be absent\n");
    }
    for (unsigned i = 0; i < diag_nseen; i++)
        if (diag_seen[i].code == s->code_addr && diag_seen[i].control == s->control &&
            diag_seen[i].stage == s->stage) { pthread_mutex_unlock(&diag_lock); return; }
    if (diag_nseen == 128 || !s->diag_snapshot) { pthread_mutex_unlock(&diag_lock); return; }
    unsigned id = diag_nseen++;
    diag_seen[id].code = s->code_addr; diag_seen[id].control = s->control; diag_seen[id].stage = s->stage;
    diag_save_shader(s, "initial");
    pthread_mutex_unlock(&diag_lock);
}

static void diag_save_tf_programs(NvnCmdBuf *cb) {
    for (unsigned stage = 0; stage < 5; stage++) {
        if (stage == 1 || !cb->diag_programs[stage]) continue;
        const NvnShaderStage *s = &cb->diag_programs[stage]->stages[stage];
        if (!s->diag_snapshot) continue;
        bool seen = false;
        for (unsigned i = 0; i < diag_tf_nseen; i++)
            if (diag_tf_seen[i].code == s->code_addr && diag_tf_seen[i].stage == stage) seen = true;
        if (seen || diag_tf_nseen == 128) continue;
        diag_tf_seen[diag_tf_nseen].code = s->code_addr;
        diag_tf_seen[diag_tf_nseen++].stage = stage;
        diag_save_shader(s, "transform-feedback");
        LOG("[tf-diag] TF program cb=%llx code=%llx stage=%u\n",
            (unsigned long long)cb->guest, (unsigned long long)s->code_addr, stage);
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

void nvn_diag_tf_call(const char *name, const Cpu *c) {
    pthread_once(&diag_once, diag_init);
    if (!diag_dir[0]) return;
    pthread_mutex_lock(&diag_lock);
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
