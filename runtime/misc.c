/* ADVAPI32, WINMM, OLE32, VERSION, WSOCK32 */
#include "runtime.h"
#include <stdio.h>

/* ---- registry: nothing is installed ---- */
HLE(ADVAPI32, RegOpenKeyExA) {
    LOG("RegOpenKeyExA(%08x, %s)\n", ARG(0), ARG(1) ? ARGP(1) : "");
    RET_STD(5, 2);   /* ERROR_FILE_NOT_FOUND */
}
HLE(ADVAPI32, RegQueryValueExA) { RET_STD(6, 2); }
HLE(ADVAPI32, RegCloseKey) { RET_STD(1, 0); }
HLE(ADVAPI32, GetUserNameA) {
    strcpy(ARGP(0), "Player"); M32(ARG(1)) = 7; RET_STD(2, 1);
}

/* ---- WINMM ---- */
HLE(WINMM, timeGetTime) { RET_STD(0, plat_ticks_ms()); }
HLE(WINMM, mixerGetNumDevs) { RET_STD(0, 0); }
HLE(WINMM, mixerOpen) { RET_STD(5, 2); }   /* MMSYSERR_BADDEVICEID */
HLE(WINMM, mixerClose) { RET_STD(1, 0); }
HLE(WINMM, mixerGetLineInfoA) { RET_STD(3, 2); }
HLE(WINMM, mixerGetLineControlsA) { RET_STD(3, 2); }
HLE(WINMM, mixerGetControlDetailsA) { RET_STD(3, 2); }
HLE(WINMM, mixerSetControlDetails) { RET_STD(3, 2); }
HLE(WINMM, mciSendCommandA) {
    LOG("mciSendCommandA(%x, msg=%x) -> error (video not supported)\n", ARG(0), ARG(1));
    RET_STD(4, 263);  /* MCIERR_INVALID_DEVICE_NAME */
}
HLE(WINMM, mciGetErrorStringA) {
    if (ARG(2)) snprintf(ARGP(1), ARG(2), "MCI unavailable");
    RET_STD(3, 1);
}
HLE(WINMM, mmioDescend) { RET_STD(4, 257); }
HLE(WINMM, mmioAdvance) { RET_STD(3, 0); }
HLE(WINMM, mmioClose) { RET_STD(2, 0); }
HLE(WINMM, mmioGetInfo) { RET_STD(3, 257); }
HLE(WINMM, mmioSetInfo) { RET_STD(3, 0); }
HLE(WINMM, mmioSeek) { RET_STD(3, 0xffffffffu); }

/* ---- OLE32 ---- */
HLE(OLE32, CoInitialize) { RET_STD(1, 0); }
HLE(OLE32, CoUninitialize) { RET_STD(0, 0); }
HLE(OLE32, CoCreateGuid) {
    uint32_t g = ARG(0);
    for (int i = 0; i < 16; i++) M8(g + i) = (uint8_t)(plat_ticks_us() * 2654435761u >> (i % 24));
    RET_STD(1, 0);
}
static void fmt_guid(uint32_t g, char *out) {
    sprintf(out, "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", M32(g), M16(g + 4), M16(g + 6),
            M8(g + 8), M8(g + 9), M8(g + 10), M8(g + 11), M8(g + 12), M8(g + 13), M8(g + 14), M8(g + 15));
}
uint32_t dmusic_create(uint32_t clsid, uint32_t iid, uint32_t ppv);
HLE(OLE32, CoCreateInstance) {
    char c[64], i[64];
    fmt_guid(ARG(0), c); fmt_guid(ARG(3), i);
    uint32_t r = dplay_create(ARG(0), ARG(3), ARG(4));
    if (r == 0x80040154u) r = video_create(ARG(0), ARG(3), ARG(4));
    if (r == 0x80040154u) r = dmusic_create(ARG(0), ARG(3), ARG(4));
    LOG("CoCreateInstance(clsid %s, iid %s) -> %08x\n", c, i, r);
    if (r) M32(ARG(4)) = 0;
    RET_STD(5, r);
}

/* ---- VERSION ---- */
HLE(VERSION, GetFileVersionInfoSizeA) {
    LOG("GetFileVersionInfoSizeA(%s)\n", ARGP(0));
    if (ARG(1)) M32(ARG(1)) = 0;
    RET_STD(2, 0);
}
HLE(VERSION, GetFileVersionInfoA) { RET_STD(4, 0); }
HLE(VERSION, VerQueryValueA) { RET_STD(4, 0); }

/* ---- WSOCK32: no networking ---- */
HLE(WSOCK32, WSAStartup) { RET_STD(2, 10091); }   /* WSASYSNOTREADY */
HLE(WSOCK32, WSACleanup) { RET_STD(0, 0); }
HLE(WSOCK32, gethostname) { strcpy(ARGP(0), "localhost"); RET_STD(2, 0); }
HLE(WSOCK32, gethostbyname) { RET_STD(1, 0); }
HLE(WSOCK32, inet_ntoa) { static uint32_t s; if (!s) s = guest_strdup("127.0.0.1"); RET_STD(1, s); }
