/*
 * lint.h - validate a DuckyScript payload before it is ever allowed to fire.
 *
 * A BadUSB payload is typed blind into someone else's machine: a typo does not
 * throw an error, it types garbage into whatever window has focus. On a real
 * engagement that is the difference between a clean test and an incident. So
 * Dolos parse-checks every payload at load time, shows the first problem on the
 * LCD, and REFUSES TO ARM while a payload has errors.
 *
 * The linter uses the real parser as its oracle (rather than a second copy of
 * the command table, which would drift), plus targeted argument checks the
 * parser is deliberately lenient about at run time.
 *
 * Pure C, no hardware: the whole validator runs in the host tests.
 */
#ifndef DOLOS_LINT_H
#define DOLOS_LINT_H

#include "ducky.h"
#include "layout.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int  line;         /* 1-based line number of the problem */
    char msg[48];      /* short, screen-sized description    */
} ducky_lint_t;

/* Validate `text`. Problems are written to out[] (up to max) and the TOTAL
 * number found is returned - so a caller can report "3 problems, first on
 * line 7" even when it only kept one. 0 means the payload is clean.
 *
 * layout/os matter: a character that cannot be produced on the target layout
 * (and cannot fall back to Unicode) is a real, reportable problem. */
int ducky_lint(const char *text, kb_layout_t layout, target_os_t os,
               ducky_lint_t *out, int max);

#ifdef __cplusplus
}
#endif
#endif /* DOLOS_LINT_H */
