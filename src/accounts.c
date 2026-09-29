// Accounts: one-time sign-ins to vendor clouds that hand out the keys for local control. Only the keys are
// saved; the password goes to the vendor, encrypted the way its own app does it, and is wiped right after.
//   AiDot (Linkind): login -> houses -> devices; each light's aesKey / password -> %APPDATA%\haku-control\aidot.json,
//   which dev_aidot.c picks up by itself. Same requests as scripts\aidot-setup.ps1 and the python-aidot project.
// Runs on its own thread; the state is part of the status JSON ("accounts"). Never logs the e-mail or password.
#include "common.h"
#include "devices.h"
#include <winhttp.h>
#include <objbase.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <process.h>
#include <stdlib.h>
#include <time.h>

#define AIDOT_APP_ID "1383974540041977857"
static const char AIDOT_KEY_PEM[] =   // AiDot's public key for the password (from its app)
    "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQCtQAnPCi8ksPnS1Du6z96PsKfN"
    "p2Gp/f/bHwlrAdplbX3p7/TnGpnbJGkLq8uRxf6cw+vOthTsZjkPCF7CatRvRnTj"
    "c9fcy7yE0oXa5TloYyXD6GkxgftBbN/movkJJGQCc7gFavuYoAdTRBOyQoXBtm0m"
    "kXMSjXOldI/290b9BQIDAQAB";

// country code -> name AiDot expects, and its server region
static const char *const COUNTRIES[][3] = {
    { "FR", "France", "eu" }, { "DE", "Germany", "eu" }, { "GB", "United Kingdom", "eu" }, { "IT", "Italy", "eu" },
    { "ES", "Spain", "eu" }, { "BE", "Belgium", "eu" }, { "CH", "Switzerland", "eu" }, { "NL", "Netherlands", "eu" },
    { "PL", "Poland", "eu" }, { "AT", "Austria", "eu" }, { "PT", "Portugal", "eu" }, { "SE", "Sweden", "eu" },
    { "CZ", "Czech Republic", "eu" }, { "RU", "Russia", "eu" }, { "UA", "Ukraine", "eu" }, { "KZ", "Kazakhstan", "jp" },
    { "US", "United States", "us" }, { "CA", "Canada", "us" },
};

static SRWLOCK lk = SRWLOCK_INIT;
static volatile LONG busy;
static int  state;          // 0 idle, 1 working, 2 done, 3 failed
static char msg[160];       // result / error for the page
static int  found;
static char a_cc[8], a_email[128], a_pass[128];

static void set_state(int st, int n, const char *m) {
    AcquireSRWLockExclusive(&lk);
    state = st; found = n; snprintf(msg, sizeof(msg), "%s", m ? m : "");
    ReleaseSRWLockExclusive(&lk);
}

