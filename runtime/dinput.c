/* DirectInput (A) emulation: system mouse + keyboard, immediate and buffered. */
#include "runtime.h"
#include <stdio.h>

#define DI_OK 0
#define DI_BUFFEROVERFLOW 1
#define DIERR_NOTACQUIRED 0x8007000cu
#define E_NOINTERFACE 0x80004002u

int vk_scancode(int vk);
extern int mouse_x, mouse_y;

enum { DEV_MOUSE, DEV_KEYBOARD };
typedef struct {
    uint32_t obj;
    int kind, acquired;
    uint32_t bufsize;               /* DIPROP_BUFFERSIZE */
    struct { uint32_t ofs, data, time, seq; } q[256];
    int qh, qt, overflow;
    int32_t dx, dy, dz;
    uint8_t buttons[4];
    uint8_t keys[256];
} Dev;

static Dev devs[4];
static int ndevs;
static uint32_t vt_di, vt_did, di_obj;
static uint32_t seq;

static Dev *D(uint32_t obj) { return &devs[M32(obj + 4)]; }

static void enqueue(Dev *d, uint32_t ofs, uint32_t data) {
    if (!d->acquired || !d->bufsize) return;
    int n = (d->qt + 1) & 255;
    if (n == d->qh || (uint32_t)((d->qt - d->qh) & 255) >= d->bufsize) { d->overflow = 1; return; }
    d->q[d->qt].ofs = ofs; d->q[d->qt].data = data; d->q[d->qt].time = plat_ticks_ms(); d->q[d->qt].seq = seq++;
    d->qt = n;
}

void dinput_mouse_event(int dx, int dy, int button, int down, int wheel) {
    for (int i = 0; i < ndevs; i++) {
        Dev *d = &devs[i];
        if (d->kind != DEV_MOUSE) continue;
        if (dx) { d->dx += dx; enqueue(d, 0, (uint32_t)dx); }
        if (dy) { d->dy += dy; enqueue(d, 4, (uint32_t)dy); }
        if (wheel) { d->dz += wheel; enqueue(d, 8, (uint32_t)wheel); }
        if (button >= 0 && button < 3) {
            d->buttons[button] = down ? 0x80 : 0;
            enqueue(d, 12 + (uint32_t)button, down ? 0x80 : 0);
        }
    }
}
void dinput_key_event(int vk, int down) {
    int sc = vk_scancode(vk);
    if (!sc) return;
    for (int i = 0; i < ndevs; i++) {
        Dev *d = &devs[i];
        if (d->kind != DEV_KEYBOARD) continue;
        d->keys[sc] = down ? 0x80 : 0;
        enqueue(d, (uint32_t)sc, down ? 0x80 : 0);
    }
}

/* ---- IDirectInputA ---- */
static void di_QueryInterface(void) { M32(ARG(2)) = ARG(0); RET_STD(3, DI_OK); }
static void di_AddRef(void) { RET_STD(1, 1); }
static void di_Release(void) { RET_STD(1, 0); }
static void di_CreateDevice(void) {
    uint32_t guid = ARG(1), pp = ARG(2);
    uint32_t g0 = M32(guid);
    if (ndevs >= 4) RET_STD(4, 0x80004005u);
    Dev *d = &devs[ndevs];
    memset(d, 0, sizeof *d);
    if (g0 == 0x6F1D2B60u) d->kind = DEV_MOUSE;
    else if (g0 == 0x6F1D2B61u) d->kind = DEV_KEYBOARD;
    else { LOG("DirectInput CreateDevice unknown guid %08x\n", g0); RET_STD(4, 0x80040154u); }
    d->obj = com_new(vt_did, 4);
    M32(d->obj + 4) = (uint32_t)ndevs++;
    LOG("DirectInput CreateDevice %s\n", d->kind == DEV_MOUSE ? "mouse" : "keyboard");
    M32(pp) = d->obj;
    RET_STD(4, DI_OK);
}
static void di_EnumDevices(void) { RET_STD(5, DI_OK); }
static void di_GetDeviceStatus(void) { RET_STD(2, DI_OK); }
static void di_RunControlPanel(void) { RET_STD(3, DI_OK); }
static void di_Initialize(void) { RET_STD(3, DI_OK); }

