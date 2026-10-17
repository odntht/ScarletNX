/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* am: the applet manager (appletOE, appletAE) and its storages */
#include "nx/hle/services/services_int.h"
#include "nx/hle/services/hid.h"
#include "hle/services/am.swipc.inc"
/* ================================================================ am */
static struct {
    KEvent* msg_ev;
    u32 msgq[16];
    int nmsg;
    KEvent* launch_ev;
    KEvent* res_ev;
    KEvent* sleep_ev; /* GetAcquiredSleepLockEvent: signaled when a RequestToAcquireSleepLock is granted */
    u64 layer;        /* the next managed display layer id, CreateManagedDisplayLayer's and SeparableLayer's */
    int user_given;   /* PreselectedUser was popped: it is there once */
} am = {.layer = 1};
u64 g_user_id[2] = {0x0123456789ABCDEFull, 0xFEDCBA9876543210ull};
static void am_push_msg(u32 m) {
    pm_lock(&g_klock);
    int queued = am.nmsg < 16;
    if (queued) am.msgq[am.nmsg++] = m;
    pm_unlock(&g_klock);
    /* a full queue is already signaled, and the message is lost: say so rather than signal for nothing */
    if (queued)
        kevent_signal(am.msg_ev);
    else
        WARN("am: applet message queue full, message %u dropped\n", m);
}
void am_init(void) {
    am.msg_ev = owned_event("applet msg");
    am.launch_ev = owned_event("launchable");
    am.res_ev = owned_event("res change");
    am.sleep_ev = owned_event("sleep lock");
    am_push_msg(0xF);  /* FocusStateChanged */
    am_push_msg(0x1F); /* PerformanceModeChanged */
    kevent_signal(am.launch_ev);
}

/* IStorage / IStorageAccessor */
typedef struct {
    u8* d;
    u64 size;
} StorageData;
static void storage_destroy(SvcObj* o) { free(((StorageData*)o->data)->d); }
/* an accessor holds a reference on its storage (u[0]), dropped when the game closes the accessor */
static void storage_acc_destroy(SvcObj* o) { svcobj_unref((SvcObj*)(size_t)o->u[0]); }
static StorageData* accessor_storage(SvcObj* accessor) { return ((SvcObj*)(size_t)accessor->u[0])->data; }
static int nn_am_service_IStorageAccessor_GetSize(SvcObj* self, i64* size) {
    *size = (i64)accessor_storage(self)->size;
    return 0;
}
static int nn_am_service_IStorageAccessor_Write(SvcObj* self, i64 offset, IpcBuf data) {
    StorageData* s = accessor_storage(self);
    u64 off = (u64)offset, n = data.size;
    /* off + n can wrap: compare against what is left instead */
    if (off > s->size || n > s->size - off) return RC(128, 503);
    memcpy(s->d + off, GP(data.addr), n);
    return 0;
}
static int nn_am_service_IStorageAccessor_Read(SvcObj* self, i64 offset, IpcBuf data) {
    StorageData* s = accessor_storage(self);
    u64 off = (u64)offset, n = data.size;
    if (off > s->size) return RC(128, 503);
    if (off + n > s->size) n = s->size - off;
    memcpy(GP(data.addr), s->d + off, n);
    return 0;
}
static int nn_am_service_IStorage_Open(SvcObj* self, SvcObj** accessor) {
    SvcObj* a = nn_am_service_IStorageAccessor_new(0);
    a->u[0] = (u64)(size_t)self;
    svcobj_ref(self);
    a->destroy = storage_acc_destroy;
    *accessor = a;
    return 0;
}
/* OpenTransferStorage gives an ITransferStorageAccessor, which only a storage made over transfer memory has. These
   storages are plain memory (CreateTransferMemoryStorage copies the transfer memory into one) */
static int nn_am_service_IStorage_OpenTransferStorage(SvcObj* self, SvcObj** accessor) { return RC(128, 503); }
/* NULL when the guest asks for more than the host can allocate */
static SvcObj* am_storage_new(u64 size, const void* init) {
    u8* d = calloc(1, size ? size : 1);
    if (!d) {
        WARN("am: no memory for a %llu byte storage\n", (unsigned long long)size);
        return NULL;
    }
    SvcObj* o = nn_am_service_IStorage_new(sizeof(StorageData));
    StorageData* s = o->data;
    s->d = d;
    s->size = size;
    if (init) memcpy(s->d, init, size);
    o->destroy = storage_destroy;
    return o;
}
/* the result of a command that makes a storage */
static int out_storage(SvcObj** storage, u64 size, const void* init) {
    *storage = am_storage_new(size, init);
    return *storage ? 0 : RC(128, 503);
}
static int is_storage(const SvcObj* o) { return o && o->dispatch == nn_am_service_IStorage_dispatch; }

