/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* RenderDoc captures of frames. RenderDoc's library comes in by renderdoc=PREFIX ([Debug] or --renderdoc=), which loads
   it before the Vulkan instance exists (its capture layer is an implicit one, which the loader enables with
   ENABLE_VULKAN_RENDERDOC_CAPTURE), or is in the process already when RenderDoc's own program started the game. Either
   way the capture key (hotkey_capture, backend/sdl3.c) or the game's menu's Capture frame (overlay.c) captures a frame,
   the one after the next present, and renderdoc_frames=N,M,... names frames to capture: a number is a present, a number
   with s a time in seconds on the clock --script counts from (the first frame after it), which a loaded machine doesn't
   move. A capture holds one guest frame: everything recorded after present N-1 up to present N, written where
   renderdoc= says (PREFIX_capture.rdc, then PREFIX_capture_2.rdc and on), or where RenderDoc's program keeps its
   captures when it is empty. Frames that never reach the swapchain (offscreen, headless) are captured the same way,
   because the capture starts and ends here rather than at RenderDoc's present hook. RenderDoc writes a capture inside
   the present that ends it, the game stopped meanwhile (70 s for a frame of 1.4 GB): a notice over the game says so as
   a capture is asked or RenderDoc's program starts its own (IsFrameCapturing), a thread of ours logs how long it has
   taken every 5 s, and the log and a notice say how long it took, the file and its size once written (GetNumCaptures
   grown), or that it failed. RenderDoc's own keys (F12, Print Screen) and overlay are off, so F12 stays the stats' key:
   the stats box says when a capture can be taken, and what the last one wrote (stats_capture). The in-application API
   is RenderDoc's renderdoc_app.h (MIT, Baldur Karlsson), fetched by cmake/dependencies.cmake. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include "common/utilities/common.h"
#include "nx/gpu/vk/vk.h"
#include "nx/runtime/options.h"
#include "nx/runtime/platform/log.h"
#include "nx/runtime/platform/hotkeys.h"
#include "nx/runtime/platform/overlay.h"
#include "nx/runtime/platform/plat.h"
#include "nx/runtime/platform/stats.h"
#include "nx/runtime/platform/unattended.h"

#if defined(__NetBSD__)
/* RenderDoc has no build for NetBSD, and renderdoc_app.h stops on a system it doesn't name */
void vk_renderdoc_load(void) {
    if (*g_settings.render_doc_captures)
        WARN("renderdoc: renderdoc=%s is set, but RenderDoc has no build for NetBSD: no frame is captured\n",
             g_settings.render_doc_captures);
}
int vk_renderdoc_capture(int ask) {
    if (ask) LOG("renderdoc: no frame is captured: RenderDoc has no build for NetBSD\n");
    return 0;
}
void vk_renderdoc_present(u64 present) { (void)present; }
#else
#include "renderdoc_app.h"
#if defined(SNX_IOS)
#include "nx/gpu/vk/capture_metal.h"
/* the name the notices and the stats box give the captures: on iOS Metal takes them (capture_metal.m) */
#define RD_NAME "Metal capture"
#else
#define RD_NAME "RenderDoc"
#endif

#define RD_FRAMES      64                     /* frames to capture at most */
#define RD_PROGRESS_NS (5ull * 1000000000ull) /* while RenderDoc writes a capture, how often the log says so */
/* over the game as a capture is asked or RenderDoc starts its own: RenderDoc writes it inside the present that ends
   it, the game stopped meanwhile (70 s for a frame of 1.4 GB). A notice's line, OVERLAY_COLS_MAX letters at most */
#if defined(SNX_IOS)
#define RD_CAPTURING "Metal capture: capturing. The game stops while it writes"
#else
#define RD_CAPTURING "RenderDoc: capturing; the game stops while it writes (a minute if large)"
#endif

