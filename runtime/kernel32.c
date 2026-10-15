/* KERNEL32 high-level emulation. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <time.h>

void vfs_invalidate(void);
static uint32_t last_error;
#define SETERR(e) (last_error = (e))

/* ------------------------------------------------------------- handles */
enum { H_FREE, H_FILE, H_FIND, H_MUTEX, H_THREAD, H_STD, H_EVENT };
#define MAXH 256
static struct {
    int type;
    int fd;
    /* find state */
    char *names; uint8_t *isdir; int n, pos; char pattern[260]; char dir[520];
} handles[MAXH];
#define H2I(h) ((int)(((h) - 0x100u) / 4))
#define I2H(i) (0x100u + 4u * (uint32_t)(i))

static int h_alloc(int type) {
    for (int i = 0; i < MAXH; i++)
        if (handles[i].type == H_FREE) { memset(&handles[i], 0, sizeof handles[i]); handles[i].type = type; handles[i].fd = -1; return i; }
    fatal("out of handles");
}
static int h_get(uint32_t h, int type) {
    int i = H2I(h);
    if (h < 0x100 || i < 0 || i >= MAXH || handles[i].type != type) return -1;
    return i;
}

/* ------------------------------------------------------------- process */
static uint32_t cmdline_ptr, env_ptr, envw_ptr;

HLE(KERNEL32, GetCommandLineA) {
    if (!cmdline_ptr) cmdline_ptr = guest_strdup("C:\\CULTURES\\GameSaga.exe");
    RET_STD(0, cmdline_ptr);
}
HLE(KERNEL32, GetVersion) { RET_STD(0, 0x0A280105u); }
HLE(KERNEL32, GetVersionExA) {
    uint32_t p = ARG(0);
    M32(p + 4) = 5; M32(p + 8) = 1; M32(p + 12) = 2600; M32(p + 16) = 2;
    strcpy((char *)PTR(p + 20), "Service Pack 3");
    RET_STD(1, 1);
}
HLE(KERNEL32, GetModuleHandleA) {
    if (ARG(0) == 0) RET_STD(1, IMAGE_BASE);
    LOG("GetModuleHandleA(%s)\n", ARGP(0));
    RET_STD(1, 0);
}
HLE(KERNEL32, GetModuleFileNameA) {
    uint32_t buf = ARG(1), n = ARG(2);
    char tmp[300];
    snprintf(tmp, sizeof tmp, "%s\\GameSaga.exe", exe_dir_win);
    uint32_t l = (uint32_t)strlen(tmp);
    if (l >= n) l = n - 1;
    memcpy(PTR(buf), tmp, l); M8(buf + l) = 0;
    RET_STD(3, l);
}
HLE(KERNEL32, GetStartupInfoA) {
    uint32_t p = ARG(0);
    memset(PTR(p), 0, 68); M32(p) = 68;
    RET_STD(1, 0);
}
HLE(KERNEL32, GetEnvironmentStrings) {
    if (!env_ptr) { env_ptr = guest_alloc_static(64); memcpy(PTR(env_ptr), "PATH=C:\\WINDOWS\0\0", 17); }
    RET_STD(0, env_ptr);
}
HLE(KERNEL32, GetEnvironmentStringsW) {
    if (!envw_ptr) {
        const char *s = "PATH=C:\\WINDOWS";
        envw_ptr = guest_alloc_static(64);
        for (int i = 0; s[i]; i++) M16(envw_ptr + 2 * i) = (uint8_t)s[i];
    }
    RET_STD(0, envw_ptr);
}
HLE(KERNEL32, FreeEnvironmentStringsA) { RET_STD(1, 1); }
HLE(KERNEL32, FreeEnvironmentStringsW) { RET_STD(1, 1); }
HLE(KERNEL32, GetEnvironmentVariableA) { SETERR(203); RET_STD(3, 0); }
HLE(KERNEL32, SetEnvironmentVariableA) { RET_STD(2, 1); }
HLE(KERNEL32, GetCurrentProcess) { RET_STD(0, 0xffffffffu); }
HLE(KERNEL32, GetCurrentProcessId) { RET_STD(0, 0x1000); }
HLE(KERNEL32, GetCurrentThreadId) { RET_STD(0, 0x1004); }
HLE(KERNEL32, ExitProcess) { LOG("ExitProcess(%d)\n", ARG(0)); plat_exit((int)ARG(0)); }
HLE(KERNEL32, TerminateProcess) { LOG("TerminateProcess(%d)\n", ARG(1)); plat_exit((int)ARG(1)); }
HLE(KERNEL32, GetLastError) { RET_STD(0, last_error); }
HLE(KERNEL32, SetLastError) { last_error = ARG(0); RET_STD(1, 0); }
HLE(KERNEL32, SetHandleCount) { RET_STD(1, ARG(0)); }
HLE(KERNEL32, GetComputerNameA) {
    strcpy(ARGP(0), "CULTURES"); M32(ARG(1)) = 8; RET_STD(2, 1);
}
HLE(KERNEL32, OutputDebugStringA) { LOG("[debug] %s", ARGP(0)); RET_STD(1, 0); }
HLE(KERNEL32, Sleep) {
    uint32_t ms = ARG(0);
    input_pump();
    bg_pump();
    plat_sleep(ms);
    RET_STD(1, 0);
}
HLE(KERNEL32, SetThreadPriority) { RET_STD(2, 1); }
/* The only threads the game starts are its multiplayer timer and file transfer loops,
 * which run as cooperative tasks (dplay.c). */
