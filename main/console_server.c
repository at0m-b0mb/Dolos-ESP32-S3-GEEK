#include "console_server.h"
#include "console_bridge.h"
#include "auth.h"
#include "sbuf.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "nvs.h"
#include "mbedtls/pkcs5.h"
#include "mbedtls/md.h"

static const char *TAG = "console";
static auth_store_t g_auth;
static char g_admin_pass[32];
#define SESSION_TTL_MS (30 * 60 * 1000u)

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

/* --- device crypto: PBKDF2-HMAC-SHA256 + hardware RNG --- */
static void dev_hash(const char *pw, const uint8_t *salt, size_t sl, uint32_t iters, uint8_t out[32])
{
    /* NO #if GUARD HERE.
     *
     * This was once wrapped in `#if defined(MBEDTLS_MD_SHA256)`. That symbol is
     * an ENUM VALUE, not a preprocessor macro, so the guard was always false,
     * the PBKDF2 call was compiled out, and this function returned uninitialised
     * stack as the "hash". Both stored and offered hashes were then garbage -
     * and different garbage each time - so every single login was rejected as a
     * wrong password, with nothing in the logs to say why. */
    memset(out, 0, 32);
    int rc = mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256,
                                           (const unsigned char *)pw, strlen(pw),
                                           salt, sl, iters, 32, out);
    if (rc != 0) ESP_LOGE(TAG, "PBKDF2 failed (-0x%04x) - logins cannot succeed", -rc);
}

/* A broken password hash fails silently and looks exactly like a user typing
 * the wrong password, so prove it works before trusting it: the same input must
 * hash the same way twice, and must not come back as zeros. */
static bool dev_hash_selftest(void)
{
    static const uint8_t salt[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint8_t a[32], b[32];
    const uint8_t zero[32] = { 0 };
    dev_hash("dolos-selftest", salt, sizeof(salt), 1000, a);
    dev_hash("dolos-selftest", salt, sizeof(salt), 1000, b);
    if (memcmp(a, b, sizeof(a)) != 0) {
        ESP_LOGE(TAG, "password hash is not deterministic - console disabled");
        return false;
    }
    if (memcmp(a, zero, sizeof(a)) == 0) {
        ESP_LOGE(TAG, "password hash produced zeros - console disabled");
        return false;
    }
    return true;
}
static void dev_rng(uint8_t *buf, size_t n) { esp_fill_random(buf, n); }

/* ---- accounts survive a reboot ----------------------------------------
 * The user table is a fixed POD array, so it round-trips as a single NVS blob.
 * It is versioned deliberately: a firmware whose auth_user_t layout differs
 * must ignore what an older one wrote rather than reinterpret raw bytes as
 * salts and password hashes. */
#define USERS_BLOB_VERSION 1

static void users_save(void)
{
    nvs_handle_t h;
    if (nvs_open("dolos", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "usersver", USERS_BLOB_VERSION);
    if (nvs_set_blob(h, "users", g_auth.users, sizeof(g_auth.users)) != ESP_OK)
        ESP_LOGE(TAG, "could not save accounts - they will not survive a reboot");
    nvs_commit(h);
    nvs_close(h);
}

static bool users_load(void)
{
    nvs_handle_t h;
    if (nvs_open("dolos", NVS_READONLY, &h) != ESP_OK) return false;
    uint8_t ver = 0;
    size_t len = sizeof(g_auth.users);
    bool ok = (nvs_get_u8(h, "usersver", &ver) == ESP_OK) && ver == USERS_BLOB_VERSION &&
              (nvs_get_blob(h, "users", g_auth.users, &len) == ESP_OK) &&
              len == sizeof(g_auth.users);
    nvs_close(h);
    if (!ok) return false;
    int n = auth_user_count(&g_auth);
    ESP_LOGI(TAG, "%d account(s) restored from NVS", n);
    return n > 0;
}

/* --- tiny helpers --- */
static void reply(httpd_req_t *r, const char *status, const char *type, const char *body)
{
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, type);
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    /* This console arms a device that types into whoever is holding it, so the
     * usual web defences are not decoration here.
     *
     * no-store: every response carries either a credential, a payload or device
     *   state. None of it belongs in a browser cache or a proxy.
     * DENY / frame-ancestors: a hidden frame could trick an authenticated
     *   operator into clicking "arm and fire". That is the attack this device
     *   most needs protecting from, and it costs one header.
     * no-referrer: the address bar holds the device's address; it should not
     *   leak to anywhere the operator browses next. */
    httpd_resp_set_hdr(r, "Cache-Control", "no-store, no-cache, must-revalidate, private");
    httpd_resp_set_hdr(r, "Pragma", "no-cache");
    httpd_resp_set_hdr(r, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(r, "Referrer-Policy", "no-referrer");
    httpd_resp_set_hdr(r, "Permissions-Policy",
                       "camera=(), microphone=(), geolocation=(), usb=()");
    httpd_resp_sendstr(r, body);
}
static void reply_json(httpd_req_t *r, const char *status, const char *json)
{ reply(r, status, "application/json", json); }

#define BODY_TOO_LARGE (-2)

static int read_body(httpd_req_t *r, char *buf, size_t cap)
{
    /* content_len is a size_t. Narrowing it to int first meant a declared
     * length above INT_MAX came out NEGATIVE, sailed past the size check, and
     * then skipped the read loop entirely - so an absurd Content-Length was
     * accepted as a valid EMPTY body instead of being refused. Compare in the
     * type the field actually has. */
    size_t total = r->content_len;
    /* Truncating silently is the wrong answer: for a payload upload it would
     * save a script that stops halfway through, with the console reporting
     * success. Refuse instead, and let the caller say so. */
    if (cap == 0 || total >= cap) return BODY_TOO_LARGE;
    size_t off = 0;
    while (off < total) {
        int k = httpd_req_recv(r, buf + off, total - off);
        if (k <= 0) return -1;
        off += (size_t)k;
    }
    buf[off] = 0;
    return (int)off;
}

/* minimal url-decode form value: key=...&... */
static bool form_val(const char *body, const char *key, char *out, size_t cap)
{
    size_t kl = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, kl) == 0 && p[kl] == '=') {
            const char *v = p + kl + 1; size_t o = 0;
            while (*v && *v != '&' && o < cap - 1) {
                if (*v == '%' && v[1] && v[2]) {
                    char h[3] = { v[1], v[2], 0 }; out[o++] = (char)strtol(h, NULL, 16); v += 3;
                } else if (*v == '+') { out[o++] = ' '; v++; }
                else out[o++] = *v++;
            }
            out[o] = 0; return true;
        }
        p = strchr(p, '&'); if (p) p++;
    }
    return false;
}

