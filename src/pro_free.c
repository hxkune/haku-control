// SPDX-License-Identifier: GPL-3.0-only
// The open build: no haku Pro. The lights on the network (Nanoleaf, AiDot, WLED, Hue, Govee...), their sign-ins and
// phone control are part of the paid build (pro\ next to src\); here they are empty, so the rest runs as it is:
// the PC's own lighting, USB devices, the makers' apps on this PC and every effect. The window hides what is not
// there ("none":1 in the status).
#include "common.h"

void pro_start(void) {}
int  pro_active(void) { return 1; }
void pro_set_key(const char *key) { (void)key; }
void pro_buy(const char *plan) { (void)plan; }
int  pro_json(char *out, int cap) { return jcat(out, cap, 0, "null"); }
void pro_ref_poll(void) {}
void pro_ref_join(void) {}
int  pro_ref_json(char *out, int cap) { return jcat(out, cap, 0, "null"); }

// Nanoleaf over the network
int  nano_start(void) { return 0; }
int  nano_present(int k) { (void)k; return 0; }
int  nano_count(int k) { (void)k; return 0; }
int  nano_layout_changed(void) { return 0; }
void nano_panel(int k, int i, float *x, float *y, float *path) { (void)k; (void)i; *x = *y = 0; if (path) *path = 0; }
void nano_title(int k, char *out, int cap) { (void)k; if (cap) out[0] = 0; }
void nano_submit(int k, const rgbf *c, int n, int enable) { (void)k; (void)c; (void)n; (void)enable; }
int  nano_on_device(int k) { (void)k; return 0; }
int  nano_bake_wanted(int k) { (void)k; return 0; }
void nano_upload(int k, const rgbf *frames, int nframes, int npanels, float step) { (void)k; (void)frames; (void)nframes; (void)npanels; (void)step; }
void nano_stop(void) {}
void nano_pair_start_ip(const char *ip) { (void)ip; }
void nano_forget(int slot) { (void)slot; }
void nano_relayout(void) {}
void nano_suspend(int sleeping) { (void)sleeping; }
int  nano_json(char *out, int cap) { return jcat(out, cap, 0, "{\"pair\":0,\"max\":0,\"ctls\":[],\"none\":1}"); }

// AiDot bulbs
int  lights_start(void) { return 0; }
int  lights_count(void) { return 0; }
const char *lights_name(int i) { (void)i; return ""; }
void lights_submit(const rgbf *c, const int *kelvin, int n, int enable) { (void)c; (void)kelvin; (void)n; (void)enable; }
void lights_stop(void) {}
int  lights_is_online(int i) { (void)i; return 0; }
void lights_suspend(int sleeping) { (void)sleeping; }
int  lights_changed(void) { return 0; }
const char *lights_ip(int i) { (void)i; return ""; }
const char *lights_mac(int i) { (void)i; return ""; }

// sign-ins (AiDot, Tuya, Govee cloud)
void accounts_aidot_login(const char *country, const char *email, const char *password) { (void)country; (void)email; (void)password; }
void accounts_tuya_login(const char *region, const char *access_id, const char *secret) { (void)region; (void)access_id; (void)secret; }
void accounts_govee_login(const char *api_key) { (void)api_key; }
int  accounts_json(char *out, int cap) {
    return jcat(out, cap, 0, "{\"none\":1,\"aidot\":{\"state\":0,\"msg\":\"\",\"found\":0,\"countries\":[]},"
                    "\"tuya\":{\"state\":0,\"msg\":\"\",\"found\":0,\"regions\":[]},\"govee\":{\"state\":0,\"msg\":\"\",\"found\":0,\"saved\":0}}");
}

// Home Assistant (MQTT)
void mqtt_apply(void) {}
void mqtt_refresh(void) {}
void mqtt_stop(void) {}
int  mqtt_json(char *out, int cap) { return jcat(out, cap, 0, "null"); }

// scenarios
int  rules_tick(int sensing) { (void)sensing; return 0; }
void rules_manual(void) {}
void rules_suspend(int sleeping) { (void)sleeping; }
int  rules_holding(void) { return 0; }
void rules_profile_gone(int id) { (void)id; }
int  rules_save(int n, const char *const *kv, int pairs) { (void)n; (void)kv; (void)pairs; return -3; }
int  rules_on(int n, int on) { (void)n; (void)on; return -3; }
int  rules_delete(int n) { (void)n; return -3; }
int  rules_move(int n, int d) { (void)n; (void)d; return -3; }
int  rules_json(char *out, int cap) { return jcat(out, cap, 0, "{\"none\":1}"); }
int  lcd_save(int n, const char *const *kv, int pairs) { (void)n; (void)kv; (void)pairs; return -3; }
int  lcd_delete(int n) { (void)n; return -3; }
unsigned lcd_sig(int n) { (void)n; return 0; }
lcd_layout *lcd_open(int n) { (void)n; return NULL; }
int  lcd_draw(lcd_layout *l, unsigned char **pack) { (void)l; *pack = NULL; return 0; }
void lcd_free(lcd_layout *l) { (void)l; }
int  lcd_json(char *out, int cap) { return jcat(out, cap, 0, "null"); }
void lcd_watch(void) {}

// phone control
void remote_apply(void) {}
void remote_stop(void) {}
void remote_forget(void) {}
void remote_new_pin(void) {}
int  remote_json(char *out, int cap) { return jcat(out, cap, 0, "{\"enabled\":0,\"on\":0,\"port\":0,\"pin\":\"\",\"paired\":0,\"urls\":[],\"none\":1}"); }

// voice control
void voice_apply(void) {}
void voice_listen(void) {}
void voice_wake_toggle(void) {}
void voice_hold(int on) { (void)on; }
int  voice_level(void) { return -1; }
void voice_setup(void) {}
void voice_suspend(int sleeping) { (void)sleeping; }
void voice_stop(void) {}
int  voice_json(char *out, int cap) { return jcat(out, cap, 0, "null"); }
void wall_find(int more) { (void)more; }
void wall_set(int k) { (void)k; }
void wall_restore(void) {}
void wall_pick_dir(HWND owner) { (void)owner; }
int  walls_json(char *out, int cap) { return jcat(out, cap, 0, "null"); }