HLE(KERNEL32, CreateThread) {
    int t = coop_thread_create(ARG(2), ARG(3));
    if (t < 0) {
        LOG("CreateThread(start=%08x) -> refused (unknown thread)\n", ARG(2));
        SETERR(8);
        RET_STD(6, 0);
    }
    int i = h_alloc(H_THREAD);
    handles[i].fd = t;
    if (ARG(5)) M32(ARG(5)) = 0x2000u + (uint32_t)t;
    RET_STD(6, I2H(i));
}
HLE(KERNEL32, WaitForSingleObject) {
    uint32_t h = ARG(0);
    int i = H2I(h);
    if (h >= 0x100 && i >= 0 && i < MAXH && handles[i].type == H_THREAD)
        RET_STD(2, coop_thread_wait(handles[i].fd, ARG(1)) ? 0 : 0x102u /* WAIT_TIMEOUT */);
    RET_STD(2, 0);
}
HLE(KERNEL32, CreateMutexA) { int i = h_alloc(H_MUTEX); SETERR(0); RET_STD(3, I2H(i)); }
HLE(KERNEL32, CloseHandle) {
    uint32_t h = ARG(0);
    int i = H2I(h);
    if (h >= 0x100 && i >= 0 && i < MAXH && handles[i].type != H_FREE) {
        if (handles[i].type == H_FILE && handles[i].fd >= 0) plat_close(handles[i].fd);
        handles[i].type = H_FREE;
    }
    RET_STD(1, 1);
}

/* critical sections / interlocked: single threaded */
HLE(KERNEL32, InitializeCriticalSection) { memset(ARGP(0), 0, 24); RET_STD(1, 0); }
HLE(KERNEL32, DeleteCriticalSection) { RET_STD(1, 0); }
HLE(KERNEL32, EnterCriticalSection) { RET_STD(1, 0); }
HLE(KERNEL32, LeaveCriticalSection) { RET_STD(1, 0); }
HLE(KERNEL32, InterlockedIncrement) { uint32_t p = ARG(0); M32(p) += 1; RET_STD(1, M32(p)); }
HLE(KERNEL32, InterlockedDecrement) { uint32_t p = ARG(0); M32(p) -= 1; RET_STD(1, M32(p)); }

/* TLS */
static uint32_t tls[64]; static int ntls;
HLE(KERNEL32, TlsAlloc) { RET_STD(0, ntls < 64 ? (uint32_t)ntls++ : 0xffffffffu); }
HLE(KERNEL32, TlsGetValue) { SETERR(0); RET_STD(1, ARG(0) < 64 ? tls[ARG(0)] : 0); }
HLE(KERNEL32, TlsSetValue) { if (ARG(0) < 64) tls[ARG(0)] = ARG(1); RET_STD(2, 1); }

