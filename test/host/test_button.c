#include "dolos_test.h"
#include "button.h"

#define HOLD 1200u
#define DBL   320u

/* Drive the recognizer over a timeline, collecting what it reports.
 * `press` is sampled every `step` ms, the way the UI task polls the pin. */
typedef struct { btn_evt_t ev[8]; int n; } evlog_t;
static void feed_span(btn_state_t *b, evlog_t *l, bool pressed, uint32_t from,
                      uint32_t to, uint32_t step)
{
    for (uint32_t t = from; t < to; t += step) {
        btn_evt_t e = button_feed(b, pressed, t);
        if (e != BTN_NONE && l->n < 8) l->ev[l->n++] = e;
    }
}

TEST_MAIN_BEGIN
    SUITE("button: a short press is a TAP, reported after the double window");
    {
        btn_state_t b; button_init(&b, HOLD, DBL);
        evlog_t l = {0};
        feed_span(&b, &l, true,   0,  80, 10);     /* 80 ms press   */
        feed_span(&b, &l, false, 80, 200, 10);     /* released      */
        CHECK(l.n == 0, "no event yet - still waiting for a possible second tap");
        feed_span(&b, &l, false, 200, 600, 10);
        CHECK(l.n == 1 && l.ev[0] == BTN_TAP, "a single TAP arrives once the window closes");
    }

    SUITE("button: two quick presses are ONE double-tap, never two taps");
    {
        btn_state_t b; button_init(&b, HOLD, DBL);
        evlog_t l = {0};
        feed_span(&b, &l, true,    0,  60, 10);
        feed_span(&b, &l, false,  60, 140, 10);
        feed_span(&b, &l, true,  140, 200, 10);    /* second press inside 320 ms */
        feed_span(&b, &l, false, 200, 800, 10);
        CHECK(l.n == 1, "exactly one event, got %d", l.n);
        CHECK(l.n && l.ev[0] == BTN_DOUBLE, "and it is a DOUBLE");
    }

    SUITE("button: two SLOW presses are two separate taps");
    {
        btn_state_t b; button_init(&b, HOLD, DBL);
        evlog_t l = {0};
        feed_span(&b, &l, true,    0,  60, 10);
        feed_span(&b, &l, false,  60, 600, 10);    /* well past the window */
        feed_span(&b, &l, true,  600, 660, 10);
        feed_span(&b, &l, false, 660, 1200, 10);
        CHECK(l.n == 2, "two events, got %d", l.n);
        CHECK(l.n == 2 && l.ev[0] == BTN_TAP && l.ev[1] == BTN_TAP, "both are taps");
    }

    SUITE("button: a long press is a HOLD, fired while still held");
    {
        btn_state_t b; button_init(&b, HOLD, DBL);
        evlog_t l = {0};
        feed_span(&b, &l, true, 0, 1100, 10);
        CHECK(l.n == 0, "nothing before the hold threshold");
        feed_span(&b, &l, true, 1100, 1400, 10);
        CHECK(l.n == 1 && l.ev[0] == BTN_HOLD, "HOLD fires at the threshold, still pressed");
        feed_span(&b, &l, false, 1400, 2200, 10);
        CHECK(l.n == 1, "releasing after a hold adds no tap, got %d events", l.n);
    }

    SUITE("button: a hold is never mistaken for part of a double-tap");
    {
        btn_state_t b; button_init(&b, HOLD, DBL);
        evlog_t l = {0};
        feed_span(&b, &l, true,    0,  60, 10);    /* tap ...            */
        feed_span(&b, &l, false,  60, 140, 10);
        feed_span(&b, &l, true,  140, 1500, 10);   /* ... then a hold     */
        feed_span(&b, &l, false, 1500, 2200, 10);
        CHECK(l.n == 2, "two events, got %d", l.n);
        CHECK(l.n == 2 && l.ev[0] == BTN_DOUBLE && l.ev[1] == BTN_HOLD,
              "the second press reports DOUBLE, then HOLD as it is kept down");
    }

    SUITE("button: a very long hold fires exactly once");
    {
        btn_state_t b; button_init(&b, HOLD, DBL);
        evlog_t l = {0};
        feed_span(&b, &l, true, 0, 9000, 10);
        CHECK(l.n == 1, "one HOLD for a 9-second press, got %d", l.n);
    }

    SUITE("button: noisy sampling and long gaps do not desync it");
    {
        btn_state_t b; button_init(&b, HOLD, DBL);
        evlog_t l = {0};
        feed_span(&b, &l, false, 0, 5000, 50);     /* idle: nothing ever fires */
        CHECK(l.n == 0, "idle produces no events");
        /* a press sampled coarsely (50 ms UI tick) still holds */
        feed_span(&b, &l, true, 5000, 6400, 50);
        CHECK(l.n == 1 && l.ev[0] == BTN_HOLD, "coarse sampling still detects HOLD");
    }
TEST_MAIN_END