static bool cookie_sid(httpd_req_t *r, char *out, size_t cap)
{
    char c[256];
    if (httpd_req_get_hdr_value_str(r, "Cookie", c, sizeof(c)) != ESP_OK) return false;
    char *p = strstr(c, "sid=");
    if (!p) return false;
    p += 4; size_t o = 0;
    while (*p && *p != ';' && *p != ' ' && o < cap - 1) out[o++] = *p++;
    out[o] = 0; return o == 32;
}

/* Resolve session + enforce permission (+ CSRF for state changes). Sends the
 * error response itself and returns NULL on failure. */
static auth_session_t *require(httpd_req_t *r, perm_t perm, bool need_csrf)
{
    char sid[40];
    if (!cookie_sid(r, sid, sizeof(sid))) { reply_json(r, "401 Unauthorized", "{\"err\":\"login\"}"); return NULL; }
    auth_session_t *s = auth_session_lookup(&g_auth, sid, now_ms());
    if (!s) { reply_json(r, "401 Unauthorized", "{\"err\":\"session\"}"); return NULL; }
    if (!rbac_allows(s->role, perm)) { reply_json(r, "403 Forbidden", "{\"err\":\"role\"}"); return NULL; }
    if (need_csrf) {
        char x[40];
        if (httpd_req_get_hdr_value_str(r, "X-CSRF", x, sizeof(x)) != ESP_OK || !auth_csrf_ok(s, x)) {
            reply_json(r, "403 Forbidden", "{\"err\":\"csrf\"}"); return NULL;
        }
    }
    return s;
}