/* ------------------------------------------------------------- memory */
HLE(KERNEL32, HeapCreate) { RET_STD(3, 0x00500000u); }
HLE(KERNEL32, HeapDestroy) { RET_STD(1, 1); }
HLE(KERNEL32, HeapAlloc) { RET_STD(3, heap_alloc(ARG(2), ARG(1) & 8)); }
HLE(KERNEL32, HeapFree) { heap_free(ARG(2)); RET_STD(3, 1); }
HLE(KERNEL32, HeapReAlloc) {
    uint32_t flags = ARG(1), p = ARG(2), sz = ARG(3);
    if (flags & 0x10) {          /* HEAP_REALLOC_IN_PLACE_ONLY */
        RET_STD(4, heap_size(p) >= sz ? p : 0);
    }
    RET_STD(4, heap_realloc(p, sz, flags & 8));
}
HLE(KERNEL32, HeapSize) { RET_STD(3, heap_size(ARG(2))); }
HLE(KERNEL32, VirtualAlloc) { RET_STD(4, va_alloc(ARG(0), ARG(1), ARG(2))); }
HLE(KERNEL32, VirtualFree) { RET_STD(3, va_free(ARG(0), ARG(1), ARG(2))); }
HLE(KERNEL32, LocalFree) { heap_free(ARG(0)); RET_STD(1, 0); }

