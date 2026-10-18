// nn::swkbd::ShowKeyboard -> host text input.
//
// The real Nintendo SDK launches the software-keyboard applet, then copies the
// applet result into nn::swkbd::String. On Windows we use a native modal text
// box centered over the Scarlet window; no Vulkan work is performed from the
// blocking game thread.
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

#ifndef _WIN32
int menu_keyboard_run(const char *initial, int max_len, char *out, int cap);
#endif

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

// nn::swkbd::ShowKeyboard(nn::swkbd::String *out, const nn::swkbd::ShowKeyboardArg &arg) -> nn::Result
// String = {void *ptr; size_t byteCapacity}
HLE_FUNC("_ZN2nn5swkbd12ShowKeyboardEPNS0_6StringERKNS0_15ShowKeyboardArgE", swkbd_show) {
    uint64_t out_obj = c->x[0], arg = c->x[1];
    uint64_t ptr = LD64(out_obj), size = LD64(out_obj + 8);

    int32_t max_len = *(const int32_t *)(uintptr_t)(arg + ARG_MAXLEN);
    int32_t min_len = *(const int32_t *)(uintptr_t)(arg + ARG_MINLEN);
    uint8_t use_utf8 = *(const uint8_t *)(uintptr_t)(arg + ARG_UTF8);
    uint32_t init_off = *(const uint32_t *)(uintptr_t)(arg + ARG_INIT_OFF);
    uint32_t init_len = *(const uint32_t *)(uintptr_t)(arg + ARG_INIT_LEN);
    uint64_t work = LD64(arg + ARG_WORKBUF);

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
    if (work && init_len)
        u16_to_utf8((const uint16_t *)(uintptr_t)(work + init_off), init_len, initial, sizeof initial);

#ifdef _WIN32
    int ok = plat_text_input_dialog(title, sub, guide, initial, min_len, max_len, text, sizeof text);
#else
    int ok = menu_keyboard_run(initial, max_len, text, sizeof text);
#endif

    LOG("[swkbd] text input max=%d min=%d utf8=%u initial=\"%s\" -> %s%s%s\n",
        max_len, min_len, use_utf8, initial,
        ok ? "entered \"" : "cancelled", ok ? text : "", ok ? "\"" : "");

    if (!ok) {
        c->x[0] = SWKBD_RESULT_CANCELED;
        return;
    }

    if (ptr && size) {
        memset((void *)(uintptr_t)ptr, 0, (size_t)size);
        if (use_utf8) {
            // isUseUtf8 is part of ShowKeyboardArg. Scarlet uses this path for
            // the player name; returning UTF-16 here leaves game-side validation
            // looking at "r\0u\0...", which was the post-name hang.
            snprintf((char *)(uintptr_t)ptr, (size_t)size, "%s", text);
        } else {
            uint16_t *dst = (uint16_t *)(uintptr_t)ptr;
            size_t cap_units = (size_t)size / 2;
            utf8_to_u16(text, dst, cap_units, (size_t)max_len);
        }
    }

    c->x[0] = 0;
}
