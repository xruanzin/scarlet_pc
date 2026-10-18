// NVN implemented on the host. Guest NVN objects hold a pointer to a host object at offset 0;
// builders and small state objects store their fields directly in guest memory.
#pragma once
#include "../hle.h"
#include "tegra.h"

#define NVN(name) HLE_FUNC(#name, hle_##name)

// argument helpers
#define A(i) (c->x[(i)])
#define AP(i) ((void *)(uintptr_t)c->x[(i)])
#define AF(i) (*(float *)&c->v[(i)])
#define RET(v) (c->x[0] = (uint64_t)(v))
#define RETF(f) do { c->v[0] = 0; *(float *)&c->v[0] = (f); } while (0)
#define HOST(T, guest) ((T *)(uintptr_t)(*(uint64_t *)(uintptr_t)(guest)))
#define SET_HOST(guest, p) (*(uint64_t *)(uintptr_t)(guest) = (uint64_t)(uintptr_t)(p))
#define G(T, base, off) (*(T *)((uint8_t *)(uintptr_t)(base) + (off)))

typedef struct NvnPool {
    uint64_t storage, size;
    uint32_t flags;
} NvnPool;

typedef struct NvnTexture {
    TexLayout layout;
    uint32_t target, format, flags, width, height, depth, levels, samples, ds_mode;
    uint8_t swizzle[4];
    uint64_t stride;
    uint64_t addr;  // guest address of texel storage
    NvnPool *pool;
    uint64_t pool_offset;
    void *backend;  // renderer-side texture
    uint64_t debug_id;
    char label[64];
} NvnTexture;

typedef struct {
    uint32_t min_filter, mag_filter, wrap[3], compare_mode, compare_func, reduction;
    float lod_min, lod_max, lod_bias, max_aniso;
    float border[4];
    void *backend;
} NvnSamplerDesc;

typedef struct {
    uint32_t setmask;  // which fields are overridden
    uint16_t base_level, num_levels, min_layer, num_layers;
    uint32_t format, target, ds_mode;
    uint8_t swizzle[4];
} NvnTextureView;  // our layout of the guest NVNtextureView (0x28 bytes)

typedef struct {
    NvnTexture *tex;
    NvnTextureView view;
    bool has_view;
    bool is_image;      // registered through nvnTexturePoolRegisterImage (storage-capable descriptor)
    uint32_t image_seq; // most-recent registrations first when compacting the bindless image set
} NvnTexEntry;

typedef struct NvnDescPool {
    uint64_t addr;
    int count;
    void *entries;  // NvnTexEntry[] or NvnSamplerDesc[]
    uint32_t generation; // bumps when descriptor contents change
} NvnDescPool;

typedef struct NvnShaderStage {
    uint32_t stage;           // NVNshaderStage
    uint64_t code_addr;       // GPU (== CPU) address of the code
    const uint8_t *control;   // control section
    uint32_t const_offset, const_size;  // compiler constants (bound as c1), within the code blob
    void *backend;
    void *diag_snapshot;      // opt-in immutable control/header copy, owned by this stage
} NvnShaderStage;

// Opt-in, bounded local diagnostics: immutable shader controls and TF API calls.
void nvn_diag_shader(NvnShaderStage *stage);
void nvn_diag_shader_release(NvnShaderStage *stage);
void nvn_diag_vertex_buffers(uint64_t cb, uint32_t first, uint32_t count, const uint64_t *ranges);
void nvn_diag_tf_call(const char *name, const Cpu *cpu);

typedef struct NvnProgram {
    NvnShaderStage stages[6];
    uint32_t stage_mask;
    void *backend;
} NvnProgram;

typedef struct NvnWindow {
    uint64_t textures[8];  // guest NVNtexture*
    int num, active, interval, next;
    int crop[4];
} NvnWindow;

typedef struct NvnSync {
    volatile int signaled;
    volatile uint64_t seq;  // GPU submission this fence belongs to (renderer_fence_seq), 0 = none
} NvnSync;

// ---- command recording (stored in guest command memory)
typedef struct {
    // Internal records are much larger than real Maxwell/NVN commands. Each
    // recorded list therefore keeps a tiny handle/jump in the game's command
    // memory and stores the expanded CmdRec stream in host overflow segments.
    uint64_t mem;         // current internal write segment (guest or host address)
    uint64_t size;        // current internal segment size
    uint64_t chunk_used;  // write cursor within the current internal segment
    uint64_t used;        // guest command-memory bytes consumed since AddCommandMemory
    uint64_t guest_mem;   // most recently supplied guest command-memory chunk
    uint64_t guest_size;
    uint64_t start;       // opaque command handle: guest address of this list's first CMD_JUMP
    uint64_t callback, callback_data;
    bool recording;
    bool host_segment;    // current internal write segment is host-owned
    uint64_t guest;       // the guest NVNcommandBuffer*
    uint64_t obj_tag;     // frame interpolation: the game object the list's last CMD_OBJECT named (cmdbuf.c)
    uint64_t host_origin_lo, host_origin_hi;  // guest handle slot owning the host segment chain
    NvnProgram *diag_programs[6]; // diagnostic-only bindings; never used by the executor
    bool diag_tf_active;
} NvnCmdBuf;

