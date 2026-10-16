/* DirectDraw 7 emulation: software surfaces in guest memory, presented through
 * the platform layer as RGBA. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>

#define DD_OK 0
#define DDERR_GENERIC 0x80004005u
#define DDERR_UNSUPPORTED 0x80004001u
#define DDERR_INVALIDPARAMS 0x80070057u
#define DDERR_NOTFOUND 0x887600ffu
#define E_NOINTERFACE 0x80004002u
#define DDENUMRET_OK 1

void user32_set_size(int w, int h);

typedef struct {
    uint32_t obj;
    int w, h, pitch, bpp;
    uint32_t pixels;
    uint32_t caps;
    uint32_t ck_lo, ck_hi; int has_ck;
    int back;          /* index of attached back buffer or -1 */
    int palette;       /* palette index or -1 */
    int primary;
    int refs;
} Surf;

#define MAXS 2048
static Surf surfs[MAXS];
static int nsurfs;
static int mode_w = 800, mode_h = 600, mode_bpp = 16;
static int primary = -1;
static int dirty;
static uint32_t last_present;

typedef struct { uint32_t obj; uint32_t entries[256]; } Pal;
static Pal pals[16];
static int npals;

static uint32_t vt_dd7, vt_dds7, vt_pal, vt_clip, dd_obj;

static Surf *S(uint32_t obj) {
    int i = (int)M32(obj + 4);
    if (i < 0 || i >= nsurfs || surfs[i].obj != obj) fatal("bad surface object %08x", obj);
    return &surfs[i];
}

/* ------------------------------------------------------------- present */
static uint32_t *rgba;
static int rgba_cap;

static void present_surface(Surf *s) {
    int w = s->w, h = s->h;
    if (rgba_cap < w * h) { free(rgba); rgba = malloc(sizeof(uint32_t) * w * h); rgba_cap = w * h; }
    if (s->bpp == 16) {
        /* 16-bit (the game's usual mode) goes out as it is: the display converts 5:6:5 on the GPU */
        const void *src = PTR(s->pixels);
        if (s->pitch != w * 2) {
            for (int y = 0; y < h; y++) memcpy((uint16_t *)rgba + y * w, PTR(s->pixels + y * s->pitch), (size_t)w * 2);
            src = rgba;
        }
        plat_present(src, w, h, PLAT_RGB565);
        last_present = plat_ticks_ms();
        dirty = 0;
        return;
    } else if (s->bpp == 8) {
        const uint32_t *pal = s->palette >= 0 ? pals[s->palette].entries : pals[0].entries;
        for (int y = 0; y < h; y++) {
            const uint8_t *src = (const uint8_t *)PTR(s->pixels + y * s->pitch);
            uint32_t *d = rgba + y * w;
            for (int x = 0; x < w; x++) d[x] = pal[src[x]] | 0xff000000u;
        }
    } else if (s->bpp == 32) {
        for (int y = 0; y < h; y++) {
            const uint32_t *src = (const uint32_t *)PTR(s->pixels + y * s->pitch);
            uint32_t *d = rgba + y * w;
            for (int x = 0; x < w; x++) { uint32_t p = src[x]; d[x] = 0xff000000u | ((p & 0xff) << 16) | (p & 0xff00) | ((p >> 16) & 0xff); }
        }
    }
    plat_present(rgba, w, h, PLAT_RGBA);
    last_present = plat_ticks_ms();
    dirty = 0;
}

void ddraw_present_if_dirty(void) {
    if (primary >= 0 && dirty && plat_ticks_ms() - last_present >= 15) present_surface(&surfs[primary]);
}

/* ------------------------------------------------------------- surfaces */
static int new_surface(int w, int h, int bpp, uint32_t caps) {
    int i;
    for (i = 0; i < nsurfs; i++) if (surfs[i].obj == 0) break;
    if (i == nsurfs) {
        if (nsurfs >= MAXS) fatal("too many surfaces");
        nsurfs++;
    }
    Surf *s = &surfs[i];
    memset(s, 0, sizeof *s);
    s->w = w; s->h = h; s->bpp = bpp;
    s->pitch = ((w * bpp / 8) + 3) & ~3;
    s->pixels = heap_alloc((uint32_t)(s->pitch * h) + 16, 1);
    if (!s->pixels) fatal("out of memory for %dx%d surface", w, h);
    s->caps = caps; s->back = -1; s->palette = -1; s->refs = 1;
    s->obj = guest_alloc_static(16);
    M32(s->obj) = vt_dds7;
    M32(s->obj + 4) = (uint32_t)i;
    return i;
}

/* offscreen surface in the display format, for the video player (video.c) */
uint32_t ddraw_make_surface(int w, int h, uint32_t *pixels, int *pitch, int *bpp) {
    int i = new_surface(w, h, mode_bpp, 0x40 | 0x800);   /* DDSCAPS_OFFSCREENPLAIN | SYSTEMMEMORY */
    *pixels = surfs[i].pixels; *pitch = surfs[i].pitch; *bpp = surfs[i].bpp;
    return surfs[i].obj;
}