// ---------------------------------------------------------------- HTTPS (WinHTTP)
// [aidot] / [tuya] server= overrides the host (tests: tools/sim/fake_aidot.py, fake_tuya.py).
static int https(const char *section, const char *method, const char *host, const char *path, const char *headers,
                 const char *body, char *out, int cap) {
    int n = 0, code = 0;
    wchar_t whost[128], wpath[512], whead[2048];
    const char *ov = cfg_get(section, "server", "");
    int port = INTERNET_DEFAULT_HTTPS_PORT, secure = 1;
    char hbuf[128]; snprintf(hbuf, sizeof(hbuf), "%s", host);
    if (*ov) {   // http://host:port
        const char *h = strstr(ov, "://"); h = h ? h + 3 : ov;
        snprintf(hbuf, sizeof(hbuf), "%s", h);
        char *c = strchr(hbuf, ':'); if (c) { *c = 0; port = atoi(c + 1); }
        secure = !_strnicmp(ov, "https", 5);
    }
    MultiByteToWideChar(CP_UTF8, 0, hbuf, -1, whost, 128);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, 512);
    MultiByteToWideChar(CP_UTF8, 0, headers, -1, whead, 2048);
    HINTERNET ses = WinHttpOpen(L"haku-control", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, NULL, NULL, 0);
    if (!ses) return 0;
    WinHttpSetTimeouts(ses, 8000, 8000, 15000, 15000);
    HINTERNET con = WinHttpConnect(ses, whost, (INTERNET_PORT)port, 0);
    HINTERNET req = con ? WinHttpOpenRequest(con, method[0] == 'P' ? L"POST" : L"GET", wpath, NULL, NULL, NULL, secure ? WINHTTP_FLAG_SECURE : 0) : NULL;
    DWORD bl = body ? (DWORD)strlen(body) : 0;
    if (req && WinHttpSendRequest(req, whead, (DWORD)-1, (LPVOID)body, bl, bl, 0) && WinHttpReceiveResponse(req, NULL)) {
        DWORD sz = sizeof(code);
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, NULL, &code, &sz, NULL);
        DWORD got;
        while (n < cap - 1 && WinHttpReadData(req, out + n, cap - 1 - n, &got) && got) n += got;
    }
    out[n] = 0;
    if (req) WinHttpCloseHandle(req);
    if (con) WinHttpCloseHandle(con);
    WinHttpCloseHandle(ses);
    return code;
}

// ---------------------------------------------------------------- RSA (PKCS#1 v1.5) with the vendor's public key
static int rsa_encrypt_b64(const char *pem_b64, const char *text, char *out, int cap) {
    BYTE der[1024]; DWORD dl = sizeof(der), ok = 0;
    CERT_PUBLIC_KEY_INFO *info = NULL; DWORD il = 0;
    BCRYPT_KEY_HANDLE key = NULL;
    if (CryptStringToBinaryA(pem_b64, 0, CRYPT_STRING_BASE64, der, &dl, NULL, NULL) &&
        CryptDecodeObjectEx(X509_ASN_ENCODING, X509_PUBLIC_KEY_INFO, der, dl, CRYPT_DECODE_ALLOC_FLAG, NULL, &info, &il) &&
        CryptImportPublicKeyInfoEx2(X509_ASN_ENCODING, info, 0, NULL, &key)) {
        BYTE enc[512]; ULONG el = 0;
        if (BCryptEncrypt(key, (PUCHAR)text, (ULONG)strlen(text), NULL, NULL, 0, enc, sizeof(enc), &el, BCRYPT_PAD_PKCS1) == 0) {
            DWORD ol = cap;
            ok = CryptBinaryToStringA(enc, el, CRYPT_STRING_BASE64 | CRYPT_STRING_NOCRLF, out, &ol);
            SecureZeroMemory(enc, sizeof(enc));
        }
    }
    if (key) BCryptDestroyKey(key);
    if (info) LocalFree(info);
    return ok;
}

// ---------------------------------------------------------------- tiny JSON walking
// Next object of an array (or any '{'), from p; *end gets the character after its closing brace.
static const char *next_obj(const char *p, const char **end) {
    p = p ? strchr(p, '{') : NULL;
    if (!p) return NULL;
    int depth = 0, str = 0;
    for (const char *q = p; *q; q++) {
        if (str) { if (*q == '\\' && q[1]) q++; else if (*q == '"') str = 0; continue; }
        if (*q == '"') str = 1;
        else if (*q == '{' || *q == '[') depth++;
        else if ((*q == '}' || *q == ']') && --depth == 0) { *end = q + 1; return p; }
    }
    return NULL;
}
// Value of key at the object's own level (not inside nested objects): string contents still JSON-escaped (so it can
// be written back into JSON as is), or the raw token.
// For an array value, the first string in it.
static int field(const char *obj, const char *end, const char *key, char *out, int cap) {
    out[0] = 0;
    int depth = 0, klen = (int)strlen(key);
    for (const char *q = obj; q < end; q++) {
        if (*q == '"') {
            const char *s = q + 1, *e = s;
            while (e < end && *e != '"') { if (*e == '\\') e++; e++; }
            if (depth == 1 && e - s == klen && !memcmp(s, key, klen)) {
                const char *v = e + 1;
                while (v < end && (*v == ' ' || *v == ':')) v++;
                if (*v == '[') { v++; while (v < end && (*v == ' ' || *v == '\n' || *v == '\r')) v++; }
                int n = 0;
                if (*v == '"') { for (v++; v < end && *v != '"' && n < cap - 2; v++) { if (*v == '\\' && v[1]) out[n++] = *v++; out[n++] = *v; } }
                else while (v < end && !strchr(",}] \r\n", *v) && n < cap - 1) out[n++] = *v++;
                out[n] = 0;
                return 1;
            }
            q = e;
            continue;
        }
        if (*q == '{' || *q == '[') depth++;
        else if (*q == '}' || *q == ']') depth--;
    }
    return 0;
}