typedef struct {
    u64 at;   /* the present, or the time in ms; 0 once taken */
    int secs; /* at is a time */
} RdFrame;
static struct {
    RENDERDOC_API_1_1_2* _Atomic api; /* NULL: RenderDoc isn't in the process */
    RdFrame frames[RD_FRAMES];
    int nframes;
    int open;  /* a capture of ours runs, to end at the next present */
    _Atomic int asked; /* the capture key's or the menu's: the main thread sets it, the next present arms it */
    /* asked a present ago: the capture starts at this one, so the frame shown while it is written was drawn after the
       ask, with RD_CAPTURING over it */
    int armed;
    int theirs;      /* a capture RenderDoc started itself (its program's), seen running at a present */
    unsigned before; /* the captures written when it was seen */
    /* the write in progress, which the progress thread reads: its number (0 for none, the writes started so far
       numbering them), and when it began (plat_ns) */
    PMutex lock;
    unsigned writing, writes;
    u64 since;
    _Atomic int ready; /* a capture can be taken: RenderDoc is in the process, or (iOS) renderdoc= names a usable stem */
    /* iOS: the captures written this run, the stem renderdoc= gives under Documents, and the running capture's bundle */
    unsigned metal_n;
    char metal_stem[STATS_PATH];
    char metal_path[STATS_PATH];
} g_renderdoc = {.lock = PMUTEX_INIT};
/* a notice's line over the game, after the capture key's icon */
static void rd_notice(const char* text) { overlay_notice(overlay_key_icon(HOTKEY_CAPTURE), text); }

