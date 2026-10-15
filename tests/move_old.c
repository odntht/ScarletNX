/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* snx_move_old, which moves an older build's files to the XDG folders: a rename, a copy across filesystems for a file,
   never over what is already there. And snx_work_dir, the drivers' extractions, which moves them. */
#define _POSIX_C_SOURCE 200809L
#include "common.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

static char* join(const char* a, const char* b) {
    char* p = snx_path_join(a, b);
    assert(p);
    return p;
}
static void put(const char* path, const char* text) { assert(!snx_write_file(path, text, strlen(text))); }
static int holds(const char* path, const char* text) {
    size_t n;
    unsigned char* data = snx_read_file(path, &n);
    int same = data && n == strlen(text) && !memcmp(data, text, n);
    free(data);
    return same;
}
/* a folder on another filesystem than near, or NULL */
static char* other_filesystem(const char* near) {
    static const char* const candidates[] = {"/dev/shm", "/run/user", "/var/tmp"};
    struct stat a, b;
    size_t i;
    char* dir;
    assert(!stat(near, &a));
    for (i = 0; i < sizeof(candidates) / sizeof(*candidates); ++i)
        if (!stat(candidates[i], &b) && S_ISDIR(b.st_mode) && b.st_dev != a.st_dev) {
            dir = snx_temp_dir(candidates[i]);
            if (dir) return dir;
        }
    return NULL;
}

int main(void) {
    char *root = snx_temp_dir(NULL), *from, *to, *folder, *other, *staged;
    struct stat st;
    assert(root);
    from = join(root, "old/games/Game.ini");
    to = join(root, "new/games/Game.ini");
    folder = snx_dirname(from);
    assert(!snx_mkdirs(folder));
    free(folder);

    /* nothing there, or the same path: nothing to do */
    assert(snx_move_old(from, to, 1) == SNX_MOVE_NONE && !snx_path_exists(to));
    put(from, "old");
    assert(snx_move_old(from, from, 1) == SNX_MOVE_NONE && holds(from, "old"));
    /* a rename, into folders made private as the spec asks */
    assert(snx_move_old(from, to, 1) == SNX_MOVE_DONE);
    assert(holds(to, "old") && !snx_path_exists(from));
    folder = join(root, "new");
    assert(!stat(folder, &st) && (st.st_mode & 0777) == 0700);
    free(folder);
    /* never over a file this build already wrote: both stay as they are */
    put(from, "older");
    put(to, "new");
    assert(snx_move_old(from, to, 1) == SNX_MOVE_NONE);
    assert(holds(from, "older") && holds(to, "new"));
    /* a folder moves whole */
    free(from);
    free(to);
    from = join(root, "old/work");
    to = join(root, "new/work");
    folder = join(from, "Game");
    assert(!snx_mkdirs(folder));
    free(folder);
    folder = join(from, "Game/main");
    put(folder, "code");
    free(folder);
    assert(snx_move_old(from, to, 0) == SNX_MOVE_DONE && !snx_path_exists(from));
    folder = join(to, "Game/main");
    assert(holds(folder, "code"));
    free(folder);
    free(from);
    free(to);

    /* work/ of a checkout or a portable copy is its own */
    folder = join(root, "checkout");
    from = snx_work_dir(folder);
    to = join(folder, "work");
    assert(!strcmp(from, to));
    free(to);
    free(from);
    free(folder);
#ifdef SNX_XDG
    /* an installed build's is in the cache folder, an older build's extractions moved there */
    folder = join(root, "data");
    setenv("XDG_DATA_HOME", folder, 1);
    free(folder);
    folder = join(root, "cache");
    setenv("XDG_CACHE_HOME", folder, 1);
    free(folder);
    folder = join(root, "data/scarletnx/work/Game/exefs");
    assert(!snx_mkdirs(folder));
    free(folder);
    folder = join(root, "data/scarletnx");
    from = snx_work_dir(folder);
    free(folder);
    to = join(root, "cache/scarletnx/work");
    assert(!strcmp(from, to));
    free(from);
    from = join(to, "Game/exefs");
    assert(snx_is_dir(from));
    free(from);
    free(to);
    folder = join(root, "data/scarletnx/work");
    assert(!snx_path_exists(folder));
    free(folder);
#endif

    /* across filesystems a rename fails: a file is copied, a folder stays */
    other = other_filesystem(root);
    if (other) {
        from = join(root, "Game.log");
        to = join(other, "state/games/Game.log");
        put(from, "log");
        assert(snx_move_old(from, to, 0) == SNX_MOVE_LEFT && holds(from, "log") && !snx_path_exists(to));
        assert(snx_move_old(from, to, 1) == SNX_MOVE_DONE && holds(to, "log") && !snx_path_exists(from));
        staged = snx_format("%s.moving", to);
        assert(!snx_path_exists(staged));
        free(staged);
        free(from);
        free(to);
        from = join(root, "work");
        to = join(other, "cache/work");
        folder = join(from, "Game");
        assert(!snx_mkdirs(folder));
        free(folder);
        assert(snx_move_old(from, to, 1) == SNX_MOVE_LEFT && snx_is_dir(from) && !snx_path_exists(to));
        free(from);
        free(to);
#ifdef SNX_XDG
        /* so an installed build's older extractions stay where they are, and a line says they can go */
        setenv("XDG_CACHE_HOME", other, 1);
        folder = join(root, "data/scarletnx/work/Game");
        assert(!snx_mkdirs(folder));
        free(folder);
        folder = join(root, "data/scarletnx");
        from = snx_work_dir(folder);
        free(folder);
        to = join(other, "scarletnx/work");
        assert(!strcmp(from, to));
        free(from);
        from = join(to, "Game");
        assert(!snx_path_exists(from));
        free(from);
        free(to);
        folder = join(root, "data/scarletnx/work/Game");
        assert(snx_is_dir(folder));
        free(folder);
#endif
        printf("snx_move_old: across filesystems, %s\n", other);
        assert(!snx_remove_tree(other));
        free(other);
    } else
        puts("snx_move_old: no second filesystem here, the copy across filesystems was not tried");

    puts("snx_move_old: renames files and folders, never over what is there, copies a file across filesystems; "
         "snx_work_dir: an installed build's extractions moved to its cache folder");
    assert(!snx_remove_tree(root));
    free(root);
    return 0;
}
