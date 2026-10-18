// NVN indirect argument records use the standard non-indexed / indexed draw layout.
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include "nvn.h"
static bool nvn_indirect_draw(const CmdRec *command, const void *args, bool indexed, CmdRec *direct) {
    uint32_t v[5] = {0};
    memcpy(v, args, indexed ? 20 : 16);
    if (!v[0] || !v[1]) return false;
    memset(direct, 0, sizeof *direct);
    direct->op = indexed ? CMD_DRAW_ELEMENTS : CMD_DRAW_ARRAYS;
    direct->size = sizeof *direct;
    direct->x[0] = command->x[0];
    direct->x[2] = v[0];
    if (indexed) {
        uint64_t index_size = command->x[1] == 0 ? 1 : command->x[1] == 1 ? 2 : 4;
        uint64_t offset = (uint64_t)v[2] * index_size;
        if (command->x[2] > UINT64_MAX - offset) return false;
        direct->x[1] = command->x[1];
        direct->x[3] = command->x[2] + offset;
        direct->x[4] = (uint64_t)(int64_t)(int32_t)v[3];
        direct->x[5] = v[4]; direct->x[6] = v[1];
    } else {
        direct->x[1] = v[2]; direct->x[3] = v[3]; direct->x[4] = v[1];
    }
    return true;
}