void vk_renderdoc_load(void) {
    const char *prefix = g_settings.render_doc_captures, *frames = g_settings.render_doc_frames, *p;
#if defined(SNX_IOS)
    /* no RenderDoc on iOS: Metal takes the captures, into renderdoc='s stem, under Documents where it is relative */
    if (!*prefix) return;
    if (snx_path_is_full(prefix)) {
        if (strlen(prefix) > sizeof g_renderdoc.metal_stem - 32) {
            WARN("renderdoc: renderdoc=%s: the path is too long, no frame is captured\n", prefix);
            return;
        }
        snprintf(g_renderdoc.metal_stem, sizeof g_renderdoc.metal_stem, "%s", prefix);
    } else {
        char* d = snx_user_data_dir();
        char* j = d ? snx_path_join(d, prefix) : NULL;
        if (!j || strlen(j) > sizeof g_renderdoc.metal_stem - 32) {
            WARN("renderdoc: renderdoc=%s: %s, no frame is captured\n", prefix,
                 j ? "the path is too long" : "no folder for the captures");
            free(d);
            free(j);
            return;
        }
        snprintf(g_renderdoc.metal_stem, sizeof g_renderdoc.metal_stem, "%s", j);
        free(d);
        free(j);
    }
    /* Metal's capture layer wraps each Metal call while it is on, so it is on only with renderdoc= set. Metal reads
       this when the first device is made, which MoltenVK does with the instance, after this */
    if (plat_setenv("MTL_CAPTURE_ENABLED", "1")) WARN("renderdoc: couldn't enable Metal's capture layer\n");
#else
    pRENDERDOC_GetAPI get_api;
    /* RenderDoc's program started the game: its library is in the process, and renderdoc= needn't load it */
    void* lib = plat_dl_loaded(SNX_VK_RENDERDOC_LIBRARY);
    void* api = NULL; /* GetAPI stores a void*, which the API's pointer isn't */
    int started = lib != NULL;
    if (!lib && !*prefix) return;
    if (!lib) lib = plat_dl_open(SNX_VK_RENDERDOC_LIBRARY);
    if (!lib) {
        WARN("renderdoc: renderdoc=%s is set but RenderDoc's library, %s, didn't load: %s\n", prefix,
             SNX_VK_RENDERDOC_LIBRARY, plat_dl_error());
        return;
    }
    get_api = (pRENDERDOC_GetAPI)plat_dl_sym(lib, "RENDERDOC_GetAPI");
    if (!get_api || !get_api(eRENDERDOC_API_Version_1_1_2, &api) || !api) {
        WARN("renderdoc: %s has no in-application API 1.1.2: no frame is captured\n", SNX_VK_RENDERDOC_LIBRARY);
        return;
    }
    RENDERDOC_API_1_1_2* rd = api;
    /* the capture layer comes in through the loader, which only enables it with this set */
    if (plat_setenv("ENABLE_VULKAN_RENDERDOC_CAPTURE", "1")) WARN("renderdoc: couldn't enable the capture layer\n");
    if (*prefix) rd->SetCaptureFilePathTemplate(prefix);
    /* no overlay in the captured frames, and nothing taken but the presents asked for: F12 is the stats' key */
    rd->MaskOverlayBits(eRENDERDOC_Overlay_None, eRENDERDOC_Overlay_None);
    rd->SetCaptureKeys(NULL, 0);
#endif
    for (p = frames; *p && g_renderdoc.nframes < RD_FRAMES;) {
        RdFrame* f = &g_renderdoc.frames[g_renderdoc.nframes];
        char* e;
        unsigned long long n = strtoull(p, &e, 10);
        if (e == p) break;
        f->secs = *e == 's';
        f->at = *e == 's' ? n * 1000 : n;
        if (*e == 's') e++;
        if (f->at) g_renderdoc.nframes++;
        p = *e == ',' ? e + 1 : e;
    }
    if (*p)
        WARN("renderdoc: renderdoc_frames: \"%s\" is no frame (N, or Ns for seconds): it captures the %d "
             "before it\n",
             p, g_renderdoc.nframes);
#if defined(SNX_IOS)
    LOG("renderdoc: Metal capture, %s%sthe menu's Capture frame captures a frame, %d frame(s) listed, to %s\n",
        g_settings.render_doc_capture_shortcut, *g_settings.render_doc_capture_shortcut ? " or " : "",
        g_renderdoc.nframes, g_renderdoc.metal_stem);
#else
    LOG("renderdoc: %s, %s%sthe menu's Capture frame captures a frame, %d frame(s) listed, to %s\n",
        started ? "in the process (RenderDoc's program started the game)" : "loaded",
        g_settings.render_doc_capture_shortcut, *g_settings.render_doc_capture_shortcut ? " or " : "",
        g_renderdoc.nframes, *prefix ? prefix : rd->GetCaptureFilePathTemplate());
    atomic_store_explicit(&g_renderdoc.api, rd, memory_order_relaxed); /* ready's release below publishes it */
#endif
    atomic_store_explicit(&g_renderdoc.ready, 1, memory_order_release); /* the main thread's capture key reads it */
    stats_capture(&(SnxStatsCapture){.ready = 1});
}

int vk_renderdoc_capture(int ask) {
    if (!atomic_load_explicit(&g_renderdoc.ready, memory_order_acquire)) {
        if (ask)
#if defined(SNX_IOS)
            LOG("renderdoc: no frame is captured: renderdoc= is empty or not usable (see the log above)\n");
#else
            LOG("renderdoc: no frame is captured: RenderDoc isn't in the process (renderdoc= is empty, and "
                "RenderDoc's program didn't start the game)\n");
#endif
        return 0;
    }
    if (ask) {
        atomic_store_explicit(&g_renderdoc.asked, 1, memory_order_release);
        rd_notice(RD_CAPTURING);
        LOG("renderdoc: a capture is asked: the frame after the next present\n");
    }
    return 1;
}