static void jcopy(char *dst, int cap, const char *src) {   // src as a JSON string body
    int n = 0;
    for (; *src && n < cap - 3; src++) {
        if (*src == '"' || *src == '\\') dst[n++] = '\\';
        dst[n++] = (unsigned char)*src < 0x20 ? ' ' : *src;
    }
    dst[n] = 0;
}

// ---------------------------------------------------------------- AiDot sign-in
static unsigned __stdcall aidot_run(void *arg) {
    (void)arg;
    static char buf[512 * 1024], out[64 * 1024];
    char country[48] = "", region[8] = "", enc[512], head[512], token[512] = "", uid[64] = "";
    for (int i = 0; i < (int)(sizeof(COUNTRIES) / sizeof(COUNTRIES[0])); i++)
        if (!_stricmp(a_cc, COUNTRIES[i][0])) { strcpy_s(country, sizeof(country), COUNTRIES[i][1]); strcpy_s(region, sizeof(region), COUNTRIES[i][2]); }
    int ok = country[0] && rsa_encrypt_b64(AIDOT_KEY_PEM, a_pass, enc, sizeof(enc));
    SecureZeroMemory(a_pass, sizeof(a_pass));
    if (!ok) { set_state(3, 0, country[0] ? "encrypt" : "country"); goto done; }

    char host[64]; snprintf(host, sizeof(host), "prod-%s-api.arnoo.com", region);
    char em[256]; jcopy(em, sizeof(em), a_email);
    GUID g; CoCreateGuid(&g);
    char body[1400];
    snprintf(body, sizeof(body), "{\"countryKey\":\"region:%s\",\"username\":\"%s\",\"password\":\"%s\",\"terminalId\":\"%08lx%04x%04x\","
             "\"webVersion\":\"0.5.0\",\"area\":\"Europe/Paris\",\"UTC\":\"UTC+1\"}", country, em, enc, g.Data1, g.Data2, g.Data3);
    SecureZeroMemory(enc, sizeof(enc));
    snprintf(head, sizeof(head), "Content-Type: application/json\r\nAppid: " AIDOT_APP_ID "\r\nTerminal: app\r\n");
    int st = https("aidot", "POST", host, "/v35/users/loginWithFreeVerification", head, body, buf, sizeof(buf));
    SecureZeroMemory(body, sizeof(body));
    const char *e;
    json_get_str(buf, "accessToken", token, sizeof(token));
    if (st != 200 || !token[0]) {
        char m[120] = ""; json_get_str(buf, "desc", m, sizeof(m)); if (!m[0]) json_get_str(buf, "message", m, sizeof(m));
        logf_("aidot: sign-in failed (%d)", st);
        set_state(3, 0, st == 0 ? "network" : m[0] ? m : "login");
        goto done;
    }
    { const char *o = next_obj(buf, &e); if (o) field(o, e, "id", uid, sizeof(uid)); }

    snprintf(head, sizeof(head), "Appid: " AIDOT_APP_ID "\r\nTerminal: app\r\nToken: %s\r\n", token);
    static char houses[64 * 1024];
    if (https("aidot", "GET", host, "/v35/houses", head, NULL, houses, sizeof(houses)) != 200) { set_state(3, 0, "houses"); goto done; }
    int n = snprintf(out, sizeof(out), "{\r\n  \"userId\": \"%s\",\r\n  \"lights\": [", uid), count = 0;
    for (const char *h = next_obj(houses, &e); h; h = next_obj(e, &e)) {
        const char *he = e;
        char hid[64], owner[8];
        field(h, he, "id", hid, sizeof(hid));
        field(h, he, "isOwner", owner, sizeof(owner));
        if (!hid[0] || !strcmp(owner, "false")) { e = he; continue; }
        char path[128]; snprintf(path, sizeof(path), "/v35/devices?houseId=%s", hid);
        if (https("aidot", "GET", host, path, head, NULL, buf, sizeof(buf)) != 200) { e = he; continue; }
        const char *de;
        for (const char *d = next_obj(buf, &de); d; d = next_obj(de, &de)) {
            char type[32], key[80], id[64], name[128], mac[32], model[64], pw[128], sv[32];
            field(d, de, "type", type, sizeof(type));
            field(d, de, "aesKey", key, sizeof(key));
            if (strcmp(type, "light") || !key[0]) continue;
            field(d, de, "id", id, sizeof(id)); field(d, de, "name", name, sizeof(name)); field(d, de, "mac", mac, sizeof(mac));
            field(d, de, "modelId", model, sizeof(model)); field(d, de, "password", pw, sizeof(pw)); field(d, de, "simpleVersion", sv, sizeof(sv));
            n += snprintf(out + n, sizeof(out) - n, "%s\r\n    {\"id\": \"%s\", \"name\": \"%s\", \"mac\": \"%s\", \"model\": \"%s\", \"aesKey\": \"%s\", "
                          "\"password\": \"%s\", \"simpleVersion\": \"%s\"}", count ? "," : "", id, name, mac, model, key, pw, strcmp(sv, "null") ? sv : "");
            count++;
            SecureZeroMemory(key, sizeof(key)); SecureZeroMemory(pw, sizeof(pw));
        }
        e = he;
    }
    n += snprintf(out + n, sizeof(out) - n, "\r\n  ]\r\n}\r\n");
    SecureZeroMemory(token, sizeof(token)); SecureZeroMemory(head, sizeof(head));
    if (!count) { set_state(3, 0, "nolights"); SecureZeroMemory(out, sizeof(out)); goto done; }

    wchar_t p[MAX_PATH]; app_data_path(L"aidot.json", p);
    FILE *f = _wfopen(p, L"wb");
    if (!f) { set_state(3, 0, "write"); SecureZeroMemory(out, sizeof(out)); goto done; }
    fwrite(out, 1, n, f); fclose(f);
    SecureZeroMemory(out, sizeof(out));
    // dev_aidot.c notices the new key file within seconds and connects the bulbs
    logf_("aidot: signed in, %d light(s) saved", count);
    set_state(2, count, "");
done:
    SecureZeroMemory(buf, sizeof(buf));
    SecureZeroMemory(a_email, sizeof(a_email));
    InterlockedExchange(&busy, 0);
    return 0;
}


