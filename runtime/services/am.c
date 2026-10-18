// Applet manager (appletOE), apm, and lm.
#include <stdlib.h>
#include <string.h>

#include "../mods.h"
#include "../hle.h"
#include "../ipc.h"

#define AM_NO_MESSAGE MAKE_RESULT(128, 3)
#define AM_NO_DATA MAKE_RESULT(128, 2)

static uint32_t msg_event_r, msg_event_w;
static KEvent *msg_event;
static uint32_t msg_queue[16];
static int msg_count;

static bool preselected_popped;
RT_ON_RESET(am_reset) {  // (reboot: the message queue starts over with FocusStateChanged)
    msg_event = NULL;
    msg_event_r = msg_event_w = 0;
    msg_count = 0;
    preselected_popped = false;
}

static void am_init_once(void) {
    if (msg_event) return;
    msg_event = kernel_create_event_pair(&msg_event_w, &msg_event_r);
    msg_queue[msg_count++] = 15;  // FocusStateChanged
    pthread_mutex_lock(&g_klock);
    kernel_signal(msg_event);
    pthread_mutex_unlock(&g_klock);
}

static uint32_t new_event(void) {
    uint32_t w, r;
    kernel_create_event_pair(&w, &r);
    return r;
}

#define UNHANDLED(iface) LOG("[am] %s cmd %u (unhandled, ok)\n", iface, req->cmd)

static void common_state_getter(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: resp->copy_handles[resp->ncopy++] = msg_event_r; break;  // GetEventHandle
    case 1:  // ReceiveMessage
        pthread_mutex_lock(&g_klock);
        if (msg_count) {
            OUT(uint32_t, 0) = msg_queue[0];
            memmove(msg_queue, msg_queue + 1, --msg_count * sizeof(uint32_t));
            if (!msg_count) msg_event->signaled = false;
            resp_size(resp, 4);
        } else {
            resp->result = AM_NO_MESSAGE;
        }
        pthread_mutex_unlock(&g_klock);
        break;
    case 5: OUT(uint8_t, 0) = 1; resp_size(resp, 1); break;   // GetOperationMode: docked
    case 6: OUT(uint32_t, 0) = 1; resp_size(resp, 4); break;  // GetPerformanceMode: boost
    case 8: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;   // GetBootMode
    case 9: OUT(uint8_t, 0) = 1; resp_size(resp, 1); break;   // GetCurrentFocusState: in focus
    case 13: case 61: resp->copy_handles[resp->ncopy++] = new_event(); break;
    case 66: {  // SetCpuBoostMode: 0 Normal, 1 FastLoad
        uint32_t mode = IN(uint32_t, 0);
        LOG("[am] SetCpuBoostMode %u (%s)\n", mode, mode == 1 ? "FastLoad" : "Normal");
#ifdef _WIN32
        plat_set_cpu_boost(mode == 1);
#endif
        break;
    }
    case 67:  // CancelCpuBoostMode
#ifdef _WIN32
        plat_set_cpu_boost(false);
#endif
        LOG("[am] CancelCpuBoostMode\n");
        break;
    case 900: break;  // SetRequestExitToLibraryAppletAtExecuteNextProgramEnabled
    case 60: {  // GetDefaultDisplayResolution
        int w = getenv("LARECOMP_DISPLAY_W") ? atoi(getenv("LARECOMP_DISPLAY_W")) : 1920;
        LOG("[am] GetDefaultDisplayResolution -> %dx1080\n", w);
        OUT(int32_t, 0) = w; OUT(int32_t, 4) = 1080; resp_size(resp, 8); break;
    }
    default: UNHANDLED("ICommonStateGetter");
    }
}

static void self_controller(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: LOG("[am] application requested exit\n"); exit(0);
    case 9: resp->copy_handles[resp->ncopy++] = new_event(); break;
    case 11: break;  // SetOperationModeChangedNotification
    case 12: break;  // SetPerformanceModeChangedNotification
    case 13: break;  // SetFocusHandlingMode
    case 16: break;  // SetOutOfFocusSuspendingEnabled
    case 40: OUT(uint64_t, 0) = 1; resp_size(resp, 8); break;  // CreateManagedDisplayLayer
    case 44:  // CreateManagedDisplaySeparableLayer (10.0.0+)
        OUT(uint64_t, 0) = 1;
        OUT(uint64_t, 8) = 2;
        resp_size(resp, 16);
        LOG("[gfx-milestone] AM separable display layers 1,2 created\n");
        break;
    case 69: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    case 90: OUT(uint64_t, 0) = 0; resp_size(resp, 8); break;  // GetAccumulatedSuspendedTickValue
    case 91: resp->copy_handles[resp->ncopy++] = new_event(); break; // GetAccumulatedSuspendedTickChangedEvent
    default: UNHANDLED("ISelfController");
    }
}