/* ---------------- handlers ---------------- */
static esp_err_t h_login(httpd_req_t *r)
{
    if (auth_locked(&g_auth, now_ms())) { reply_json(r, "429 Too Many Requests", "{\"err\":\"locked\"}"); return ESP_OK; }
    char body[256], user[24], pass[64];
    if (read_body(r, body, sizeof(body)) < 0) { reply_json(r, "400 Bad Request", "{\"err\":\"body\"}"); return ESP_OK; }
    if (!form_val(body, "user", user, sizeof(user)) || !form_val(body, "pass", pass, sizeof(pass))) {
        reply_json(r, "400 Bad Request", "{\"err\":\"fields\"}"); return ESP_OK;
    }
    role_t role = auth_verify(&g_auth, user, pass);
    if (role == ROLE_NONE) { auth_note_fail(&g_auth, now_ms()); reply_json(r, "401 Unauthorized", "{\"err\":\"bad\"}"); return ESP_OK; }
    auth_note_success(&g_auth);
    bridge_note_console_login();   /* stop showing the password on screen */
    auth_session_t *s = auth_session_create(&g_auth, role, now_ms(), SESSION_TTL_MS);
    if (!s) { reply_json(r, "503 Service Unavailable", "{\"err\":\"full\"}"); return ESP_OK; }
    char ck[96]; snprintf(ck, sizeof(ck), "sid=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=1800", s->token);
    httpd_resp_set_hdr(r, "Set-Cookie", ck);
    char out[96]; snprintf(out, sizeof(out), "{\"role\":\"%s\",\"csrf\":\"%s\"}", role_name(role), s->csrf);
    reply_json(r, "200 OK", out);
    return ESP_OK;
}
static esp_err_t h_logout(httpd_req_t *r)
{
    char sid[40]; if (cookie_sid(r, sid, sizeof(sid))) auth_session_destroy(&g_auth, sid);
    httpd_resp_set_hdr(r, "Set-Cookie", "sid=; Path=/; Max-Age=0");
    reply_json(r, "200 OK", "{\"ok\":1}"); return ESP_OK;
}
/* GET /api/users - who can sign in, and as what. Admin only: the account list
 * tells an attacker exactly which names are worth guessing passwords for. */
static esp_err_t h_users_get(httpd_req_t *r)
{
    if (!require(r, PERM_EDIT_CONFIG, false)) return ESP_OK;
    char buf[512];
    sbuf_t sb; sbuf_init(&sb, buf, sizeof(buf));
    sappend(&sb, "{\"users\":[");
    const char *n; role_t role; bool first = true;
    for (int i = 0; i < AUTH_MAX_USERS; i++) {
        if (!auth_user_at(&g_auth, i, &n, &role)) continue;
        sappend(&sb, "%s{\"name\":\"%s\",\"role\":\"%s\"}",
                first ? "" : ",", n, role_name(role));
        first = false;
    }
    sappend(&sb, "],\"max\":%d}", AUTH_MAX_USERS);
    reply_json(r, "200 OK", buf);
    return ESP_OK;
}

/* POST /api/users - create an account (or reset an existing one's password). */
static esp_err_t h_users_post(httpd_req_t *r)
{
    if (!require(r, PERM_EDIT_CONFIG, true)) return ESP_OK;
    char body[256], user[24], pass[64], role[16];
    if (read_body(r, body, sizeof(body)) < 0 ||
        !form_val(body, "user", user, sizeof(user)) ||
        !form_val(body, "pass", pass, sizeof(pass)) ||
        !form_val(body, "role", role, sizeof(role))) {
        reply_json(r, "400 Bad Request",
                   "{\"err\":\"Username, password and role are all required.\"}");
        return ESP_OK;
    }
    if (strlen(user) < 2) {
        reply_json(r, "400 Bad Request", "{\"err\":\"Username is too short.\"}"); return ESP_OK;
    }
    if (strlen(pass) < 8) {
        reply_json(r, "400 Bad Request",
                   "{\"err\":\"Password must be at least 8 characters.\"}"); return ESP_OK;
    }
    role_t rl = ROLE_VIEWER;
    if      (strcmp(role, "admin")    == 0) rl = ROLE_ADMIN;
    else if (strcmp(role, "operator") == 0) rl = ROLE_OPERATOR;
    if (!auth_set_user(&g_auth, user, pass, rl)) {
        reply_json(r, "507 Insufficient Storage",
                   "{\"err\":\"No free account slots.\"}"); return ESP_OK;
    }
    users_save();
    ESP_LOGW(TAG, "account '%s' created/updated as %s", user, role_name(rl));
    reply_json(r, "200 OK", "{\"ok\":1}");
    return ESP_OK;
}