/* the progress thread's: every RD_PROGRESS_NS while write gen is in progress, how long it has taken. It ends with it */
static void rd_progress(void* arg) {
    unsigned gen = (unsigned)(uintptr_t)arg;
    int on;
    do {
        plat_sleep_ns((i64)RD_PROGRESS_NS);
        pm_lock(&g_renderdoc.lock);
        on = g_renderdoc.writing == gen;
        if (on)
            LOG("renderdoc: writing capture, %llu s\n",
                (unsigned long long)((plat_ns() - g_renderdoc.since) / 1000000000u));
        pm_unlock(&g_renderdoc.lock);
    } while (on);
}
/* a write begins, its progress thread with it */
static void rd_write_begin(void) {
    unsigned gen;
    pm_lock(&g_renderdoc.lock);
    gen = g_renderdoc.writing = ++g_renderdoc.writes;
    g_renderdoc.since = plat_ns();
    pm_unlock(&g_renderdoc.lock);
    if (plat_thread(rd_progress, (void*)(uintptr_t)gen, 0))
        WARN("renderdoc: no thread for the write's progress: the log says the capture once it is written\n");
}
/* the write ended: how long it took, in seconds */
static u64 rd_write_end(void) {
    u64 secs;
    pm_lock(&g_renderdoc.lock);
    g_renderdoc.writing = 0;
    secs = (plat_ns() - g_renderdoc.since) / 1000000000u;
    pm_unlock(&g_renderdoc.lock);
    return secs;
}
/* a capture's size as the notices say it, ? where it isn't known */
static void rd_size(u64 bytes, int sized, char* out, size_t n) {
    if (!sized)
        snprintf(out, n, "?");
    else if (bytes < 1024)
        snprintf(out, n, "%llu bytes", (unsigned long long)bytes);
    else if (bytes < 1024 * 1024)
        snprintf(out, n, "%llu KiB", (unsigned long long)(bytes >> 10));
    else if (bytes < 1024ull * 1024 * 1024)
        snprintf(out, n, "%.1f MiB", (double)bytes / (1024.0 * 1024));
    else
        snprintf(out, n, "%.1f GiB", (double)bytes / (1024.0 * 1024 * 1024));
}
/* capture n (from 1) written in secs to path ("" where it isn't known), of bytes when sized: its name and size over
   the game, a name too long for the notice's line keeping its end after "...", and the stats box told. The name is
   the path's leaf by this loop, not snx_path_leaf: the host test has its own snx_is_file and snx_file_size, which
   common.c, snx_path_leaf's file, also defines */
static void rd_shown(const char* path, u64 bytes, int sized, unsigned n, u64 secs) {
    SnxStatsCapture shown = {.ready = 1, .taken = n};
    char size[32], lead[48], line[OVERLAY_COLS_MAX + 1];
    const char *name = "?", *c;
    size_t room, len;
    snprintf(shown.path, sizeof shown.path, "%s", path);
    if (*shown.path)
        for (name = c = shown.path; *c; ++c)
            if (*c == '/' || *c == '\\') name = c + 1;
    rd_size(bytes, sized, size, sizeof size);
    snprintf(lead, sizeof lead, RD_NAME ": written in %llu s: ", (unsigned long long)secs);
    room = OVERLAY_COLS_MAX - strlen(lead) - strlen(size) - 3; /* " (" and ")" */
    len = strlen(name);
    if (len <= room)
        snprintf(line, sizeof line, "%s%s (%s)", lead, name, size);
    else
        snprintf(line, sizeof line, "%s...%s (%s)", lead, name + len - (room - 3), size);
    rd_notice(line);
    stats_capture(&shown);
}
/* a capture that wrote no file after secs, what it held, n captures written before it: in the log with what to read,
   over the game, the stats box told. why is the back end's reason, or NULL where RenderDoc's own log has it */
static void rd_failed(unsigned n, u64 secs, const char* what, const char* why) {
    char line[OVERLAY_COLS_MAX + 1];
    if (why) {
        WARN("renderdoc: %s failed after %llu s: " RD_NAME " wrote no file: %s\n", what, (unsigned long long)secs,
             why);
        snprintf(line, sizeof line, RD_NAME ": no file written: %s", why);
    } else {
        WARN("renderdoc: %s failed after %llu s: RenderDoc wrote no file (its own log says why)\n", what,
             (unsigned long long)secs);
        snprintf(line, sizeof line, "RenderDoc: capture failed after %llu s, no file written (see its log)",
                 (unsigned long long)secs);
    }
    rd_notice(line);
    stats_capture(&(SnxStatsCapture){.ready = 1, .taken = n, .failed = 1});
}
#if !defined(SNX_IOS)
/* capture n (from 1) written in secs, what it holds (present N, or RenderDoc's own): its path and size in the log,
   and rd_shown's notice */
