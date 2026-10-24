/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* The menu's side of the iOS and macOS apps (docs/APPLE.md): the folders, the game the menu plays, and the windows
   SDL's give dist/apple/Frontend.swift to present its pickers and share sheet over */
#include "gui/apple.h"
#include "gui/i18n.h"
#include <SDL3/SDL.h>
#include <TargetConditionals.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/resource.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <mach/mach.h>
#include <dlfcn.h>
#include <stdint.h>
#include <time.h>
#import <Foundation/Foundation.h>

/* dist/apple/Frontend.swift. window is the SDL window's UIWindow or NSWindow; done gets the path picked, NULL for none */
extern bool snx_apple_pick(void* window, bool folder, void (*done)(void* data, const char* path), void* data);
extern bool snx_apple_share(void* window, const char* path);

#if TARGET_OS_IPHONE
extern size_t os_proc_available_memory(void); /* <os/proc.h>, which not every SDK header set declares the same way */
static void sysctl_line(const char* name) {
    char text[256] = "?";
    size_t n = sizeof text;
    if (sysctlbyname(name, text, &n, NULL, 0)) snprintf(text, sizeof text, "(unavailable)");
    fprintf(stderr, "  %s = %s\n", name, text);
}
static void sysctl_number(const char* name) {
    unsigned long long v = 0;
    size_t n = sizeof v;
    if (sysctlbyname(name, &v, &n, NULL, 0))
        fprintf(stderr, "  %s = (unavailable)\n", name);
    else
        fprintf(stderr, "  %s = %llu\n", name, n == 4 ? (unsigned long long)(unsigned)v : v);
}
static void limit_line(const char* name, int resource) {
    struct rlimit r;
    if (getrlimit(resource, &r))
        fprintf(stderr, "  rlimit %s = (unavailable)\n", name);
    else
        fprintf(stderr, "  rlimit %s = soft %llu hard %llu\n", name, (unsigned long long)r.rlim_cur,
                (unsigned long long)r.rlim_max);
}
/* the entitlements the signed app carries, from the provisioning profile in the bundle: which of the ones that matter
   to a game the sideloading tool gave this app (a profile is a signed plist, its keys readable in the file) */
static void entitlement_lines(void) {
    static const char* const keys[] = {"get-task-allow", "com.apple.developer.kernel.extended-virtual-addressing",
                                       "com.apple.developer.kernel.increased-memory-limit",
                                       "com.apple.developer.kernel.increased-debugging-memory-limit",
                                       "com.apple.security.application-groups", "dynamic-codesigning"};
    NSData* data = [NSData dataWithContentsOfFile:[NSBundle.mainBundle.bundlePath
                                                      stringByAppendingPathComponent:@"embedded.mobileprovision"]];
    size_t i;
    if (!data) {
        fprintf(stderr, "entitlements: no embedded.mobileprovision in the bundle\n");
        return;
    }
    fprintf(stderr, "entitlements in the provisioning profile (%lu bytes):\n", (unsigned long)data.length);
    for (i = 0; i < sizeof keys / sizeof *keys; ++i)
        fprintf(stderr, "  %s: %s\n", keys[i], memmem(data.bytes, data.length, keys[i], strlen(keys[i])) ? "yes" : "no");
}
#endif

#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
/* the game's software keyboard: an alert with a text field over the topmost view controller, the game's thread waiting
   for the answer. 0 with the text in out, 1 for Cancel */