/* ---- IDirectInputDeviceA ---- */
static void did_QueryInterface(void) { M32(ARG(2)) = ARG(0); RET_STD(3, DI_OK); }
static void did_AddRef(void) { RET_STD(1, 1); }
static void did_Release(void) { RET_STD(1, 0); }
static void did_GetCapabilities(void) {
    Dev *d = D(ARG(0));
    uint32_t c = ARG(1);
    uint32_t sz = M32(c);
    memset(PTR(c), 0, sz ? sz : 44); M32(c) = sz;
    M32(c + 4) = 1 | 4;                       /* attached, polled-ish */
    M32(c + 8) = d->kind == DEV_MOUSE ? 0x12 : 0x13;
    M32(c + 12) = d->kind == DEV_MOUSE ? 3 : 0;
    M32(c + 16) = d->kind == DEV_MOUSE ? 3 : 128;
    RET_STD(2, DI_OK);
}
static void did_EnumObjects(void) { RET_STD(4, DI_OK); }
static void did_GetProperty(void) {
    Dev *d = D(ARG(0));
    if (ARG(1) == 1) M32(ARG(2) + 16) = d->bufsize;
    RET_STD(3, DI_OK);
}
static void did_SetProperty(void) {
    Dev *d = D(ARG(0));
    uint32_t prop = ARG(1), ph = ARG(2);
    if (prop == 1) { d->bufsize = M32(ph + 16); if (d->bufsize > 255) d->bufsize = 255; }   /* DIPROP_BUFFERSIZE */
    RET_STD(3, DI_OK);
}
static void did_Acquire(void) { D(ARG(0))->acquired = 1; RET_STD(1, DI_OK); }
static void did_Unacquire(void) { D(ARG(0))->acquired = 0; RET_STD(1, DI_OK); }
static void did_GetDeviceState(void) {
    Dev *d = D(ARG(0));
    uint32_t n = ARG(1), p = ARG(2);
    input_pump();
    memset(PTR(p), 0, n);
    if (d->kind == DEV_MOUSE) {
        if (n >= 12) { M32(p) = (uint32_t)d->dx; M32(p + 4) = (uint32_t)d->dy; M32(p + 8) = (uint32_t)d->dz; }
        for (uint32_t i = 0; i < 4 && 12 + i < n; i++) M8(p + 12 + i) = d->buttons[i];
        d->dx = d->dy = d->dz = 0;
    } else {
        memcpy(PTR(p), d->keys, n < 256 ? n : 256);
    }
    RET_STD(3, DI_OK);
}
static void did_GetDeviceData(void) {
    Dev *d = D(ARG(0));
    uint32_t objsz = ARG(1), out = ARG(2), pn = ARG(3), flags = ARG(4);
    input_pump();
    uint32_t want = M32(pn), got = 0;
    int peek = flags & 1;
    int h = d->qh;
    while (h != d->qt && (out == 0 ? (want == 0xffffffffu || got < want) : got < want)) {
        if (out) {
            uint32_t o = out + got * objsz;
            M32(o) = d->q[h].ofs; M32(o + 4) = d->q[h].data; M32(o + 8) = d->q[h].time; M32(o + 12) = d->q[h].seq;
        }
        got++; h = (h + 1) & 255;
    }
    if (!peek) d->qh = h;
    M32(pn) = got;
    uint32_t r = d->overflow ? DI_BUFFEROVERFLOW : DI_OK;
    d->overflow = 0;
    RET_STD(5, r);
}
static void did_SetDataFormat(void) { RET_STD(2, DI_OK); }
static void did_SetEventNotification(void) { RET_STD(2, DI_OK); }
static void did_SetCooperativeLevel(void) { RET_STD(3, DI_OK); }
static void did_GetObjectInfo(void) { RET_STD(4, 0x80004001u); }
static void did_GetDeviceInfo(void) { RET_STD(2, 0x80004001u); }
static void did_RunControlPanel(void) { RET_STD(3, DI_OK); }
static void did_Initialize(void) { RET_STD(4, DI_OK); }

static const gfn_t di_m[] = {di_QueryInterface, di_AddRef, di_Release, di_CreateDevice, di_EnumDevices,
                             di_GetDeviceStatus, di_RunControlPanel, di_Initialize};
static const char *const di_n[] = {"di_QueryInterface", "di_AddRef", "di_Release", "di_CreateDevice", "di_EnumDevices", "di_GetDeviceStatus", "di_RunControlPanel", "di_Initialize"};

static const gfn_t did_m[] = {did_QueryInterface, did_AddRef, did_Release, did_GetCapabilities, did_EnumObjects,
                              did_GetProperty, did_SetProperty, did_Acquire, did_Unacquire, did_GetDeviceState,
                              did_GetDeviceData, did_SetDataFormat, did_SetEventNotification,
                              did_SetCooperativeLevel, did_GetObjectInfo, did_GetDeviceInfo, did_RunControlPanel,
                              did_Initialize};
static const char *const did_n[] = {"did_QueryInterface", "did_AddRef", "did_Release", "did_GetCapabilities", "did_EnumObjects", "did_GetProperty", "did_SetProperty", "did_Acquire", "did_Unacquire", "did_GetDeviceState", "did_GetDeviceData", "did_SetDataFormat", "did_SetEventNotification", "did_SetCooperativeLevel", "did_GetObjectInfo", "did_GetDeviceInfo", "did_RunControlPanel", "did_Initialize"};


HLE(DINPUT, DirectInputCreateA) {
    if (!vt_di) {
        vt_di = com_make_vtbl(di_m, di_n, 8);
        vt_did = com_make_vtbl(did_m, did_n, 18);
        di_obj = com_new(vt_di, 4);
    }
    LOG("DirectInputCreateA(version %x)\n", ARG(1));
    M32(ARG(2)) = di_obj;
    RET_STD(4, DI_OK);
}