static void window_controller(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1: OUT(uint64_t, 0) = 0x1234; resp_size(resp, 8); break;  // GetAppletResourceUserId
    case 2: OUT(uint64_t, 0) = 0x1234; resp_size(resp, 8); break;  // GetAppletResourceUserIdOfCallerApplet
    case 10: break;  // AcquireForegroundRights
    default: UNHANDLED("IWindowController");
    }
}

static void generic_ok(Service *s, IpcReq *req, IpcResp *resp) {
    LOG("[am] %s cmd %u (ok)\n", s->name, req->cmd);
    resp_size(resp, 0x10);
}

// ------------------------------------------------------------------ IStorage / IStorageAccessor
typedef struct { uint8_t *data; uint64_t size; } AmStorage;

static void storage_accessor(Service *s, IpcReq *req, IpcResp *resp) {
    AmStorage *st = s->state;
    switch (req->cmd) {
    case 0: OUT(int64_t, 0) = (int64_t)st->size; resp_size(resp, 8); break;  // GetSize
    case 10: {  // Write(offset, A)
        int64_t off = IN(int64_t, 0);
        IpcBuf *a = in_buf(req, 0);
        uint64_t n = a->size;
        if ((uint64_t)off + n > st->size) n = st->size - (uint64_t)off;
        memcpy(st->data + off, (void *)(uintptr_t)a->addr, n);
        break;
    }
    case 11: {  // Read(offset, B)
        int64_t off = IN(int64_t, 0);
        IpcBuf *b = out_buf(req, 0);
        uint64_t n = b->size;
        if ((uint64_t)off + n > st->size) n = st->size - (uint64_t)off;
        memcpy((void *)(uintptr_t)b->addr, st->data + off, n);
        break;
    }
    default: LOG("[am] IStorageAccessor cmd %u (ok)\n", req->cmd);
    }
}

static void am_storage(Service *s, IpcReq *req, IpcResp *resp) {
    if (req->cmd == 0) ipc_return_object(resp, service_new("IStorageAccessor", storage_accessor, s->state));
    else LOG("[am] IStorage cmd %u (ok)\n", req->cmd);
}

static AmStorage *alloc_am_storage(const void *data, uint64_t size) {
    AmStorage *st = calloc(1, sizeof *st);
    if (!st) return NULL;
    st->data = calloc(1, size ? (size_t)size : 1);
    if (!st->data) { free(st); return NULL; }
    st->size = size;
    if (data && size) memcpy(st->data, data, (size_t)size);
    return st;
}

static Service *service_for_am_storage(AmStorage *st) {
    return st ? service_new("IStorage", am_storage, st) : NULL;
}

static Service *new_am_storage(const void *data, uint64_t size) {
    return service_for_am_storage(alloc_am_storage(data, size));
}

// ------------------------------------------------------------------ ILibraryAppletCreator / Software Keyboard library applet
// Scarlet's recompiled NintendoSDK now talks to this service through the same
// AM protocol used on Horizon. The visible keyboard is a host frontend only;
// ShowKeyboard itself is no longer replaced by HLE.
#define SWKBD_APPLET_ID 0x11u
#define SWKBD_STRING_BUFFER 0x7D4u
#define SWKBD_CONFIG_COMMON 0x3D4u

typedef struct {
    pthread_mutex_t lock;
    uint32_t applet_id, mode;
    AmStorage *in[8];
    int nin;
    AmStorage *out;
    AmStorage *interactive_out;
    KEvent *state_event, *out_event, *interactive_event;
    uint32_t state_r, state_w, out_r, out_w, interactive_r, interactive_w;
    bool started, completed, worker_running, awaiting_text_check;
    bool use_utf8, use_text_check, bypass_text_check_once;
    int min_len, max_len;
    char title[320], sub[640], guide[1100], initial[2100];
    char candidate[2100], error[2100];
} SwkbdApplet;