static void rd_written(RENDERDOC_API_1_1_2* rd, unsigned n, u64 secs, const char* what) {
    char size[32], path[STATS_PATH];
    u32 path_len = 0;
    /* GetCapture writes the whole path, unbounded: its length first */
    int told = n && rd->GetCapture(n - 1, NULL, &path_len, NULL);
    if (told && path_len < sizeof path && rd->GetCapture(n - 1, path, NULL, NULL)) {
        rd_size(snx_file_size(path), snx_is_file(path), size, sizeof size);
        LOG("renderdoc: %s written in %llu s to %s (%s)\n", what, (unsigned long long)secs, path, size);
        rd_shown(path, snx_file_size(path), snx_is_file(path), n, secs);
        return;
    }
    if (told)
        LOG("renderdoc: %s written in %llu s, capture %u, to a path of %u bytes, past the %zu kept\n", what,
            (unsigned long long)secs, n, path_len, sizeof path);
    else
        LOG("renderdoc: %s written in %llu s, capture %u, to a path RenderDoc doesn't give\n", what,
            (unsigned long long)secs, n);
    rd_shown("", 0, 0, n, secs);
}
/* RenderDoc's own capture, seen at an earlier present, which it writes in the present that ends it: written once
   its count grew, failed once it stopped without a new one */
static void rd_theirs(RENDERDOC_API_1_1_2* rd) {
    unsigned n = rd->GetNumCaptures();
    if (n > g_renderdoc.before) {
        g_renderdoc.theirs = 0;
        rd_written(rd, n, rd_write_end(), "RenderDoc's own capture");
    } else if (!rd->IsFrameCapturing()) {
        g_renderdoc.theirs = 0;
        rd_failed(n, rd_write_end(), "RenderDoc's own capture", NULL);
    }
}
#endif

/* whether the frame after this present is one to capture (each is taken once) */
static int rd_wanted(u64 present) {
    u64 ms = unattended_ms();
    int i, want = 0;
    for (i = 0; i < g_renderdoc.nframes; i++) {
        RdFrame* f = &g_renderdoc.frames[i];
        if (f->at && (f->secs ? ms >= f->at : f->at == present + 1)) {
            f->at = 0;
            want = 1;
        }
    }
    return want;
}
#if defined(SNX_IOS)
/* bytes as MiB, or ? where the system didn't say */
static void rd_mib(int known, SnxU64 bytes, char* out, size_t n) {
    if (known)
        snprintf(out, n, "%llu", (unsigned long long)(bytes >> 20));
    else
        snprintf(out, n, "?");
}
/* the capture of the frame after present starts, into the first free <stem>_capture[_N].gputrace: whether it runs */
static int rd_metal_start(u64 present) {
    char what[48], why[256], room_mib[24], mem_mib[24], *folder;
    SnxU64 room = 0, total = 0, mem = 0;
    unsigned i;
    snprintf(what, sizeof what, "the capture of present %llu", (unsigned long long)(present + 1));
    for (i = 1; i <= 999; i++) {
        if (i == 1)
            snprintf(g_renderdoc.metal_path, sizeof g_renderdoc.metal_path, "%s_capture.gputrace",
                     g_renderdoc.metal_stem);
        else
            snprintf(g_renderdoc.metal_path, sizeof g_renderdoc.metal_path, "%s_capture_%u.gputrace",
                     g_renderdoc.metal_stem, i);
        if (!snx_path_exists(g_renderdoc.metal_path)) break;
    }
    if (i > 999) {
        rd_failed(g_renderdoc.metal_n, 0, what, "no free name");
        return 0;
    }
    folder = snx_dirname(g_renderdoc.metal_path);
    rd_mib(folder && !snx_disk_room(folder, &room, &total), room, room_mib, sizeof room_mib);
    free(folder);
    rd_mib(snx_memory_available(&mem), mem, mem_mib, sizeof mem_mib);
    LOG("renderdoc: Metal capture of present %llu to %s: %s MiB free, %s MiB of memory\n",
        (unsigned long long)(present + 1), g_renderdoc.metal_path, room_mib, mem_mib);
    if (!vk_metal_capture_start(g_renderdoc.metal_path, why, sizeof why)) {
        rd_failed(g_renderdoc.metal_n, 0, what, why);
        return 0;
    }
    return 1;
}
#endif
/* at the present that ends frame `present`, after its last submit and before the swapchain present, under vk_lock: the
   capture of this frame ends here, and the next frame's starts */
