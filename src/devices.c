// LAN / bridge light devices ("ext devices"), see devices.h.
// Threads: the render thread hands frames over with ext_submit(); one worker thread owns the devices
// (connects with backoff, sends at most drv->rate frames/s, skips unchanged frames apart from a 1 s keep-alive,
// and lets go of them on exit / sleep / disable); discovery runs in short-lived threads per driver.
#include "devices.h"
#include <process.h>
#include <stdlib.h>

static const ext_driver *drivers[] = { &drv_wled, &drv_openrgb, &drv_govee, &drv_lifx, &drv_yeelight, &drv_hue, &drv_wiz, &drv_tuya };
#define NDRV (int)(sizeof(drivers) / sizeof(drivers[0]))

const ext_driver *ext_driver_by_kind(const char *kind) {
    for (int i = 0; i < NDRV; i++) if (!_stricmp(drivers[i]->kind, kind)) return drivers[i];
    return NULL;
}

// ---- worker-owned
static ext_dev devs[EXT_MAX];
static int     ndevs;

// ---- shared (lock)
static SRWLOCK lk = SRWLOCK_INIT;
typedef struct { int id, nleds, per_led, online, enabled; char name[64], host[64], info[96], kind[16]; int sub; } slot_t;
static slot_t  slots[EXT_MAX];
static int     nslots;
static rgbf    frame[EXT_MAX][EXT_MAX_LEDS];
static int     frame_n[EXT_MAX];
static LONG    frame_seq[EXT_MAX];
static volatile LONG layout_new, reload_req, run, suspend_req;
static HANDLE  th, wake, suspend_done;

// discovery
static disc_t  found[DISC_MAX];
static int     nfound;
static volatile LONG scanning;

static int leave_mode(void) {
    const char *m = cfg_get("general", "on_exit", "off");
    return !_stricmp(m, "keep") ? LEAVE_KEEP : !_stricmp(m, "restore") ? LEAVE_RESTORE : LEAVE_OFF;
}

static void publish_slots(void) {
    AcquireSRWLockExclusive(&lk);
    nslots = ndevs;
    for (int k = 0; k < ndevs; k++) {
        ext_dev *d = &devs[k]; slot_t *s = &slots[k];
        int n = d->nleds > 0 ? d->nleds : d->cfg_leds;
        if (n > EXT_MAX_LEDS) n = EXT_MAX_LEDS;
        if (!d->drv->per_led && n < 1) n = 1;
        if (s->id != d->id || s->nleds != n) InterlockedExchange(&layout_new, 1);
        s->id = d->id; s->nleds = n; s->per_led = d->drv->per_led; s->online = d->online; s->enabled = d->enabled;
        s->sub = d->sub;
        strcpy_s(s->name, sizeof(s->name), d->name); strcpy_s(s->host, sizeof(s->host), d->host);
        strcpy_s(s->info, sizeof(s->info), d->info); strcpy_s(s->kind, sizeof(s->kind), d->drv->kind);
    }
    ReleaseSRWLockExclusive(&lk);
}

// ---- connecting happens on short-lived threads, on a copy of the device, so one light that is off or unplugged
// (connect timeouts of a second or more) never holds up the frames of the others. The worker takes the result
// over on its next round, if the device is still there and unchanged; otherwise the connection is closed.
typedef struct { ext_dev d; int ok; volatile LONG done; } opener_t;
static opener_t *openers[EXT_MAX];
static volatile LONG openers_running;

static unsigned __stdcall opener_fn(void *p) {
    opener_t *o = p;
    net_init();
    o->ok = o->d.drv->open(&o->d);
    InterlockedExchange(&o->done, 1);
    InterlockedDecrement(&openers_running);
    return 0;
}

static int opening(int id) {
    for (int i = 0; i < EXT_MAX; i++) if (openers[i] && openers[i]->d.id == id) return 1;
    return 0;
}

static void start_open(ext_dev *d) {
    for (int i = 0; i < EXT_MAX; i++) {
        if (openers[i]) continue;
        opener_t *o = calloc(1, sizeof(opener_t));
        if (!o) return;
        o->d = *d;
        o->d.sock = INVALID_SOCKET; o->d.priv = NULL;
        InterlockedIncrement(&openers_running);
        HANDLE t = (HANDLE)_beginthreadex(NULL, 0, opener_fn, o, 0, NULL);
        if (!t) { InterlockedDecrement(&openers_running); free(o); return; }
        CloseHandle(t);
        openers[i] = o;
        return;
    }
}

