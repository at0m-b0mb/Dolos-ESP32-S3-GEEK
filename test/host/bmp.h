/* Minimal 24-bit BMP writer for host-side canvas previews (upscaled). */
#ifndef AEGIS_BMP_H
#define AEGIS_BMP_H
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "canvas.h"

static inline void bmp_write(const char *path, const canvas_t *cv, int scale)
{
    int W = cv->w * scale, H = cv->h * scale;
    int rowbytes = (W * 3 + 3) & ~3;
    uint32_t imgsize = (uint32_t)rowbytes * H;
    uint32_t filesize = 54 + imgsize;
    FILE *fp = fopen(path, "wb");
    if (!fp) return;
    uint8_t hdr[54] = {0};
    hdr[0]='B'; hdr[1]='M';
    hdr[2]=filesize; hdr[3]=filesize>>8; hdr[4]=filesize>>16; hdr[5]=filesize>>24;
    hdr[10]=54;
    hdr[14]=40; hdr[18]=W; hdr[19]=W>>8; hdr[20]=W>>16; hdr[21]=W>>24;
    hdr[22]=H; hdr[23]=H>>8; hdr[24]=H>>16; hdr[25]=H>>24;
    hdr[26]=1; hdr[28]=24;
    hdr[34]=imgsize; hdr[35]=imgsize>>8; hdr[36]=imgsize>>16; hdr[37]=imgsize>>24;
    fwrite(hdr, 1, 54, fp);
    uint8_t *row = (uint8_t*)calloc(1, rowbytes);
    for (int y = H - 1; y >= 0; y--) {          /* BMP is bottom-up */
        int sy = y / scale;
        for (int x = 0; x < W; x++) {
            uint16_t p = cv->px[sy * cv->w + (x / scale)];
            uint8_t r = ((p >> 11) & 0x1F) << 3;
            uint8_t g = ((p >> 5) & 0x3F) << 2;
            uint8_t b = (p & 0x1F) << 3;
            row[x*3+0]=b; row[x*3+1]=g; row[x*3+2]=r;
        }
        fwrite(row, 1, rowbytes, fp);
    }
    free(row);
    fclose(fp);
}
#endif