// ---------------------------------------------------------------- Tuya (cloud project on iot.tuya.com)
// The user makes a free cloud project, links their Smart Life / Tuya app account to it, and enters the project's
// Access ID / Secret here. We read every linked light's local key and DP numbers into tuya.json (drv_tuya.c),
// then forget the project keys. Requests are signed as Tuya's OpenAPI asks (HMAC-SHA256, see sign()).
static const char *const TUYA_REGIONS[][3] = {
    { "eu", "Central Europe", "openapi.tuyaeu.com" }, { "we", "Western Europe", "openapi-weaz.tuyaeu.com" },
    { "us", "Western America", "openapi.tuyaus.com" }, { "ue", "Eastern America", "openapi-ueaz.tuyaus.com" },
    { "cn", "China", "openapi.tuyacn.com" }, { "in", "India", "openapi.tuyain.com" }, { "sg", "Singapore", "openapi-sg.iotbing.com" },
};
static int  t_state, t_found;
static char t_msg[160], t_region[8], t_id[80], t_secret[80];
static volatile LONG t_busy;

static void set_tstate(int st, int n, const char *m) {
    AcquireSRWLockExclusive(&lk);
    t_state = st; t_found = n; snprintf(t_msg, sizeof(t_msg), "%s", m ? m : "");
    ReleaseSRWLockExclusive(&lk);
}