static void fill_pixfmt(uint32_t pf, int bpp) {
    memset(PTR(pf), 0, 32);
    M32(pf) = 32;
    if (bpp == 8) { M32(pf + 4) = 0x60; M32(pf + 12) = 8; }      /* DDPF_RGB | DDPF_PALETTEINDEXED8 */
    else if (bpp == 16) { M32(pf + 4) = 0x40; M32(pf + 12) = 16; M32(pf + 16) = 0xf800; M32(pf + 20) = 0x07e0; M32(pf + 24) = 0x001f; }
    else { M32(pf + 4) = 0x40; M32(pf + 12) = 32; M32(pf + 16) = 0xff0000; M32(pf + 20) = 0xff00; M32(pf + 24) = 0xff; }
}

static void fill_desc(uint32_t d, Surf *s) {
    uint32_t size = M32(d);
    memset(PTR(d), 0, size >= 108 ? size : 108);
    M32(d) = size ? size : 124;
    M32(d + 4) = 0x100f | 0x800;   /* CAPS|HEIGHT|WIDTH|PITCH|PIXELFORMAT|LPSURFACE */
    M32(d + 8) = (uint32_t)s->h; M32(d + 12) = (uint32_t)s->w; M32(d + 16) = (uint32_t)s->pitch;
    M32(d + 36) = s->pixels;
    if (s->has_ck) { M32(d + 4) |= 0x10000; M32(d + 64) = s->ck_lo; M32(d + 68) = s->ck_hi; }
    fill_pixfmt(d + 72, s->bpp);
    M32(d + 104) = s->caps;
}