/* ------------------------------------------------------------- strings / locale */
HLE(KERNEL32, GetACP) { RET_STD(0, 1252); }
HLE(KERNEL32, GetOEMCP) { RET_STD(0, 850); }
HLE(KERNEL32, GetCPInfo) {
    uint32_t p = ARG(1);
    memset(PTR(p), 0, 20);
    M32(p) = 1; M8(p + 4) = '?';
    RET_STD(2, 1);
}
HLE(KERNEL32, MultiByteToWideChar) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4); int32_t cap = (int32_t)ARG(5);
    if (n < 0) n = (int32_t)strlen((char *)PTR(src)) + 1;
    if (cap == 0) RET_STD(6, n);
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) M16(dst + 2 * i) = M8(src + i);
    RET_STD(6, n);
}
HLE(KERNEL32, WideCharToMultiByte) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4); int32_t cap = (int32_t)ARG(5);
    if (n < 0) { n = 0; while (M16(src + 2 * n)) n++; n++; }
    if (cap == 0) RET_STD(8, n);
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) { uint16_t c = M16(src + 2 * i); M8(dst + i) = c < 256 ? (uint8_t)c : '?'; }
    RET_STD(8, n);
}
static int lower1252(int c) { return (c >= 'A' && c <= 'Z') || (c >= 0xc0 && c <= 0xde && c != 0xd7) ? c + 32 : c; }
static int upper1252(int c) { return (c >= 'a' && c <= 'z') || (c >= 0xe0 && c <= 0xfe && c != 0xf7) ? c - 32 : c; }
HLE(KERNEL32, LCMapStringA) {
    uint32_t flags = ARG(1), src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4); int32_t cap = (int32_t)ARG(5);
    if (n < 0) n = (int32_t)strlen((char *)PTR(src)) + 1;
    if (cap == 0) RET_STD(6, n);
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) {
        int c = M8(src + i);
        if (flags & 0x100) c = lower1252(c); else if (flags & 0x200) c = upper1252(c);
        M8(dst + i) = (uint8_t)c;
    }
    RET_STD(6, n);
}
HLE(KERNEL32, LCMapStringW) {
    uint32_t flags = ARG(1), src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4); int32_t cap = (int32_t)ARG(5);
    if (n < 0) { n = 0; while (M16(src + 2 * n)) n++; n++; }
    if (cap == 0) RET_STD(6, n);
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) {
        int c = M16(src + 2 * i);
        if (c < 256) { if (flags & 0x100) c = lower1252(c); else if (flags & 0x200) c = upper1252(c); }
        M16(dst + 2 * i) = (uint16_t)c;
    }
    RET_STD(6, n);
}
static uint16_t ctype1(int c) {
    uint16_t t = 0;
    if (c >= 'A' && c <= 'Z') t |= 0x101; else if (c >= 0xc0 && c <= 0xde && c != 0xd7) t |= 0x101;
    if (c >= 'a' && c <= 'z') t |= 0x102; else if (c >= 0xdf && c <= 0xff && c != 0xf7) t |= 0x102;
    if (c >= '0' && c <= '9') t |= 0x084;
    if (c == ' ' || (c >= 9 && c <= 13)) t |= 0x008;
    if (c == ' ') t |= 0x040;
    if (c < 32 || c == 127) t |= 0x020;
    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) t |= 0x080;
    if (c > 32 && c < 127 && !(t & 0x103) && !(c >= '0' && c <= '9')) t |= 0x010;
    return t;
}
HLE(KERNEL32, GetStringTypeA) {
    uint32_t src = ARG(2); int32_t n = (int32_t)ARG(3); uint32_t dst = ARG(4);
    if (n < 0) n = (int32_t)strlen((char *)PTR(src)) + 1;
    for (int i = 0; i < n; i++) M16(dst + 2 * i) = ctype1(M8(src + i));
    RET_STD(5, 1);
}
HLE(KERNEL32, GetStringTypeW) {
    uint32_t src = ARG(1); int32_t n = (int32_t)ARG(2); uint32_t dst = ARG(3);
    if (n < 0) { n = 0; while (M16(src + 2 * n)) n++; n++; }
    for (int i = 0; i < n; i++) { int c = M16(src + 2 * i); M16(dst + 2 * i) = c < 256 ? ctype1(c) : 0x100; }
    RET_STD(4, 1);
}
HLE(KERNEL32, CompareStringA) {
    uint32_t flags = ARG(1), a = ARG(2); int32_t na = (int32_t)ARG(3); uint32_t b = ARG(4); int32_t nb = (int32_t)ARG(5);
    if (na < 0) na = (int32_t)strlen((char *)PTR(a));
    if (nb < 0) nb = (int32_t)strlen((char *)PTR(b));
    int i = 0, r = 0;
    for (; i < na && i < nb; i++) {
        int ca = M8(a + i), cb = M8(b + i);
        if (flags & 1) { ca = lower1252(ca); cb = lower1252(cb); }
        if (ca != cb) { r = ca < cb ? -1 : 1; break; }
    }
    if (!r) r = na < nb ? -1 : na > nb ? 1 : 0;
    RET_STD(6, r + 2);
}
HLE(KERNEL32, CompareStringW) {
    uint32_t flags = ARG(1), a = ARG(2); int32_t na = (int32_t)ARG(3); uint32_t b = ARG(4); int32_t nb = (int32_t)ARG(5);
    if (na < 0) { na = 0; while (M16(a + 2 * na)) na++; }
    if (nb < 0) { nb = 0; while (M16(b + 2 * nb)) nb++; }
    int r = 0;
    for (int i = 0; i < na && i < nb; i++) {
        int ca = M16(a + 2 * i), cb = M16(b + 2 * i);
        if (flags & 1) { ca = lower1252(ca); cb = lower1252(cb); }
        if (ca != cb) { r = ca < cb ? -1 : 1; break; }
    }
    if (!r) r = na < nb ? -1 : na > nb ? 1 : 0;
    RET_STD(6, r + 2);
}
HLE(KERNEL32, FormatMessageA) {
    uint32_t buf = ARG(4), n = ARG(5);
    const char *msg = "error";
    if (ARG(0) & 0x100) { uint32_t p = guest_strdup(msg); M32(buf) = p; }
    else if (n > 6) strcpy((char *)PTR(buf), msg);
    RET_STD(7, 5);
}