static void hexs(const BYTE *d, int n, char *out, int upper) {
    for (int i = 0; i < n; i++) sprintf(out + i * 2, upper ? "%02X" : "%02x", d[i]);
    out[n * 2] = 0;
}
static void sha256hex(const char *s, char *out) {
    BCRYPT_ALG_HANDLE a; BYTE h[32];
    BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, 0);
    BCryptHash(a, NULL, 0, (PUCHAR)s, (ULONG)strlen(s), h, 32);
    BCryptCloseAlgorithmProvider(a, 0);
    hexs(h, 32, out, 0);
}
// sign = HMAC-SHA256(secret, client_id + access_token + t + nonce + method\nsha256(body)\n\nurl), uppercase hex
static void tuya_headers(const char *method, const char *url, const char *token, char *out, int cap) {
    char t[24], bh[65], str[1024];
    FILETIME ft; GetSystemTimeAsFileTime(&ft);   // milliseconds since 1970
    snprintf(t, sizeof(t), "%llu", ((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000ULL) - 11644473600000ULL);
    sha256hex("", bh);
    snprintf(str, sizeof(str), "%s%s%s%s\n%s\n\n%s", t_id, token, t, method, bh, url);
    BCRYPT_ALG_HANDLE a; BYTE mac[32]; char sig[65];
    BCryptOpenAlgorithmProvider(&a, BCRYPT_SHA256_ALGORITHM, NULL, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    BCryptHash(a, (PUCHAR)t_secret, (ULONG)strlen(t_secret), (PUCHAR)str, (ULONG)strlen(str), mac, 32);
    BCryptCloseAlgorithmProvider(a, 0);
    hexs(mac, 32, sig, 1);
    snprintf(out, cap, "client_id: %s\r\nsign: %s\r\nt: %s\r\nsign_method: HMAC-SHA256\r\n%s%s%s", t_id, sig, t,
             *token ? "access_token: " : "", token, *token ? "\r\n" : "");
    SecureZeroMemory(str, sizeof(str));
}
static int tuya_get(const char *host, const char *url, const char *token, char *out, int cap) {
    char head[1024];
    tuya_headers("GET", url, token, head, sizeof(head));
    int st = https("tuya", "GET", host, url, head, NULL, out, cap);
    char ok[8] = ""; field(out, out + strlen(out), "success", ok, sizeof(ok));
    return st == 200 && !strcmp(ok, "true");
}

static unsigned __stdcall tuya_run(void *arg) {
    (void)arg;
    static char buf[512 * 1024], spec[64 * 1024], out[96 * 1024];
    const char *host = NULL;
    for (int i = 0; i < (int)(sizeof(TUYA_REGIONS) / sizeof(TUYA_REGIONS[0])); i++) if (!strcmp(t_region, TUYA_REGIONS[i][0])) host = TUYA_REGIONS[i][2];
    char token[128] = "";
    int count = 0, n = 0;
    if (!host || !t_id[0] || !t_secret[0]) { set_tstate(3, 0, "tfields"); goto done; }
    const char *e;
    if (!tuya_get(host, "/v1.0/token?grant_type=1", "", buf, sizeof(buf))) {
        char m[120] = ""; const char *o = next_obj(buf, &e); if (o) field(o, e, "msg", m, sizeof(m));
        logf_("tuya: project sign-in failed");
        set_tstate(3, 0, !buf[0] ? "network" : m[0] ? m : "tlogin");
        goto done;
    }
    { const char *r = json_get_obj(buf, "result"); if (r) json_get_str(r, "access_token", token, sizeof(token)); }
    n = snprintf(out, sizeof(out), "{\r\n  \"devices\": [");
    char last[128] = "";
    for (int page = 0; page < 20; page++) {
        char url[256];
        if (last[0]) snprintf(url, sizeof(url), "/v1.0/iot-01/associated-users/devices?last_row_key=%s&size=50", last);
        else snprintf(url, sizeof(url), "/v1.0/iot-01/associated-users/devices?size=50");
        if (!tuya_get(host, url, token, buf, sizeof(buf))) { set_tstate(3, 0, count ? "tpartial" : "tdevices"); goto done; }
        const char *r = json_get_obj(buf, "result"), *arr = r ? strstr(r, "\"devices\"") : NULL;
        const char *de;
        for (const char *d = arr ? next_obj(arr, &de) : NULL; d; d = next_obj(de, &de)) {
            char id[48], key[40], name[128], cat[16], prod[128];
            field(d, de, "id", id, sizeof(id)); field(d, de, "local_key", key, sizeof(key));
            field(d, de, "name", name, sizeof(name)); field(d, de, "category", cat, sizeof(cat)); field(d, de, "product_name", prod, sizeof(prod));
            if (!id[0] || strlen(key) != 16) continue;
            // the DP numbers of switch / mode / brightness / colour, from the device's specification
            char su[160]; snprintf(su, sizeof(su), "/v1.1/devices/%s/specifications", id);
            if (!tuya_get(host, su, token, spec, sizeof(spec))) continue;
            int dsw = 0, dmode = 0, dbri = 0, dcol = 0, v2 = 0;
            const char *fe;
            for (const char *f = next_obj(strstr(spec, "\"functions\"") ? strstr(spec, "\"functions\"") : spec, &fe); f; f = next_obj(fe, &fe)) {
                char code[48], dp[12]; field(f, fe, "code", code, sizeof(code)); field(f, fe, "dp_id", dp, sizeof(dp));
                int n2 = atoi(dp);
                if (!n2) continue;
                if (!strcmp(code, "switch_led") || (!dsw && !strcmp(code, "switch"))) dsw = n2;
                else if (!strcmp(code, "work_mode")) dmode = n2;
                else if (!strcmp(code, "bright_value_v2") || (!dbri && !strcmp(code, "bright_value"))) dbri = n2;
                else if (!strcmp(code, "colour_data_v2")) { dcol = n2; v2 = 1; }
                else if (!dcol && !strcmp(code, "colour_data")) { dcol = n2; v2 = n2 != 5; }
            }
            if (!dsw || !dcol) continue;   // lights with colour only
            n += snprintf(out + n, sizeof(out) - n, "%s\r\n    {\"id\": \"%s\", \"key\": \"%s\", \"name\": \"%s\", \"category\": \"%s\", \"product\": \"%s\", "
                          "\"dp_switch\": %d, \"dp_mode\": %d, \"dp_bright\": %d, \"dp_colour\": %d, \"colour_v2\": %d}",
                          count ? "," : "", id, key, name, cat, prod, dsw, dmode ? dmode : dsw + 1, dbri ? dbri : dsw + 2, dcol, v2);
            count++;
            SecureZeroMemory(key, sizeof(key));
            if (n > (int)sizeof(out) - 1024) break;
        }
        char more[8] = "";
        if (r) { const char *re; const char *ro = next_obj(r, &re); if (ro) { field(ro, re, "has_more", more, sizeof(more)); field(ro, re, "last_row_key", last, sizeof(last)); } }
        if (strcmp(more, "true") || !last[0]) break;
    }
    n += snprintf(out + n, sizeof(out) - n, "\r\n  ]\r\n}\r\n");
    if (!count) { set_tstate(3, 0, "tnolights"); goto done; }
    {
        wchar_t p2[MAX_PATH]; app_data_path(L"tuya.json", p2);
        FILE *f = _wfopen(p2, L"wb");
        if (!f) { set_tstate(3, 0, "write"); goto done; }
        fwrite(out, 1, n, f); fclose(f);
    }
    logf_("tuya: signed in, %d light(s) saved", count);
    set_tstate(2, count, "");
done:
    SecureZeroMemory(out, sizeof(out)); SecureZeroMemory(buf, sizeof(buf)); SecureZeroMemory(spec, sizeof(spec));
    SecureZeroMemory(token, sizeof(token)); SecureZeroMemory(t_secret, sizeof(t_secret)); SecureZeroMemory(t_id, sizeof(t_id));
    InterlockedExchange(&t_busy, 0);
    return 0;
}

void accounts_tuya_login(const char *region, const char *access_id, const char *secret) {
    if (InterlockedCompareExchange(&t_busy, 1, 0)) return;
    snprintf(t_region, sizeof(t_region), "%s", region ? region : "");
    snprintf(t_id, sizeof(t_id), "%s", access_id ? access_id : "");
    snprintf(t_secret, sizeof(t_secret), "%s", secret ? secret : "");
    set_tstate(1, 0, "");
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, tuya_run, NULL, 0, NULL);
    if (t) CloseHandle(t); else { SecureZeroMemory(t_secret, sizeof(t_secret)); set_tstate(3, 0, "thread"); InterlockedExchange(&t_busy, 0); }
}

void accounts_aidot_login(const char *country, const char *email, const char *password) {
    if (InterlockedCompareExchange(&busy, 1, 0)) return;
    snprintf(a_cc, sizeof(a_cc), "%s", country ? country : "");
    snprintf(a_email, sizeof(a_email), "%s", email ? email : "");
    snprintf(a_pass, sizeof(a_pass), "%s", password ? password : "");
    set_state(1, 0, "");
    HANDLE t = (HANDLE)_beginthreadex(NULL, 0, aidot_run, NULL, 0, NULL);
    if (t) CloseHandle(t); else { SecureZeroMemory(a_pass, sizeof(a_pass)); set_state(3, 0, "thread"); InterlockedExchange(&busy, 0); }
}

// {"aidot":{"state":0..3,"msg":"...","found":3,"countries":[["FR","France"],...]}}
int accounts_json(char *out, int cap) {
    AcquireSRWLockShared(&lk);
    char m[340]; jcopy(m, sizeof(m), msg);
    int n = snprintf(out, cap, "{\"aidot\":{\"state\":%d,\"msg\":\"%s\",\"found\":%d,\"countries\":[", state, m, found);
    ReleaseSRWLockShared(&lk);
    for (int i = 0; i < (int)(sizeof(COUNTRIES) / sizeof(COUNTRIES[0])); i++)
        n += snprintf(out + n, cap - n, "%s[\"%s\",\"%s\"]", i ? "," : "", COUNTRIES[i][0], COUNTRIES[i][1]);
    AcquireSRWLockShared(&lk);
    jcopy(m, sizeof(m), t_msg);
    n += snprintf(out + n, cap - n, "]},\"tuya\":{\"state\":%d,\"msg\":\"%s\",\"found\":%d,\"regions\":[", t_state, m, t_found);
    ReleaseSRWLockShared(&lk);
    for (int i = 0; i < (int)(sizeof(TUYA_REGIONS) / sizeof(TUYA_REGIONS[0])); i++)
        n += snprintf(out + n, cap - n, "%s[\"%s\",\"%s\"]", i ? "," : "", TUYA_REGIONS[i][0], TUYA_REGIONS[i][1]);
    n += snprintf(out + n, cap - n, "]}}");
    return n;
}