/* ---- IDirectDraw7 ---- */
static void dd_QueryInterface(void) {
    uint32_t iid = ARG(1), ppv = ARG(2);
    LOG("IDirectDraw::QueryInterface %08x\n", M32(iid));
    extern uint32_t dd_for_iid(uint32_t iid);
    M32(ppv) = dd_for_iid(iid);
    RET_STD(3, DD_OK);
}
static void dd_AddRef(void) { RET_STD(1, 1); }
static void dd_Release(void) { RET_STD(1, 0); }
static void dd_Compact(void) { RET_STD(1, DD_OK); }
static void dd_CreateClipper(void) {
    uint32_t o = com_new(vt_clip, 4);
    M32(ARG(2)) = o;
    RET_STD(4, DD_OK);
}
static void dd_CreatePalette(void) {
    uint32_t flags = ARG(1), entries = ARG(2), pp = ARG(3);
    if (npals >= 16) fatal("too many palettes");
    Pal *p = &pals[npals];
    p->obj = com_new(vt_pal, 4);
    M32(p->obj + 4) = (uint32_t)npals;
    int n = (flags & 4) ? 256 : 16;
    for (int i = 0; i < n; i++) p->entries[i] = M32(entries + 4 * i) & 0xffffff;
    npals++;
    M32(pp) = p->obj;
    RET_STD(5, DD_OK);
}
static void dd_CreateSurface(void) {
    uint32_t d = ARG(1), pps = ARG(2);
    uint32_t flags = M32(d + 4), caps = M32(d + 104);
    int w = (flags & 4) ? (int)M32(d + 12) : mode_w;
    int h = (flags & 2) ? (int)M32(d + 8) : mode_h;
    int bpp = (flags & 0x1000) ? (int)M32(d + 84) : mode_bpp;
    if (caps & 0x200) {   /* primary */
        w = mode_w; h = mode_h; bpp = mode_bpp;
        int i = new_surface(w, h, bpp, caps | 0x4000);
        surfs[i].primary = 1;
        primary = i;
        if ((flags & 0x20) && M32(d + 20) > 0) {
            int b = new_surface(w, h, bpp, 0x4 | 0x4000 | 0x10);
            surfs[i].back = b;
        }
        LOG("CreateSurface primary %dx%dx%d backbuffers=%d\n", w, h, bpp, (flags & 0x20) ? M32(d + 20) : 0);
        M32(pps) = surfs[i].obj;
        RET_STD(4, DD_OK);
    }
    int i = new_surface(w, h, bpp, caps);
    if (flags & 0x10000) { surfs[i].has_ck = 1; surfs[i].ck_lo = M32(d + 64); surfs[i].ck_hi = M32(d + 68); }
    TRACE("CreateSurface %dx%dx%d caps=%x -> %d\n", w, h, bpp, caps, i);
    M32(pps) = surfs[i].obj;
    RET_STD(4, DD_OK);
}
static void dd_DuplicateSurface(void) { RET_STD(3, DDERR_UNSUPPORTED); }
static void dd_EnumDisplayModes(void) {
    uint32_t ctx = ARG(3), cb = ARG(4);
    /* 4:3 and 5:4 like a 2003 monitor, plus widescreen 16:10, 16:9 and 21:9 modes */
    static const int modes[][2] = {
        {640, 480}, {800, 600}, {1024, 768}, {1280, 1024},
        {1280, 800}, {1440, 900}, {1680, 1050}, {1920, 1200},
        {1280, 720}, {1366, 768}, {1600, 900}, {1920, 1080}, {2560, 1440},
        {2560, 1080}, {3440, 1440}};
    static const int bpps[] = {8, 16, 32};
    uint32_t d = guest_alloc_static(124);
    for (int b = 0; b < 3; b++)
        for (int m = 0; m < (int)(sizeof modes / sizeof *modes); m++) {
            memset(PTR(d), 0, 124);
            M32(d) = 124; M32(d + 4) = 0x1006 | 8;
            M32(d + 8) = (uint32_t)modes[m][1]; M32(d + 12) = (uint32_t)modes[m][0];
            M32(d + 16) = (uint32_t)(modes[m][0] * bpps[b] / 8);
            M32(d + 24) = 60;
            fill_pixfmt(d + 72, bpps[b]);
            if (CALL_GUEST(cb, d, ctx) == 0) goto done;
        }
done:
    RET_STD(5, DD_OK);
}
static void dd_EnumSurfaces(void) { RET_STD(5, DD_OK); }
static void dd_FlipToGDISurface(void) { RET_STD(1, DD_OK); }
static void dd_GetCaps(void) {
    uint32_t c = ARG(1), c2 = ARG(2);
    for (int k = 0; k < 2; k++) {
        uint32_t p = k ? c2 : c;
        if (!p) continue;
        uint32_t size = M32(p);
        memset(PTR(p), 0, size ? size : 380);
        M32(p) = size;
        M32(p + 4) = 0x00000040 | 0x00000200 | 0x00002000;   /* BLT, BLTCOLORFILL... */
        M32(p + 8) = 0x00080000;
        M32(p + 12) = 0x00000001;
        M32(p + 60) = 64 << 20;        /* dwVidMemTotal */
        M32(p + 64) = 60 << 20;        /* dwVidMemFree */
        M32(p + 104) = 0x200 | 0x40 | 0x4000 | 0x10 | 0x4;   /* ddsCaps */
    }
    RET_STD(3, DD_OK);
}
static void dd_GetDisplayMode(void) {
    uint32_t d = ARG(1);
    memset(PTR(d), 0, 124);
    M32(d) = 124; M32(d + 4) = 0x1006 | 8;
    M32(d + 8) = (uint32_t)mode_h; M32(d + 12) = (uint32_t)mode_w; M32(d + 16) = (uint32_t)(mode_w * mode_bpp / 8);
    M32(d + 24) = 60;
    fill_pixfmt(d + 72, mode_bpp);
    RET_STD(2, DD_OK);
}
static void dd_GetFourCCCodes(void) { if (ARG(1)) M32(ARG(1)) = 0; RET_STD(3, DD_OK); }
static void dd_GetGDISurface(void) { RET_STD(2, DDERR_NOTFOUND); }
static void dd_GetMonitorFrequency(void) { M32(ARG(1)) = 60; RET_STD(2, DD_OK); }
static void dd_GetScanLine(void) { M32(ARG(1)) = 0; RET_STD(2, DD_OK); }
static void dd_GetVerticalBlankStatus(void) { M32(ARG(1)) = 1; RET_STD(2, DD_OK); }
static void dd_Initialize(void) { RET_STD(2, DD_OK); }
static void dd_RestoreDisplayMode(void) { RET_STD(1, DD_OK); }
static void dd_SetCooperativeLevel(void) { LOG("SetCooperativeLevel(%x)\n", ARG(2)); RET_STD(3, DD_OK); }
static void dd_SetDisplayMode(void) {
    mode_w = (int)ARG(1); mode_h = (int)ARG(2); mode_bpp = (int)ARG(3);
    LOG("SetDisplayMode %dx%dx%d\n", mode_w, mode_h, mode_bpp);
    plat_set_mode(mode_w, mode_h);
    user32_set_size(mode_w, mode_h);
    RET_STD(6, DD_OK);
}
static void dd_SetDisplayMode1(void) {   /* IDirectDraw v1 signature: 3 args */
    mode_w = (int)ARG(1); mode_h = (int)ARG(2); mode_bpp = (int)ARG(3);
    plat_set_mode(mode_w, mode_h);
    user32_set_size(mode_w, mode_h);
    RET_STD(4, DD_OK);
}
static void dd_WaitForVerticalBlank(void) { RET_STD(3, DD_OK); }
static void dd_GetAvailableVidMem(void) {
    if (ARG(2)) M32(ARG(2)) = 64 << 20;
    if (ARG(3)) M32(ARG(3)) = 60 << 20;
    RET_STD(4, DD_OK);
}
static void dd_GetSurfaceFromDC(void) { RET_STD(3, DDERR_NOTFOUND); }
static void dd_RestoreAllSurfaces(void) { RET_STD(1, DD_OK); }
static void dd_TestCooperativeLevel(void) { RET_STD(1, DD_OK); }
static void dd_GetDeviceIdentifier(void) {
    uint32_t p = ARG(1);
    memset(PTR(p), 0, 1072);
    strcpy((char *)PTR(p), "display");
    strcpy((char *)PTR(p + 512), "WebAssembly Software Renderer");
    M32(p + 1032) = 0x10de; M32(p + 1036) = 0x0110;
    RET_STD(3, DD_OK);
}
static void dd_StartModeTest(void) { RET_STD(4, DDERR_UNSUPPORTED); }
static void dd_EvaluateMode(void) { RET_STD(3, DDERR_UNSUPPORTED); }

