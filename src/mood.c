// Mood: "sunset on the beach" -> palette + effect + speed, picked by a local language model through Ollama
// (https://ollama.com, http://127.0.0.1:11434). Nothing leaves the PC. The model is [mood] model=, or the first
// chat model Ollama has installed. One request at a time on its own thread; the result is part of the status
// JSON ("mood"), so the window and the phone page both see it and apply it with the usual commands.
#include "common.h"
#include "devices.h"
#include <process.h>
#include <stdlib.h>

#define OLLAMA_PORT 11434

static SRWLOCK lk = SRWLOCK_INIT;
static volatile LONG busy;
static int  seq;                    // bumps with every finished request, so the page knows a result is new
static char err[16];                // "", "offline", "nomodel", "answer"
static char name[96], effect[24], model[96], colors[6][8];
static int  ncolors, speed;
static char req_text[600];
static int  req_again;

// the effects a mood may use, with what they look like (the model picks one)
static const char *const FX[][2] = {
    { "flow", "palette colours travel along all the lights" }, { "caustic", "rippling light under water" },
    { "bubbles", "lights rising slowly" }, { "comet", "a running tail of light" }, { "lava", "slow warm plasma" },
    { "breathe", "fades from one colour to the next" }, { "static", "still colours, no motion" },
    { "audio", "reacts to the music playing" },
};

static int jstr(char *out, int cap, const char *s) {   // JSON string contents, escaped
    int n = 0;
    for (; *s && n < cap - 8; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') { out[n++] = '\\'; out[n++] = (char)c; }
        else if (c < 0x20) out[n++] = ' ';
        else out[n++] = (char)c;
    }
    out[n] = 0;
    return n;
}

static unsigned hex4(const char *s) {
    unsigned u = 0;
    for (int i = 0; i < 4 && isxdigit((unsigned char)s[i]); i++) u = u * 16 + (unsigned)(isdigit((unsigned char)s[i]) ? s[i] - '0' : tolower(s[i]) - 'a' + 10);
    return u;
}

// Decodes the JSON string that starts right after the opening quote at s (\n, \", \uXXXX -> UTF-8).
static void junesc(const char *s, char *out, int cap) {
    int n = 0;
    while (*s && *s != '"' && n < cap - 5) {
        if (*s != '\\') { out[n++] = *s++; continue; }
        s++;
        char c = *s ? *s++ : 0;
        if (c == 'u') {
            unsigned u = hex4(s);
            for (int i = 0; i < 4 && isxdigit((unsigned char)*s); i++) s++;
            if (u >= 0xD800 && u < 0xDC00 && s[0] == '\\' && s[1] == 'u') {   // surrogate pair
                u = 0x10000 + ((u - 0xD800) << 10) + (hex4(s + 2) - 0xDC00); s += 6;
            }
            if (u < 0x80) out[n++] = (char)u;
            else if (u < 0x800) { out[n++] = (char)(0xC0 | u >> 6); out[n++] = (char)(0x80 | (u & 63)); }
            else if (u < 0x10000) { out[n++] = (char)(0xE0 | u >> 12); out[n++] = (char)(0x80 | (u >> 6 & 63)); out[n++] = (char)(0x80 | (u & 63)); }
            else { out[n++] = (char)(0xF0 | u >> 18); out[n++] = (char)(0x80 | (u >> 12 & 63)); out[n++] = (char)(0x80 | (u >> 6 & 63)); out[n++] = (char)(0x80 | (u & 63)); }
        } else out[n++] = c == 'n' || c == 'r' || c == 't' ? ' ' : c;
    }
    out[n] = 0;
}

// Small models like greys, browns and pastels, which look dirty or white on LEDs. Keep each colour's hue, but
// give it at least some saturation and lift it to near full value (dimming is the brightness slider's job);
// drop greys unless everything is grey (then the user asked for white light), and drop near-duplicates.
static void led_fix(char cols[][8], int *nc) {
    float h[6], s[6], v[6];
    int n = *nc, grey = 0;
    for (int i = 0; i < n; i++) {
        unsigned x = (unsigned)strtoul(cols[i] + 1, NULL, 16);
        float r = (x >> 16) / 255.f, g = (x >> 8 & 255) / 255.f, b = (x & 255) / 255.f;
        float mx = max(r, max(g, b)), mn = min(r, min(g, b)), d = mx - mn;
        v[i] = mx; s[i] = mx > 0 ? d / mx : 0;
        h[i] = d == 0 ? 0 : mx == r ? fmodf((g - b) / d + 6, 6) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
        if (s[i] < .2f) grey++;
    }
    char res[6][8];
    int out = 0;
    for (int i = 0; i < n; i++) {
        int keep_grey = grey == n;
        if (s[i] < .2f && !keep_grey) continue;
        float S = keep_grey ? s[i] : max(s[i], .5f), V = .8f + .2f * v[i];
        int dup = 0;
        for (int j = 0; j < out; j++) {   // compare with what is kept so far (stored back as hex below)
            unsigned y = (unsigned)strtoul(res[j] + 1, NULL, 16);
            float r = (y >> 16) / 255.f, g = (y >> 8 & 255) / 255.f, b = (y & 255) / 255.f;
            float mx = max(r, max(g, b)), mn = min(r, min(g, b)), d = mx - mn;
            float hj = d == 0 ? 0 : mx == r ? fmodf((g - b) / d + 6, 6) : mx == g ? (b - r) / d + 2 : (r - g) / d + 4;
            float dh = fabsf(hj - h[i]); if (dh > 3) dh = 6 - dh;
            if (dh < .12f && fabsf((mx > 0 ? d / mx : 0) - S) < .2f) dup = 1;
        }
        if (dup) continue;
        // HSV -> RGB
        float c = V * S, hh = h[i], x = c * (1 - fabsf(fmodf(hh, 2) - 1)), m = V - c, r, g, b;
        if (hh < 1) { r = c; g = x; b = 0; } else if (hh < 2) { r = x; g = c; b = 0; } else if (hh < 3) { r = 0; g = c; b = x; }
        else if (hh < 4) { r = 0; g = x; b = c; } else if (hh < 5) { r = x; g = 0; b = c; } else { r = c; g = 0; b = x; }
        snprintf(res[out++], 8, "#%02X%02X%02X", (int)((r + m) * 255 + .5f), (int)((g + m) * 255 + .5f), (int)((b + m) * 255 + .5f));
    }
    if (out >= 2) { memcpy(cols, res, sizeof(res[0]) * out); *nc = out; }
}