int gui_apple_text_input(char* out, size_t cap, const char* initial) {
    __block int cancelled = 1;
    NSString* start = [NSString stringWithUTF8String:initial ? initial : ""];
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
        UIViewController* top = nil;
        for (UIScene* sc in UIApplication.sharedApplication.connectedScenes) {
            if (![sc isKindOfClass:UIWindowScene.class]) continue;
            for (UIWindow* w in ((UIWindowScene*)sc).windows)
                if (w.isKeyWindow || !top) top = w.rootViewController;
        }
        while (top.presentedViewController) top = top.presentedViewController;
        if (!top) {
            dispatch_semaphore_signal(done);
            return;
        }
        UIAlertController* a = [UIAlertController alertControllerWithTitle:@"Enter text"
                                                                   message:nil
                                                            preferredStyle:UIAlertControllerStyleAlert];
        [a addTextFieldWithConfigurationHandler:^(UITextField* f) {
            f.text = start;
        }];
        [a addAction:[UIAlertAction actionWithTitle:@"Cancel"
                                              style:UIAlertActionStyleCancel
                                            handler:^(UIAlertAction* x) {
                                                dispatch_semaphore_signal(done);
                                            }]];
        [a addAction:[UIAlertAction actionWithTitle:@"OK"
                                              style:UIAlertActionStyleDefault
                                            handler:^(UIAlertAction* x) {
                                                const char* t = a.textFields.firstObject.text.UTF8String;
                                                if (t && strlen(t) < cap) {
                                                    strcpy(out, t);
                                                    cancelled = 0;
                                                }
                                                dispatch_semaphore_signal(done);
                                            }]];
        [top presentViewController:a animated:YES completion:nil];
    });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    return cancelled;
}
#endif

/* the system's thermal state as the menu's counter shows it: 0 nominal, 1 fair, 2 serious, 3 critical (the app is not told degrees) */
int gui_apple_thermal(void) {
#if TARGET_OS_IPHONE
    return (int)NSProcessInfo.processInfo.thermalState;
#else
    return 0;
#endif
}

void gui_apple_stats(char* out, size_t size) {
#if TARGET_OS_IPHONE
    task_vm_info_data_t info;
    mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    struct rusage usage;
    unsigned long long footprint = 0;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
        footprint = info.phys_footprint >> 20;
    if (getrusage(RUSAGE_SELF, &usage)) memset(&usage, 0, sizeof usage);
    snprintf(out, size,
             "resources: footprint %llu MiB, may still take %llu MiB, cpu %.1f s user %.1f s system, thermal %ld, "
             "low power %d",
             footprint, (unsigned long long)(os_proc_available_memory() >> 20),
             usage.ru_utime.tv_sec + usage.ru_utime.tv_usec / 1e6, usage.ru_stime.tv_sec + usage.ru_stime.tv_usec / 1e6,
             (long)NSProcessInfo.processInfo.thermalState, (int)NSProcessInfo.processInfo.lowPowerModeEnabled);
#else
    snprintf(out, size, "resources: (not iOS)");
#endif
}

#if TARGET_OS_IPHONE
/* P_TRACED: a debugger is attached to this process now (the loader's check before it asks one for code memory) */
static bool debugger_traced(void) {
    int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, (int)getpid()};
    struct kinfo_proc kp;
    size_t n = sizeof(kp);
    memset(&kp, 0, sizeof(kp));
    return sysctl(mib, 4, &kp, &n, NULL, 0) == 0 && (kp.kp_proc.p_flag & P_TRACED);
}
static void stamped(const char* what) {
    time_t now = time(NULL);
    struct tm local;
    char clock[16];
    localtime_r(&now, &local);
    strftime(clock, sizeof clock, "%H:%M:%S", &local);
    fprintf(stderr, "[%s] %s\n", clock, what);
}
/* Says in the console log when the debugger attaches or lets go, and when the app goes behind others, comes back or is
   warned of memory: what a game finds when it asks for code memory depends on them, and a log line has no time */
static bool watched_debugger;
/* once a second: whether a debugger is there, and the footprint each time it has moved 128 MiB from the last one said,
   which is what a run of long work costs, up to the system taking the app */
/* where the footprint is, by the kind of region the system tags it as: dirty and compressed pages of each tag, the
   largest first (the guest's memory, malloc, the graphics driver's and the code arenas show apart) */