/* DELETE /api/users?name=... - the auth core refuses to remove the last admin. */
static esp_err_t h_users_delete(httpd_req_t *r)
{
    if (!require(r, PERM_EDIT_CONFIG, true)) return ESP_OK;
    char q[128], name[24];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "name", name, sizeof(name)) != ESP_OK) {
        reply_json(r, "400 Bad Request", "{\"err\":\"name\"}"); return ESP_OK;
    }
    if (!auth_delete_user(&g_auth, name)) {
        reply_json(r, "409 Conflict",
                   "{\"err\":\"Cannot delete: unknown account, or it is the last administrator.\"}");
        return ESP_OK;
    }
    users_save();
    ESP_LOGW(TAG, "account '%s' deleted", name);
    reply_json(r, "200 OK", "{\"ok\":1}");
    return ESP_OK;
}

static esp_err_t h_status(httpd_req_t *r)
{
    auth_session_t *sess = require(r, PERM_VIEW, false);
    if (!sess) return ESP_OK;
    /* Room for the device status plus the session fields spliced in below. */
    char buf[640];
    bridge_status_json(buf, sizeof(buf));
    /* Splice in how long this session has left, so the console can warn before
     * it logs someone out mid-edit rather than after. Polling status does NOT
     * extend it - only the person can, via /api/extend. */
    size_t n = strlen(buf);
    if (n > 1 && buf[n - 1] == '}') {
        /* Build the tail separately and splice it only if it FITS. Writing in
         * place with a length of (sizeof - n) was one byte short: on a full
         * buffer it replaced the closing brace with a terminator and nothing
         * else, and the console then received JSON it could not parse - so the
         * whole status panel stopped updating. A status object without the
         * extra field beats a truncated one. */
        char tail[48];
        int t = snprintf(tail, sizeof(tail), ",\"session_s\":%lu}",
                         (unsigned long)(auth_session_remaining_ms(sess, now_ms()) / 1000));
        if (t > 0 && (n - 1) + (size_t)t < sizeof(buf))
            memcpy(buf + n - 1, tail, (size_t)t + 1);
    }
    reply_json(r, "200 OK", buf);
    return ESP_OK;
}

/* "Stay signed in": restart the idle clock. Needs a live session and a CSRF
 * token, so it cannot be triggered by anything but the console itself. */
static esp_err_t h_extend(httpd_req_t *r)
{
    auth_session_t *sess = require(r, PERM_VIEW, true);
    if (!sess) return ESP_OK;
    auth_session_extend(sess, now_ms(), SESSION_TTL_MS);
    char ck[96];
    snprintf(ck, sizeof(ck), "sid=%s; Path=/; HttpOnly; SameSite=Strict; Max-Age=%u",
             sess->token, (unsigned)(SESSION_TTL_MS / 1000));
    httpd_resp_set_hdr(r, "Set-Cookie", ck);
    char out[64];
    snprintf(out, sizeof(out), "{\"session_s\":%lu}", (unsigned long)(SESSION_TTL_MS / 1000));
    reply_json(r, "200 OK", out);
    return ESP_OK;
}
static esp_err_t h_payloads(httpd_req_t *r)
{
    if (!require(r, PERM_VIEW, false)) return ESP_OK;
    char buf[1024]; bridge_list_payloads(buf, sizeof(buf)); reply_json(r, "200 OK", buf); return ESP_OK;
}
static esp_err_t h_payload_get(httpd_req_t *r)
{
    if (!require(r, PERM_RUN, false)) return ESP_OK;
    char q[128], name[64];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "name", name, sizeof(name)) != ESP_OK) { reply_json(r, "400 Bad Request", "{\"err\":\"name\"}"); return ESP_OK; }
    static char buf[6144];
    if (bridge_read_payload(name, buf, sizeof(buf)) < 0) { reply_json(r, "404 Not Found", "{\"err\":\"nf\"}"); return ESP_OK; }
    reply(r, "200 OK", "text/plain", buf); return ESP_OK;
}
/* Choose the active payload. Clicking a payload in the console used to only
 * open it in the editor; the device kept whatever was picked with the button. */
