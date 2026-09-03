#include "dolos_test.h"
#include "auth.h"
#include <string.h>

/* deterministic stubs so the LOGIC is tested without mbedTLS */
int g_hash_calls;
static void mock_hash(const char *pw, const uint8_t *salt, size_t sl, uint32_t iters, uint8_t out[32])
{
    g_hash_calls++;
    (void)iters;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < sl; i++) { h ^= salt[i]; h *= 16777619u; }
    for (const char *p = pw; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    for (int i = 0; i < 32; i++) { h ^= h >> 13; h *= 16777619u; out[i] = (uint8_t)(h >> ((i % 4) * 8)); }
}
static uint32_t g_rng = 12345;
static void mock_rng(uint8_t *buf, size_t n)
{
    for (size_t i = 0; i < n; i++) { g_rng = g_rng * 1664525u + 1013904223u; buf[i] = (uint8_t)(g_rng >> 24); }
}

TEST_MAIN_BEGIN
    SUITE("rbac: roles gate the right permissions");
    {
        CHECK(rbac_allows(ROLE_VIEWER, PERM_VIEW), "viewer can view");
        CHECK(!rbac_allows(ROLE_VIEWER, PERM_RUN), "viewer cannot run");
        CHECK(rbac_allows(ROLE_OPERATOR, PERM_RUN), "operator can run");
        CHECK(!rbac_allows(ROLE_OPERATOR, PERM_EDIT_CONFIG), "operator cannot edit config");
        CHECK(rbac_allows(ROLE_ADMIN, PERM_EDIT_CONFIG), "admin can edit config");
        CHECK(rbac_allows(ROLE_ADMIN, PERM_TOGGLE_REMOTE_FIRE), "only admin toggles remote fire");
        CHECK(!rbac_allows(ROLE_OPERATOR, PERM_TOGGLE_REMOTE_FIRE), "operator cannot toggle remote fire");
        CHECK(!rbac_allows(ROLE_NONE, PERM_VIEW), "no role -> nothing");
    }

    auth_store_t s; auth_init(&s, mock_hash, mock_rng, 1000);

    SUITE("auth: credential verify is salted + constant-time");
    {
        CHECK(auth_set_user(&s, "root", "s3cr3t!", ROLE_ADMIN), "add admin");
        CHECK(auth_verify(&s, "root", "s3cr3t!") == ROLE_ADMIN, "correct password -> admin");
        CHECK(auth_verify(&s, "root", "wrong") == ROLE_NONE, "wrong password -> none");
        CHECK(auth_verify(&s, "ghost", "s3cr3t!") == ROLE_NONE, "unknown user -> none");
    }

    SUITE("auth: lockout after repeated failures, then recovery");
    {
        auth_store_t l; auth_init(&l, mock_hash, mock_rng, 1000);
        for (int i = 0; i < AUTH_LOCK_THRESHOLD; i++) { CHECK(!auth_locked(&l, 1000), "not locked yet"); auth_note_fail(&l, 1000); }
        CHECK(auth_locked(&l, 1000), "locked after threshold");
        CHECK(!auth_locked(&l, 1000 + AUTH_LOCK_MS + 1), "unlocks after the window");
        auth_note_success(&l);
        CHECK(!auth_locked(&l, 1000), "success resets the lockout");
    }

    SUITE("auth: sessions expire, tokens are opaque, CSRF is checked");
    {
        auth_session_t *a = auth_session_create(&s, ROLE_ADMIN, 0, 1000);
        CHECK(a && strlen(a->token) == 32, "session token is 32 hex chars");
        char tok[33]; strcpy(tok, a->token);
        char csrf[33]; strcpy(csrf, a->csrf);
        CHECK(auth_session_lookup(&s, tok, 500) != NULL, "valid token within ttl resolves");
        CHECK(auth_session_lookup(&s, tok, 2000) == NULL, "token past ttl is rejected");
        /* recreate (previous expired) and test csrf + destroy */
        auth_session_t *b = auth_session_create(&s, ROLE_OPERATOR, 3000, 1000);
        CHECK(auth_csrf_ok(b, b->csrf), "matching CSRF passes");
        CHECK(!auth_csrf_ok(b, "00000000000000000000000000000000"), "wrong CSRF fails");
        char btok[33]; strcpy(btok, b->token);
        auth_session_destroy(&s, btok);
        CHECK(auth_session_lookup(&s, btok, 3200) == NULL, "destroyed session is gone");
        CHECK(strcmp(tok, btok) != 0, "tokens differ between sessions");
    }

    SUITE("auth: constant-time compare correctness");
    {
        CHECK(ct_memcmp("abcd", "abcd", 4) == 0, "equal -> 0");
        CHECK(ct_memcmp("abcd", "abce", 4) != 0, "differ -> nonzero");
    }

    SUITE("auth: a session reports its remaining time and can be extended");
    {
        auth_store_t e2; auth_init(&e2, mock_hash, mock_rng, 1000);
        auth_session_t *x = auth_session_create(&e2, ROLE_ADMIN, 1000, 60000);
        CHECK(auth_session_remaining_ms(x, 1000) == 60000, "full ttl at creation");
        CHECK(auth_session_remaining_ms(x, 31000) == 30000, "counts down");
        CHECK(auth_session_remaining_ms(x, 61000) == 0, "zero once expired");
        CHECK(auth_session_remaining_ms(x, 99000) == 0, "stays zero past expiry");

        /* extending restores the full window, and the session resolves again */
        auth_session_extend(x, 50000, 60000);
        CHECK(auth_session_remaining_ms(x, 50000) == 60000, "extend restores the ttl");
        char tok[33]; strcpy(tok, x->token);
        CHECK(auth_session_lookup(&e2, tok, 100000) != NULL,
              "an extended session survives past its original expiry");
    }

    SUITE("auth: multiple accounts, roles, and deleting them safely");
    {
        auth_store_t m; auth_init(&m, mock_hash, mock_rng, 1000);
        CHECK(auth_set_user(&m, "root", "pw-admin", ROLE_ADMIN), "add admin");
        CHECK(auth_set_user(&m, "kim",  "pw-oper",  ROLE_OPERATOR), "add operator");
        CHECK(auth_set_user(&m, "sam",  "pw-view",  ROLE_VIEWER), "add viewer");
        CHECK(auth_user_count(&m) == 3, "three accounts, got %d", auth_user_count(&m));

        /* each authenticates as its own role, and not as another */
        CHECK(auth_verify(&m, "kim", "pw-oper") == ROLE_OPERATOR, "operator signs in");
        CHECK(auth_verify(&m, "kim", "pw-view") == ROLE_NONE, "wrong password refused");
        CHECK(auth_verify(&m, "sam", "pw-view") == ROLE_VIEWER, "viewer signs in");

        /* enumeration */
        const char *n = NULL; role_t r = ROLE_NONE;
        CHECK(auth_user_at(&m, 0, &n, &r) && r == ROLE_ADMIN, "first slot is the admin");
        CHECK(!auth_user_at(&m, 3, &n, &r), "no fourth account");

        /* the last admin cannot be removed */
        CHECK(auth_delete_user(&m, "sam"), "a viewer can be deleted");
        CHECK(auth_user_count(&m) == 2, "two left");
        CHECK(!auth_delete_user(&m, "root"), "the LAST admin must not be deletable");
        CHECK(auth_verify(&m, "root", "pw-admin") == ROLE_ADMIN, "and still works");

        /* with two admins, one may go */
        CHECK(auth_set_user(&m, "alex", "pw-admin2", ROLE_ADMIN), "second admin");
        CHECK(auth_delete_user(&m, "root"), "one of two admins can be deleted");
        CHECK(auth_verify(&m, "root", "pw-admin") == ROLE_NONE, "deleted account cannot sign in");
        CHECK(!auth_delete_user(&m, "nobody"), "deleting an unknown user fails");
    }

    SUITE("auth: a full session table evicts the stalest, it does not refuse");
    {
        /* Signing in AUTH_MAX_SESSIONS times used to fill the table and every
         * later login was refused until one aged out - being locked out of your
         * own device by your own logins. */
        auth_store_t f; auth_init(&f, mock_hash, mock_rng, 1000);
        auth_set_user(&f, "root", "pw", ROLE_ADMIN);
        char first_tok[33] = {0};
        for (int i = 0; i < AUTH_MAX_SESSIONS; i++) {
            auth_session_t *x = auth_session_create(&f, ROLE_ADMIN, 1000 + i, 600000);
            CHECK(x != NULL, "session %d created", i);
            if (i == 0 && x) strcpy(first_tok, x->token);
        }
        auth_session_t *extra = auth_session_create(&f, ROLE_ADMIN, 2000, 600000);
        CHECK(extra != NULL, "one more login still succeeds when the table is full");
        CHECK(auth_session_lookup(&f, first_tok, 2000) == NULL,
              "and it is the OLDEST session that was dropped");
    }

    SUITE("auth: an unknown user costs the same as a wrong password");
    {
        /* Returning early for an unknown name leaked which accounts exist:
         * a real user paid for full PBKDF2, a bogus one returned at once. */
        auth_store_t t2; auth_init(&t2, mock_hash, mock_rng, 1000);
        auth_set_user(&t2, "real", "correct-horse", ROLE_ADMIN);
        g_hash_calls = 0;
        CHECK(auth_verify(&t2, "real", "wrong") == ROLE_NONE, "wrong password refused");
        int calls_known = g_hash_calls;
        g_hash_calls = 0;
        CHECK(auth_verify(&t2, "ghost", "wrong") == ROLE_NONE, "unknown user refused");
        int calls_unknown = g_hash_calls;
        CHECK(calls_known == calls_unknown && calls_known == 1,
              "both paths hash exactly once (known %d, unknown %d)",
              calls_known, calls_unknown);
        CHECK(auth_verify(&t2, "real", "correct-horse") == ROLE_ADMIN, "and the real login still works");
    }

    SUITE("auth: lockout backoff cannot wrap into a shorter lockout");
    {
        auth_store_t b; auth_init(&b, mock_hash, mock_rng, 1000);
        for (int i = 0; i < 200; i++) auth_note_fail(&b, 1000);
        CHECK(auth_locked(&b, 1000), "still locked after many failures");
        CHECK(!auth_locked(&b, 1000 + 31u * 60u * 1000u), "and it does expire eventually");
    }
TEST_MAIN_END