/* ------------------------------------------------------------- time */
HLE(KERNEL32, GetLocalTime) {
    uint32_t p = ARG(0);
    time_t t = time(NULL); struct tm tm;
#ifdef __wasm__
    tm = *gmtime(&t);
#else
    localtime_r(&t, &tm);
#endif
    M16(p) = (uint16_t)(tm.tm_year + 1900); M16(p + 2) = (uint16_t)(tm.tm_mon + 1); M16(p + 4) = (uint16_t)tm.tm_wday;
    M16(p + 6) = (uint16_t)tm.tm_mday; M16(p + 8) = (uint16_t)tm.tm_hour; M16(p + 10) = (uint16_t)tm.tm_min;
    M16(p + 12) = (uint16_t)tm.tm_sec; M16(p + 14) = (uint16_t)(plat_ticks_ms() % 1000);
    RET_STD(1, 0);
}
HLE(KERNEL32, GetTimeZoneInformation) { memset(ARGP(0), 0, 172); RET_STD(1, 0); }
HLE(KERNEL32, FileTimeToLocalFileTime) { M64(ARG(1)) = M64(ARG(0)); RET_STD(2, 1); }
HLE(KERNEL32, FileTimeToSystemTime) {
    uint64_t ft = M64(ARG(0)); uint32_t p = ARG(1);
    time_t t = (time_t)(ft / 10000000ull - 11644473600ull);
    struct tm tm = *gmtime(&t);
    M16(p) = (uint16_t)(tm.tm_year + 1900); M16(p + 2) = (uint16_t)(tm.tm_mon + 1); M16(p + 4) = (uint16_t)tm.tm_wday;
    M16(p + 6) = (uint16_t)tm.tm_mday; M16(p + 8) = (uint16_t)tm.tm_hour; M16(p + 10) = (uint16_t)tm.tm_min;
    M16(p + 12) = (uint16_t)tm.tm_sec; M16(p + 14) = 0;
    RET_STD(2, 1);
}

/* ------------------------------------------------------------- files */
HLE(KERNEL32, GetStdHandle) { RET_STD(1, ARG(0)); }   /* -10/-11/-12 are recognised by WriteFile */
HLE(KERNEL32, SetStdHandle) { RET_STD(2, 1); }
HLE(KERNEL32, GetFileType) {
    uint32_t h = ARG(0);
    if (h >= 0xfffffff4u) RET_STD(1, 2);
    RET_STD(1, h_get(h, H_FILE) >= 0 ? 1 : 0);
}