void nvn_diag_bind_program(NvnCmdBuf *cb, NvnProgram *program, uint32_t mask);
void nvn_diag_tf_state(NvnCmdBuf *cb, const char *name);

// One recorded command: header, the call's integer and float arguments, then copied data.
typedef struct {
    uint32_t op, size;  // size includes the header, multiple of 8
    uint64_t x[7];      // x1..x7 of the call
    float f[4];         // s0..s3 of the call
    uint8_t extra[];
} CmdRec;

// Expanded command streams are host-backed. Queue submission pins the stream
// so command-memory reuse on another guest thread cannot recycle/overwrite it
// while the Vulkan executor is still reading it. pin() returns the resolved
// host start address (or the original handle when no host stream is known).
uint64_t nvn_cmdstream_pin(uint64_t handle);
void nvn_cmdstream_unpin(uint64_t handle);

enum {
    CMD_END = 0, CMD_JUMP, CMD_CALL,
    CMD_BIND_PROGRAM, CMD_BIND_BLEND, CMD_BIND_CHANNEL_MASK, CMD_BIND_COLOR, CMD_BIND_DEPTH_STENCIL,
    CMD_BIND_MULTISAMPLE, CMD_BIND_POLYGON, CMD_BIND_VERTEX_ATTRIB, CMD_BIND_VERTEX_STREAM,
    CMD_BIND_VERTEX_BUFFERS, CMD_BIND_UNIFORM_BUFFERS, CMD_BIND_STORAGE_BUFFERS, CMD_BIND_TEXTURES,
    CMD_BIND_IMAGES, CMD_BIND_TRANSFORM_FEEDBACK_BUFFERS, CMD_BEGIN_TRANSFORM_FEEDBACK,
    CMD_END_TRANSFORM_FEEDBACK, CMD_PAUSE_TRANSFORM_FEEDBACK, CMD_RESUME_TRANSFORM_FEEDBACK, CMD_SET_TEXTURE_POOL, CMD_SET_SAMPLER_POOL, CMD_SET_RENDER_TARGETS,
    CMD_SET_VIEWPORTS, CMD_SET_SCISSORS, CMD_SET_DEPTH_RANGES, CMD_SET_BLEND_COLOR, CMD_SET_STENCIL,
    CMD_SET_PRIMITIVE_RESTART, CMD_SET_ALPHA_REF, CMD_SET_DEPTH_BOUNDS, CMD_SET_POLYGON_OFFSET,
    CMD_SET_LINE_WIDTH, CMD_SET_POINT_SIZE, CMD_SET_SAMPLE_MASK, CMD_SET_RENDER_ENABLE,
    CMD_CLEAR_COLOR, CMD_CLEAR_DEPTH_STENCIL, CMD_CLEAR_TEXTURE, CMD_CLEAR_BUFFER,
    CMD_DRAW_ARRAYS, CMD_DRAW_ELEMENTS, CMD_DRAW_ARRAYS_INDIRECT, CMD_DRAW_ELEMENTS_INDIRECT,
    CMD_DISPATCH, CMD_DISPATCH_INDIRECT,
    CMD_COPY_BUFFER_TO_BUFFER, CMD_COPY_BUFFER_TO_TEXTURE, CMD_COPY_TEXTURE_TO_BUFFER,
    CMD_COPY_TEXTURE_TO_TEXTURE, CMD_DOWNSAMPLE, CMD_UPDATE_UNIFORM_BUFFER, CMD_FENCE_SYNC, CMD_WAIT_SYNC,
    CMD_BARRIER, CMD_SIGNAL_EVENT, CMD_WAIT_EVENT, CMD_REPORT_COUNTER, CMD_DISCARD_COLOR,
    CMD_DISCARD_DEPTH_STENCIL, CMD_SET_COPY_STRIDES, CMD_DRAW_TEXTURE, CMD_RESOLVE_DEPTH,
    CMD_OBJECT,  // (ours) x[0]: the game object the following draws belong to, 0 = none (frame interpolation)
    CMD_MULTI_DRAW_ARRAYS_INDIRECT_COUNT, CMD_MULTI_DRAW_ELEMENTS_INDIRECT_COUNT,
    CMD_COUNT
};



// renderer backend (runtime/nvn/metal.m)
void backend_init(void);
void backend_execute(uint64_t handle);            // run a recorded command list
void backend_present(NvnTexture *tex);            // show a texture in the window
void backend_texture_destroyed(NvnTexture *tex);
void backend_pool_written(uint64_t addr, uint64_t size);