static void am_event_signal(KEvent *e) {
    if (!e) return;
    pthread_mutex_lock(&g_klock);
    kernel_signal(e);
    pthread_mutex_unlock(&g_klock);
}

static void am_event_clear(KEvent *e) {
    if (!e) return;
    pthread_mutex_lock(&g_klock);
    e->signaled = false;
    pthread_mutex_unlock(&g_klock);
}

static size_t swkbd_u16_to_utf8(const uint16_t *src, size_t units, char *out, size_t cap) {
    size_t o = 0;
    if (!out || !cap) return 0;
    for (size_t i = 0; src && i < units && src[i]; i++) {
        uint32_t cp = src[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units &&
            src[i + 1] >= 0xDC00 && src[i + 1] <= 0xDFFF)
            cp = 0x10000 + ((cp - 0xD800) << 10) + (src[++i] - 0xDC00);
        unsigned char b[4];
        int n;
        if (cp < 0x80) { b[0] = (unsigned char)cp; n = 1; }
        else if (cp < 0x800) {
            b[0] = 0xC0 | (cp >> 6); b[1] = 0x80 | (cp & 0x3F); n = 2;
        } else if (cp < 0x10000) {
            b[0] = 0xE0 | (cp >> 12); b[1] = 0x80 | ((cp >> 6) & 0x3F);
            b[2] = 0x80 | (cp & 0x3F); n = 3;
        } else {
            b[0] = 0xF0 | (cp >> 18); b[1] = 0x80 | ((cp >> 12) & 0x3F);
            b[2] = 0x80 | ((cp >> 6) & 0x3F); b[3] = 0x80 | (cp & 0x3F); n = 4;
        }
        if (o + (size_t)n + 1 > cap) break;
        memcpy(out + o, b, (size_t)n); o += (size_t)n;
    }
    out[o] = 0;
    return o;
}