static void let_go(ext_dev *d, int how) {
    if (!d->online) return;
    if (d->drv->leave) d->drv->leave(d, how);
    d->drv->close(d);
    d->online = 0;
    logf_("dev.%d (%s %s): released", d->id, d->drv->kind, d->host);
}

// Re-reads [dev.N] sections: unchanged devices stay connected, others are (re)created.
static void load_config(void) {
    ext_dev nd[EXT_MAX]; int nn = 0;
    for (int id = 1; id <= 64 && nn < EXT_MAX; id++) {
        char sec[16]; snprintf(sec, sizeof(sec), "dev.%d", id);
        const char *kind = cfg_get(sec, "kind", NULL);
        const ext_driver *drv = kind ? ext_driver_by_kind(kind) : NULL;
        if (!drv) continue;
        ext_dev *d = &nd[nn++]; memset(d, 0, sizeof(*d));
        d->id = id; d->drv = drv; d->sock = INVALID_SOCKET;
        strcpy_s(d->host, sizeof(d->host), cfg_get(sec, "host", ""));
        strcpy_s(d->name, sizeof(d->name), cfg_get(sec, "name", drv->title));
        d->sub = cfg_geti(sec, "sub", -1);
        d->cfg_leds = cfg_geti(sec, "leds", 0);
        d->reverse = cfg_geti(sec, "reverse", 0);
        d->enabled = cfg_geti(sec, "enabled", 1);
        strcpy_s(d->key, sizeof(d->key), cfg_get(sec, "key", ""));
    }
    // keep connections of devices whose address did not change
    for (int i = 0; i < nn; i++) {
        for (int k = 0; k < ndevs; k++) {
            ext_dev *o = &devs[k];
            if (o->id == nd[i].id && o->drv == nd[i].drv && !strcmp(o->host, nd[i].host) && o->sub == nd[i].sub && o->cfg_leds == nd[i].cfg_leds) {
                ext_dev keep = *o;
                keep.reverse = nd[i].reverse; keep.enabled = nd[i].enabled;
                strcpy_s(keep.name, sizeof(keep.name), nd[i].name);
                nd[i] = keep;
                o->id = -1;   // taken over
                break;
            }
        }
    }
    for (int k = 0; k < ndevs; k++) if (devs[k].id >= 0) let_go(&devs[k], LEAVE_RESTORE);   // removed / changed
    memcpy(devs, nd, sizeof(ext_dev) * nn);
    ndevs = nn;
    AcquireSRWLockExclusive(&lk);
    memset(frame_n, 0, sizeof(frame_n));
    ReleaseSRWLockExclusive(&lk);
    publish_slots();
    InterlockedExchange(&layout_new, 1);
}