/* ---- IDirectDrawSurface7 ---- */
static void dds_QueryInterface(void) {
    TRACE("IDirectDrawSurface::QueryInterface %08x\n", M32(ARG(1)));
    S(ARG(0))->refs++;   /* COM: the returned interface holds a reference */
    M32(ARG(2)) = ARG(0);
    RET_STD(3, DD_OK);
}
static void dds_AddRef(void) { Surf *s = S(ARG(0)); RET_STD(1, ++s->refs); }
static void dds_Release(void) {
    Surf *s = S(ARG(0));
    int r = --s->refs;
    int attached = 0;   /* a flip chain's back buffer lives as long as its front buffer */
    for (int i = 0; i < nsurfs; i++) if (surfs[i].obj && surfs[i].back == (int)(s - surfs)) attached = 1;
    if (r <= 0 && !s->primary && !attached) {
        heap_free(s->pixels);
        s->pixels = 0;
        int idx = (int)(s - surfs);
        (void)idx;
        /* keep the guest object alive but mark the slot reusable */
        M32(s->obj + 4) = 0xffffffffu;
        s->obj = 0;
    }
    RET_STD(1, r < 0 ? 0 : r);
}
static void dds_AddAttachedSurface(void) { RET_STD(2, DD_OK); }
static void dds_AddOverlayDirtyRect(void) { RET_STD(2, DDERR_UNSUPPORTED); }

static inline uint32_t getpx(Surf *s, int x, int y) {
    uint32_t a = s->pixels + (uint32_t)(y * s->pitch);
    if (s->bpp == 16) return M16(a + 2 * x);
    if (s->bpp == 8) return M8(a + x);
    return M32(a + 4 * x);
}
static inline void putpx(Surf *s, int x, int y, uint32_t v) {
    uint32_t a = s->pixels + (uint32_t)(y * s->pitch);
    if (s->bpp == 16) M16(a + 2 * x) = (uint16_t)v;
    else if (s->bpp == 8) M8(a + x) = (uint8_t)v;
    else M32(a + 4 * x) = v;
}

static void mark(Surf *d) {
    if (d->primary) dirty = 1;
}

typedef struct { int l, t, r, b; } Rect;
static Rect rect_or_full(uint32_t r, Surf *s) {
    Rect x;
    if (r) { x.l = (int32_t)M32(r); x.t = (int32_t)M32(r + 4); x.r = (int32_t)M32(r + 8); x.b = (int32_t)M32(r + 12); }
    else { x.l = 0; x.t = 0; x.r = s->w; x.b = s->h; }
    return x;
}

static void do_blt(Surf *d, Rect dr, Surf *s, Rect sr, int use_ck, uint32_t ck) {
    int dw = dr.r - dr.l, dh = dr.b - dr.t, sw = sr.r - sr.l, sh = sr.b - sr.t;
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    int bpp = d->bpp / 8;
    if (dw == sw && dh == sh) {
        /* clip */
        if (dr.l < 0) { sr.l -= dr.l; dr.l = 0; }
        if (dr.t < 0) { sr.t -= dr.t; dr.t = 0; }
        if (sr.l < 0) { dr.l -= sr.l; sr.l = 0; }
        if (sr.t < 0) { dr.t -= sr.t; sr.t = 0; }
        int w = dr.r - dr.l, h = dr.b - dr.t;
        if (dr.l + w > d->w) w = d->w - dr.l;
        if (dr.t + h > d->h) h = d->h - dr.t;
        if (sr.l + w > s->w) w = s->w - sr.l;
        if (sr.t + h > s->h) h = s->h - sr.t;
        if (w <= 0 || h <= 0) return;
        if (!use_ck && s->bpp == d->bpp) {
            int ystart = 0, yend = h, ystep = 1;
            if (s == d && dr.t > sr.t) { ystart = h - 1; yend = -1; ystep = -1; }
            for (int y = ystart; y != yend; y += ystep)
                memmove(PTR(d->pixels + (uint32_t)((dr.t + y) * d->pitch + dr.l * bpp)),
                        PTR(s->pixels + (uint32_t)((sr.t + y) * s->pitch + sr.l * bpp)), (size_t)(w * bpp));
        } else if (bpp == 2) {
            for (int y = 0; y < h; y++) {
                uint16_t *dp = (uint16_t *)PTR(d->pixels + (uint32_t)((dr.t + y) * d->pitch + dr.l * 2));
                const uint16_t *sp = (const uint16_t *)PTR(s->pixels + (uint32_t)((sr.t + y) * s->pitch + sr.l * 2));
                for (int x = 0; x < w; x++) if (sp[x] != ck) dp[x] = sp[x];
            }
        } else {
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    uint32_t v = getpx(s, sr.l + x, sr.t + y);
                    if (!use_ck || v != ck) putpx(d, dr.l + x, dr.t + y, v);
                }
        }
        return;
    }
    /* stretch, nearest neighbour */
    for (int y = 0; y < dh; y++) {
        int dy = dr.t + y; if (dy < 0 || dy >= d->h) continue;
        int sy = sr.t + y * sh / dh; if (sy < 0 || sy >= s->h) continue;
        for (int x = 0; x < dw; x++) {
            int dx = dr.l + x; if (dx < 0 || dx >= d->w) continue;
            int sx = sr.l + x * sw / dw; if (sx < 0 || sx >= s->w) continue;
            uint32_t v = getpx(s, sx, sy);
            if (!use_ck || v != ck) putpx(d, dx, dy, v);
        }
    }
}

