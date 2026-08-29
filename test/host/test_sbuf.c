#include "dolos_test.h"
#include "sbuf.h"
#include <string.h>

/* The bug this guards against: `off += snprintf(buf+off, cap-off, ...)` walks
 * off past the end of the buffer once it fills, making (cap-off) underflow and
 * the next write unbounded. These run under ASan in the fuzz target, so an
 * escape is a hard failure rather than a silent corruption. */
TEST_MAIN_BEGIN
    SUITE("sbuf: never writes past the end, however much you append");
    {
        char b[32];
        char guard[16];
        memset(guard, 0xAA, sizeof(guard));
        sbuf_t s; sbuf_init(&s, b, sizeof(b));
        for (int i = 0; i < 200; i++) sappend(&s, "0123456789");
        CHECK(sbuf_truncated(&s), "it reports that it truncated");
        CHECK(strlen(b) < sizeof(b), "stays inside the buffer: len=%u", (unsigned)strlen(b));
        CHECK(sbuf_len(&s) <= sizeof(b) - 1, "offset never exceeds the capacity");
        char expect[16]; memset(expect, 0xAA, sizeof(expect));
        CHECK(memcmp(guard, expect, sizeof(guard)) == 0, "nothing after the buffer was touched");
    }

    SUITE("sbuf: exact fits and off-by-one boundaries");
    {
        char b[8];
        sbuf_t s; sbuf_init(&s, b, sizeof(b));
        sappend(&s, "1234567");                       /* 7 chars + NUL = exactly 8 */
        CHECK(strcmp(b, "1234567") == 0, "an exact fit is kept whole, got '%s'", b);
        CHECK(!sbuf_truncated(&s), "an exact fit is not truncation");
        sappend(&s, "x");
        CHECK(sbuf_truncated(&s), "one more character truncates");
        CHECK(strcmp(b, "1234567") == 0, "and does not corrupt what was there");
    }

    SUITE("sbuf: builds normal content correctly");
    {
        char b[64];
        sbuf_t s; sbuf_init(&s, b, sizeof(b));
        sappend(&s, "{\"a\":[");
        for (int i = 0; i < 3; i++) sappend(&s, "%s%d", i ? "," : "", i);
        sappend(&s, "]}");
        CHECK(strcmp(b, "{\"a\":[0,1,2]}") == 0, "assembled '%s'", b);
        CHECK(!sbuf_truncated(&s), "no truncation for content that fits");
    }

    SUITE("sbuf: a zero-capacity buffer is not written to at all");
    {
        char b[1] = { 0x7F };
        sbuf_t s; sbuf_init(&s, b, 0);
        sappend(&s, "hello");
        CHECK(b[0] == 0x7F, "untouched");
        CHECK(sbuf_truncated(&s), "and reports truncation");
    }
TEST_MAIN_END