static size_t swkbd_utf8_to_u16(const char *src, uint16_t *out, size_t cap) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)(src ? src : "");
    if (!out || !cap) return 0;
    while (*p && o + 1 < cap) {
        uint32_t cp;
        int n;
        if (*p < 0x80) { cp = *p; n = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; n = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; n = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 7; n = 4; }
        else { p++; continue; }
        bool valid = true;
        for (int i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (!valid) { p++; continue; }
        if (cp >= 0x10000) {
            if (o + 2 >= cap) break;
            cp -= 0x10000;
            out[o++] = (uint16_t)(0xD800 | (cp >> 10));
            out[o++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        } else out[o++] = (uint16_t)cp;
        p += n;
    }
    out[o] = 0;
    return o;
}

static void swkbd_complete_locked(SwkbdApplet *a, uint32_t result, const char *text) {
    uint8_t reply[4 + SWKBD_STRING_BUFFER];
    memset(reply, 0, sizeof reply);
    memcpy(reply, &result, 4);
    if (result == 0 && text) {
        if (a->use_utf8)
            snprintf((char *)reply + 4, SWKBD_STRING_BUFFER, "%s", text);
        else
            swkbd_utf8_to_u16(text, (uint16_t *)(reply + 4), SWKBD_STRING_BUFFER / 2);
    }
    a->out = alloc_am_storage(reply, sizeof reply);
    a->completed = true;
    a->awaiting_text_check = false;
    LOG("[swkbd-applet] complete result=%u text=\"%s\"\n", result, text ? text : "");
    am_event_signal(a->out_event);
    am_event_signal(a->state_event);
}

static void swkbd_queue_text_check_locked(SwkbdApplet *a, const char *text) {
    uint8_t reply[8 + SWKBD_STRING_BUFFER];
    memset(reply, 0, sizeof reply);
    if (a->use_utf8) {
        uint64_t n = strlen(text) + 1;
        if (n > SWKBD_STRING_BUFFER) n = SWKBD_STRING_BUFFER;
        memcpy(reply, &n, 8);
        memcpy(reply + 8, text, (size_t)(n ? n - 1 : 0));
    } else {
        uint16_t tmp[SWKBD_STRING_BUFFER / 2] = {0};
        size_t units = swkbd_utf8_to_u16(text, tmp, SWKBD_STRING_BUFFER / 2);
        uint64_t bytes = (units + 1) * 2;
        memcpy(reply, &bytes, 8);
        memcpy(reply + 8, tmp, bytes <= SWKBD_STRING_BUFFER ? (size_t)bytes : SWKBD_STRING_BUFFER);
    }
    a->interactive_out = alloc_am_storage(reply, sizeof reply);
    a->awaiting_text_check = true;
    LOG("[swkbd-applet] candidate -> TextCheck \"%s\"\n", text);
    am_event_signal(a->interactive_event);
}

static bool swkbd_prepare(SwkbdApplet *a) {
    AmStorage *cfg = NULL, *work = NULL;
    int cfg_index = -1;
    for (int i = 0; i < a->nin; i++) {
        AmStorage *st = a->in[i];
        if (!st) continue;
        if (!cfg && st->size >= SWKBD_CONFIG_COMMON && st->size <= 0x1000) {
            cfg = st; cfg_index = i; continue;
        }
    }
    if (!cfg) {
        LOG("[swkbd-applet] no config storage among %d PushInData objects\n", a->nin);
        return false;
    }
    for (int i = cfg_index + 1; i < a->nin; i++)
        if (a->in[i] && a->in[i]->size) { work = a->in[i]; break; }

    const uint8_t *p = cfg->data;
    a->max_len = *(const int32_t *)(p + 0x3AC);
    a->min_len = *(const int32_t *)(p + 0x3B0);
    a->use_utf8 = p[0x3BD] != 0;
    a->use_text_check = p[0x3D0] != 0;
    if (a->max_len <= 0 || a->max_len > 500) a->max_len = 500;
    if (a->min_len < 0 || a->min_len > a->max_len) a->min_len = 0;

    swkbd_u16_to_utf8((const uint16_t *)(p + 0x24), 65, a->title, sizeof a->title);
    swkbd_u16_to_utf8((const uint16_t *)(p + 0xA6), 129, a->sub, sizeof a->sub);
    swkbd_u16_to_utf8((const uint16_t *)(p + 0x1A8), 257, a->guide, sizeof a->guide);
    a->initial[0] = 0;
    uint32_t off = *(const uint32_t *)(p + 0x3C0);
    uint32_t len = *(const uint32_t *)(p + 0x3C4);
    if (work && len && off < work->size) {
        uint64_t avail = work->size - off;
        size_t units = len;
        if (units * 2 > avail) units = (size_t)(avail / 2);
        swkbd_u16_to_utf8((const uint16_t *)(work->data + off), units, a->initial, sizeof a->initial);
    }
    LOG("[swkbd-applet] config=%llx bytes work=%llx max=%d min=%d utf8=%d textCheck=%d title=\"%s\"\n",
        (unsigned long long)cfg->size, (unsigned long long)(work ? work->size : 0),
        a->max_len, a->min_len, a->use_utf8, a->use_text_check, a->title);
    return true;
}

static void *swkbd_frontend_worker(void *opaque) {
    SwkbdApplet *a = opaque;
    char title[sizeof a->title], sub[sizeof a->sub], guide[sizeof a->guide];
    char initial[sizeof a->initial], error[sizeof a->error], text[sizeof a->candidate];
    int min_len, max_len;

    pthread_mutex_lock(&a->lock);
    snprintf(title, sizeof title, "%s", a->title);
    snprintf(sub, sizeof sub, "%s", a->sub);
    snprintf(guide, sizeof guide, "%s", a->guide);
    snprintf(initial, sizeof initial, "%s", a->candidate[0] ? a->candidate : a->initial);
    snprintf(error, sizeof error, "%s", a->error);
    min_len = a->min_len; max_len = a->max_len;
    pthread_mutex_unlock(&a->lock);

#ifdef _WIN32
    int ok = plat_swkbd_applet_show(title, sub, guide, initial, error,
                                    min_len, max_len, text, sizeof text);
#else
    int ok = 0;
    text[0] = 0;
#endif

    pthread_mutex_lock(&a->lock);
    a->worker_running = false;
    if (!ok) {
        swkbd_complete_locked(a, 1, "");
    } else {
        snprintf(a->candidate, sizeof a->candidate, "%s", text);
        a->error[0] = 0;
        if (a->bypass_text_check_once) {
            a->bypass_text_check_once = false;
            swkbd_complete_locked(a, 0, a->candidate);
        } else if (a->use_text_check) {
            swkbd_queue_text_check_locked(a, a->candidate);
        } else {
            swkbd_complete_locked(a, 0, a->candidate);
        }
    }
    pthread_mutex_unlock(&a->lock);
    return NULL;
}

static void swkbd_start_frontend_locked(SwkbdApplet *a) {
    if (a->worker_running || a->completed) return;
    a->worker_running = true;
    pthread_t th;
    if (pthread_create(&th, NULL, swkbd_frontend_worker, a) == 0) pthread_detach(th);
    else {
        a->worker_running = false;
        swkbd_complete_locked(a, 1, "");
    }
}

static AmStorage *req_storage(IpcReq *req) {
    for (int i = 0; i < req->nin_objects; i++) {
        Service *o = req->in_objects[i];
        if (o && o->state && o->name && !strcmp(o->name, "IStorage")) return (AmStorage *)o->state;
    }
    return NULL;
}

static void library_applet_accessor(Service *s, IpcReq *req, IpcResp *resp) {
    SwkbdApplet *a = s->state;
    switch (req->cmd) {
    case 0: // GetAppletStateChangedEvent
        resp->copy_handles[resp->ncopy++] = a->state_r;
        break;
    case 1: // IsCompleted
        pthread_mutex_lock(&a->lock);
        OUT(uint8_t, 0) = a->completed ? 1 : 0;
        pthread_mutex_unlock(&a->lock);
        resp_size(resp, 1);
        break;
    case 10: { // Start
        pthread_mutex_lock(&a->lock);
        if (!a->started) {
            a->started = true;
            if (!swkbd_prepare(a)) swkbd_complete_locked(a, 1, "");
            else swkbd_start_frontend_locked(a);
        }
        pthread_mutex_unlock(&a->lock);
        break;
    }
    case 20: case 25: // RequestExit / Terminate
        pthread_mutex_lock(&a->lock);
        if (!a->completed) swkbd_complete_locked(a, 1, "");
        pthread_mutex_unlock(&a->lock);
        break;
    case 30: // GetResult
        resp->result = 0;
        break;
    case 50: case 60: case 90: // focus/GPU slice/newer firmware helper
        break;
    case 100: { // PushInData
        AmStorage *st = req_storage(req);
        pthread_mutex_lock(&a->lock);
        if (st && a->nin < (int)(sizeof a->in / sizeof a->in[0])) {
            a->in[a->nin++] = st;
            LOG("[swkbd-applet] PushInData #%d size=%llx\n", a->nin,
                (unsigned long long)st->size);
        } else LOG("[swkbd-applet] PushInData missing IStorage object\n");
        pthread_mutex_unlock(&a->lock);
        break;
    }
    case 101: { // PopOutData
        pthread_mutex_lock(&a->lock);
        AmStorage *st = a->out;
        a->out = NULL;
        if (st) {
            ipc_return_object(resp, service_for_am_storage(st));
            am_event_clear(a->out_event);
        } else resp->result = AM_NO_DATA;
        pthread_mutex_unlock(&a->lock);
        break;
    }
    case 103: { // PushInteractiveInData: TextCheck response
        AmStorage *st = req_storage(req);
        pthread_mutex_lock(&a->lock);
        if (!st || st->size < 4) {
            LOG("[swkbd-applet] invalid TextCheck storage\n");
            pthread_mutex_unlock(&a->lock);
            break;
        }
        uint32_t check = *(const uint32_t *)st->data;
        char message[sizeof a->error] = "";
        if (st->size > 4) {
            if (a->use_utf8)
                snprintf(message, sizeof message, "%.*s",
                         (int)((st->size - 4) < sizeof message - 1 ? st->size - 4 : sizeof message - 1),
                         (const char *)st->data + 4);
            else
                swkbd_u16_to_utf8((const uint16_t *)(st->data + 4),
                                  (size_t)((st->size - 4) / 2), message, sizeof message);
        }
        a->awaiting_text_check = false;
        LOG("[swkbd-applet] TextCheck result=%u message=\"%s\"\n", check, message);
        if (check == 0) {
            swkbd_complete_locked(a, 0, a->candidate);
        } else {
            if (check == 1 || check == 2) snprintf(a->error, sizeof a->error, "%s", message);
            else a->error[0] = 0;
            // Confirm means the next OK is an explicit confirmation and should
            // bypass a second callback, matching the applet's "use anyway" path.
            a->bypass_text_check_once = check == 2;
            swkbd_start_frontend_locked(a);
        }
        pthread_mutex_unlock(&a->lock);
        break;
    }
    case 104: { // PopInteractiveOutData
        pthread_mutex_lock(&a->lock);
        AmStorage *st = a->interactive_out;
        a->interactive_out = NULL;
        if (st) {
            ipc_return_object(resp, service_for_am_storage(st));
            am_event_clear(a->interactive_event);
        } else resp->result = AM_NO_DATA;
        pthread_mutex_unlock(&a->lock);
        break;
    }
    case 105:
        resp->copy_handles[resp->ncopy++] = a->out_r;
        break;
    case 106:
        resp->copy_handles[resp->ncopy++] = a->interactive_r;
        break;
    case 110:
        OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    case 120:
        memset(resp->data, 0, 0x10); resp_size(resp, 0x10); break;
    case 160:
        OUT(uint64_t, 0) = 0; resp_size(resp, 8); break;
    case 170:
        resp->copy_handles[resp->ncopy++] = new_event(); break;
    default:
        UNHANDLED("ILibraryAppletAccessor");
    }
}

static SwkbdApplet *new_swkbd_applet(uint32_t id, uint32_t mode) {
    SwkbdApplet *a = calloc(1, sizeof *a);
    if (!a) return NULL;
    pthread_mutex_init(&a->lock, NULL);
    a->applet_id = id; a->mode = mode;
    a->state_event = kernel_create_event_pair(&a->state_w, &a->state_r);
    a->out_event = kernel_create_event_pair(&a->out_w, &a->out_r);
    a->interactive_event = kernel_create_event_pair(&a->interactive_w, &a->interactive_r);
    return a;
}

static void library_applet_creator(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 0: case 3: { // CreateLibraryApplet / CreateLibraryAppletEx
        uint32_t id = req->data_size >= 4 ? IN(uint32_t, 0) : 0;
        uint32_t mode = req->data_size >= 8 ? IN(uint32_t, 4) : 0;
        LOG("[am] CreateLibraryApplet id=%#x mode=%u\n", id, mode);
        if (id != SWKBD_APPLET_ID) {
            LOG("[am] unsupported library applet %#x\n", id);
            resp->result = MAKE_RESULT(128, 1);
            break;
        }
        SwkbdApplet *a = new_swkbd_applet(id, mode);
        ipc_return_object(resp, service_new("ILibraryAppletAccessor", library_applet_accessor, a));
        break;
    }
    case 1: // TerminateAllLibraryApplets
        break;
    case 2: // AreAnyLibraryAppletsLeft
        OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;
    case 10: { // CreateStorage
        int64_t n = req->data_size >= 8 ? IN(int64_t, 0) : 0;
        if (n <= 0 || n > (64ll << 20)) {
            LOG("[am] CreateStorage invalid size %lld\n", (long long)n);
            resp->result = MAKE_RESULT(128, 1);
        } else ipc_return_object(resp, new_am_storage(NULL, (uint64_t)n));
        break;
    }
    case 11: { // CreateTransferMemoryStorage(bool writable, s64 size, handle)
        int64_t n = req->data_size >= 16 ? IN(int64_t, 8) : 0;
        KTransferMem *tm = NULL;
        for (int i = 0; i < req->ncopy && !tm; i++) {
            KObject *o = handle_get(req->copy_handles[i]);
            if (o && o->type == KOBJ_TMEM) tm = (KTransferMem *)o;
        }
        if (!tm) for (int i = 0; i < req->nmove && !tm; i++) {
            KObject *o = handle_get(req->move_handles[i]);
            if (o && o->type == KOBJ_TMEM) tm = (KTransferMem *)o;
        }
        if (n <= 0 && tm) n = (int64_t)tm->size;
        if (!tm || n <= 0) {
            LOG("[am] CreateTransferMemoryStorage missing transfer memory (size=%lld)\n", (long long)n);
            resp->result = MAKE_RESULT(128, 1);
        } else {
            uint64_t copy = (uint64_t)n < tm->size ? (uint64_t)n : tm->size;
            ipc_return_object(resp, new_am_storage((const void *)(uintptr_t)tm->addr, copy));
        }
        break;
    }
    case 12: { // CreateHandleStorage(s64 size, handle)
        int64_t n = req->data_size >= 8 ? IN(int64_t, 0) : 0;
        if (n <= 0) n = 1;
        ipc_return_object(resp, new_am_storage(NULL, (uint64_t)n));
        break;
    }
    default:
        UNHANDLED("ILibraryAppletCreator");
    }
}

