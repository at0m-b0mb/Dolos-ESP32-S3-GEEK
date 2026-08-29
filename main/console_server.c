#include "console_server.h"
#include "console_bridge.h"
#include "auth.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "esp_http_server.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "esp_log.h"
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
#if defined(MBEDTLS_MD_SHA256)
    mbedtls_pkcs5_pbkdf2_hmac_ext(MBEDTLS_MD_SHA256, (const unsigned char *)pw, strlen(pw),
                                  salt, sl, iters, 32, out);
#endif
}
static void dev_rng(uint8_t *buf, size_t n) { esp_fill_random(buf, n); }

/* --- tiny helpers --- */
static void reply(httpd_req_t *r, const char *status, const char *type, const char *body)
{
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, type);
    httpd_resp_set_hdr(r, "X-Content-Type-Options", "nosniff");
    httpd_resp_sendstr(r, body);
}
static void reply_json(httpd_req_t *r, const char *status, const char *json)
{ reply(r, status, "application/json", json); }

static int read_body(httpd_req_t *r, char *buf, size_t cap)
{
    int total = r->content_len;
    if (total > (int)cap - 1) total = cap - 1;
    int off = 0;
    while (off < total) {
        int k = httpd_req_recv(r, buf + off, total - off);
        if (k <= 0) return -1;
        off += k;
    }
    buf[off] = 0;
    return off;
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
static esp_err_t h_status(httpd_req_t *r)
{
    if (!require(r, PERM_VIEW, false)) return ESP_OK;
    char buf[512]; bridge_status_json(buf, sizeof(buf)); reply_json(r, "200 OK", buf); return ESP_OK;
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
static esp_err_t h_payload_put(httpd_req_t *r)
{
    auth_session_t *s = require(r, PERM_EDIT_PAYLOAD, true); if (!s) return ESP_OK;
    char q[128], name[64];
    if (httpd_req_get_url_query_str(r, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, "name", name, sizeof(name)) != ESP_OK) { reply_json(r, "400 Bad Request", "{\"err\":\"name\"}"); return ESP_OK; }
    static char body[6144];
    int n = read_body(r, body, sizeof(body));
    if (n < 0 || !bridge_write_payload(name, body, n)) { reply_json(r, "400 Bad Request", "{\"err\":\"write\"}"); return ESP_OK; }
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
static esp_err_t h_audit(httpd_req_t *r)
{
    if (!require(r, PERM_AUDIT, false)) return ESP_OK;
    static char buf[4096]; bridge_read_audit(buf, sizeof(buf)); reply(r, "200 OK", "text/plain", buf); return ESP_OK;
}
static esp_err_t h_remotefire(httpd_req_t *r)
{
    if (!require(r, PERM_TOGGLE_REMOTE_FIRE, true)) return ESP_OK;
    char body[64], en[8]; read_body(r, body, sizeof(body));
    bool on = form_val(body, "enable", en, sizeof(en)) && (en[0] == '1' || en[0] == 't' || en[0] == 'o');
    bridge_set_remote_fire_enabled(on);
    reply_json(r, "200 OK", on ? "{\"remote_fire\":1}" : "{\"remote_fire\":0}"); return ESP_OK;
}
static esp_err_t h_arm(httpd_req_t *r)
{
    if (!require(r, PERM_RUN, true)) return ESP_OK;
    if (!bridge_remote_fire_enabled()) { reply_json(r, "403 Forbidden", "{\"err\":\"remote_fire_off\"}"); return ESP_OK; }
    reply_json(r, bridge_remote_arm() ? "200 OK" : "409 Conflict",
               bridge_remote_arm() ? "{\"armed\":1}" : "{\"err\":\"busy\"}"); return ESP_OK;
}
static esp_err_t h_abort(httpd_req_t *r)
{
    if (!require(r, PERM_RUN, true)) return ESP_OK;
    bridge_remote_abort(); reply_json(r, "200 OK", "{\"ok\":1}"); return ESP_OK;
}

extern const char CONSOLE_HTML[] asm("_binary_console_html_start");
static esp_err_t h_root(httpd_req_t *r)
{
    httpd_resp_set_hdr(r, "Content-Security-Policy", "default-src 'self' 'unsafe-inline'");
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
    auth_init(&g_auth, dev_hash, dev_rng, 20000);   /* 20k PBKDF2 iterations */

    /* effective admin password: config value, or a random one shown on the LCD */
    if (admin_pass && *admin_pass) { strncpy(g_admin_pass, admin_pass, sizeof(g_admin_pass) - 1); }
    else {
        uint8_t rb[6]; dev_rng(rb, sizeof(rb));
        snprintf(g_admin_pass, sizeof(g_admin_pass), "%02X%02X%02X%02X%02X%02X",
                 rb[0], rb[1], rb[2], rb[3], rb[4], rb[5]);
    }
    const char *au = (admin_user && *admin_user) ? admin_user : "admin";
    auth_set_user(&g_auth, au, g_admin_pass, ROLE_ADMIN);

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 16; cfg.lru_purge_enable = true; cfg.stack_size = 8192;
    httpd_handle_t srv = NULL;
    if (httpd_start(&srv, &cfg) != ESP_OK) { ESP_LOGE(TAG, "httpd start failed"); return false; }

    reg(srv, "/",               HTTP_GET,  h_root);
    reg(srv, "/api/login",      HTTP_POST, h_login);
    reg(srv, "/api/logout",     HTTP_POST, h_logout);
    reg(srv, "/api/status",     HTTP_GET,  h_status);
    reg(srv, "/api/payloads",   HTTP_GET,  h_payloads);
    reg(srv, "/api/payload",    HTTP_GET,  h_payload_get);
    reg(srv, "/api/payload",    HTTP_PUT,  h_payload_put);
    reg(srv, "/api/config",     HTTP_GET,  h_config_get);
    reg(srv, "/api/config",     HTTP_POST, h_config_post);
    reg(srv, "/api/audit",      HTTP_GET,  h_audit);
    reg(srv, "/api/remotefire", HTTP_POST, h_remotefire);
    reg(srv, "/api/arm",        HTTP_POST, h_arm);
    reg(srv, "/api/abort",      HTTP_POST, h_abort);

    ESP_LOGI(TAG, "console up. admin user='%s' pass='%s'", au, g_admin_pass);
    return true;
}
