/*
 * sbuf.h - bounded string building.
 *
 * snprintf() returns the length it WOULD have written, not the length it did.
 * The obvious idiom
 *
 *     off += snprintf(buf + off, sizeof(buf) - off, ...);
 *
 * is therefore wrong the moment the buffer fills: `off` runs past the end of
 * the buffer, `buf + off` points outside it, and `sizeof(buf) - off` is size_t
 * arithmetic that UNDERFLOWS to an enormous value. The next call then writes
 * out of bounds with essentially no limit, corrupting whatever follows. It
 * looks fine in testing, because it only triggers once the content grows.
 *
 * sappend() clamps instead: it never advances past the buffer and never passes
 * a bogus size. Truncation is visible via sbuf_truncated().
 */
#ifndef DOLOS_SBUF_H
#define DOLOS_SBUF_H

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct { char *buf; size_t cap; size_t off; bool full; } sbuf_t;

static inline void sbuf_init(sbuf_t *s, char *buf, size_t cap)
{
    s->buf = buf; s->cap = cap; s->off = 0; s->full = false;
    if (cap) buf[0] = 0;
}

static inline void sappend(sbuf_t *s, const char *fmt, ...)
{
    if (!s->cap || s->off + 1 >= s->cap) { s->full = true; return; }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(s->buf + s->off, s->cap - s->off, fmt, ap);
    va_end(ap);
    if (n < 0) { s->full = true; return; }
    if ((size_t)n >= s->cap - s->off) {        /* would not fit: clamp */
        s->off = s->cap - 1;
        s->full = true;
    } else {
        s->off += (size_t)n;
    }
}

static inline bool sbuf_truncated(const sbuf_t *s) { return s->full; }
static inline size_t sbuf_len(const sbuf_t *s)     { return s->off; }

#endif /* DOLOS_SBUF_H */