extern const uint8_t g_user_id[16];

const char *g_game_version = "1.0.0";  // the set-up game's version (runtime/gamesetup.c game_version)

static void application_functions(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    switch (req->cmd) {
    case 1:  // PopLaunchParameter(kind)
        if (IN(uint32_t, 0) == 2 && !preselected_popped) {  // PreselectedUser
            uint8_t p[0x88] = {0};
            *(uint32_t *)p = 0xC79497CA;
            *(uint32_t *)(p + 4) = 1;
            memcpy(p + 8, g_user_id, 16);
            preselected_popped = true;
            ipc_return_object(resp, new_am_storage(p, sizeof p));
        } else {
            resp->result = AM_NO_DATA;
        }
        break;
    case 20: OUT(uint64_t, 0) = 0; resp_size(resp, 8); break;  // EnsureSaveData
    case 21: memset(resp->data, 0, 8); memcpy(resp->data, game_language(), strlen(game_language())); resp_size(resp, 8);  // GetDesiredLanguage
        LOG("[am] GetDesiredLanguage: %s\n", game_language()); break;
    case 22: break;  // SetTerminateResult(Result)
    case 23: {  // GetDisplayVersion: the version of the set-up build (runtime/main.c)
        extern const char *g_game_version;
        memset(resp->data, 0, 0x10);
        memcpy(resp->data, g_game_version, strlen(g_game_version));
        resp_size(resp, 0x10);
        break;
    }
    case 40: OUT(uint8_t, 0) = 0; resp_size(resp, 1); break;  // NotifyRunning
    case 130: case 140: resp->copy_handles[resp->ncopy++] = new_event(); break;
    default: UNHANDLED("IApplicationFunctions");
    }
}