void vk_renderdoc_present(u64 present) {
    int want;
    if (!atomic_load_explicit(&g_renderdoc.ready, memory_order_acquire) || !g_vk.instance) return;
#if !defined(SNX_IOS)
    RENDERDOC_API_1_1_2* rd = atomic_load_explicit(&g_renderdoc.api, memory_order_relaxed);
    void* dev = RENDERDOC_DEVICEPOINTER_FROM_VKINSTANCE(g_vk.instance);
    if (g_renderdoc.theirs) rd_theirs(rd);
#endif
    want = rd_wanted(present) || g_renderdoc.armed; /* listed once: it takes the frame it answers for */
    g_renderdoc.armed = atomic_exchange_explicit(&g_renderdoc.asked, 0, memory_order_acq_rel);
    if (g_renderdoc.open || want) vk_rec_sync(); /* the frame's commands are recorded within the capture */
    if (g_renderdoc.open) {                      /* the back end writes it here, the game stopped */
        char what[48];
        u64 secs;
        g_renderdoc.open = 0;
        snprintf(what, sizeof what, "the capture of present %llu", (unsigned long long)present);
        rd_write_begin();
#if defined(SNX_IOS)
        uint64_t bytes = 0;
        SnxU64 mem = 0;
        char why[256], size[32], mem_mib[24];
        int ok = vk_metal_capture_end(&bytes, why, sizeof why);
        secs = rd_write_end();
        rd_mib(snx_memory_available(&mem), mem, mem_mib, sizeof mem_mib);
        LOG("renderdoc: %s MiB of memory after the capture\n", mem_mib);
        if (ok) {
            g_renderdoc.metal_n++;
            rd_size(bytes, 1, size, sizeof size);
            LOG("renderdoc: %s written in %llu s to %s (%s)\n", what, (unsigned long long)secs, g_renderdoc.metal_path,
                size);
            rd_shown(g_renderdoc.metal_path, bytes, 1, g_renderdoc.metal_n, secs);
        } else {
            rd_failed(g_renderdoc.metal_n, secs, what, why);
        }
#else
        int ok = (int)rd->EndFrameCapture(dev, NULL);
        secs = rd_write_end();
        if (ok)
            rd_written(rd, rd->GetNumCaptures(), secs, what);
        else
            rd_failed(rd->GetNumCaptures(), secs, what, NULL);
    } else if (!g_renderdoc.theirs && rd->IsFrameCapturing()) {
        /* RenderDoc's own, its program's: it writes it in the swapchain's present that ends the frame */
        g_renderdoc.theirs = 1;
        g_renderdoc.before = rd->GetNumCaptures();
        rd_notice(RD_CAPTURING);
        LOG("renderdoc: RenderDoc's own capture runs, from present %llu: the game stops while it writes\n",
            (unsigned long long)present);
        rd_write_begin();
#endif
    }
#if defined(SNX_IOS)
    if (want) g_renderdoc.open = rd_metal_start(present);
#else
    if (want) {
        rd->StartFrameCapture(dev, NULL);
        g_renderdoc.open = 1;
    }
#endif
}
#endif