static void footprint_by_tag(void) {
    unsigned long long sum[256] = {0};
    vm_address_t addr = 0;
    int pass;
    for (;;) {
        vm_size_t size = 0;
        vm_region_extended_info_data_t ex;
        mach_msg_type_number_t cnt = VM_REGION_EXTENDED_INFO_COUNT;
        mach_port_t obj;
        if (vm_region_64(mach_task_self(), &addr, &size, VM_REGION_EXTENDED_INFO, (vm_region_info_t)&ex, &cnt, &obj) !=
            KERN_SUCCESS)
            break;
        sum[ex.user_tag & 255] += (unsigned long long)(ex.pages_dirtied + ex.pages_swapped_out) * vm_page_size;
        addr += size;
        if (!size) break;
    }
    for (pass = 0; pass < 6; ++pass) {
        int best = -1, t;
        char text[64];
        for (t = 0; t < 256; ++t)
            if (sum[t] && (best < 0 || sum[t] > sum[best])) best = t;
        if (best < 0 || sum[best] < (16ull << 20)) break;
        snprintf(text, sizeof text, "app:   tag %d: %llu MiB", best, sum[best] >> 20);
        stamped(text);
        sum[best] = 0;
    }
}
static void* watch_thread(void* unused) {
    unsigned long long shown = 0;
    (void)unused;
    for (;;) {
        task_vm_info_data_t info;
        mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
        bool now = debugger_traced();
        sleep(1);
        if (now != watched_debugger) stamped(now ? "debugger: attached" : "debugger: let go");
        watched_debugger = now;
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS) {
            unsigned long long used = info.phys_footprint >> 20;
            if (used + 128 <= shown || used >= shown + 128) {
                char line[64];
                snprintf(line, sizeof line, "app: footprint %llu MiB", used);
                stamped(line);
                if (used >= 2500) footprint_by_tag();
                shown = used;
            }
        }
    }
    return NULL;
}
/* A test build's CPU sampler: every 10 ms the program counter of each running thread, and every 5 s the functions
   they were in most, by thread. Threads are only suspended for thread_get_state: no lock is taken while one is */
