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
    /* An unknown username used to return immediately, while a known one paid
     * for 20,000 rounds of PBKDF2. The difference is trivially measurable over
     * HTTP, so anyone could enumerate valid account names without ever guessing
     * a password. Hash every time - against a decoy salt when there is no such
     * user - so a wrong name and a wrong password cost the same. */
    const auth_user_t *found = NULL;
    for (int i = 0; i < AUTH_MAX_USERS; i++) {
        const auth_user_t *u = &s->users[i];
        if (u->used && strncmp(u->user, user, sizeof(u->user)) == 0) { found = u; break; }
    }

    static const uint8_t decoy_salt[16] = {
        0x5b,0x1e,0xa7,0x30,0xc4,0x92,0x6d,0xf1,
        0x08,0xbb,0x47,0x2c,0x9e,0x53,0xd0,0x86
    };
    uint8_t h[32];
    s->hash(password,
            found ? found->salt : decoy_salt,
            found ? sizeof(found->salt) : sizeof(decoy_salt),
            s->iters, h);

    if (!found) return ROLE_NONE;
    return ct_memcmp(h, found->hash, 32) == 0 ? found->role : ROLE_NONE;
}

bool auth_user_at(const auth_store_t *s, int i, const char **name, role_t *role)
{
    if (i < 0 || i >= AUTH_MAX_USERS || !s->users[i].used) return false;
    if (name) *name = s->users[i].user;
    if (role) *role = s->users[i].role;
    return true;
}

int auth_user_count(const auth_store_t *s)
{
    int n = 0;
    for (int i = 0; i < AUTH_MAX_USERS; i++) if (s->users[i].used) n++;
    return n;
}

bool auth_delete_user(auth_store_t *s, const char *user)
{
    int idx = -1, admins = 0;
    for (int i = 0; i < AUTH_MAX_USERS; i++) {
        if (!s->users[i].used) continue;
        if (s->users[i].role == ROLE_ADMIN) admins++;
        if (strncmp(s->users[i].user, user, sizeof(s->users[i].user)) == 0) idx = i;
    }
    if (idx < 0) return false;
    /* Deleting the last admin would lock everyone out of their own device with
     * no way back except a factory reset, so it is simply not allowed. */
    if (s->users[idx].role == ROLE_ADMIN && admins <= 1) return false;
    memset(&s->users[idx], 0, sizeof(s->users[idx]));
    return true;
}

bool auth_locked(const auth_store_t *s, uint32_t now_ms)
{
    return s->lock_until_ms != 0 && (int32_t)(now_ms - s->lock_until_ms) < 0;
}
void auth_note_fail(auth_store_t *s, uint32_t now_ms)
{
    s->fail_count++;
    if (s->fail_count >= AUTH_LOCK_THRESHOLD) {
        /* Clamped: unbounded growth eventually overflows the multiply and wraps
         * to a tiny lockout, which would turn the brute-force defence into a
         * brute-force ENABLER for anyone patient enough to get there. */
        uint32_t mult = s->fail_count - AUTH_LOCK_THRESHOLD + 1;
        if (mult > 60) mult = 60;                    /* caps at 30 minutes */
        s->lock_until_ms = now_ms + AUTH_LOCK_MS * mult;
        if (s->fail_count > 100000u) s->fail_count = 100000u;   /* no wrap either */
    }
}
void auth_note_success(auth_store_t *s) { s->fail_count = 0; s->lock_until_ms = 0; }

auth_session_t *auth_session_create(auth_store_t *s, role_t role, uint32_t now_ms, uint32_t ttl_ms)
{
    auth_session_t *slot = NULL;

    /* First pass: reap anything expired and take the first free slot. */
    for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
        auth_session_t *e = &s->sess[i];
        if (e->used && (int32_t)(now_ms - e->expires_ms) >= 0) e->used = false;
        if (!e->used && !slot) slot = e;
    }

    /* All four still live? Evict the one closest to expiry rather than refusing.
     *
     * Signing in four times - which testing, a browser refresh, or a second
     * device does in a minute - filled the table, and every later login was
     * turned away with "service unavailable" until the oldest aged out half an
     * hour later. Being locked out of your own device by your own logins is a
     * worse failure than dropping the stalest session. */
    if (!slot) {
        slot = &s->sess[0];
        for (int i = 1; i < AUTH_MAX_SESSIONS; i++)
            if ((int32_t)(s->sess[i].expires_ms - slot->expires_ms) < 0) slot = &s->sess[i];
    }

    memset(slot, 0, sizeof(*slot));
    hex32(s->rng, slot->token); hex32(s->rng, slot->csrf);
    slot->role = role; slot->expires_ms = now_ms + ttl_ms; slot->used = true;
    return slot;
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

uint32_t auth_session_remaining_ms(const auth_session_t *sess, uint32_t now_ms)
{
    if (!sess || !sess->used) return 0;
    if ((int32_t)(now_ms - sess->expires_ms) >= 0) return 0;
    return sess->expires_ms - now_ms;
}

void auth_session_extend(auth_session_t *sess, uint32_t now_ms, uint32_t ttl_ms)
{
    if (sess && sess->used) sess->expires_ms = now_ms + ttl_ms;
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