static esp_err_t h_select(httpd_req_t *r)
{
    if (!require(r, PERM_RUN, true)) return ESP_OK;
    char q[128], name[64];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "name", name, sizeof(name)) != ESP_OK) {
        reply_json(r, "400 Bad Request", "{\"err\":\"name\"}"); return ESP_OK;
    }
    if (!bridge_remote_select(name)) {
        reply_json(r, "404 Not Found", "{\"err\":\"No such payload on the card.\"}");
        return ESP_OK;
    }
    reply_json(r, "200 OK", "{\"ok\":1}");
    return ESP_OK;
}

static esp_err_t h_payload_put(httpd_req_t *r)
{
    auth_session_t *s = require(r, PERM_EDIT_PAYLOAD, true); if (!s) return ESP_OK;
    char q[128], name[64];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "name", name, sizeof(name)) != ESP_OK) { reply_json(r, "400 Bad Request", "{\"err\":\"name\"}"); return ESP_OK; }
    static char body[6144];
    int n = read_body(r, body, sizeof(body));
    if (n == BODY_TOO_LARGE) {
        reply_json(r, "413 Payload Too Large",
                   "{\"err\":\"That payload is too large for the device (6 KB maximum).\"}");
        return ESP_OK;
    }
    if (n < 0 || !bridge_write_payload(name, body, n)) {
        reply_json(r, "400 Bad Request",
                   "{\"err\":\"Could not write the payload to the card.\"}");
        return ESP_OK;
    }
    reply_json(r, "200 OK", "{\"ok\":1}"); return ESP_OK;
}
static esp_err_t h_config_get(httpd_req_t *r)
{
    if (!require(r, PERM_EDIT_CONFIG, false)) return ESP_OK;   /* config can hold secrets */
    char buf[1024]; bridge_get_config_text(buf, sizeof(buf)); reply(r, "200 OK", "text/plain", buf); return ESP_OK;
}
static esp_err_t h_config_post(httpd_req_t *r)
{
    if (!require(r, PERM_EDIT_CONFIG, true)) return ESP_OK;
    char body[1024]; int n = read_body(r, body, sizeof(body));
    if (n < 0 || !bridge_set_config(body)) { reply_json(r, "400 Bad Request", "{\"err\":\"cfg\"}"); return ESP_OK; }
    reply_json(r, "200 OK", "{\"ok\":1}"); return ESP_OK;
}
/* GET /api/settings - the live values, for the console's form controls. */
static esp_err_t h_settings_get(httpd_req_t *r)
{
    if (!require(r, PERM_EDIT_CONFIG, false)) return ESP_OK;
    char buf[512]; bridge_settings_json(buf, sizeof(buf));
    reply_json(r, "200 OK", buf); return ESP_OK;
}

/* POST /api/settings - change ONE setting, applied immediately. */
static esp_err_t h_settings_post(httpd_req_t *r)
{
    if (!require(r, PERM_EDIT_CONFIG, true)) return ESP_OK;
    char body[192], key[32], val[96];
    if (read_body(r, body, sizeof(body)) < 0 ||
        !form_val(body, "key", key, sizeof(key)) ||
        !form_val(body, "value", val, sizeof(val))) {
        reply_json(r, "400 Bad Request", "{\"err\":\"key and value are required\"}");
        return ESP_OK;
    }
    if (!bridge_set_setting(key, val)) {
        reply_json(r, "400 Bad Request",
                   "{\"err\":\"That setting was not recognised, or the value did not change anything.\"}");
        return ESP_OK;
    }
    char buf[512]; bridge_settings_json(buf, sizeof(buf));
    reply_json(r, "200 OK", buf);
    return ESP_OK;
}

/* POST /api/settings/save - persist to DOLOS.CFG on the card. */
static esp_err_t h_settings_save(httpd_req_t *r)
{
    if (!require(r, PERM_EDIT_CONFIG, true)) return ESP_OK;
    if (!bridge_save_settings()) {
        reply_json(r, "409 Conflict",
                   "{\"err\":\"Could not write DOLOS.CFG - is a card inserted?\"}");
        return ESP_OK;
    }
    reply_json(r, "200 OK", "{\"ok\":1}");
    return ESP_OK;
}