static void application_proxy(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    static const struct { int cmd; const char *name; ServiceHandler h; } subs[] = {
        {0, "ICommonStateGetter", common_state_getter},
        {1, "ISelfController", self_controller},
        {2, "IWindowController", window_controller},
        {3, "IAudioController", generic_ok},
        {4, "IDisplayController", generic_ok},
        {10, "IProcessWindingController", generic_ok},
        {11, "ILibraryAppletCreator", library_applet_creator},
        {20, "IApplicationFunctions", application_functions},
        {1000, "IDebugFunctions", generic_ok},
    };
    for (unsigned i = 0; i < sizeof subs / sizeof *subs; i++) {
        if (subs[i].cmd == (int)req->cmd) {
            ipc_return_object(resp, service_new(subs[i].name, subs[i].h, NULL));
            return;
        }
    }
    UNHANDLED("IApplicationProxy");
}

SERVICE("appletOE", applet_oe) {
    (void)s;
    am_init_once();
    if (req->cmd == 0) ipc_return_object(resp, service_new("IApplicationProxy", application_proxy, NULL));
    else UNHANDLED("appletOE");
}

// ------------------------------------------------------------------ apm
static void apm_session(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s;
    if (req->cmd == 1) { OUT(uint32_t, 0) = 0x92220008; resp_size(resp, 4); }  // GetPerformanceConfiguration
}

