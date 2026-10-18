// nn::swkbd::ShowKeyboard -> host text input.
//
// The real Nintendo SDK launches the software-keyboard applet, then copies the
// applet result into nn::swkbd::String. This port keeps that blocking SDK call,
// while runtime/ui/menu.mm renders an in-window Switch-style keyboard on the
// normal render thread.
//
// ShowKeyboardArg layout used by this SDK:
//   +0x024 header UTF-16[65]
//   +0x0a6 sub text UTF-16[129]
//   +0x1a8 guide text UTF-16[257]
//   +0x3ac max length (s32)
//   +0x3b0 min length (s32)
//   +0x3bd isUseUtf8 (u8)
//   +0x3c0 initial text offset in work buffer (u32)
//   +0x3c4 initial text UTF-16 length (u32)
//   +0x4d0 work buffer pointer
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../hle.h"
#include "../platform.h"

// Switch-style in-window software keyboard (runtime/ui/menu.mm). The game
// thread waits here while SDL/main + render threads keep presenting the applet.
int menu_keyboard_run(const char *initial, int min_len, int max_len,
                      const char *title, const char *sub, const char *guide,
                      const char *error, int prompt_mode,
                      char *out, int cap);

#define SWKBD_RESULT_CANCELED 0x29Fu
enum {
    ARG_HEADER = 0x24,
    ARG_SUB = 0xa6,
    ARG_GUIDE = 0x1a8,
    ARG_MAXLEN = 0x3ac,
    ARG_MINLEN = 0x3b0,
    ARG_UTF8 = 0x3bd,
    ARG_INIT_OFF = 0x3c0,
    ARG_INIT_LEN = 0x3c4,
    ARG_WORKBUF = 0x4d0,
    ARG_WORKBUF_SIZE = 0x4d8,
    ARG_CHECKBUF = 0x4e0,
    ARG_CHECKBUF_SIZE = 0x4e8,
    HEADER_N = 65,
    SUB_N = 129,
    GUIDE_N = 257,
};

static size_t u16_to_utf8(const uint16_t *src, size_t units, char *out, size_t cap) {
    size_t o = 0;
    if (!cap) return 0;
    for (size_t i = 0; i < units && src[i]; i++) {
        uint32_t cp = src[i];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < units &&
            src[i + 1] >= 0xDC00 && src[i + 1] <= 0xDFFF) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (src[++i] - 0xDC00);
        }

        unsigned char b[4];
        int n;
        if (cp < 0x80) {
            b[0] = (unsigned char)cp; n = 1;
        } else if (cp < 0x800) {
            b[0] = 0xC0 | (cp >> 6);
            b[1] = 0x80 | (cp & 0x3F);
            n = 2;
        } else if (cp < 0x10000) {
            b[0] = 0xE0 | (cp >> 12);
            b[1] = 0x80 | ((cp >> 6) & 0x3F);
            b[2] = 0x80 | (cp & 0x3F);
            n = 3;
        } else {
            b[0] = 0xF0 | (cp >> 18);
            b[1] = 0x80 | ((cp >> 12) & 0x3F);
            b[2] = 0x80 | ((cp >> 6) & 0x3F);
            b[3] = 0x80 | (cp & 0x3F);
            n = 4;
        }
        if (o + (size_t)n + 1 > cap) break;
        memcpy(out + o, b, (size_t)n);
        o += (size_t)n;
    }
    out[o] = 0;
    return o;
}

