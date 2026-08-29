#ifndef AEGIS_FONT5X8_HDR
#define AEGIS_FONT5X8_HDR
#include <stdint.h>
#define FONT5X8_W     5
#define FONT5X8_H     8
#define FONT5X8_FIRST 0x20
#define FONT5X8_LAST  0x7E
#define FONT5X8_COUNT 95
/* 8 rows per glyph, MSB-first: column c set when row & (0x80 >> c), c in 0..4 */
extern const uint8_t aegis_font5x8[FONT5X8_COUNT * FONT5X8_H];
#endif