static int nn_am_service_ICommonStateGetter_GetEventHandle(SvcObj* self, u32* event) {
    *event = ev_handle(am.msg_ev);
    return 0;
}
static int nn_am_service_ICommonStateGetter_ReceiveMessage(SvcObj* self, nn_am_AppletMessage* message) {
    pm_lock(&g_klock);
    if (!am.nmsg) {
        am.msg_ev->signaled = 0;
        pm_unlock(&g_klock);
        return RC(128, 3);
    }
    *message = am.msgq[0];
    memmove(am.msgq, am.msgq + 1, (--am.nmsg) * 4);
    if (!am.nmsg) am.msg_ev->signaled = 0;
    pm_unlock(&g_klock);
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetOperationMode(SvcObj* self, u8* mode) {
    *mode = (u8)g_opt_docked;
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetPerformanceMode(SvcObj* self, u32* mode) {
    *mode = (u32)g_opt_docked;
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetCurrentFocusState(SvcObj* self, u8* state) {
    *state = 1; /* InFocus */
    return 0;
}
/* granted at once, there is no sleep here */
static int nn_am_service_ICommonStateGetter_RequestToAcquireSleepLock(SvcObj* self) {
    kevent_signal(am.sleep_ev);
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetAcquiredSleepLockEvent(SvcObj* self, u32* event) {
    *event = ev_handle(am.sleep_ev);
    return 0;
}
/* a lock accessor's event (u[0]) and whether it is held (u[1], atomic: game threads call at once) */
static void lock_accessor_destroy(SvcObj* o) { kobj_unref(&((KEvent*)(size_t)o->u[0])->o); }
static int lock_accessor(SvcObj** accessor) {
    SvcObj* o = nn_am_service_ILockAccessor_new(0);
    o->u[0] = (u64)(size_t)owned_event("button lock");
    o->destroy = lock_accessor_destroy;
    *accessor = o;
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetHomeButtonReaderLockAccessor(SvcObj* self, SvcObj** accessor) {
    return lock_accessor(accessor);
}
static int nn_am_service_ICommonStateGetter_GetReaderLockAccessorEx(SvcObj* self, u32 button_type, SvcObj** accessor) {
    return lock_accessor(accessor);
}
static int nn_am_service_ICommonStateGetter_GetWriterLockAccessorEx(SvcObj* self, u32 button_type, SvcObj** accessor) {
    return lock_accessor(accessor);
}
static int nn_am_service_ILockAccessor_TryLock(SvcObj* self, u8 return_handle, u8* is_locked, u32* event) {
    __atomic_store_n(&self->u[1], 1, __ATOMIC_RELAXED);
    *is_locked = 1;
    if (return_handle) *event = ev_handle((KEvent*)(size_t)self->u[0]);
    return 0;
}
static int nn_am_service_ILockAccessor_Unlock(SvcObj* self) {
    __atomic_store_n(&self->u[1], 0, __ATOMIC_RELAXED);
    kevent_signal((KEvent*)(size_t)self->u[0]);
    return 0;
}
static int nn_am_service_ILockAccessor_GetEvent(SvcObj* self, u32* event) {
    *event = ev_handle((KEvent*)(size_t)self->u[0]);
    return 0;
}
static int nn_am_service_ILockAccessor_IsLocked(SvcObj* self, u8* is_locked) {
    *is_locked = (u8)__atomic_load_n(&self->u[1], __ATOMIC_RELAXED);
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetDefaultDisplayResolution(SvcObj* self, i32* width, i32* height) {
    *width = g_opt_docked ? 1920 : 1280;
    *height = g_opt_docked ? 1080 : 720;
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetDefaultDisplayResolutionChangeEvent(SvcObj* self, u32* event) {
    *event = ev_handle(am.res_ev);
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetHdcpAuthenticationStateChangeEvent(SvcObj* self, u32* event) {
    *event = ev_handle(am.res_ev);
    return 0;
}
static int nn_am_service_ICommonStateGetter_GetCurrentPerformanceConfiguration(SvcObj* self, u32* configuration) {
    *configuration = 0x00020003; /* the one apm reports */
    return 0;
}
/* the main loop saves what it keeps and ends the process */
static int nn_am_service_ISelfController_Exit(SvcObj* self) {
    g_quit = 1;
    return 0;
}
static int nn_am_service_ISelfController_GetLibraryAppletLaunchableEvent(SvcObj* self, u32* event) {
    *event = ev_handle(am.launch_ev);
    return 0;
}
static int nn_am_service_ISelfController_CreateManagedDisplayLayer(SvcObj* self, u64* layer_id) {
    *layer_id = __atomic_fetch_add(&am.layer, 1, __ATOMIC_RELAXED);
    return 0;
}
static int nn_am_service_ISelfController_CreateManagedDisplaySeparableLayer(SvcObj* self, u64* layer_id,
                                                                            u64* recording_layer_id) {
    *layer_id = __atomic_fetch_add(&am.layer, 2, __ATOMIC_RELAXED); /* two layers, never one handed out before */
    *recording_layer_id = *layer_id + 1;
    return 0;
}
static int nn_am_service_ISelfController_GetAccumulatedSuspendedTickChangedEvent(SvcObj* self, u32* event) {
    return new_event(event, "suspend", 0);
}
static int nn_am_service_IWindowController_GetAppletResourceUserId(SvcObj* self,
                                                                   nn_applet_AppletResourceUserId* aruid) {
    *aruid = 0x1234;
    return 0;
}
static int nn_am_service_IApplicationFunctions_PopLaunchParameter(SvcObj* self, u32 kind, SvcObj** storage) {
    u8 buf[0x88];
    u32 magic = 0xC79497CA, one = 1;
    /* only the PreselectedUser (2), there once */
    if (kind != 2 || __atomic_exchange_n(&am.user_given, 1, __ATOMIC_SEQ_CST)) return RC(128, 2);
    memset(buf, 0, sizeof buf);
    memcpy(buf, &magic, 4);
    memcpy(buf + 4, &one, 4);
    memcpy(buf + 8, g_user_id, 16);
    return out_storage(storage, sizeof buf, buf);
}
static int nn_am_service_IApplicationFunctions_GetDesiredLanguage(SvcObj* self, nn_settings_LanguageCode* code) {
    set_language_code(g_opt_lang, code->v);
    return 0;
}
static int nn_am_service_IApplicationFunctions_GetDisplayVersion(SvcObj* self, nn_oe_DisplayVersion* version) {
    strcpy((char*)version->v, "1.0.0");
    return 0;
}
static int nn_am_service_IApplicationFunctions_GetSaveDataSize(SvcObj* self, u8 type, const nn_account_Uid* uid,
                                                               i64* normal_size, i64* journal_size) {
    *normal_size = 0x4000000;
    *journal_size = 0x4000000;
    return 0;
}
static int nn_am_service_IApplicationFunctions_GetLimitedApplicationLicenseUpgradableEvent(SvcObj* self, u32* event) {
    return new_event(event, "appfn ev", 0);
}
static int nn_am_service_IApplicationFunctions_GetPseudoDeviceId(SvcObj* self, nn_util_Uuid* id) {
    static const u64 pseudo[2] = {0x1111222233334444ull, 0x5555666677778888ull};
    memcpy(id->v, pseudo, 16);
    return 0;
}
static int nn_am_service_IApplicationFunctions_GetGpuErrorDetectedSystemEvent(SvcObj* self, u32* event) {
    return new_event(event, "appfn ev", 0);
}
static int nn_am_service_IApplicationFunctions_GetFriendInvitationStorageChannelEvent(SvcObj* self, u32* event) {
    return new_event(event, "appfn ev", 0);
}
static int nn_am_service_IApplicationFunctions_TryPopFromFriendInvitationStorageChannel(SvcObj* self,
                                                                                        SvcObj** storage) {
    return RC(128, 2); /* nothing queued */
}
static int nn_am_service_IApplicationFunctions_GetNotificationStorageChannelEvent(SvcObj* self, u32* event) {
    return new_event(event, "appfn ev", 0);
}
static int nn_am_service_IApplicationFunctions_TryPopFromNotificationStorageChannel(SvcObj* self, SvcObj** storage) {
    return RC(128, 2); /* nothing queued */
}
static int nn_am_service_IApplicationFunctions_GetHealthWarningDisappearedSystemEvent(SvcObj* self, u32* event) {
    return new_event(event, "appfn ev", 0);
}
static int nn_am_service_IApplicationFunctions_GetUnknownEvent210(SvcObj* self, u32* event) {
    return new_event(event, "appfn ev", 0);
}
/* Inline swkbd stays alive between text entries, until the client sends Finalize. */
typedef struct {
    u32 id, mode;
    int popped_out, popped_interactive;
    SvcObj* in[8]; /* the storages PushInData passed, in order, under lib_lock: game threads push and pop at once */
    int nin;
    KEvent* state_ev;
    KEvent* interactive_ev;
    SvcObj* replies[8];
    int nreplies, completed;
    u32 keyboard_state;
    int utf8, changed_v2;
    u16 text[502];
} LibApplet;
static PMutex lib_lock = PMUTEX_INIT;
static int inline_keyboard(const LibApplet* la) { return la->id == 0x11 && (la->mode == 1 || la->mode == 3); }
static void lib_applet_destroy(SvcObj* o) {
    LibApplet* la = o->data;
    for (int i = 0; i < la->nin; i++) svcobj_unref(la->in[i]);
    for (int i = 0; i < la->nreplies; i++) svcobj_unref(la->replies[i]);
    if (la->state_ev) kobj_unref(&la->state_ev->o);
    if (la->interactive_ev) kobj_unref(&la->interactive_ev->o);
}
/* the controller applet (ShowControllerSupport) answers ControllerSupportResultInfo {i8 player_count; pad[3];
   u32 selected_id} and a u32 result, which the SDK turns into success (0) or cancelled (2). The last storage
   pushed is the ControllerSupportArg, whose byte 0 is min_player_count. */
static i8 controller_min_players(LibApplet* la) { /* lib_lock held */
    SvcObj* arg = la->nin ? la->in[la->nin - 1] : NULL;
    return is_storage(arg) && ((StorageData*)arg->data)->size ? (i8)((StorageData*)arg->data)->d[0] : 0;
}
static SvcObj* controller_result(i8 min_players) {
    u8 r[0xC];
    u32 selected_id = 0;
    int players = hid_connected_players(&selected_id);
    memset(r, 0, sizeof r);
    u32 result = min_players <= players ? 0 : 2;
    if (!result) {
        r[0] = (u8)players;
        memcpy(r + 4, &selected_id, 4);
    }
    memcpy(r + 8, &result, 4);
    if (result) LOG("applet: controller support asks for %d players, %d connected: cancelled\n", min_players, players);
    return am_storage_new(sizeof r, r);
}
/* Standard swkbd returns 0x7D8 bytes: u32 close result, then a UTF-16 name. */
static u32 swkbd_text(u8* out, const char* text, u32 max_chars) {
    u32 count = 0;
    const u8* p = (const u8*)text;
    while (*p && count < max_chars && count < (0x7D8 - 4) / 2 - 1) {
        u32 cp = *p++;
        if (cp >= 0xF0 && cp < 0xF5 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80 && (p[2] & 0xC0) == 0x80 &&
            !(cp == 0xF0 && p[0] < 0x90) && !(cp == 0xF4 && p[0] >= 0x90)) {
            cp = ((cp & 7) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
        } else if (cp >= 0xE0 && cp < 0xF0 && (p[0] & 0xC0) == 0x80 && (p[1] & 0xC0) == 0x80 &&
                   !(cp == 0xE0 && p[0] < 0xA0) && !(cp == 0xED && p[0] >= 0xA0)) {
            cp = ((cp & 15) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F);
            p += 2;
        } else if (cp >= 0xC2 && cp < 0xE0 && (p[0] & 0xC0) == 0x80) {
            cp = ((cp & 31) << 6) | (p[0] & 0x3F);
            p++;
        } else if (cp >= 0x80) {
            cp = 0xFFFD;
        }
        if (cp >= 0x10000) {
            if (count + 2 > max_chars || count + 2 >= (0x7D8 - 4) / 2) break;
            cp -= 0x10000;
            u16 hi = 0xD800 | (cp >> 10), lo = 0xDC00 | (cp & 0x3FF);
            memcpy(out + 4 + count++ * 2, &hi, 2);
            memcpy(out + 4 + count++ * 2, &lo, 2);
        } else {
            u16 ch = cp;
            memcpy(out + 4 + count++ * 2, &ch, 2);
        }
    }
    return count;
}
#define SWKBD_MAX_CHARS ((0x7D8 - 4) / 2 - 1)
/* the text length limits of the config the game pushed second (lib_lock held) */
static void swkbd_limits(LibApplet* la, u32* max_chars, u32* min_chars) {
    StorageData* cfg = la->nin > 1 && is_storage(la->in[1]) ? la->in[1]->data : NULL;
    *max_chars = SWKBD_MAX_CHARS;
    *min_chars = 0;
    if (cfg && cfg->size >= 0x3B4) {
        u32 limit;
        memcpy(&limit, cfg->d + 0x3AC, 4);
        if (limit && limit < *max_chars) *max_chars = limit;
        memcpy(min_chars, cfg->d + 0x3B0, 4);
    }
}
/* asks the host for the text, which can take as long as the player types: no lock held */
static SvcObj* swkbd_result(u32 max_chars, u32 min_chars) {
    u8 r[0x7D8] = {0};
    if (min_chars > max_chars) {
        WARN("applet: swkbd minimum text length %u exceeds maximum %u\n", min_chars, max_chars);
        min_chars = max_chars;
    }
    char input[4096];
    int result;
    u32 chars = 0;
    const char* preset = getenv("SNX_SWKBD_TEXT");
    if (preset) {
        chars = swkbd_text(r, preset, max_chars);
        result = chars < min_chars ? 1 : 0;
    } else {
        do {
            result = plat_text_input(input, sizeof input, "");
            if (result) break;
            memset(r + 4, 0, sizeof r - 4);
            chars = swkbd_text(r, input, max_chars);
        } while (chars < min_chars);
    }
    LOG("applet: swkbd %s with %u UTF-16 code units\n", result ? "cancelled" : "accepted", chars);
    if (result) {
        u32 cancelled = 1;
        memcpy(r, &cancelled, 4);
        if (result < 0) WARN("applet: host software keyboard unavailable (install zenity or kdialog on POSIX)\n");
    }
    return am_storage_new(sizeof r, r);
}
/* The inline protocol has an eight-byte state/type header. Its fixed string fields and
   trailing length are different from the foreground keyboard's close result. */
static u32 swkbd_utf8(char* out, const u16* text) {
    u32 i = 0, n = 0;
    while (i < 501 && text[i]) {
        u32 cp = text[i++];
        if (cp >= 0xD800 && cp <= 0xDBFF && i < 501 && text[i] >= 0xDC00 && text[i] <= 0xDFFF)
            cp = 0x10000 + ((cp - 0xD800) << 10) + text[i++] - 0xDC00;
        else if (cp >= 0xD800 && cp <= 0xDFFF)
            cp = 0xFFFD;
        if (cp < 0x80)
            out[n++] = (char)cp;
        else {
            if (cp < 0x800)
                out[n++] = (char)(0xC0 | (cp >> 6));
            else {
                if (cp < 0x10000)
                    out[n++] = (char)(0xE0 | (cp >> 12));
                else {
                    out[n++] = (char)(0xF0 | (cp >> 18));
                    out[n++] = (char)(0x80 | ((cp >> 12) & 63));
                }
                out[n++] = (char)(0x80 | ((cp >> 6) & 63));
            }
            out[n++] = (char)(0x80 | (cp & 63));
        }
    }
    out[n] = 0;
    return i;
}
/* lib_lock held. The queue owns each storage until PopInteractiveOutData transfers it. */
static int inline_reply(LibApplet* la, u32 type) {
    u8 packet[0x7ED];
    u32 bytes = 8, len = 0, field = la->utf8 ? 0x7D4 : 0x3EC;
    SvcObj* reply;
    if (la->nreplies == 8) return RC(128, 503);
    memset(packet, 0, sizeof packet);
    memcpy(packet, &la->keyboard_state, 4);
    memcpy(packet + 4, &type, 4);
    if (type == 0) bytes++; /* FinishedInitialize carries a one-byte result. */
    if (type == 2 || type == 7 || type == 0xD || type == 0xF || type == 5 || type == 9) {
        while (len < 501 && la->text[len]) len++;
        if (la->utf8)
            swkbd_utf8((char*)packet + 8, la->text);
        else
            memcpy(packet + 8, la->text, sizeof la->text);
        memcpy(packet + 8 + field, &len, 4);
        bytes += field + 4;
        if (type != 5 && type != 9) {
            memset(packet + 8 + field + 4, 0xFF, 8); /* no active dictionary selection */
            memcpy(packet + 8 + field + 12, &len, 4);
            bytes += 12;
            if (type == 0xD || type == 0xF) bytes++;
        }
    }
    reply = am_storage_new(bytes, packet);
    if (!reply) return RC(128, 503);
    la->replies[la->nreplies++] = reply;
    kevent_signal(la->interactive_ev);
    return 0;
}
static int nn_am_service_ILibraryAppletAccessor_RequestExit(SvcObj* self) {
    LibApplet* la = self->data;
    pm_lock(&lib_lock);
    la->completed = 1;
    if (la->state_ev) kevent_signal(la->state_ev);
    pm_unlock(&lib_lock);
    return 0;
}
static int nn_am_service_ILibraryAppletAccessor_Terminate(SvcObj* self) {
    return nn_am_service_ILibraryAppletAccessor_RequestExit(self);
}
static int nn_am_service_ILibraryAppletAccessor_PushInteractiveInData(SvcObj* self, SvcObj* storage) {
    LibApplet* la = self->data;
    StorageData* data;
    u32 request, max_chars = 501, min_chars = 0, len;
    u32 text_offset, appear_offset;
    u64 flags;
    int result = 0, dialog;
    char input[4096], initial[2004];
    u8 converted[0x7D8];
    const char* preset;
    const u8* calc;
    if (!inline_keyboard(la)) return 0;
    if (!is_storage(storage)) return RC(128, 503);
    data = storage->data;
    if (data->size < 4) return RC(128, 503);
    memcpy(&request, data->d, 4);
    if (request == 4) return nn_am_service_ILibraryAppletAccessor_RequestExit(self);
    pm_lock(&lib_lock);
    if (la->completed) goto done;
    if (request == 0xD) {
        if (data->size < 5)
            result = RC(128, 503);
        else
            la->changed_v2 = data->d[4] != 0;
        goto done;
    }
    if (request != 0xA) {
        /* Dictionaries are not used by the host dialog, so release their guest storage. */
        if (request == 6) result = inline_reply(la, 0xB);
        if (request == 7) result = inline_reply(la, 0xA);
        if (request == 0xC) result = inline_reply(la, 0xC);
        goto done;
    }
    if (data->size < 4 + 0x45D || la->nreplies > 3) {
        result = RC(128, 503);
        goto done;
    }
    calc = data->d + 4;
    /* The 0x4E8 SDK packet moves AppearArg before volume/cursor and extends it
       to 0x70 bytes. SetInputText and SetUtf8Mode address 0x90 and 0x484. */
    text_offset = data->size >= 4 + 0x4E8 ? 0x90 : 0x68;
    appear_offset = data->size >= 4 + 0x4E8 ? 0x18 : 0x20;
    memcpy(&flags, calc + 8, 8);
    if (flags & 0x20) la->utf8 = calc[text_offset + 0x3F4] != 0;
    if (flags & 8) {
        memcpy(la->text, calc + text_offset, sizeof la->text);
        la->text[501] = 0;
    }
    if (flags & 1) {
        la->keyboard_state = 1;
        result = inline_reply(la, 0);
        if (result) goto done;
    }
    if (flags & 0x80) {
        la->keyboard_state = 1;
        result = inline_reply(la, 1);
    }
    if (!(flags & 4) || result || la->keyboard_state == 3) goto done;
    memcpy(&max_chars, calc + appear_offset + 0x20, 4);
    memcpy(&min_chars, calc + appear_offset + 0x24, 4);
    if (!max_chars || max_chars > 501) max_chars = 501;
    if ((i32)min_chars < 0) min_chars = 0;
    if (min_chars > max_chars) min_chars = max_chars;
    la->keyboard_state = 3;
    result = inline_reply(la, 1);
    if (result) goto done;
    swkbd_utf8(initial, la->text);
    /* The host dialog may wait for the player. Other applets and RequestExit must
       still be able to run, and an exit during the dialog must not revive swkbd. */
    pm_unlock(&lib_lock);
    preset = getenv("SNX_SWKBD_TEXT");
    do {
        dialog = preset ? 0 : plat_text_input(input, sizeof input, initial);
        memset(converted, 0, sizeof converted);
        len = dialog ? 0 : swkbd_text(converted, preset ? preset : input, max_chars);
        if (preset && len < min_chars) dialog = 1;
    } while (!dialog && len < min_chars);
    pm_lock(&lib_lock);
    if (la->completed) goto done;
    LOG("applet: inline swkbd %s with %u UTF-16 code units\n", dialog ? "cancelled" : "accepted", len);
    if (dialog)
        result = inline_reply(la, 6);
    else {
        memcpy(la->text, converted + 4, sizeof la->text);
        result = inline_reply(la, la->changed_v2 ? (la->utf8 ? 0xF : 0xD) : (la->utf8 ? 7 : 2));
        if (!result) result = inline_reply(la, la->utf8 ? 9 : 5);
    }
    la->keyboard_state = 1;
    if (!result) result = inline_reply(la, 1);
done:
    pm_unlock(&lib_lock);
    return result;
}

static int nn_am_service_ILibraryAppletAccessor_GetAppletStateChangedEvent(SvcObj* self, u32* event) {
    LibApplet* la = self->data;
    if (!inline_keyboard(la)) return new_event(event, "applet state", 1);
    *event = ev_handle(la->state_ev);
    return 0;
}
static int nn_am_service_ILibraryAppletAccessor_IsCompleted(SvcObj* self, u8* completed) {
    LibApplet* la = self->data;
    pm_lock(&lib_lock);
    *completed = !inline_keyboard(la) || la->completed;
    pm_unlock(&lib_lock);
    return 0;
}
/* the controller applet's answer depends on what was pushed */
static int nn_am_service_ILibraryAppletAccessor_PushInData(SvcObj* self, SvcObj* storage) {
    LibApplet* la = self->data;
    pm_lock(&lib_lock);
    if (storage && la->nin < 8) {
        svcobj_ref(storage);
        la->in[la->nin++] = storage;
    }
    pm_unlock(&lib_lock);
    return 0;
}
/* PopOutData and PopInteractiveOutData are separate channels, each with one storage. Web applets (Web, Shop,
   OfflineWeb, LoginShare, WifiWebAuth) return a 0x1010 byte WebCommonReturnValue (exit reason 0, last url, url size),
   whose size callers check. PlayerSelect returns UiReturnArg {u64 result; Uid uid}: the one user, selected */
static int pop_data(LibApplet* la, int interactive, SvcObj** storage) {
    if (inline_keyboard(la)) {
        int result = RC(128, 2);
        pm_lock(&lib_lock);
        if (interactive && la->nreplies) {
            *storage = la->replies[0];
            la->nreplies--;
            memmove(la->replies, la->replies + 1, la->nreplies * sizeof la->replies[0]);
            if (!la->nreplies) kevent_clear(la->interactive_ev);
            result = 0;
        }
        pm_unlock(&lib_lock);
        return result;
    }
    u32 id = la->id;
    u64 n = 0x10;
    /* Standard swkbd has no interactive output unless it requests application text validation. */
    if (id == 0x11 && interactive) return RC(128, 2);
    if (__atomic_exchange_n(interactive ? &la->popped_interactive : &la->popped_out, 1, __ATOMIC_SEQ_CST))
        return RC(128, 2);
    SvcObj* st;
    if (id == 0x0C && !interactive) {
        pm_lock(&lib_lock);
        i8 min_players = controller_min_players(la);
        pm_unlock(&lib_lock);
        st = controller_result(min_players);
        n = 0xC;
    } else if (id == 0x11 && !interactive) {
        u32 max_chars, min_chars;
        pm_lock(&lib_lock);
        swkbd_limits(la, &max_chars, &min_chars);
        pm_unlock(&lib_lock);
        st = swkbd_result(max_chars, min_chars);
        n = 0x7D8;
    } else if (id == 0x10) {
        u8 r[0x18];
        memset(r, 0, sizeof r);
        memcpy(r + 8, g_user_id, 16);
        n = sizeof r;
        st = am_storage_new(n, r);
    } else {
        if (id == 0x13 || id == 0x14 || id == 0x17 || id == 0x18 || id == 0x19) n = 0x1010;
        st = am_storage_new(n, NULL);
    }
    if (!st) return RC(128, 503);
    if (id != 0x11)
        LOG_LIMIT(4, "applet: library applet %02x closed right away (no applet UI here), %llu bytes of output\n", id,
                  (unsigned long long)n);
    *storage = st;
    return 0;
}
static int nn_am_service_ILibraryAppletAccessor_PopOutData(SvcObj* self, SvcObj** storage) {
    return pop_data(self->data, 0, storage);
}
static int nn_am_service_ILibraryAppletAccessor_PopInteractiveOutData(SvcObj* self, SvcObj** storage) {
    return pop_data(self->data, 1, storage);
}
static int nn_am_service_ILibraryAppletAccessor_GetPopOutDataEvent(SvcObj* self, u32* event) {
    return new_event(event, "applet data", 1);
}
/* Foreground swkbd has no interactive output. Inline swkbd signals while replies are queued. */
static int nn_am_service_ILibraryAppletAccessor_GetPopInteractiveOutDataEvent(SvcObj* self, u32* event) {
    LibApplet* la = self->data;
    if (!inline_keyboard(la)) return new_event(event, "applet data", la->id != 0x11);
    *event = ev_handle(la->interactive_ev);
    return 0;
}
static int nn_am_service_ILibraryAppletAccessor_GetLibraryAppletInfo(SvcObj* self,
                                                                     nn_am_service_LibraryAppletInfo* info) {
    LibApplet* la = self->data;
    info->applet_id = la->id;
    info->library_applet_mode = la->mode;
    return 0;
}
static int nn_am_service_ILibraryAppletCreator_CreateLibraryApplet(SvcObj* self, u32 applet_id, u32 library_applet_mode,
                                                                   SvcObj** accessor) {
    SvcObj* a = nn_am_service_ILibraryAppletAccessor_new(sizeof(LibApplet));
    ((LibApplet*)a->data)->id = applet_id;
    ((LibApplet*)a->data)->mode = library_applet_mode;
    if (inline_keyboard(a->data)) {
        LibApplet* la = a->data;
        la->state_ev = owned_event("inline keyboard state");
        la->interactive_ev = owned_event("inline keyboard reply");
    }
    a->destroy = lib_applet_destroy;
    *accessor = a;
    return 0;
}
/* the thread id names the caller's thread for the applet's scheduling: nothing to schedule here */
static int nn_am_service_ILibraryAppletCreator_CreateLibraryAppletEx(SvcObj* self, u32 applet_id,
                                                                     u32 library_applet_mode, u64 thread_id,
                                                                     SvcObj** accessor) {
    return nn_am_service_ILibraryAppletCreator_CreateLibraryApplet(self, applet_id, library_applet_mode, accessor);
}
static int nn_am_service_ILibraryAppletCreator_CreateStorage(SvcObj* self, i64 size, SvcObj** storage) {
    return out_storage(storage, (u64)size, NULL);
}
/* a storage holding what the transfer memory holds. A copy is enough: the library applets here finish at once and
   only read what was pushed */
static int nn_am_service_ILibraryAppletCreator_CreateTransferMemoryStorage(SvcObj* self, u8 is_writable, i64 size,
                                                                           u32 transfer_memory, SvcObj** storage) {
    KTmem* m = (KTmem*)h_get_ref(transfer_memory, KO_TMEM);
    if (!m) return out_storage(storage, (u64)size, NULL);
    SvcObj* st = am_storage_new((u64)size, NULL);
    if (st) memcpy(((StorageData*)st->data)->d, GP(m->addr), (u64)size < m->size ? (u64)size : m->size);
    kobj_unref(&m->o);
    if (!st) return RC(128, 503);
    *storage = st;
    return 0;
}
/* a handle storage is transfer memory the caller doesn't map: the applet reads it the same way */
static int nn_am_service_ILibraryAppletCreator_CreateHandleStorage(SvcObj* self, i64 size, u32 transfer_memory,
                                                                   SvcObj** storage) {
    return nn_am_service_ILibraryAppletCreator_CreateTransferMemoryStorage(self, 0, size, transfer_memory, storage);
}
static int nn_am_service_IAudioController_GetMainAppletExpectedMasterVolume(SvcObj* self, float* volume) {
    *volume = 1.0f;
    return 0;
}
static int nn_am_service_IAudioController_GetLibraryAppletExpectedMasterVolume(SvcObj* self, float* volume) {
    *volume = 1.0f;
    return 0;
}
static int nn_am_service_IApplicationProxy_GetCommonStateGetter(SvcObj* self, SvcObj** getter) {
    *getter = nn_am_service_ICommonStateGetter_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxy_GetSelfController(SvcObj* self, SvcObj** controller) {
    *controller = nn_am_service_ISelfController_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxy_GetWindowController(SvcObj* self, SvcObj** controller) {
    *controller = nn_am_service_IWindowController_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxy_GetAudioController(SvcObj* self, SvcObj** controller) {
    *controller = nn_am_service_IAudioController_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxy_GetDisplayController(SvcObj* self, SvcObj** controller) {
    *controller = nn_am_service_IDisplayController_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxy_GetProcessWindingController(SvcObj* self, SvcObj** controller) {
    *controller = nn_am_service_IProcessWindingController_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxy_GetLibraryAppletCreator(SvcObj* self, SvcObj** creator) {
    *creator = nn_am_service_ILibraryAppletCreator_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxy_GetApplicationFunctions(SvcObj* self, SvcObj** functions) {
    *functions = nn_am_service_IApplicationFunctions_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxy_GetDebugFunctions(SvcObj* self, SvcObj** functions) {
    *functions = nn_am_service_IDebugFunctions_new(0);
    return 0;
}
static int nn_am_service_IApplicationProxyService_OpenApplicationProxy(SvcObj* self, u64 pid, u32 process,
                                                                       SvcObj** proxy) {
    *proxy = nn_am_service_IApplicationProxy_new(0);
    return 0;
}
static int nn_am_service_IAllSystemAppletProxiesService_OpenSystemApplicationProxy(SvcObj* self, u64 pid, u32 process,
                                                                                   SvcObj** proxy) {
    *proxy = nn_am_service_IApplicationProxy_new(0);
    return 0;
}
SvcObj* new_appletoe(void) { return nn_am_service_IApplicationProxyService_new(0); }
SvcObj* new_appletae(void) { return nn_am_service_IAllSystemAppletProxiesService_new(0); }