// HTTP to Ollama with a long read timeout: the first answer may wait for the model to load.
static int ollama(const char *method, const char *path, const char *body, char *buf, int cap) {
    buf[0] = 0;
    SOCKET s = tcp_connect("127.0.0.1", OLLAMA_PORT, 1500);
    if (s == INVALID_SOCKET) return 0;
    DWORD to = 120000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&to, sizeof(to));
    char head[256];
    int bl = body ? (int)strlen(body) : 0;
    int n = snprintf(head, sizeof(head), "%s %s HTTP/1.0\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\nContent-Length: %d\r\n\r\n", method, path, bl);
    int ok = tcp_send_all(s, head, n) && (!bl || tcp_send_all(s, body, bl)), got = 0;
    while (ok && got < cap - 1) { int r = recv(s, buf + got, cap - 1 - got, 0); if (r <= 0) break; got += r; }
    closesocket(s);
    buf[got] = 0;
    int status = 0;
    if (sscanf_s(buf, "HTTP/%*d.%*d %d", &status) != 1) return 0;
    char *b = strstr(buf, "\r\n\r\n");
    if (b) memmove(buf, b + 4, strlen(b + 4) + 1); else buf[0] = 0;
    return status;
}

// [mood] model, or the first installed model that can chat (embedding models can't)
static int pick_model(char *out, int cap, char *buf, int bufcap) {
    const char *m = cfg_get("mood", "model", "");
    if (*m) { snprintf(out, cap, "%s", m); return 1; }
    if (ollama("GET", "/api/tags", NULL, buf, bufcap) != 200) return -1;
    for (const char *p = buf; (p = strstr(p, "\"name\"")) != NULL; ) {
        p += 6;
        while (*p == ' ' || *p == ':') p++;
        if (*p++ != '"') continue;
        char nm[96]; int i = 0;
        while (*p && *p != '"' && i < (int)sizeof(nm) - 1) nm[i++] = *p++;
        nm[i] = 0;
        if (!strstr(nm, "embed")) { snprintf(out, cap, "%s", nm); return 1; }
    }
    return 0;
}

static void finish(const char *e) {
    AcquireSRWLockExclusive(&lk);
    snprintf(err, sizeof(err), "%s", e);
    seq++;
    ReleaseSRWLockExclusive(&lk);
    InterlockedExchange(&busy, 0);
}

