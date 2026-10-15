/* Windows .cur decoding to RGBA (used for the game's hardware cursors). */
#include "runtime.h"
#include <stdlib.h>

#define RD16(p) (*(u16u *)(p))
#define RD32(p) (*(u32u *)(p))

/* Returns malloc'd RGBA (w*h*4) or NULL. */
uint32_t *cursor_decode(const char *host_path, int *w, int *h, int *hx, int *hy) {
    int fd = plat_open(host_path, 0, 0, 0);
    if (fd < 0) return NULL;
    int64_t n = plat_size(fd);
    if (n < 22 || n > (1 << 20)) { plat_close(fd); return NULL; }
    uint8_t *d = malloc((size_t)n);
    plat_read(fd, d, (uint32_t)n);
    plat_close(fd);
    int count = RD16(d + 4), best = -1, bestbpp = -1;
    for (int i = 0; i < count && 6 + 16 * i + 16 <= n; i++) {
        uint32_t off = RD32(d + 6 + 16 * i + 12);
        if (off + 40 > n) continue;
        int bpp = RD16(d + off + 14);
        if (bpp > bestbpp) { bestbpp = bpp; best = i; }
    }
    if (best < 0) { free(d); return NULL; }
    const uint8_t *e = d + 6 + 16 * best;
    *hx = RD16(e + 4); *hy = RD16(e + 6);
    const uint8_t *bi = d + RD32(e + 12);
    int W = (int32_t)RD32(bi + 4), H = (int32_t)RD32(bi + 8) / 2, bpp = RD16(bi + 14);
    uint32_t hs = RD32(bi), ncol = RD32(bi + 32);
    if (!ncol && bpp <= 8) ncol = 1u << bpp;
    const uint8_t *pal = bi + hs;
    const uint8_t *xorp = pal + 4 * (bpp <= 8 ? ncol : 0);
    int xstride = ((W * bpp + 31) / 32) * 4, astride = ((W + 31) / 32) * 4;
    const uint8_t *andp = xorp + xstride * H;
    if (andp + astride * H > d + n || W <= 0 || H <= 0 || W > 256 || H > 256) { free(d); return NULL; }
    uint32_t *out = malloc((size_t)W * H * 4);
    for (int y = 0; y < H; y++) {
        const uint8_t *row = xorp + (H - 1 - y) * xstride, *arow = andp + (H - 1 - y) * astride;
        for (int x = 0; x < W; x++) {
            uint32_t r, g, b, a = 255;
            if (bpp == 32) { b = row[4 * x]; g = row[4 * x + 1]; r = row[4 * x + 2]; a = row[4 * x + 3]; }
            else if (bpp == 24) { b = row[3 * x]; g = row[3 * x + 1]; r = row[3 * x + 2]; }
            else {
                int idx = bpp == 8 ? row[x] : bpp == 4 ? (row[x / 2] >> (x & 1 ? 0 : 4)) & 15 : (row[x / 8] >> (7 - (x & 7))) & 1;
                b = pal[4 * idx]; g = pal[4 * idx + 1]; r = pal[4 * idx + 2];
            }
            int mask = (arow[x / 8] >> (7 - (x & 7))) & 1;
            if (bpp != 32 && mask) a = 0;   /* transparent (inverted pixels are rare; treat as transparent) */
            out[y * W + x] = r | (g << 8) | (b << 16) | (a << 24);
        }
    }
    free(d);
    *w = W; *h = H;
    return out;
}
