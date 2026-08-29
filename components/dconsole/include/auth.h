/*
 * auth.h - the security core for the Dolos wireless console. Pure C, no ESP-IDF,
 * host-testable. Roles + RBAC, salted-hashed credentials, opaque session tokens
 * with CSRF, failed-login lockout, and constant-time comparison.
 *
 * The password hash and the RNG are injected (auth_hash_fn / auth_rng_fn) so the
 * logic is exercised on a laptop with a deterministic stub, while the device
 * plugs in PBKDF2-HMAC-SHA256 (mbedTLS) + the hardware RNG.
 */
#ifndef DOLOS_AUTH_H
#define DOLOS_AUTH_H
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { ROLE_NONE = 0, ROLE_VIEWER, ROLE_OPERATOR, ROLE_ADMIN } role_t;

/* Fine-grained permissions, mapped to roles by rbac_allows(). */
typedef enum {
    PERM_VIEW = 0,          /* status dashboard        (viewer+)   */
    PERM_AUDIT,             /* read the audit log      (viewer+)   */
    PERM_RUN,               /* start the selected payload (operator+) */
    PERM_EDIT_PAYLOAD,      /* upload/edit payloads    (admin)     */
    PERM_EDIT_CONFIG,       /* change config/layout... (admin)     */
    PERM_MANAGE_WIFI,       /* wifi + identity         (admin)     */
    PERM_TOGGLE_REMOTE_FIRE,/* enable/disable remote fire (admin)  */
    PERM__COUNT
} perm_t;

bool        rbac_allows(role_t role, perm_t perm);
const char *role_name(role_t r);

typedef void (*auth_hash_fn)(const char *pw, const uint8_t *salt, size_t saltlen,
                             uint32_t iters, uint8_t out[32]);
typedef void (*auth_rng_fn)(uint8_t *buf, size_t n);

#define AUTH_MAX_USERS      4
#define AUTH_MAX_SESSIONS   4
#define AUTH_LOCK_THRESHOLD 5      /* failed logins before lockout kicks in   */
#define AUTH_LOCK_MS        30000u /* base lockout window                     */

typedef struct { char user[24]; uint8_t salt[16]; uint8_t hash[32]; role_t role; bool used; } auth_user_t;
typedef struct { char token[33]; char csrf[33]; role_t role; uint32_t expires_ms; bool used; } auth_session_t;

typedef struct {
    auth_user_t    users[AUTH_MAX_USERS];
    auth_session_t sess[AUTH_MAX_SESSIONS];
    uint32_t       iters;
    auth_hash_fn   hash;
    auth_rng_fn    rng;
    uint32_t       fail_count;
    uint32_t       lock_until_ms;
} auth_store_t;

void   auth_init(auth_store_t *s, auth_hash_fn hash, auth_rng_fn rng, uint32_t iters);
bool   auth_set_user(auth_store_t *s, const char *user, const char *password, role_t role);
role_t auth_verify(auth_store_t *s, const char *user, const char *password);

bool   auth_locked(const auth_store_t *s, uint32_t now_ms);
void   auth_note_fail(auth_store_t *s, uint32_t now_ms);
void   auth_note_success(auth_store_t *s);

auth_session_t *auth_session_create(auth_store_t *s, role_t role, uint32_t now_ms, uint32_t ttl_ms);
auth_session_t *auth_session_lookup(auth_store_t *s, const char *token, uint32_t now_ms);
bool   auth_csrf_ok(const auth_session_t *sess, const char *csrf);
void   auth_session_destroy(auth_store_t *s, const char *token);

/* Constant-time compare: returns 0 iff equal (no early-out timing leak). */
int    ct_memcmp(const void *a, const void *b, size_t n);

#ifdef __cplusplus
}
#endif
#endif /* DOLOS_AUTH_H */
