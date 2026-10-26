/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
#include "gui/aot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "common/cpu/recompiler/llvm/jit_cache.h"
#include "common/utilities/common.h"
#include "common/utilities/game_ini.h"

#define AOT_KEY_MAX 8192 /* the key names the host, LLVM and its features: far less */

static uint32_t get32(const unsigned char* p) {
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

char* gui_game_kind_dir(const char* folder, const char* name, int kind) {
    static const char* const keys[] = {"config", "state", "cache"}; /* SNX_USER_CONFIG's on */
    char record[SNX_PATH_SIZE], *recorded, *moved;
    if (kind < SNX_USER_CONFIG || kind > SNX_USER_CACHE ||
        snx_path_format(record, sizeof record, "%s/%s.folders", folder, name))
        return NULL;
    recorded = snx_ini_path(record, keys[kind - SNX_USER_CONFIG]);
    if (recorded && snx_is_dir(recorded)) return recorded;
    free(recorded);
    /* the runtime makes its user path; where it could not, the files stay beside the game */
    moved = snx_user_path(folder, kind);
    if (moved && strcmp(moved, folder) && snx_is_dir(moved)) return moved;
    free(moved);
    return snx_strdup(folder);
}
char* gui_aot_cache_path(const char* folder, const char* name) {
    char path[SNX_PATH_SIZE];
    char *dir, *moved;
    int cut;
    if (!folder || !name) return NULL;
    dir = gui_game_kind_dir(folder, name, SNX_USER_CACHE);
    if (!dir) return NULL;
    moved = snx_absolute(dir);
    if (moved) {
        free(dir);
        dir = moved;
    }
    cut = snx_path_format(path, sizeof path, "%s/%s.jitcache", dir, name);
    free(dir);
    return cut ? NULL : snx_strdup(path);
}

int gui_aot_cache_info(const char* path, GuiAotInfo* info) {
    unsigned char head[12];
    uint32_t length;
    char* key;
    const char* field;
    size_t got;
    FILE* f;
    if (!path || !info) return 0;
    f = snx_fopen(path, "rb");
    if (!f) return 0;
    if (fread(head, 1, sizeof head, f) != sizeof head || memcmp(head, SNX_JIT_CACHE_MAGIC, 8)) {
        fclose(f);
        return 0;
    }
    length = get32(head + 8);
    if (!length || length > AOT_KEY_MAX) {
        fclose(f);
        return 0;
    }
    key = (char*)malloc((size_t)length + 1);
    if (!key) {
        fclose(f);
        return 0;
    }
    got = fread(key, 1, length, f);
    fclose(f);
    if (got != length) {
        free(key);
        return 0;
    }
    key[length] = 0;
    field = strstr(key, " opt=");
    if (!field || !strchr("0123sz", field[5])) {
        free(key);
        return 0;
    }
    info->opt = field[5];
    field = strstr(key, " hook=");
    if (!field || (field[6] != '0' && field[6] != '1')) {
        free(key);
        return 0;
    }
    info->hook = field[6] - '0';
    info->size = (uint64_t)snx_file_size(path);
    free(key);
    return 1;
}

int gui_aot_levels_match(const char* game_opt, char cache_opt) {
    char game = game_opt && game_opt[0] ? game_opt[0] : '2';
    return game == cache_opt;
}