static unsigned __stdcall worker(void *p) {
    (void)p;
    net_init();
    static rgbf  cur[EXT_MAX_LEDS];
    static BYTE  last8[EXT_MAX][EXT_MAX_LEDS * 3];
    DWORD last_send[EXT_MAX] = { 0 };
    LONG  seen[EXT_MAX] = { 0 };
    load_config();
    while (run) {
        WaitForSingleObject(wake, 20);
        if (!run) break;
        if (InterlockedExchange(&reload_req, 0)) { load_config(); memset(seen, 0, sizeof(seen)); memset(last8, 0, sizeof(last8)); }
        if (suspend_req) {
            int how = leave_mode();
            for (int k = 0; k < ndevs; k++) let_go(&devs[k], how);
            publish_slots();
            SetEvent(suspend_done);
            while (run && suspend_req) Sleep(100);
            for (int k = 0; k < ndevs; k++) devs[k].next_try = 0;
            continue;
        }
        DWORD now = GetTickCount();
        int changed_state = 0;
        // finished connection attempts
        for (int i = 0; i < EXT_MAX; i++) {
            opener_t *o = openers[i];
            if (!o || !o->done) continue;
            openers[i] = NULL;
            ext_dev *d = NULL;
            for (int k = 0; k < ndevs; k++)
                if (devs[k].id == o->d.id && devs[k].drv == o->d.drv && !strcmp(devs[k].host, o->d.host) && devs[k].sub == o->d.sub && !devs[k].online) d = &devs[k];
            if (o->ok && d && d->enabled) {
                int k = (int)(d - devs);
                d->sock = o->d.sock; d->priv = o->d.priv; d->nleds = o->d.nleds;
                strcpy_s(d->info, sizeof(d->info), o->d.info); strcpy_s(d->key, sizeof(d->key), o->d.key);
                d->online = 1; d->fails = 0; seen[k] = 0;
                memset(last8[k], 0, sizeof(last8[k]));
                if (d->cfg_leds > 0 && d->drv->per_led) d->nleds = min(d->cfg_leds, d->nleds > 0 ? d->nleds : d->cfg_leds);
                if (d->nleds > EXT_MAX_LEDS) d->nleds = EXT_MAX_LEDS;
                logf_("dev.%d (%s %s): online, %d LEDs %s", d->id, d->drv->kind, d->host, d->nleds, d->info);
            } else if (o->ok) {
                o->d.drv->close(&o->d);   // removed, changed or switched off meanwhile
            } else if (d) {
                d->fails++;
                d->next_try = now + (d->fails < 3 ? 3000 : d->fails < 10 ? 10000 : 30000);
                if (d->fails == 1) logf_("dev.%d (%s %s): not reachable, retrying", d->id, d->drv->kind, d->host);
            }
            free(o);
            changed_state = 1;   // online state / info text for the UI
        }
        for (int k = 0; k < ndevs; k++) {
            ext_dev *d = &devs[k];
            if (!d->enabled) {
                if (d->online) { let_go(d, LEAVE_OFF); changed_state = 1; }
                continue;
            }
            if (!d->online) {
                if ((int)(now - d->next_try) >= 0 && !opening(d->id)) start_open(d);
                continue;
            }
            // frame from the render thread
            int n = 0; LONG seq;
            AcquireSRWLockShared(&lk);
            seq = frame_seq[k]; n = frame_n[k];
            if (n > d->nleds) n = d->nleds;
            if (n) memcpy(cur, frame[k], n * sizeof(rgbf));
            ReleaseSRWLockShared(&lk);
            if (!n) continue;
            int interval = (int)(1000 / (d->drv->rate > 0 ? d->drv->rate : 10));
            if ((int)(now - last_send[k]) < interval - interval / 4) continue;   // frames come every ~33 ms: allow jitter
            BYTE q[EXT_MAX_LEDS * 3];
            for (int i = 0; i < n; i++) { q[i * 3] = to8(cur[i].r); q[i * 3 + 1] = to8(cur[i].g); q[i * 3 + 2] = to8(cur[i].b); }
            int same = seq == seen[k] || !memcmp(q, last8[k], n * 3);
            if (same && now - last_send[k] < 1000) continue;   // unchanged: keep-alive only
            seen[k] = seq;
            if (d->reverse) for (int i = 0; i < n / 2; i++) { rgbf t = cur[i]; cur[i] = cur[n - 1 - i]; cur[n - 1 - i] = t; }
            if (d->drv->send(d, cur, n)) {
                memcpy(last8[k], q, n * 3);
                last_send[k] = now;
            } else {
                logf_("dev.%d (%s %s): connection lost", d->id, d->drv->kind, d->host);
                d->drv->close(d);
                d->online = 0; d->next_try = now + 2000;
                changed_state = 1;
            }
        }
        if (changed_state) publish_slots();
    }
    int how = leave_mode();
    for (int k = 0; k < ndevs; k++) let_go(&devs[k], how);
    // connection attempts still running: wait a little, close what connected, leave the rest to the process exit
    for (int w = 0; w < 30 && openers_running > 0; w++) Sleep(100);
    for (int i = 0; i < EXT_MAX; i++) if (openers[i] && openers[i]->done) {
        if (openers[i]->ok) openers[i]->d.drv->close(&openers[i]->d);
        free(openers[i]); openers[i] = NULL;
    }
    return 0;
}

// ---------------------------------------------------------------- API (render / UI threads)
void ext_start(void) {
    if (th) return;
    wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    suspend_done = CreateEventW(NULL, FALSE, FALSE, NULL);
    run = 1;
    th = (HANDLE)_beginthreadex(NULL, 0, worker, NULL, 0, NULL);
}