#define SAMPLE_SLOTS 8192
#define SAMPLE_THREADS 96
static struct {
    uint64_t pc;
    uint32_t thread, n;
} g_samples[SAMPLE_SLOTS];
static struct {
    uint64_t id;
    char name[32];
    uint32_t running, total;
} g_sthreads[SAMPLE_THREADS];
static uint32_t g_nsthreads;
static uint32_t sample_thread_index(thread_act_t t) {
    thread_identifier_info_data_t idi;
    mach_msg_type_number_t c = THREAD_IDENTIFIER_INFO_COUNT;
    uint32_t i;
    if (thread_info(t, THREAD_IDENTIFIER_INFO, (thread_info_t)&idi, &c) != KERN_SUCCESS) return UINT32_MAX;
    for (i = 0; i < g_nsthreads; i++)
        if (g_sthreads[i].id == idi.thread_id) return i;
    if (g_nsthreads == SAMPLE_THREADS) return UINT32_MAX;
    g_sthreads[i].id = idi.thread_id;
    {
        thread_extended_info_data_t ex;
        mach_msg_type_number_t ec = THREAD_EXTENDED_INFO_COUNT;
        if (thread_info(t, THREAD_EXTENDED_INFO, (thread_info_t)&ex, &ec) == KERN_SUCCESS && ex.pth_name[0])
            snprintf(g_sthreads[i].name, sizeof g_sthreads[i].name, "%s", ex.pth_name);
        else
            snprintf(g_sthreads[i].name, sizeof g_sthreads[i].name, "thread %llu", (unsigned long long)idi.thread_id);
    }
    g_nsthreads++;
    return i;
}
static void sample_report(void) {
    struct {
        uintptr_t start;
        uint32_t thread, n;
        const char* name;
        const char* image;
    } top[512];
    uint32_t ntop = 0, i, j, all = 0;
    char line[256];
    for (i = 0; i < SAMPLE_SLOTS; i++) {
        Dl_info di;
        uintptr_t start;
        const char *name = "(guest code)", *image = "";
        if (!g_samples[i].n) continue;
        all += g_samples[i].n;
        start = (uintptr_t)(g_samples[i].pc >> 12 << 12);
        if (dladdr((const void*)(uintptr_t)g_samples[i].pc, &di) && di.dli_fname) {
            const char* slash = strrchr(di.dli_fname, '/');
            image = slash ? slash + 1 : di.dli_fname;
            name = di.dli_sname ? di.dli_sname : "?";
            start = di.dli_saddr ? (uintptr_t)di.dli_saddr : (uintptr_t)di.dli_fbase;
        }
        for (j = 0; j < ntop; j++)
            if (top[j].start == start && top[j].thread == g_samples[i].thread) break;
        if (j == ntop) {
            if (ntop == 512) continue;
            top[ntop].start = start;
            top[ntop].thread = g_samples[i].thread;
            top[ntop].n = 0;
            top[ntop].name = name;
            top[ntop].image = image;
            ntop++;
        }
        top[j].n += g_samples[i].n;
    }
    snprintf(line, sizeof line, "sampler: %u samples of running threads in 5 s, by thread (running of all samples):", all);
    stamped(line);
    for (i = 0; i < g_nsthreads; i++)
        if (g_sthreads[i].running >= 10) {
            snprintf(line, sizeof line, "sampler:   %s: %u of %u", g_sthreads[i].name, g_sthreads[i].running,
                     g_sthreads[i].total);
            stamped(line);
        }
    for (int k = 0; k < 25; k++) {
        uint32_t best = UINT32_MAX;
        for (j = 0; j < ntop; j++)
            if (top[j].n && (best == UINT32_MAX || top[j].n > top[best].n)) best = j;
        if (best == UINT32_MAX || top[best].n < 5) break;
        snprintf(line, sizeof line, "sampler:   %u in %s (%s) on %s", top[best].n, top[best].name, top[best].image,
                 g_sthreads[top[best].thread].name);
        stamped(line);
        top[best].n = 0;
    }
    memset(g_samples, 0, sizeof g_samples);
    for (i = 0; i < g_nsthreads; i++) g_sthreads[i].running = g_sthreads[i].total = 0;
}
static void* sample_thread(void* unused) {
    thread_act_t self = mach_thread_self();
    unsigned ticks = 0;
    (void)unused;
    pthread_setname_np("snx-sampler");
    for (;;) {
        thread_act_array_t list;
        mach_msg_type_number_t n, k;
        usleep(10000);
        if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) continue;
        for (k = 0; k < n; k++) {
            thread_basic_info_data_t bi;
            mach_msg_type_number_t bc = THREAD_BASIC_INFO_COUNT;
            uint32_t ti;
            if (list[k] == self) goto next;
            ti = sample_thread_index(list[k]);
            if (ti == UINT32_MAX) goto next;
            g_sthreads[ti].total++;
            if (thread_info(list[k], THREAD_BASIC_INFO, (thread_info_t)&bi, &bc) != KERN_SUCCESS ||
                bi.run_state != TH_STATE_RUNNING)
                goto next;
            if (thread_suspend(list[k]) == KERN_SUCCESS) {
                arm_thread_state64_t st;
                mach_msg_type_number_t sc = ARM_THREAD_STATE64_COUNT;
                kern_return_t r = thread_get_state(list[k], ARM_THREAD_STATE64, (thread_state_t)&st, &sc);
                thread_resume(list[k]);
                if (r == KERN_SUCCESS) {
                    uint64_t pc = (uint64_t)arm_thread_state64_get_pc(st);
                    uint32_t h = (uint32_t)((pc >> 2) * 0x9E3779B1u ^ ti * 0x85EBCA77u) % SAMPLE_SLOTS, probe;
                    g_sthreads[ti].running++;
                    for (probe = 0; probe < 64; probe++, h = (h + 1) % SAMPLE_SLOTS)
                        if (!g_samples[h].n || (g_samples[h].pc == pc && g_samples[h].thread == ti)) {
                            g_samples[h].pc = pc;
                            g_samples[h].thread = ti;
                            g_samples[h].n++;
                            break;
                        }
                }
            }
        next:
            mach_port_deallocate(mach_task_self(), list[k]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof *list);
        if (++ticks % 500 == 0) sample_report();
    }
    return NULL;
}
static void watch_start(void) {
    NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
    static NSString* const names[][2] = {{@"UIApplicationDidEnterBackgroundNotification", @"app: in the background"},
                                         {@"UIApplicationWillEnterForegroundNotification", @"app: back in the foreground"},
                                         {@"UIApplicationDidReceiveMemoryWarningNotification", @"app: memory warning"},
                                         {@"UIApplicationWillResignActiveNotification", @"app: resigning active"},
                                         {@"UIApplicationDidBecomeActiveNotification", @"app: active"}};
    size_t i;
    watched_debugger = debugger_traced();
    stamped(watched_debugger ? "debugger: attached at start" : "debugger: not attached at start");
    for (i = 0; i < sizeof names / sizeof *names; ++i) {
        NSString* line = names[i][1];
        [center addObserverForName:names[i][0]
                            object:nil
                             queue:nil
                        usingBlock:^(NSNotification* note) {
                            (void)note;
                            stamped(line.UTF8String);
                        }];
    }
    {
        pthread_t thread;
        if (!pthread_create(&thread, NULL, watch_thread, NULL)) pthread_detach(thread);
        if (!pthread_create(&thread, NULL, sample_thread, NULL)) pthread_detach(thread);
    }
}
#endif