static void dds_Blt(void) {
    Surf *d = S(ARG(0));
    uint32_t dstr = ARG(1), src = ARG(2), srcr = ARG(3), flags = ARG(4), fx = ARG(5);
    Rect dr = rect_or_full(dstr, d);
    if (flags & 0x400) {   /* DDBLT_COLORFILL */
        uint32_t c = M32(fx + 80);
        if (dr.l < 0) dr.l = 0; if (dr.t < 0) dr.t = 0;
        if (dr.r > d->w) dr.r = d->w; if (dr.b > d->h) dr.b = d->h;
        for (int y = dr.t; y < dr.b; y++)
            for (int x = dr.l; x < dr.r; x++) putpx(d, x, y, c);
        mark(d);
        RET_STD(6, DD_OK);
    }
    if (!src) RET_STD(6, DD_OK);
    Surf *s = S(src);
    Rect sr = rect_or_full(srcr, s);
    int use_ck = 0; uint32_t ck = 0;
    if (flags & 0x8000) { use_ck = s->has_ck; ck = s->ck_lo; }          /* DDBLT_KEYSRC */
    if (flags & 0x10000) { use_ck = 1; ck = M32(fx + 92 + 8); }         /* DDBLT_KEYSRCOVERRIDE: ddckSrcColorkey */
    do_blt(d, dr, s, sr, use_ck, ck);
    mark(d);
    RET_STD(6, DD_OK);
}
static void dds_BltBatch(void) { RET_STD(4, DDERR_UNSUPPORTED); }
static void dds_BltFast(void) {
    Surf *d = S(ARG(0));
    int x = (int)ARG(1), y = (int)ARG(2);
    Surf *s = S(ARG(3));
    Rect sr = rect_or_full(ARG(4), s);
    uint32_t trans = ARG(5);
    Rect dr = {x, y, x + sr.r - sr.l, y + sr.b - sr.t};
    do_blt(d, dr, s, sr, (trans & 1) && s->has_ck, s->ck_lo);
    mark(d);
    RET_STD(6, DD_OK);
}
static void dds_DeleteAttachedSurface(void) { RET_STD(3, DD_OK); }
static void dds_EnumAttachedSurfaces(void) {
    Surf *s = S(ARG(0));
    if (s->back >= 0) {
        uint32_t d = guest_alloc_static(124);
        M32(d) = 124;
        fill_desc(d, &surfs[s->back]);
        CALL_GUEST(ARG(2), surfs[s->back].obj, d, ARG(1));
    }
    RET_STD(3, DD_OK);
}
static void dds_EnumOverlayZOrders(void) { RET_STD(4, DD_OK); }
static uint32_t last_flip;
static void dds_Flip(void) {
    Surf *s = S(ARG(0));
    /* emulate DDFLIP_WAIT / vsync: cap at ~60 Hz */
    uint32_t now = plat_ticks_ms();
    if (now - last_flip < 16) plat_sleep(16 - (now - last_flip));
    last_flip = plat_ticks_ms();
    if (s->back >= 0) {
        Surf *b = &surfs[s->back];
        uint32_t t = s->pixels; s->pixels = b->pixels; b->pixels = t;
    }
    present_surface(s);
    RET_STD(3, DD_OK);
}
static void dds_GetAttachedSurface(void) {
    Surf *s = S(ARG(0));
    if (s->back < 0) RET_STD(3, DDERR_NOTFOUND);
    surfs[s->back].refs++;
    M32(ARG(2)) = surfs[s->back].obj;
    RET_STD(3, DD_OK);
}
static void dds_GetBltStatus(void) { RET_STD(2, DD_OK); }
static void dds_GetCaps(void) {
    Surf *s = S(ARG(0));
    memset(ARGP(1), 0, 16);
    M32(ARG(1)) = s->caps;
    RET_STD(2, DD_OK);
}
static void dds_GetClipper(void) { RET_STD(2, DDERR_NOTFOUND); }
static void dds_GetColorKey(void) {
    Surf *s = S(ARG(0));
    if (!s->has_ck) RET_STD(3, DDERR_NOTFOUND);
    M32(ARG(2)) = s->ck_lo; M32(ARG(2) + 4) = s->ck_hi;
    RET_STD(3, DD_OK);
}
static void dds_GetDC(void) {
    Surf *s = S(ARG(0));
    M32(ARG(1)) = 0x00050000u | (uint32_t)(s - surfs);   /* HDC encodes the surface index */
    RET_STD(2, DD_OK);
}
static uint32_t rgb_to_px(int bpp, uint32_t cr) {
    uint32_t r = cr & 0xff, g = (cr >> 8) & 0xff, b = (cr >> 16) & 0xff;
    if (bpp == 16) return ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
    if (bpp == 32) return (r << 16) | (g << 8) | b;
    return 0;
}
static uint32_t px_to_rgb(int bpp, uint32_t p) {
    if (bpp == 16) { uint32_t r = (p >> 11) & 31, g = (p >> 5) & 63, b = p & 31; return (r << 3) | (g << 2) << 8 | (b << 3) << 16; }
    if (bpp == 32) return ((p >> 16) & 0xff) | (p & 0xff00) | ((p & 0xff) << 16);
    return 0;
}
/* GDI SetPixel/GetPixel on a surface DC */
void imp_GDI32_SetPixel(void) {
    uint32_t hdc = ARG(0); int x = (int)ARG(1), y = (int)ARG(2); uint32_t c = ARG(3);
    if ((hdc & 0xffff0000u) == 0x00050000u) {
        Surf *s = &surfs[hdc & 0xffff];
        if (x >= 0 && y >= 0 && x < s->w && y < s->h) putpx(s, x, y, rgb_to_px(s->bpp, c));
    }
    RET_STD(4, c);
}
void imp_GDI32_GetPixel(void) {
    uint32_t hdc = ARG(0); int x = (int)ARG(1), y = (int)ARG(2);
    if ((hdc & 0xffff0000u) == 0x00050000u) {
        Surf *s = &surfs[hdc & 0xffff];
        if (x >= 0 && y >= 0 && x < s->w && y < s->h) RET_STD(3, px_to_rgb(s->bpp, getpx(s, x, y)));
    }
    RET_STD(3, 0);
}
static void dds_GetFlipStatus(void) { RET_STD(2, DD_OK); }
static void dds_GetOverlayPosition(void) { RET_STD(3, DDERR_UNSUPPORTED); }
static void dds_GetPalette(void) {
    Surf *s = S(ARG(0));
    if (s->palette < 0) RET_STD(2, DDERR_NOTFOUND);
    M32(ARG(1)) = pals[s->palette].obj;
    RET_STD(2, DD_OK);
}
static void dds_GetPixelFormat(void) { fill_pixfmt(ARG(1), S(ARG(0))->bpp); RET_STD(2, DD_OK); }
static void dds_GetSurfaceDesc(void) { fill_desc(ARG(1), S(ARG(0))); RET_STD(2, DD_OK); }
static void dds_Initialize(void) { RET_STD(3, DD_OK); }
static void dds_IsLost(void) { RET_STD(1, DD_OK); }
static void dds_Lock(void) {
    Surf *s = S(ARG(0));
    uint32_t r = ARG(1), d = ARG(2);
    fill_desc(d, s);
    if (r) {
        int x = (int32_t)M32(r), y = (int32_t)M32(r + 4);
        M32(d + 36) = s->pixels + (uint32_t)(y * s->pitch + x * s->bpp / 8);
    }
    RET_STD(5, DD_OK);
}
static void dds_ReleaseDC(void) { RET_STD(2, DD_OK); }
static void dds_Restore(void) { RET_STD(1, DD_OK); }
static void dds_SetClipper(void) { RET_STD(2, DD_OK); }
static void dds_SetColorKey(void) {
    Surf *s = S(ARG(0));
    uint32_t flags = ARG(1), ck = ARG(2);
    if (flags & 8) {   /* DDCKEY_SRCBLT */
        if (ck) { s->has_ck = 1; s->ck_lo = M32(ck); s->ck_hi = M32(ck + 4); }
        else s->has_ck = 0;
    }
    RET_STD(3, DD_OK);
}
static void dds_SetOverlayPosition(void) { RET_STD(3, DDERR_UNSUPPORTED); }
static void dds_SetPalette(void) {
    Surf *s = S(ARG(0));
    s->palette = ARG(1) ? (int)M32(ARG(1) + 4) : -1;
    if (s->primary) dirty = 1;
    RET_STD(2, DD_OK);
}
static void dds_Unlock(void) {
    Surf *s = S(ARG(0));
    mark(s);
    RET_STD(2, DD_OK);
}
static void dds_UpdateOverlay(void) { RET_STD(6, DDERR_UNSUPPORTED); }
static void dds_UpdateOverlayDisplay(void) { RET_STD(2, DDERR_UNSUPPORTED); }
static void dds_UpdateOverlayZOrder(void) { RET_STD(3, DDERR_UNSUPPORTED); }
static void dds_GetDDInterface(void) { M32(ARG(1)) = dd_obj; RET_STD(2, DD_OK); }
static void dds_PageLock(void) { RET_STD(2, DD_OK); }
static void dds_PageUnlock(void) { RET_STD(2, DD_OK); }
static void dds_SetSurfaceDesc(void) {
    Surf *s = S(ARG(0));
    uint32_t d = ARG(1);
    if (M32(d + 4) & 0x800) s->pixels = M32(d + 36);
    if (M32(d + 4) & 4) s->w = (int)M32(d + 12);
    if (M32(d + 4) & 2) s->h = (int)M32(d + 8);
    if (M32(d + 4) & 8) s->pitch = (int)M32(d + 16);
    RET_STD(3, DD_OK);
}
static void dds_SetPrivateData(void) { RET_STD(5, DD_OK); }
static void dds_GetPrivateData(void) { RET_STD(4, DDERR_NOTFOUND); }
static void dds_FreePrivateData(void) { RET_STD(2, DD_OK); }
static void dds_GetUniquenessValue(void) { M32(ARG(1)) = 1; RET_STD(2, DD_OK); }
static void dds_ChangeUniquenessValue(void) { RET_STD(1, DD_OK); }
static void dds_SetPriority(void) { RET_STD(2, DD_OK); }
static void dds_GetPriority(void) { M32(ARG(1)) = 0; RET_STD(2, DD_OK); }
static void dds_SetLOD(void) { RET_STD(2, DD_OK); }
static void dds_GetLOD(void) { M32(ARG(1)) = 0; RET_STD(2, DD_OK); }