HLE(KERNEL32, CreateFileA) {
    const char *name = ARGP(0);
    uint32_t access = ARG(1), disp = ARG(4);
    char host[520];
    int exists = vfs_resolve(name, host, sizeof host);
    int write = (access & 0x40000000u) != 0;
    /* CREATE_NEW=1 CREATE_ALWAYS=2 OPEN_EXISTING=3 OPEN_ALWAYS=4 TRUNCATE_EXISTING=5 */
    if (disp == 1 && exists) { SETERR(80); RET_STD(7, 0xffffffffu); }
    if ((disp == 3 || disp == 5) && !exists) {
        LOG("CreateFileA(%s): not found\n", name);
        SETERR(2); RET_STD(7, 0xffffffffu);
    }
    int create = disp == 1 || disp == 2 || disp == 4;
    int trunc = disp == 2 || disp == 5;
    int fd = plat_open(host, write || create, create, trunc);
    if (fd < 0) {
        LOG("CreateFileA(%s -> %s): open failed\n", name, host);
        SETERR(create ? 3 : 2); RET_STD(7, 0xffffffffu);
    }
    if (create && !exists) vfs_invalidate();
    int i = h_alloc(H_FILE);
    handles[i].fd = fd;
    LOG("CreateFileA(%s) -> %x\n", name, I2H(i));
    SETERR((disp == 2 || disp == 4) && exists ? 183 : 0);
    RET_STD(7, I2H(i));
}
HLE(KERNEL32, ReadFile) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pread = ARG(3);
    int i = h_get(h, H_FILE);
    if (i < 0) { SETERR(6); RET_STD(5, 0); }
    int r = plat_read(handles[i].fd, PTR(buf), n);
    if (pread) M32(pread) = r < 0 ? 0 : (uint32_t)r;
    RET_STD(5, r >= 0);
}
HLE(KERNEL32, WriteFile) {
    uint32_t h = ARG(0), buf = ARG(1), n = ARG(2), pw = ARG(3);
    if (h >= 0xfffffff4u) {
        char tmp[1024]; uint32_t m = n < 1023 ? n : 1023;
        memcpy(tmp, PTR(buf), m); tmp[m] = 0;
        logf_("%s", tmp);
        if (pw) M32(pw) = n;
        RET_STD(5, 1);
    }
    int i = h_get(h, H_FILE);
    if (i < 0) { SETERR(6); RET_STD(5, 0); }
    int r = plat_write(handles[i].fd, PTR(buf), n);
    if (pw) M32(pw) = r < 0 ? 0 : (uint32_t)r;
    RET_STD(5, r >= 0);
}
HLE(KERNEL32, SetFilePointer) {
    uint32_t h = ARG(0); int32_t lo = (int32_t)ARG(1); uint32_t phi = ARG(2), method = ARG(3);
    int i = h_get(h, H_FILE);
    if (i < 0) { SETERR(6); RET_STD(4, 0xffffffffu); }
    int64_t off = lo;
    if (phi) off = (int64_t)(((uint64_t)M32(phi) << 32) | (uint32_t)lo);
    int64_t r = plat_seek(handles[i].fd, off, (int)method);
    if (r < 0) { SETERR(131); RET_STD(4, 0xffffffffu); }
    if (phi) M32(phi) = (uint32_t)(r >> 32);
    SETERR(0);
    RET_STD(4, (uint32_t)r);
}
HLE(KERNEL32, SetEndOfFile) { RET_STD(1, 1); }
HLE(KERNEL32, FlushFileBuffers) { RET_STD(1, 1); }
HLE(KERNEL32, DeleteFileA) {
    char host[520]; vfs_resolve(ARGP(0), host, sizeof host);
    int r = plat_unlink(host) == 0; vfs_invalidate();
    RET_STD(1, r);
}
HLE(KERNEL32, MoveFileA) {
    char a[520], b[520];
    vfs_resolve(ARGP(0), a, sizeof a); vfs_resolve(ARGP(1), b, sizeof b);
    int r = plat_rename(a, b) == 0; vfs_invalidate();
    RET_STD(2, r);
}
HLE(KERNEL32, CreateDirectoryA) {
    char host[520];
    if (vfs_resolve(ARGP(0), host, sizeof host)) { SETERR(183); RET_STD(2, 0); }
    int r = plat_mkdir(host) == 0; vfs_invalidate();
    RET_STD(2, r);
}
HLE(KERNEL32, GetCurrentDirectoryA) {
    uint32_t n = ARG(0), buf = ARG(1);
    uint32_t l = (uint32_t)strlen(exe_dir_win);
    if (n <= l) RET_STD(2, l + 1);
    strcpy((char *)PTR(buf), exe_dir_win);
    RET_STD(2, l);
}
HLE(KERNEL32, GetTempFileNameA) {
    static int ctr;
    sprintf((char *)PTR(ARG(3)), "%s\\tmp%04x.tmp", ARGP(0), ++ctr);
    RET_STD(4, ctr);
}
HLE(KERNEL32, GetDiskFreeSpaceA) {
    if (ARG(1)) M32(ARG(1)) = 8; if (ARG(2)) M32(ARG(2)) = 512;
    if (ARG(3)) M32(ARG(3)) = 0x100000; if (ARG(4)) M32(ARG(4)) = 0x200000;
    RET_STD(5, 1);
}
HLE(KERNEL32, GetDriveTypeA) {
    const char *r = ARGP(0);
    LOG("GetDriveTypeA(%s)\n", r ? r : "(null)");
    RET_STD(1, (r && (r[0] == 'C' || r[0] == 'c')) || !r ? 3 : 1);
}
HLE(KERNEL32, GetVolumeInformationA) {
    LOG("GetVolumeInformationA(%s)\n", ARGP(0) ? ARGP(0) : "(null)");
    if (ARG(1)) strcpy(ARGP(1), "CULTURES");
    if (ARG(3)) M32(ARG(3)) = 0x12345678;
    if (ARG(4)) M32(ARG(4)) = 255;
    if (ARG(5)) M32(ARG(5)) = 0;
    if (ARG(6)) strcpy(ARGP(6), "NTFS");
    RET_STD(8, 1);
}

