#ifndef AEGIS_FONT7X12_HDR
#define AEGIS_FONT7X12_HDR
#include <stdint.h>
#define FONT7X12_W     7
#define FONT7X12_H     12
#define FONT7X12_FIRST 0x20
#define FONT7X12_LAST  0x5F
#define FONT7X12_COUNT (FONT7X12_LAST - FONT7X12_FIRST + 1)
/* 12 rows per glyph, MSB-first: column c is set when row & (0x80 >> c), c<7. */
extern const uint8_t aegis_font7x12[FONT7X12_COUNT * FONT7X12_H];
#endif