void ext_stop(void) {
    if (!th) return;
    run = 0; SetEvent(wake);
    WaitForSingleObject(th, 4000);
    CloseHandle(th); th = NULL;
}

void ext_reload(void) { InterlockedExchange(&reload_req, 1); if (wake) SetEvent(wake); }

void ext_suspend(int sleeping) {
    if (!th) return;
    if (sleeping) {
        ResetEvent(suspend_done);
        InterlockedExchange(&suspend_req, 1); SetEvent(wake);
        WaitForSingleObject(suspend_done, 2500);
    } else InterlockedExchange(&suspend_req, 0);
}

int ext_layout_changed(void) { return InterlockedExchange(&layout_new, 0); }

int ext_count(void) { AcquireSRWLockShared(&lk); int n = nslots; ReleaseSRWLockShared(&lk); return n; }

int ext_slot_leds(int k) {
    AcquireSRWLockShared(&lk);
    int n = k < nslots && slots[k].enabled ? slots[k].nleds : 0;
    ReleaseSRWLockShared(&lk);
    return n;
}

int ext_slot_strip(int k) {
    AcquireSRWLockShared(&lk);
    int s = k < nslots ? slots[k].per_led : 0;
    ReleaseSRWLockShared(&lk);
    return s;
}

int ext_slot_id(int k) {
    AcquireSRWLockShared(&lk);
    int id = k < nslots ? slots[k].id : 0;
    ReleaseSRWLockShared(&lk);
    return id;
}

void ext_submit(int k, const rgbf *c, int n) {
    if (k < 0 || k >= EXT_MAX) return;
    if (n > EXT_MAX_LEDS) n = EXT_MAX_LEDS;
    AcquireSRWLockExclusive(&lk);
    memcpy(frame[k], c, n * sizeof(rgbf));
    frame_n[k] = n; frame_seq[k]++;
    ReleaseSRWLockExclusive(&lk);
    if (wake) SetEvent(wake);
}

// ---------------------------------------------------------------- discovery
static void on_found(const disc_t *x) {
    AcquireSRWLockExclusive(&lk);
    int dup = 0;
    for (int i = 0; i < nfound; i++)
        if (!strcmp(found[i].kind, x->kind) && !strcmp(found[i].host, x->host) && found[i].sub == x->sub) { found[i] = *x; dup = 1; break; }
    if (!dup && nfound < DISC_MAX) found[nfound++] = *x;
    ReleaseSRWLockExclusive(&lk);
    if (!dup) logf_("devices: found %s %s sub=%d '%s' %d LEDs (%s)", x->kind, x->host, x->sub, x->name, x->nleds, x->info);
}

static unsigned __stdcall disc_one(void *p) {
    const ext_driver *drv = (const ext_driver *)p;
    drv->discover(2500, on_found);
    return 0;
}

static unsigned __stdcall disc_all(void *p) {
    (void)p;
    net_init();
    HANDLE h[16]; int nh = 0;
    for (int i = 0; i < NDRV; i++)
        if (drivers[i]->discover) h[nh++] = (HANDLE)_beginthreadex(NULL, 0, disc_one, (void *)drivers[i], 0, NULL);
    WaitForMultipleObjects(nh, h, TRUE, 8000);
    for (int i = 0; i < nh; i++) CloseHandle(h[i]);
    AcquireSRWLockShared(&lk); int n = nfound; ReleaseSRWLockShared(&lk);
    logf_("devices: scan finished, %d found", n);
    InterlockedExchange(&scanning, 0);
    return 0;
}

void ext_scan(void) {
    if (InterlockedCompareExchange(&scanning, 1, 0) != 0) return;
    AcquireSRWLockExclusive(&lk); nfound = 0; ReleaseSRWLockExclusive(&lk);
    HANDLE h = (HANDLE)_beginthreadex(NULL, 0, disc_all, NULL, 0, NULL);
    if (h) CloseHandle(h); else InterlockedExchange(&scanning, 0);
}