/* FindFirstFile / FindNextFile with simple wildcard matching */
static int wildmatch(const char *p, const char *s) {
    for (; *p; p++, s++) {
        if (*p == '*') {
            while (p[1] == '*') p++;
            if (!p[1]) return 1;
            for (; *s; s++) if (wildmatch(p + 1, s)) return 1;
            return wildmatch(p + 1, s);
        }
        if (!*s) return strcmp(p, ".*") == 0 || strcmp(p, ".") == 0;
        if (*p != '?' && tolower((unsigned char)*p) != tolower((unsigned char)*s)) return 0;
    }
    return *s == 0;
}
static int find_fill(int i, uint32_t fd) {
    while (handles[i].pos < handles[i].n) {
        char *nm = handles[i].names;
        for (int k = 0; k < handles[i].pos; k++) nm += strlen(nm) + 1;
        int isdir = handles[i].isdir[handles[i].pos];
        handles[i].pos++;
        if (!wildmatch(handles[i].pattern, nm)) continue;
        memset(PTR(fd), 0, 320);
        M32(fd) = isdir ? 0x10 : 0x80;
        char path[1100]; uint32_t size = 0; int d;
        snprintf(path, sizeof path, "%s/%s", handles[i].dir, nm);
        plat_stat(path, &d, &size);
        M32(fd + 32) = size;
        M64(fd + 20) = 0x01c3000000000000ull;   /* some last-write time */
        snprintf((char *)PTR(fd + 44), 260, "%s", nm);
        return 1;
    }
    return 0;
}
HLE(KERNEL32, FindFirstFileA) {
    const char *pat = ARGP(0);
    uint32_t fd = ARG(1);
    char dirwin[520], *bs;
    snprintf(dirwin, sizeof dirwin, "%s", pat);
    for (char *c = dirwin; *c; c++) if (*c == '/') *c = '\\';
    bs = strrchr(dirwin, '\\');
    const char *mask = bs ? bs + 1 : dirwin;
    char maskbuf[260]; snprintf(maskbuf, sizeof maskbuf, "%s", mask);
    if (bs) *bs = 0; else strcpy(dirwin, ".");
    char host[520];
    if (!vfs_resolve(dirwin, host, sizeof host)) { SETERR(3); RET_STD(2, 0xffffffffu); }
    int i = h_alloc(H_FIND);
    handles[i].names = malloc(1 << 18); handles[i].isdir = malloc(8192);
    handles[i].n = plat_listdir(host, handles[i].names, 1 << 18, handles[i].isdir, 8192);
    snprintf(handles[i].pattern, sizeof handles[i].pattern, "%s", maskbuf);
    snprintf(handles[i].dir, sizeof handles[i].dir, "%s", host);
    if (!find_fill(i, fd)) {
        free(handles[i].names); free(handles[i].isdir); handles[i].type = H_FREE;
        LOG("FindFirstFileA(%s): none\n", pat);
        SETERR(2); RET_STD(2, 0xffffffffu);
    }
    LOG("FindFirstFileA(%s): %s\n", pat, (char *)PTR(fd + 44));
    RET_STD(2, I2H(i));
}
HLE(KERNEL32, FindNextFileA) {
    int i = h_get(ARG(0), H_FIND);
    if (i < 0 || !find_fill(i, ARG(1))) { SETERR(18); RET_STD(2, 0); }
    RET_STD(2, 1);
}
HLE(KERNEL32, FindClose) {
    int i = h_get(ARG(0), H_FIND);
    if (i >= 0) { free(handles[i].names); free(handles[i].isdir); handles[i].type = H_FREE; }
    RET_STD(1, 1);
}

/* ------------------------------------------------------------- modules */
struct native_export { const char *dll, *name; gfn_t fn; };
extern const struct native_export native_exports[];