SERVICE("apm", apm) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("apm:ISession", apm_session, NULL));
    else if (req->cmd == 1) { OUT(uint32_t, 0) = 1; resp_size(resp, 4); }
}

// ------------------------------------------------------------------ lm (guest log output)
static void lm_logger(Service *s, IpcReq *req, IpcResp *resp) {
    (void)s; (void)resp;
    if (req->cmd != 0) return;
    IpcBuf *b = in_buf(req, 0);
    if (!b || !b->size) return;
    // Log packets are TLV chunks after a 0x18-byte header; print the string chunks.
    const uint8_t *p = (const uint8_t *)(uintptr_t)b->addr, *end = p + b->size;
    p += 0x18;
    char line[1024];
    int n = 0;
    while (p + 2 <= end) {
        uint8_t key = p[0], len = p[1];
        p += 2;
        if (p + len > end) break;
        if (key == 2 || key == 4 || key == 5 || key == 6 || key == 7) {  // message, file, function, module, thread
            n += snprintf(line + n, sizeof line - n, "%s%.*s", n ? " | " : "", len, (const char *)p);
            if (n >= (int)sizeof line) break;
        }
        p += len;
    }
    if (n) LOG("[lm] %s\n", line);
}

SERVICE("lm", lm) {
    (void)s;
    if (req->cmd == 0) ipc_return_object(resp, service_new("lm:ILogger", lm_logger, NULL));
}
