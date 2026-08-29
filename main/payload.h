/*
 * payload.h - plays a DuckyScript payload over USB HID.
 *
 * Reads a payload from the SD card (/sdcard/PAYLOAD.TXT) if present, else uses a
 * deliberately harmless built-in demo that only types a banner into whatever
 * text field already has focus - it contains no GUI/launch commands. The player
 * checks the abort flag between every keystroke so the operator can stop it.
 */
#ifndef DOLOS_PAYLOAD_H
#define DOLOS_PAYLOAD_H
#include <stdbool.h>
#include "layout.h"

typedef struct {
    volatile bool *abort;                                   /* set to stop mid-run */
    void (*progress)(int cur_line, int total_lines, void *user);
    void *user;
    kb_layout_t layout;         /* target keyboard layout            */
    bool        dry_run;        /* preview only - never send a key    */
    uint32_t    default_delay;  /* ms between commands (config)       */
} payload_ctx_t;

extern const char DOLOS_DEMO_PAYLOAD[];

int  payload_count_lines(const char *text);
/* Load payload text into buf (NUL-terminated). Returns the built-in demo if no
 * SD payload is found. */
const char *payload_load(char *buf, int cap);
/* Play the payload. Blocks (call from a task). Honors *ctx->abort. */
/* Plays the payload; returns the number of script lines executed. */
int  payload_run(const char *text, const payload_ctx_t *ctx);
#endif