static esp_err_t h_audit(httpd_req_t *r)
{
    if (!require(r, PERM_AUDIT, false)) return ESP_OK;
    static char buf[4096]; bridge_read_audit(buf, sizeof(buf)); reply(r, "200 OK", "text/plain", buf); return ESP_OK;
}
static esp_err_t h_remotefire(httpd_req_t *r)
{
    if (!require(r, PERM_TOGGLE_REMOTE_FIRE, true)) return ESP_OK;
    /* The return value used to be discarded. On a failed or oversized read the
     * buffer is UNINITIALISED STACK, and it was then parsed to decide whether
     * remote fire is on - reusing whatever an earlier request left behind. This
     * is the one switch that must never move on its own, so a body that does
     * not parse is refused outright rather than guessed at. */
    char body[64], en[8];
    int n = read_body(r, body, sizeof(body));
    if (n < 0 || !form_val(body, "enable", en, sizeof(en))) {
        reply_json(r, "400 Bad Request",
                   "{\"err\":\"Say explicitly whether remote fire should be on or off.\"}");
        return ESP_OK;
    }
    bool on = (en[0] == '1' || en[0] == 't' || en[0] == 'o');
    bridge_set_remote_fire_enabled(on);
    reply_json(r, "200 OK", on ? "{\"remote_fire\":1}" : "{\"remote_fire\":0}"); return ESP_OK;
}
static esp_err_t h_arm(httpd_req_t *r)
{
    if (!require(r, PERM_RUN, true)) return ESP_OK;
    /* Exactly ONE call. This used to be invoked twice - once for the status
     * line and once for the body - which queued the fire request twice and let
     * the two results disagree, so a successful arm could be reported as a
     * failure. */
    arm_result_t rc = bridge_remote_arm();
    switch (rc) {
        case ARM_OK:
            reply_json(r, "200 OK", "{\"armed\":1}");
            break;
        case ARM_ERR_REMOTE_OFF:
            reply_json(r, "403 Forbidden",
                       "{\"err\":\"Remote fire is disabled. Enable it above first.\"}");
            break;
        case ARM_ERR_FLASH_MODE:
            reply_json(r, "409 Conflict",
                       "{\"err\":\"Device is in FLASH MODE - USB-HID is not running, so it cannot type.\"}");
            break;
        case ARM_ERR_LINT:
            reply_json(r, "409 Conflict",
                       "{\"err\":\"The selected payload has errors and cannot be armed.\"}");
            break;
        default:
            reply_json(r, "409 Conflict",
                       "{\"err\":\"Device is busy - it is already arming, firing or running.\"}");
            break;
    }
    return ESP_OK;
}
static esp_err_t h_factory_reset(httpd_req_t *r)
{
    /* Admin only, and CSRF-checked: this throws away every credential on the
     * device. It is the authorised counterpart to the eFuse options, which
     * nothing can reverse. */
    if (!require(r, PERM_EDIT_CONFIG, true)) return ESP_OK;
    reply_json(r, "200 OK", "{\"ok\":1,\"restarting\":1}");
    vTaskDelay(pdMS_TO_TICKS(250));      /* let the reply reach the browser */
    dolos_factory_reset();
    return ESP_OK;
}

static esp_err_t h_abort(httpd_req_t *r)
{
    if (!require(r, PERM_RUN, true)) return ESP_OK;
    bridge_remote_abort(); reply_json(r, "200 OK", "{\"ok\":1}"); return ESP_OK;
}

extern const char CONSOLE_HTML[] asm("_binary_console_html_start");
static esp_err_t h_root(httpd_req_t *r)
{
    /* The console loads NOTHING from anywhere else - no CDN, no font host, no
     * external script - so it can forbid all of it outright. An injected tag
     * then has nowhere to fetch from and no frame to sit in. */
    httpd_resp_set_hdr(r, "Content-Security-Policy",
                       "default-src 'self' 'unsafe-inline' data:; "
                       "connect-src 'self'; img-src 'self' data:; "
                       "object-src 'none'; base-uri 'none'; "
                       "form-action 'self'; frame-ancestors 'none'");
    reply(r, "200 OK", "text/html", CONSOLE_HTML); return ESP_OK;
}