// ---------------------------------------------------------------- settings
int ext_add(const char *kind, const char *host, int sub, const char *name, int leds) {
    if (!ext_driver_by_kind(kind) || !host[0]) return 0;
    int id = 0;
    for (int i = 1; i <= 64; i++) {
        char sec[16]; snprintf(sec, sizeof(sec), "dev.%d", i);
        const char *k = cfg_get(sec, "kind", NULL);
        if (k && !_stricmp(k, kind) && !strcmp(cfg_get(sec, "host", ""), host) && cfg_geti(sec, "sub", -1) == sub) return i;   // already there
        if (!k && !id) id = i;
    }
    if (!id) return 0;
    char sec[16], v[16]; snprintf(sec, sizeof(sec), "dev.%d", id);
    cfg_set(sec, "kind", kind);
    cfg_set(sec, "host", host);
    snprintf(v, sizeof(v), "%d", sub); cfg_set(sec, "sub", v);
    cfg_set(sec, "name", name && name[0] ? name : ext_driver_by_kind(kind)->title);
    snprintf(v, sizeof(v), "%d", leds > 0 ? leds : 0); cfg_set(sec, "leds", v);
    cfg_set(sec, "enabled", "1");
    cfg_save_if_dirty();
    ext_reload();
    logf_("devices: added dev.%d %s %s", id, kind, host);
    return id;
}

void ext_save_key(ext_dev *d, const char *key) {
    char sec[16]; snprintf(sec, sizeof(sec), "dev.%d", d->id);
    strcpy_s(d->key, sizeof(d->key), key);
    cfg_set(sec, "key", key);
    cfg_save_if_dirty();
}

void ext_remove(int id) {
    char sec[16]; snprintf(sec, sizeof(sec), "dev.%d", id);
    cfg_remove_section(sec);
    snprintf(sec, sizeof(sec), "zone.dev%d", id);
    cfg_remove_section(sec);
    cfg_save_if_dirty();
    ext_reload();
}

// {"devs":[...],"found":[...],"scanning":0,"kinds":[...]}
int ext_json(char *out, int cap) {
    int n = snprintf(out, cap, "{\"devs\":[");
    AcquireSRWLockShared(&lk);
    for (int k = 0; k < nslots && n < cap - 600; k++) {
        slot_t *s = &slots[k];
        char nm[140], inf[200], host[140];
        json_escape_to(nm, sizeof(nm), s->name); json_escape_to(inf, sizeof(inf), s->info); json_escape_to(host, sizeof(host), s->host);
        const ext_driver *drv = ext_driver_by_kind(s->kind);
        n += snprintf(out + n, cap - n, "%s{\"id\":%d,\"kind\":\"%s\",\"title\":\"%s\",\"name\":\"%s\",\"host\":\"%s\",\"sub\":%d,"
                      "\"leds\":%d,\"per_led\":%d,\"online\":%d,\"enabled\":%d,\"info\":\"%s\"}",
                      k ? "," : "", s->id, s->kind, drv ? drv->title : s->kind, nm, host, s->sub, s->nleds, s->per_led, s->online, s->enabled, inf);
    }
    n += snprintf(out + n, cap - n, "],\"found\":[");
    for (int i = 0; i < nfound && n < cap - 600; i++) {
        disc_t *f = &found[i];
        char nm[140], inf[200], host[140];
        json_escape_to(nm, sizeof(nm), f->name); json_escape_to(inf, sizeof(inf), f->info); json_escape_to(host, sizeof(host), f->host);
        int added = 0;
        for (int k = 0; k < nslots; k++) if (!strcmp(slots[k].kind, f->kind) && !strcmp(slots[k].host, f->host) && slots[k].sub == f->sub) added = 1;
        const ext_driver *drv = ext_driver_by_kind(f->kind);
        n += snprintf(out + n, cap - n, "%s{\"kind\":\"%s\",\"title\":\"%s\",\"host\":\"%s\",\"sub\":%d,\"name\":\"%s\",\"leds\":%d,\"info\":\"%s\",\"added\":%d}",
                      i ? "," : "", f->kind, drv ? drv->title : f->kind, host, f->sub, nm, f->nleds, inf, added);
    }
    ReleaseSRWLockShared(&lk);
    n += snprintf(out + n, cap - n, "],\"scanning\":%d,\"kinds\":[", (int)scanning);
    for (int i = 0; i < NDRV; i++)
        n += snprintf(out + n, cap - n, "%s{\"kind\":\"%s\",\"title\":\"%s\",\"per_led\":%d}", i ? "," : "", drivers[i]->kind, drivers[i]->title, drivers[i]->per_led);
    n += snprintf(out + n, cap - n, "]}");
    return n;
}