static size_t utf8_to_u16(const char *src, uint16_t *out, size_t cap_units, size_t max_units) {
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)src;
    if (!cap_units) return 0;

    while (*p) {
        uint32_t cp;
        int n;
        if (*p < 0x80) { cp = *p; n = 1; }
        else if ((*p & 0xE0) == 0xC0) { cp = *p & 0x1F; n = 2; }
        else if ((*p & 0xF0) == 0xE0) { cp = *p & 0x0F; n = 3; }
        else if ((*p & 0xF8) == 0xF0) { cp = *p & 0x07; n = 4; }
        else { p++; continue; }

        bool valid = true;
        for (int i = 1; i < n; i++) {
            if ((p[i] & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (!valid) { p++; continue; }

        size_t need = cp >= 0x10000 ? 2 : 1;
        if (o + need > max_units || o + need + 1 > cap_units) break;
        if (need == 2) {
            cp -= 0x10000;
            out[o++] = (uint16_t)(0xD800 | (cp >> 10));
            out[o++] = (uint16_t)(0xDC00 | (cp & 0x3FF));
        } else {
            out[o++] = (uint16_t)cp;
        }
        p += n;
    }
    out[o] = 0;
    return o;
}


static uint64_t swkbd_store_string(uint64_t string_obj, const char *text, uint8_t use_utf8, size_t max_len) {
    uint64_t ptr = LD64(string_obj);
    uint64_t cap = LD64(string_obj + 8);
    if (!ptr || !cap) return 0;

    memset((void *)(uintptr_t)ptr, 0, (size_t)cap);
    if (use_utf8) {
        size_t n = strlen(text);
        if (n >= cap) n = (size_t)cap - 1;
        // Do not split a UTF-8 continuation sequence at the buffer boundary.
        while (n && (((const unsigned char *)text)[n] & 0xC0) == 0x80) n--;
        memcpy((void *)(uintptr_t)ptr, text, n);
        ((char *)(uintptr_t)ptr)[n] = 0;
        return (uint64_t)n + 1;
    }

    size_t cap_units = (size_t)cap / 2;
    if (!cap_units) return 0;
    size_t units = utf8_to_u16(text, (uint16_t *)(uintptr_t)ptr, cap_units, max_len);
    return (uint64_t)(units + 1) * 2;
}

static void swkbd_check_message_utf8(uint64_t check_buf, uint64_t check_cap, uint64_t message_size,
                                     uint8_t use_utf8, char *out, size_t out_cap) {
    if (!out_cap) return;
    out[0] = 0;
    if (!check_buf || !check_cap) return;

    uint64_t n = message_size && message_size < check_cap ? message_size : check_cap;
    if (use_utf8) {
        if (n >= out_cap) n = out_cap - 1;
        memcpy(out, (const void *)(uintptr_t)check_buf, (size_t)n);
        out[n] = 0;
        // The SDK buffer is normally terminated, but enforce it if outMessageSize included extra bytes.
        for (size_t i = 0; i < (size_t)n; i++) if (!out[i]) return;
        out[n] = 0;
    } else {
        size_t units = (size_t)(n / 2);
        u16_to_utf8((const uint16_t *)(uintptr_t)check_buf, units, out, out_cap);
    }
}

// nn::swkbd::ShowKeyboard(nn::swkbd::String *out, const nn::swkbd::ShowKeyboardArg &arg) -> nn::Result
// String = {void *ptr; size_t byteCapacity}
HLE_FUNC("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_show) {
    uint64_t out_obj = c->x[0], arg = c->x[1];
    uint64_t ptr = LD64(out_obj), size = LD64(out_obj + 8);

    int32_t max_len = *(const int32_t *)(uintptr_t)(arg + ARG_MAXLEN);
    int32_t min_len = *(const int32_t *)(uintptr_t)(arg + ARG_MINLEN);
    uint8_t use_utf8 = *(const uint8_t *)(uintptr_t)(arg + ARG_UTF8);
    uint8_t use_text_check = *(const uint8_t *)(uintptr_t)(arg + 0x3d0);
    uint64_t text_check_cb = LD64(arg + 0x3d8);
    uint32_t init_off = *(const uint32_t *)(uintptr_t)(arg + ARG_INIT_OFF);
    uint32_t init_len = *(const uint32_t *)(uintptr_t)(arg + ARG_INIT_LEN);
    uint64_t work = LD64(arg + ARG_WORKBUF);
    uint64_t work_size = LD64(arg + ARG_WORKBUF_SIZE);
    uint64_t check_buf = LD64(arg + ARG_CHECKBUF);
    uint64_t check_buf_size = LD64(arg + ARG_CHECKBUF_SIZE);

    if (max_len <= 0 || max_len > 500) max_len = 500;
    if (min_len < 0 || min_len > max_len) min_len = 0;

    char title[HEADER_N * 4 + 1] = "";
    char sub[SUB_N * 4 + 1] = "";
    char guide[GUIDE_N * 4 + 1] = "";
    char initial[501 * 4 + 1] = "";
    char text[501 * 4 + 1] = "";

    u16_to_utf8((const uint16_t *)(uintptr_t)(arg + ARG_HEADER), HEADER_N, title, sizeof title);
    u16_to_utf8((const uint16_t *)(uintptr_t)(arg + ARG_SUB), SUB_N, sub, sizeof sub);
    u16_to_utf8((const uint16_t *)(uintptr_t)(arg + ARG_GUIDE), GUIDE_N, guide, sizeof guide);
    if (work && init_len && (!work_size || (uint64_t)init_off + (uint64_t)init_len * 2 <= work_size))
        u16_to_utf8((const uint16_t *)(uintptr_t)(work + init_off), init_len, initial, sizeof initial);

    char error[0x800] = "";
    char current[sizeof initial];
    snprintf(current, sizeof current, "%s", initial);
    int ok = 0;

    for (;;) {
        ok = menu_keyboard_run(current, min_len, max_len, title, sub, guide,
                               error, 0, text, sizeof text);
        if (!ok) break;

        LOG("[swkbd] candidate max=%d min=%d utf8=%u textCheck=%u callback=%llx -> \"%s\"\n",
            max_len, min_len, use_utf8, use_text_check,
            (unsigned long long)text_check_cb, text);

        // Store the candidate in the caller's nn::swkbd::String first. The NintendoSDK
        // text-check callback receives that same String object, with bufSize temporarily
        // changed from capacity to the actual encoded string size.
        uint64_t bytes = swkbd_store_string(out_obj, text, use_utf8, (size_t)max_len);

        if (!use_text_check || !text_check_cb) break;

        if (!check_buf || check_buf_size < 2) {
            LOG("[swkbd] text-check requested but ShowKeyboardArg has no work buffer (buf=%llx size=%llu); accepting candidate safely\n",
                (unsigned long long)check_buf, (unsigned long long)check_buf_size);
            break;
        }

        // NintendoSDK ABI:
        //   TextCheckResult cb(void *textCheckWorkBuf, size_t *outMessageSize, String *text)
        // Scarlet uses the pre-8.0 64-bit size_t form (the callback symbol encodes Pm).
        memset((void *)(uintptr_t)check_buf, 0, (size_t)check_buf_size);
        uint64_t *message_size = (uint64_t *)hle_alloc(sizeof(uint64_t));
        *message_size = 0;

        uint64_t saved_cap = LD64(out_obj + 8);
        uint64_t saved_regs[12];
        for (int i = 0; i < 12; i++) saved_regs[i] = c->x[19 + i];

        *(uint64_t *)(uintptr_t)(out_obj + 8) = bytes;
        c->x[0] = check_buf;
        c->x[1] = (uint64_t)(uintptr_t)message_size;
        c->x[2] = out_obj;
        LOG("[swkbd] text-check call cb=%llx work=%llx workSize=%llu sizePtr=%llx string=%llx bytes=%llu\n",
            (unsigned long long)text_check_cb, (unsigned long long)check_buf,
            (unsigned long long)check_buf_size, (unsigned long long)(uintptr_t)message_size,
            (unsigned long long)out_obj, (unsigned long long)bytes);

        guest_call(c, text_check_cb);
        uint32_t check = (uint32_t)c->x[0];

        for (int i = 0; i < 12; i++) c->x[19 + i] = saved_regs[i];
        *(uint64_t *)(uintptr_t)(out_obj + 8) = saved_cap;

        char check_message[0x1000];
        swkbd_check_message_utf8(check_buf, check_buf_size, *message_size, use_utf8,
                                 check_message, sizeof check_message);
        LOG("[swkbd] text-check result=%u messageSize=%llu message=\"%s\"\n",
            check, (unsigned long long)*message_size, check_message);

        if (check == 0) break;  // Success

        snprintf(current, sizeof current, "%s", text);
        if (check == 2) {       // ShowConfirmDialog
            int prompt = menu_keyboard_run(current, min_len, max_len, title, sub, guide,
                                           check_message[0] ? check_message : "Use this text anyway?", 1,
                                           text, sizeof text);
            if (prompt == 2) {
                swkbd_store_string(out_obj, text, use_utf8, (size_t)max_len);
                break;
            }
            if (!prompt) { ok = 0; break; }
            snprintf(current, sizeof current, "%s", text);
            error[0] = 0;
        } else {                // ShowFailureDialog (and unknown values: reject safely)
            snprintf(error, sizeof error, "%s", check_message);
        }
    }

    LOG("[swkbd] final -> %s%s%s\n",
        ok ? "entered \"" : "cancelled", ok ? text : "", ok ? "\"" : "");

    if (!ok) {
        c->x[0] = SWKBD_RESULT_CANCELED;
        return;
    }

    if (ptr && size)
        swkbd_store_string(out_obj, text, use_utf8, (size_t)max_len);

    c->x[0] = 0;
}
