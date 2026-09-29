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
// [aidot] server= overrides the host (tests: tools/sim/fake_aidot.py on http://127.0.0.1:8779).
static int https(const char *method, const char *host, const char *path, const char *headers, const char *body,
                 char *out, int cap) {
    int n = 0, code = 0;
    wchar_t whost[128], wpath[512], whead[1024];
    const char *ov = cfg_get("aidot", "server", "");
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
    MultiByteToWideChar(CP_UTF8, 0, headers, -1, whead, 1024);
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
    int st = https("POST", host, "/v35/users/loginWithFreeVerification", head, body, buf, sizeof(buf));
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
    if (https("GET", host, "/v35/houses", head, NULL, houses, sizeof(houses)) != 200) { set_state(3, 0, "houses"); goto done; }
    int n = snprintf(out, sizeof(out), "{\r\n  \"userId\": \"%s\",\r\n  \"lights\": [", uid), count = 0;
    for (const char *h = next_obj(houses, &e); h; h = next_obj(e, &e)) {
        const char *he = e;
        char hid[64], owner[8];
        field(h, he, "id", hid, sizeof(hid));
        field(h, he, "isOwner", owner, sizeof(owner));
        if (!hid[0] || !strcmp(owner, "false")) { e = he; continue; }
        char path[128]; snprintf(path, sizeof(path), "/v35/devices?houseId=%s", hid);
        if (https("GET", host, path, head, NULL, buf, sizeof(buf)) != 200) { e = he; continue; }
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
    n += snprintf(out + n, cap - n, "]}}");
    return n;
}