static void reg(httpd_handle_t s, const char *uri, httpd_method_t m, esp_err_t (*fn)(httpd_req_t *))
{
    httpd_uri_t u = { .uri = uri, .method = m, .handler = fn }; httpd_register_uri_handler(s, &u);
}

const char *console_admin_password(void) { return g_admin_pass; }

bool console_server_start(const char *admin_user, const char *admin_pass, bool remote_fire_default)
{
    (void)remote_fire_default;
    if (!dev_hash_selftest()) return false;   /* never serve a login that cannot pass */
    auth_init(&g_auth, dev_hash, dev_rng, 20000);   /* 20k PBKDF2 iterations */

    /* effective admin password: config value, or a random one shown on the LCD */
    if (admin_pass && *admin_pass) { strncpy(g_admin_pass, admin_pass, sizeof(g_admin_pass) - 1); }
    else {
        uint8_t rb[6]; dev_rng(rb, sizeof(rb));
        snprintf(g_admin_pass, sizeof(g_admin_pass), "%02X%02X%02X%02X%02X%02X",
                 rb[0], rb[1], rb[2], rb[3], rb[4], rb[5]);
    }
    /* Restore saved accounts; only mint the built-in admin on a truly fresh
     * device, so accounts created in the console are not wiped every boot. */
    const char *au = (admin_user && *admin_user) ? admin_user : "admin";
    if (!users_load()) {
        auth_set_user(&g_auth, au, g_admin_pass, ROLE_ADMIN);
        users_save();
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 24; cfg.lru_purge_enable = true; cfg.stack_size = 8192;
    httpd_handle_t srv = NULL;
    /* Idempotent, for the same reason the Wi-Fi bring-up is: starting a second
     * server on the same port fails, and re-running auth_init() would throw
     * away every account. Being asked twice must be harmless. */
    static bool started;
    if (started) return true;
    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return false; }
    started = true;

    reg(srv, "/",               HTTP_GET,  h_root);
    reg(srv, "/api/login",      HTTP_POST, h_login);
    reg(srv, "/api/logout",     HTTP_POST, h_logout);
    reg(srv, "/api/extend",     HTTP_POST, h_extend);
    reg(srv, "/api/settings",      HTTP_GET,  h_settings_get);
    reg(srv, "/api/settings",      HTTP_POST, h_settings_post);
    reg(srv, "/api/settings/save", HTTP_POST, h_settings_save);
    reg(srv, "/api/users",      HTTP_GET,    h_users_get);
    reg(srv, "/api/users",      HTTP_POST,   h_users_post);
    reg(srv, "/api/users",      HTTP_DELETE, h_users_delete);
    reg(srv, "/api/status",     HTTP_GET,  h_status);
    reg(srv, "/api/payloads",   HTTP_GET,  h_payloads);
    reg(srv, "/api/payload",    HTTP_GET,  h_payload_get);
    reg(srv, "/api/payload",    HTTP_PUT,  h_payload_put);
    reg(srv, "/api/select",     HTTP_POST, h_select);
    reg(srv, "/api/config",     HTTP_GET,  h_config_get);
    reg(srv, "/api/config",     HTTP_POST, h_config_post);
    reg(srv, "/api/audit",      HTTP_GET,  h_audit);
    reg(srv, "/api/remotefire", HTTP_POST, h_remotefire);
    reg(srv, "/api/arm",        HTTP_POST, h_arm);
    reg(srv, "/api/abort",      HTTP_POST, h_abort);
    reg(srv, "/api/factory_reset", HTTP_POST, h_factory_reset);

    /* The password is NOT logged. Logs are teed to /sdcard/DOLOS_BOOT.LOG, and
     * a card is readable on any laptop - printing the console password there
     * would undo exactly the protection that keeps it out of DOLOS.CFG. It is
     * shown on the device's own screen instead, where seeing it requires
     * holding the device. */
    ESP_LOGI(TAG, "console up. admin user='%s' (password is shown on the device screen)", au);
    return true;
}