/* ---- IDirectDrawPalette ---- */
static void pal_QueryInterface(void) { RET_STD(3, E_NOINTERFACE); }
static void pal_AddRef(void) { RET_STD(1, 1); }
static void pal_Release(void) { RET_STD(1, 0); }
static void pal_GetCaps(void) { M32(ARG(1)) = 4; RET_STD(2, DD_OK); }
static void pal_GetEntries(void) {
    Pal *p = &pals[M32(ARG(0) + 4)];
    uint32_t start = ARG(2), n = ARG(3), out = ARG(4);
    for (uint32_t i = 0; i < n && start + i < 256; i++) M32(out + 4 * i) = p->entries[start + i];
    RET_STD(5, DD_OK);
}
static void pal_Initialize(void) { RET_STD(4, DD_OK); }
static void pal_SetEntries(void) {
    Pal *p = &pals[M32(ARG(0) + 4)];
    uint32_t start = ARG(2), n = ARG(3), in = ARG(4);
    for (uint32_t i = 0; i < n && start + i < 256; i++) p->entries[start + i] = M32(in + 4 * i) & 0xffffff;
    dirty = 1;
    RET_STD(5, DD_OK);
}

/* ---- IDirectDrawClipper ---- */
static void clip_QueryInterface(void) { RET_STD(3, E_NOINTERFACE); }
static void clip_AddRef(void) { RET_STD(1, 1); }
static void clip_Release(void) { RET_STD(1, 0); }
static void clip_GetClipList(void) { RET_STD(4, DDERR_UNSUPPORTED); }
static void clip_GetHWnd(void) { M32(ARG(1)) = 0x00010010u; RET_STD(2, DD_OK); }
static void clip_Initialize(void) { RET_STD(3, DD_OK); }
static void clip_IsClipListChanged(void) { M32(ARG(1)) = 0; RET_STD(2, DD_OK); }
static void clip_SetClipList(void) { RET_STD(3, DD_OK); }
static void clip_SetHWnd(void) { RET_STD(3, DD_OK); }