static unsigned __stdcall run(void *arg) {
    (void)arg;
    static char buf[256 * 1024], body[8192], content[4096];
    char mdl[96];
    int pm = pick_model(mdl, sizeof(mdl), buf, sizeof(buf));
    if (pm < 0) { finish("offline"); return 0; }
    if (!pm) { finish("nomodel"); return 0; }

    char fx[1024] = "", txt[1300], mj[200];
    int fn = 0;
    for (int i = 0; i < (int)(sizeof(FX) / sizeof(FX[0])); i++)
        fn += snprintf(fx + fn, sizeof(fx) - fn, "%s%s = %s", i ? "; " : "", FX[i][0], FX[i][1]);
    jstr(txt, sizeof(txt), req_text);
    jstr(mj, sizeof(mj), mdl);
    snprintf(body, sizeof(body),
        "{\"model\":\"%s\",\"stream\":false,\"keep_alive\":\"1m\",\"options\":{\"temperature\":%s,\"seed\":%u},"
        "\"format\":{\"type\":\"object\",\"properties\":{\"name\":{\"type\":\"string\"},"
        "\"colors\":{\"type\":\"array\",\"items\":{\"type\":\"string\"},\"minItems\":3,\"maxItems\":6},"
        "\"effect\":{\"type\":\"string\"},\"speed\":{\"type\":\"integer\"}},\"required\":[\"name\",\"colors\",\"effect\",\"speed\"]},"
        "\"messages\":[{\"role\":\"system\",\"content\":\"You design lighting scenes for RGB lights: PC parts, LED strips, "
        "light panels and bulbs in a room. The user describes a mood, place or theme in any language. Answer with JSON: "
        "name = a short title of 2 to 4 words in the user's language; colors = 3 to 6 colours as #RRGGBB, in the order they "
        "should follow each other. LEDs cannot show dark, grey or brown colours (those look off or dirty): use clear, saturated "
        "colours and express darkness through deep hues such as navy, violet or deep red; use white or pale colours only when "
        "the mood asks for them. Think like a lighting designer: 4 or 5 colours that belong together, not generic web "
        "colour names. effect = one of: %s. speed = 1 (very calm) to 10 (fast). Pick what fits the feeling, not only the "
        "literal words. Example: northern lights -> {\\\"name\\\":\\\"Northern lights\\\",\\\"colors\\\":[\\\"#00FFA3\\\","
        "\\\"#00C2FF\\\",\\\"#7A2BFF\\\",\\\"#FF2BD6\\\"],\\\"effect\\\":\\\"flow\\\",\\\"speed\\\":3}\"},"
        "{\"role\":\"user\",\"content\":\"%s%s\"}]}",
        mj, req_again ? "1.1" : "0.7", (unsigned)GetTickCount(), fx, txt,
        req_again ? " (give a different variation from before)" : "");

    int st = ollama("POST", "/api/chat", body, buf, sizeof(buf));
    if (st != 200) { logf_("mood: ollama answered %d: %.200s", st, buf); finish(st ? "answer" : "offline"); return 0; }
    const char *msg = json_get_obj(buf, "message");
    const char *c = msg ? strstr(msg, "\"content\"") : NULL;
    if (c) { c += 9; while (*c == ' ' || *c == ':') c++; }
    if (!c || *c != '"') { logf_("mood: no answer text: %.200s", buf); finish("answer"); return 0; }
    junesc(c + 1, content, sizeof(content));

    char nm[96] = "", ef[24] = "", cols[6][8]; int nc = 0;
    json_get_str(content, "name", nm, sizeof(nm));
    json_get_str(content, "effect", ef, sizeof(ef));
    int sp = (int)json_get_num(content, "speed", 5);
    const char *a = strstr(content, "\"colors\"");
    const char *e = a ? strchr(a, ']') : NULL;
    for (const char *p = a; p && p < e && nc < 6; p++) {
        if (*p != '#') continue;
        int ok = 1;
        for (int i = 1; i <= 6; i++) if (!isxdigit((unsigned char)p[i])) ok = 0;
        if (!ok) continue;
        snprintf(cols[nc], 8, "#%.6s", p + 1);
        for (char *q = cols[nc]; *q; q++) *q = (char)toupper(*q);
        nc++;
    }
    led_fix(cols, &nc);
    if (nc < 2) { logf_("mood: unusable answer: %.300s", content); finish("answer"); return 0; }
    int known = 0;
    for (int i = 0; i < (int)(sizeof(FX) / sizeof(FX[0])); i++) if (!_stricmp(ef, FX[i][0])) known = 1;
    if (!known) strcpy_s(ef, sizeof(ef), "flow");

    AcquireSRWLockExclusive(&lk);
    snprintf(name, sizeof(name), "%s", nm);
    snprintf(effect, sizeof(effect), "%s", ef);
    snprintf(model, sizeof(model), "%s", mdl);
    memcpy(colors, cols, sizeof(cols));
    ncolors = nc;
    speed = sp < 1 ? 1 : sp > 10 ? 10 : sp;
    ReleaseSRWLockExclusive(&lk);
    logf_("mood: %s, %d colours, speed %d (%s)", ef, nc, speed, mdl);
    finish("");
    return 0;
}

void mood_request(const char *text, int again) {
    if (!text || !*text || InterlockedCompareExchange(&busy, 1, 0)) return;
    snprintf(req_text, sizeof(req_text), "%s", text);
    req_again = again;
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, run, NULL, 0, NULL);
    if (t) CloseHandle(t); else InterlockedExchange(&busy, 0);
}

// {"seq":3,"busy":0,"err":"","name":"...","effect":"lava","speed":4,"model":"...","colors":["#FF5500",...]}
int mood_json(char *out, int cap) {
    AcquireSRWLockShared(&lk);
    char nm[200]; jstr(nm, sizeof(nm), name);
    int n = snprintf(out, cap, "{\"seq\":%d,\"busy\":%d,\"err\":\"%s\",\"name\":\"%s\",\"effect\":\"%s\",\"speed\":%d,\"model\":\"%s\",\"colors\":[",
                     seq, (int)busy, err, nm, effect, speed, model);
    for (int i = 0; i < ncolors; i++) n += snprintf(out + n, cap - n, "%s\"%s\"", i ? "," : "", colors[i]);
    n += snprintf(out + n, cap - n, "]}");
    ReleaseSRWLockShared(&lk);
    return n;
}