void gui_apple_log_start(void) {
#if TARGET_OS_IPHONE
    @autoreleasepool {
        NSURL* documents = [NSFileManager.defaultManager URLsForDirectory:NSDocumentDirectory
                                                                inDomains:NSUserDomainMask].firstObject;
        NSURL* logs = [documents URLByAppendingPathComponent:@"logs" isDirectory:YES];
        const char* path = NULL;
        if (logs) {
            [NSFileManager.defaultManager createDirectoryAtURL:logs withIntermediateDirectories:YES attributes:nil error:NULL];
            path = [logs URLByAppendingPathComponent:@"scarletnx-console.log"].path.fileSystemRepresentation;
        }
        time_t now = time(NULL);
        if (!path || !freopen(path, "a", stderr)) return;
        freopen(path, "a", stdout);
        setvbuf(stdout, NULL, _IONBF, 0);
        setvbuf(stderr, NULL, _IONBF, 0);
        fprintf(stderr, "\n===== ScarletNX starts %s", ctime(&now));
        fprintf(stderr, "device:\n");
        sysctl_line("hw.machine");
        sysctl_line("hw.model");
        sysctl_line("kern.osversion");
        sysctl_line("kern.osrelease");
        sysctl_number("hw.memsize");
        sysctl_number("hw.pagesize");
        sysctl_number("hw.ncpu");
        sysctl_number("hw.logicalcpu");
        sysctl_number("hw.optional.arm.FEAT_LSE");
        sysctl_number("vm.pagesize");
        sysctl_number("kern.maxfilesperproc");
        fprintf(stderr, "system: iOS %s, physical memory %llu MiB, thermal state %ld, low power %d\n",
                NSProcessInfo.processInfo.operatingSystemVersionString.UTF8String,
                (unsigned long long)(NSProcessInfo.processInfo.physicalMemory >> 20),
                (long)NSProcessInfo.processInfo.thermalState, (int)NSProcessInfo.processInfo.lowPowerModeEnabled);
        fprintf(stderr, "limits:\n");
        limit_line("AS", RLIMIT_AS);
        limit_line("DATA", RLIMIT_DATA);
        limit_line("RSS", RLIMIT_RSS);
        limit_line("NOFILE", RLIMIT_NOFILE);
        {
            task_vm_info_data_t info;
            mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
            if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&info, &count) == KERN_SUCCESS)
                fprintf(stderr, "  task address space: %#llx to %#llx (%llu GiB), virtual size %llu MiB, footprint %llu MiB\n",
                        (unsigned long long)info.min_address, (unsigned long long)info.max_address,
                        (unsigned long long)((info.max_address - info.min_address) >> 30),
                        (unsigned long long)(info.virtual_size >> 20), (unsigned long long)(info.phys_footprint >> 20));
            else
                fprintf(stderr, "  task address space: (unavailable)\n");
        }
        fprintf(stderr, "  available memory before the first allocation: %llu MiB\n",
                (unsigned long long)(os_proc_available_memory() >> 20));
        entitlement_lines();
        fprintf(stderr, "app: %s\n", NSBundle.mainBundle.bundleIdentifier.UTF8String);
        fprintf(stderr, "documents: %s\n", documents.fileSystemRepresentation);
        watch_start();
    }