#define E(f) f
#define N(f) #f
static const gfn_t dd7_m[] = {
    dd_QueryInterface, dd_AddRef, dd_Release, dd_Compact, dd_CreateClipper, dd_CreatePalette, dd_CreateSurface,
    dd_DuplicateSurface, dd_EnumDisplayModes, dd_EnumSurfaces, dd_FlipToGDISurface, dd_GetCaps, dd_GetDisplayMode,
    dd_GetFourCCCodes, dd_GetGDISurface, dd_GetMonitorFrequency, dd_GetScanLine, dd_GetVerticalBlankStatus,
    dd_Initialize, dd_RestoreDisplayMode, dd_SetCooperativeLevel, dd_SetDisplayMode, dd_WaitForVerticalBlank,
    dd_GetAvailableVidMem, dd_GetSurfaceFromDC, dd_RestoreAllSurfaces, dd_TestCooperativeLevel,
    dd_GetDeviceIdentifier, dd_StartModeTest, dd_EvaluateMode};
static const char *const dd7_n[] = {
    "DD::QueryInterface", "DD::AddRef", "DD::Release", "DD::Compact", "DD::CreateClipper", "DD::CreatePalette",
    "DD::CreateSurface", "DD::DuplicateSurface", "DD::EnumDisplayModes", "DD::EnumSurfaces", "DD::FlipToGDISurface",
    "DD::GetCaps", "DD::GetDisplayMode", "DD::GetFourCCCodes", "DD::GetGDISurface", "DD::GetMonitorFrequency",
    "DD::GetScanLine", "DD::GetVerticalBlankStatus", "DD::Initialize", "DD::RestoreDisplayMode",
    "DD::SetCooperativeLevel", "DD::SetDisplayMode", "DD::WaitForVerticalBlank", "DD::GetAvailableVidMem",
    "DD::GetSurfaceFromDC", "DD::RestoreAllSurfaces", "DD::TestCooperativeLevel", "DD::GetDeviceIdentifier",
    "DD::StartModeTest", "DD::EvaluateMode"};
