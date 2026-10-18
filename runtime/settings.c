#include "settings.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hle.h"
#include "input_bind.h"
#include "paths.h"

#define SETTINGS_FILE settings_file()

// LARECOMP_SETTINGS=<path>: another settings file (test runs never touch the player's larecomp.ini)
static const char *settings_file(void) {
    const char *e = getenv("LARECOMP_SETTINGS");
    return e && *e ? e : "larecomp.ini";
}

Settings g_settings = {
    .fullscreen = false,
    .fps = 30,
    .volume = 1.0f,
    .mouse_sens = 0.1f,
    .stick_deadzone = 0.15f,
    .reverb = true,
    .movie_audio = true,
    .show_fps = true,  // (Scarlet port: always show the frame rate while the port is being brought up)
    .upscaler = UPSCALE_SHARP,
    .antialias = AA_SMAA,  // (the game has no anti-aliasing of its own)
};

// Unknown keys (an older larecomp.ini: Link's Awakening's options from LAS Recomp) are ignored, and dropped at the
// next save.
void settings_load(void) {
    static bool loaded;
    if (loaded) return;
    loaded = true;
    FILE *f = fopen(SETTINGS_FILE, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[64];
        char val[128];
        if (sscanf(line, " %63[^= ] = %127[^\n]", key, val) < 1) continue;
        if (!strcmp(key, "game_lang") || !strcmp(key, "mod")) {  // (may be empty)
            if (sscanf(line, " %63[^= ] = %127[^\n]", key, val) != 2) val[0] = 0;
            for (size_t n = strlen(val); n && (val[n - 1] == '\r' || val[n - 1] == ' '); n--) val[n - 1] = 0;  // (edited by hand: CRLF)
            if (key[0] == 'g') strlcpy(g_settings.game_lang, val, sizeof g_settings.game_lang);
            else strlcpy(g_settings.mod, val, sizeof g_settings.mod);
            continue;
        }
        if (!strcmp(key, "fullscreen")) g_settings.fullscreen = atoi(val) != 0;
        else if (!strcmp(key, "fps")) g_settings.fps = atoi(val) == 60 ? 60 : 30;
        else if (!strcmp(key, "render_height")) g_settings.res_height = atoi(val) >= 360 && atoi(val) <= 4320 ? atoi(val) : 0;
        else if (!strcmp(key, "volume")) g_settings.volume = (float)atof(val);
        else if (!strcmp(key, "mouse_sens")) g_settings.mouse_sens = (float)atof(val);
        else if (!strcmp(key, "stick_deadzone")) g_settings.stick_deadzone = (float)atof(val);
        else if (!strcmp(key, "reverb")) g_settings.reverb = atoi(val) != 0;
        else if (!strcmp(key, "movie_audio")) g_settings.movie_audio = atoi(val) != 0;
        else if (!strcmp(key, "show_fps")) g_settings.show_fps = atoi(val) != 0;
        else if (!strcmp(key, "resolution")) {
            int n = atoi(val);
            g_settings.res_mode = !strcmp(val, "dynamic") ? RES_DYNAMIC : (!strcmp(val, "75%") || !strcmp(val, "67%")) ? RES_Q75 :
                                  !strcmp(val, "50%") ? RES_Q50 : !strcmp(val, "25%") ? RES_Q25 :
                                  (n >= 1 && n <= 4) ? n : n > 4 ? 4 : RES_MATCH;
        }
        else if (!strcmp(key, "upscaler")) g_settings.upscaler = !strcmp(val, "bilinear") ? UPSCALE_BILINEAR : UPSCALE_SHARP;
        // (an older "antialias = off" was only LAS Recomp's default being saved: a new key, so SMAA starts on)
        else if (!strcmp(key, "smaa")) g_settings.antialias = atoi(val) != 0 ? AA_SMAA : AA_OFF;
        else if (!strcmp(key, "dpad_move")) g_settings.dpad_move = atoi(val) != 0;
        else if (!strcmp(key, "wild_battle")) g_settings.wild_battle = atoi(val) != 0;
        else bind_settings_load(key, val);  // bind_pad_* / bind_key_*: runtime/hle/bindings.c
    }
    fclose(f);
    if (g_settings.volume < 0) g_settings.volume = 0;
    if (g_settings.volume > 2) g_settings.volume = 2;
    LOG("[settings] loaded %s\n", SETTINGS_FILE);
}

void settings_save(void) {
    FILE *f = fopen(SETTINGS_FILE, "w");
    if (!f) { LOG("[settings] cannot write %s\n", SETTINGS_FILE); return; }
    fprintf(f, "# " RT_APP_NAME " settings (set on the start screen)\n");
    fprintf(f, "fullscreen = %d\n", g_settings.fullscreen);
    fprintf(f, "fps = %d\n", g_settings.fps);
    fprintf(f, "render_height = %d\n", g_settings.res_height);
    fprintf(f, "smaa = %d\n", g_settings.antialias == AA_SMAA);
    fprintf(f, "mod = %s\n", g_settings.mod);
    fprintf(f, "mouse_sens = %.3f\n", g_settings.mouse_sens);
    fprintf(f, "stick_deadzone = %.2f\n", g_settings.stick_deadzone);
    fprintf(f, "volume = %.2f\n", g_settings.volume);
    fprintf(f, "reverb = %d\n", g_settings.reverb);
    fprintf(f, "movie_audio = %d\n", g_settings.movie_audio);
    fprintf(f, "show_fps = %d\n", g_settings.show_fps);
    fprintf(f, "upscaler = %s\n", g_settings.upscaler == UPSCALE_FSR1 ? "fsr1" : "bilinear");
    if (g_settings.res_mode == RES_DYNAMIC) fprintf(f, "resolution = dynamic\n");
    else if (g_settings.res_mode == RES_Q75) fprintf(f, "resolution = 75%%\n");
    else if (g_settings.res_mode == RES_Q25) fprintf(f, "resolution = 25%%\n");
    else if (g_settings.res_mode == RES_Q50) fprintf(f, "resolution = 50%%\n");
    else if (g_settings.res_mode >= 1 && g_settings.res_mode <= 8) fprintf(f, "resolution = %dx\n", g_settings.res_mode);
    else fprintf(f, "resolution = match\n");
    fprintf(f, "dpad_move = %d\n", g_settings.dpad_move);
    fprintf(f, "wild_battle = %d\n", g_settings.wild_battle);
    fprintf(f, "game_lang = %s\n", g_settings.game_lang);
    bind_settings_save(f);
    fclose(f);
}
