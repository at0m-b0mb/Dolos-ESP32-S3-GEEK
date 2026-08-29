#include "auth.h"
#include <string.h>

/* RBAC matrix: which minimum role each permission needs. */
static role_t perm_min_role(perm_t p)
{
    switch (p) {
        case PERM_VIEW:
        case PERM_AUDIT:            return ROLE_VIEWER;
        case PERM_RUN:              return ROLE_OPERATOR;
        case PERM_EDIT_PAYLOAD:
        case PERM_EDIT_CONFIG:
        case PERM_MANAGE_WIFI:
        case PERM_TOGGLE_REMOTE_FIRE:
        default:                    return ROLE_ADMIN;
    }
}
bool rbac_allows(role_t role, perm_t perm)
{
    if (role == ROLE_NONE || perm >= PERM__COUNT) return false;
    return role >= perm_min_role(perm);   /* roles are ordered viewer<oper<admin */
}
const char *role_name(role_t r)
{
    switch (r) { case ROLE_ADMIN: return "admin"; case ROLE_OPERATOR: return "operator";
                 case ROLE_VIEWER: return "viewer"; default: return "none"; }
}

int ct_memcmp(const void *a, const void *b, size_t n)
{
    const volatile uint8_t *x = a, *y = b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (uint8_t)(x[i] ^ y[i]);
    return diff;                          /* 0 iff equal, always scans all n */
}

static void hex32(auth_rng_fn rng, char out[33])
{
    uint8_t r[16]; rng(r, sizeof(r));
    static const char *H = "0123456789abcdef";
    for (int i = 0; i < 16; i++) { out[i*2] = H[r[i] >> 4]; out[i*2+1] = H[r[i] & 0xF]; }
    out[32] = 0;
}

void auth_init(auth_store_t *s, auth_hash_fn hash, auth_rng_fn rng, uint32_t iters)
{
    memset(s, 0, sizeof(*s));
    s->hash = hash; s->rng = rng; s->iters = iters ? iters : 1000;
}

bool auth_set_user(auth_store_t *s, const char *user, const char *password, role_t role)
{
    int slot = -1;
    for (int i = 0; i < AUTH_MAX_USERS; i++) {
        if (s->users[i].used && strncmp(s->users[i].user, user, sizeof(s->users[i].user)) == 0) { slot = i; break; }
        if (slot < 0 && !s->users[i].used) slot = i;
    }
    if (slot < 0) return false;
    auth_user_t *u = &s->users[slot];
    memset(u, 0, sizeof(*u));
    strncpy(u->user, user, sizeof(u->user) - 1);
    s->rng(u->salt, sizeof(u->salt));
    s->hash(password, u->salt, sizeof(u->salt), s->iters, u->hash);
    u->role = role; u->used = true;
    return true;
}

role_t auth_verify(auth_store_t *s, const char *user, const char *password)
{
    for (int i = 0; i < AUTH_MAX_USERS; i++) {
        auth_user_t *u = &s->users[i];
        if (!u->used || strncmp(u->user, user, sizeof(u->user)) != 0) continue;
        uint8_t h[32];
        s->hash(password, u->salt, sizeof(u->salt), s->iters, h);
        return ct_memcmp(h, u->hash, 32) == 0 ? u->role : ROLE_NONE;
    }
    return ROLE_NONE;
}

bool auth_locked(const auth_store_t *s, uint32_t now_ms)
{
    return s->lock_until_ms != 0 && (int32_t)(now_ms - s->lock_until_ms) < 0;
}
void auth_note_fail(auth_store_t *s, uint32_t now_ms)
{
    s->fail_count++;
    if (s->fail_count >= AUTH_LOCK_THRESHOLD) {
        uint32_t mult = s->fail_count - AUTH_LOCK_THRESHOLD + 1;   /* exponential-ish backoff */
        s->lock_until_ms = now_ms + AUTH_LOCK_MS * mult;
    }
}
void auth_note_success(auth_store_t *s) { s->fail_count = 0; s->lock_until_ms = 0; }

auth_session_t *auth_session_create(auth_store_t *s, role_t role, uint32_t now_ms, uint32_t ttl_ms)
{
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        auth_session_t *e = &s->sess[i];
        if (e->used && (int32_t)(now_ms - e->expires_ms) >= 0) e->used = false;  /* reap expired */
        if (!e->used) {
            memset(e, 0, sizeof(*e));
            hex32(s->rng, e->token); hex32(s->rng, e->csrf);
            e->role = role; e->expires_ms = now_ms + ttl_ms; e->used = true;
            return e;
        }
    }
    return NULL;
}

auth_session_t *auth_session_lookup(auth_store_t *s, const char *token, uint32_t now_ms)
{
    if (!token || strlen(token) != 32) return NULL;
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        auth_session_t *e = &s->sess[i];
        if (!e->used) continue;
        if ((int32_t)(now_ms - e->expires_ms) >= 0) { e->used = false; continue; }
        if (ct_memcmp(e->token, token, 32) == 0) return e;
    }
    return NULL;
}

bool auth_csrf_ok(const auth_session_t *sess, const char *csrf)
{
    return sess && csrf && strlen(csrf) == 32 && ct_memcmp(sess->csrf, csrf, 32) == 0;
}

void auth_session_destroy(auth_store_t *s, const char *token)
{
    if (!token) return;
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++)
        if (s->sess[i].used && ct_memcmp(s->sess[i].token, token, 32) == 0) s->sess[i].used = false;
}