#endif
}

void gui_apple_start(void) {
#if TARGET_OS_IPHONE
    @autoreleasepool {
        NSFileManager* files = NSFileManager.defaultManager;
        NSURL* documents = [files URLsForDirectory:NSDocumentDirectory inDomains:NSUserDomainMask].firstObject;
        /* what the player sees: dumps to put a game's .nsp or .xci in, keys for prod.keys, recompiled for what the app
           makes of them, logs for what it prints */
        for (NSString* name in @[ @"dumps", @"keys", @"recompiled", @"logs" ])
            [files createDirectoryAtURL:[documents URLByAppendingPathComponent:name isDirectory:YES]
                withIntermediateDirectories:YES
                                 attributes:nil
                                      error:NULL];
        if (documents) chdir(documents.fileSystemRepresentation);
    }
#endif
}

/* the window AppKit or UIKit has under the SDL one */
static void* native_window(SDL_Window* window) {
    SDL_PropertiesID properties = window ? SDL_GetWindowProperties(window) : 0;
#if TARGET_OS_IPHONE
    return properties ? SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, NULL) : NULL;
#else
    return properties ? SDL_GetPointerProperty(properties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, NULL) : NULL;
#endif
}

static SDL_DialogFileCallback pick_done;
static void* pick_data;
/* the picker's answer, as an SDL dialog gives it: a list of the one path, empty when none was picked */
static void picked(void* data, const char* path) {
    const char* list[2] = {path, NULL};
    SDL_DialogFileCallback done = pick_done;
    (void)data;
    pick_done = NULL;
    if (done) done(pick_data, list, -1);
}
int gui_apple_pick(SDL_Window* window, int folder, SDL_DialogFileCallback done, void* data, char* message,
                   size_t size) {
    void* native = native_window(window);
    pick_done = done;
    pick_data = data;
    if (native && snx_apple_pick(native, folder != 0, picked, NULL)) return 1;
    pick_done = NULL;
    snprintf(message, size, "%s",
             native ? snx_tr("The document picker did not open.") : snx_tr("No window to show the picker over."));
    return 0;
}

int gui_apple_share(SDL_Window* window, const char* path, char* message, size_t size) {
    void* native = native_window(window);
    if (native && snx_apple_share(native, path)) return 1;
    snprintf(message, size, snx_tr("The log could not be shown: it is at %s"), path);
    return 0;
}

static char** played;
static int played_count;
void gui_apple_play(const char* const* argv) {
    int n = 0, i;
    while (argv[n]) ++n;
    for (i = 0; i < played_count; ++i) free(played[i]);
    free(played);
    played_count = 0;
    played = (char**)calloc((size_t)n + 1, sizeof(*played));
    if (!played) return;
    for (i = 0; i < n; ++i) played[i] = strdup(argv[i]);
    played_count = n;
}
char** gui_apple_played(int* argc) {
    *argc = played_count;
    return played_count ? played : NULL;
}