static const gfn_t dds7_m[] = {
    dds_QueryInterface, dds_AddRef, dds_Release, dds_AddAttachedSurface, dds_AddOverlayDirtyRect, dds_Blt,
    dds_BltBatch, dds_BltFast, dds_DeleteAttachedSurface, dds_EnumAttachedSurfaces, dds_EnumOverlayZOrders,
    dds_Flip, dds_GetAttachedSurface, dds_GetBltStatus, dds_GetCaps, dds_GetClipper, dds_GetColorKey, dds_GetDC,
    dds_GetFlipStatus, dds_GetOverlayPosition, dds_GetPalette, dds_GetPixelFormat, dds_GetSurfaceDesc,
    dds_Initialize, dds_IsLost, dds_Lock, dds_ReleaseDC, dds_Restore, dds_SetClipper, dds_SetColorKey,
    dds_SetOverlayPosition, dds_SetPalette, dds_Unlock, dds_UpdateOverlay, dds_UpdateOverlayDisplay,
    dds_UpdateOverlayZOrder, dds_GetDDInterface, dds_PageLock, dds_PageUnlock, dds_SetSurfaceDesc,
    dds_SetPrivateData, dds_GetPrivateData, dds_FreePrivateData, dds_GetUniquenessValue,
    dds_ChangeUniquenessValue, dds_SetPriority, dds_GetPriority, dds_SetLOD, dds_GetLOD};
static const char *const dds7_n[] = {"dds_QueryInterface", "dds_AddRef", "dds_Release", "dds_AddAttachedSurface", "dds_AddOverlayDirtyRect", "dds_Blt", "dds_BltBatch", "dds_BltFast", "dds_DeleteAttachedSurface", "dds_EnumAttachedSurfaces", "dds_EnumOverlayZOrders", "dds_Flip", "dds_GetAttachedSurface", "dds_GetBltStatus", "dds_GetCaps", "dds_GetClipper", "dds_GetColorKey", "dds_GetDC", "dds_GetFlipStatus", "dds_GetOverlayPosition", "dds_GetPalette", "dds_GetPixelFormat", "dds_GetSurfaceDesc", "dds_Initialize", "dds_IsLost", "dds_Lock", "dds_ReleaseDC", "dds_Restore", "dds_SetClipper", "dds_SetColorKey", "dds_SetOverlayPosition", "dds_SetPalette", "dds_Unlock", "dds_UpdateOverlay", "dds_UpdateOverlayDisplay", "dds_UpdateOverlayZOrder", "dds_GetDDInterface", "dds_PageLock", "dds_PageUnlock", "dds_SetSurfaceDesc", "dds_SetPrivateData", "dds_GetPrivateData", "dds_FreePrivateData", "dds_GetUniquenessValue", "dds_ChangeUniquenessValue", "dds_SetPriority", "dds_GetPriority", "dds_SetLOD", "dds_GetLOD"};

static const gfn_t pal_m[] = {pal_QueryInterface, pal_AddRef, pal_Release, pal_GetCaps, pal_GetEntries, pal_Initialize, pal_SetEntries};
static const char *const pal_n[] = {"pal_QueryInterface", "pal_AddRef", "pal_Release", "pal_GetCaps", "pal_GetEntries", "pal_Initialize", "pal_SetEntries"};

static const gfn_t clip_m[] = {clip_QueryInterface, clip_AddRef, clip_Release, clip_GetClipList, clip_GetHWnd,
                               clip_Initialize, clip_IsClipListChanged, clip_SetClipList, clip_SetHWnd};
static const char *const clip_n[] = {"clip_QueryInterface", "clip_AddRef", "clip_Release", "clip_GetClipList", "clip_GetHWnd", "clip_Initialize", "clip_IsClipListChanged", "clip_SetClipList", "clip_SetHWnd"};


static void ddraw_init(void) {
    if (vt_dd7) return;
    vt_dd7 = com_make_vtbl(dd7_m, dd7_n, 30);
    vt_dds7 = com_make_vtbl(dds7_m, dds7_n, 49);
    vt_pal = com_make_vtbl(pal_m, pal_n, 7);
    vt_clip = com_make_vtbl(clip_m, clip_n, 9);
    dd_obj = com_new(vt_dd7, 4);
    for (int i = 0; i < 256; i++) pals[0].entries[i] = (uint32_t)(i * 0x010101);
}

/* DirectDrawCreateEx(GUID*, LPVOID*, REFIID, IUnknown*) */
void ddraw_DirectDrawCreateEx(void) {
    ddraw_init();
    LOG("DirectDrawCreateEx\n");
    M32(ARG(1)) = dd_obj;
    RET_STD(4, DD_OK);
}
/* DirectDrawCreate(GUID*, LPDIRECTDRAW*, IUnknown*): v1 interface shares our vtable layout
 * except SetDisplayMode takes 3 args. */
static uint32_t vt_dd1, dd1_obj;
void ddraw_DirectDrawCreate(void) {
    ddraw_init();
    if (!vt_dd1) {
        gfn_t m[23]; memcpy(m, dd7_m, sizeof m);
        m[21] = dd_SetDisplayMode1;
        vt_dd1 = com_make_vtbl(m, dd7_n, 23);
        dd1_obj = com_new(vt_dd1, 4);
    }
    LOG("DirectDrawCreate\n");
    M32(ARG(1)) = dd1_obj;
    RET_STD(3, DD_OK);
}

uint32_t dd_for_iid(uint32_t iid) {
    if (M32(iid) == 0x6C14DB80u) {   /* IID_IDirectDraw */
        if (!dd1_obj) { uint32_t esp = R.esp; (void)esp; }
        return dd1_obj ? dd1_obj : dd_obj;
    }
    return dd_obj;
}