HLE(KERNEL32, LoadLibraryA) {
    const char *n = ARGP(0);
    LOG("LoadLibraryA(%s)\n", n);
    static const char *known[] = {"ddraw.dll", "dsound.dll", "dinput.dll", "dplayx.dll", NULL};
    const char *base = strrchr(n, '\\'); base = base ? base + 1 : n;
    if (strcasecmp(base, "gedx8musicdrv.dll") == 0) {
        static uint32_t loaded;
        if (!loaded) {
            char host[520];
            if (!vfs_resolve(n, host, sizeof host)) { SETERR(126); RET_STD(1, 0); }
            uint32_t entry, esp = R.esp;
            loaded = map_pe(host, &entry, NULL);
            uint32_t ok = CALL_GUEST(entry, loaded, 1, 0);     /* DllMain(DLL_PROCESS_ATTACH) */
            R.esp = esp;
            LOG("gedx8musicdrv.dll mapped at %08x, DllMain -> %u\n", loaded, ok);
        }
        RET_STD(1, loaded);
    }
    for (int i = 0; known[i]; i++)
        if (strcasecmp(base, known[i]) == 0 || (strncasecmp(base, known[i], strlen(known[i]) - 4) == 0 && !base[strlen(known[i]) - 4]))
            RET_STD(1, 0x30000000u + 0x10000u * (uint32_t)i);
    SETERR(126);
    RET_STD(1, 0);
}
HLE(KERNEL32, FreeLibrary) { RET_STD(1, 1); }
HLE(KERNEL32, GetProcAddress) {
    uint32_t mod = ARG(0);
    const char *name = ARG(1) < 0x10000 ? "#ord" : ARGP(1);
    static const char *dlls[] = {"ddraw.dll", "dsound.dll", "dinput.dll", "dplayx.dll"};
    const char *dll = NULL;
    if (mod == DLL_BASE) {
        uint32_t a = ARG(1) >= 0x10000 ? pe_export(mod, name) : 0;
        LOG("GetProcAddress(gedx8musicdrv, %s) -> %08x\n", name, a);
        RET_STD(2, a);
    }
    if (mod >= 0x30000000u && mod < 0x30040000u) dll = dlls[(mod - 0x30000000u) >> 16];
    for (int i = 0; native_exports[i].name; i++)
        if ((!dll || strcasecmp(native_exports[i].dll, dll) == 0) && strcmp(native_exports[i].name, name) == 0) {
            LOG("GetProcAddress(%s, %s)\n", dll ? dll : "?", name);
            RET_STD(2, thunk_register(native_exports[i].fn, native_exports[i].name));
        }
    LOG("GetProcAddress(%08x, %s) -> NULL\n", mod, name);
    SETERR(127);
    RET_STD(2, 0);
}

/* ------------------------------------------------------------- exceptions */
HLE(KERNEL32, RaiseException) {
    uint32_t code = ARG(0), nargs = ARG(2), args = ARG(3);
    if (code == 0xe06d7363 && nargs >= 3) {
        /* MSVC C++ exception: args[1] = object, args[2] = ThrowInfo */
        uint32_t obj = M32(args + 4), ti = M32(args + 8);
        uint32_t cta = ti ? M32(ti + 12) : 0;
        uint32_t ct = cta ? M32(cta + 4) : 0;
        uint32_t td = ct ? M32(ct + 4) : 0;
        fatal("C++ exception thrown: type %s, object %08x (from %08x)", td ? (char *)PTR(td + 8) : "?", obj, M32(R.esp));
    }
    fatal("RaiseException(%08x) from %08x", code, M32(R.esp));
}
HLE(KERNEL32, RtlUnwind) { RET_STD(4, 0); }
HLE(KERNEL32, UnhandledExceptionFilter) { RET_STD(1, 1); }

/* ------------------------------------------------------------- used by gedx8musicdrv.dll */
HLE(KERNEL32, GlobalAlloc) {
    uint32_t flags = ARG(0), n = ARG(1);
    uint32_t p = heap_alloc(n ? n : 1, flags & 0x40);   /* GMEM_ZEROINIT; handles == pointers */
    RET_STD(2, p);
}
HLE(KERNEL32, GlobalLock) { RET_STD(1, ARG(0)); }
HLE(KERNEL32, GlobalUnlock) { RET_STD(1, 1); }
HLE(KERNEL32, GlobalFree) { heap_free(ARG(0)); RET_STD(1, 0); }
HLE(KERNEL32, IsBadWritePtr) { RET_STD(2, ARG(0) < 0x10000 || ARG(0) >= GUEST_END); }
HLE(KERNEL32, IsBadReadPtr) { RET_STD(2, ARG(0) < 0x10000 || ARG(0) >= GUEST_END); }
HLE(KERNEL32, IsBadCodePtr) { RET_STD(1, ARG(0) < 0x10000 || ARG(0) >= GUEST_END); }
HLE(KERNEL32, SetUnhandledExceptionFilter) { RET_STD(1, 0); }
HLE(KERNEL32, TlsFree) { RET_STD(1, 1); }
