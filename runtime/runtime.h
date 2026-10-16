/* Runtime-internal API: HLE helpers, guest heap, platform layer. */
#pragma once
#include "recomp.h"
#include <stddef.h>
#include <stdbool.h>

#define VA_BASE  0x30000000u   /* VirtualAlloc region (to GUEST_END) */
#define DLL_BASE 0x10000000u   /* recompiled DLLs (gedx8musicdrv.dll) */

/* ---- logging ---- */
void fatal(const char *fmt, ...) __attribute__((noreturn, format(printf, 1, 2)));
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
extern int log_level;
#define LOG(...) do { if (log_level > 0) logf_(__VA_ARGS__); } while (0)
#define TRACE(...) do { if (log_level > 1) logf_(__VA_ARGS__); } while (0)

/* ---- HLE calling convention helpers ----
 * On entry to an import implementation R.esp points at the return address and
 * the arguments follow. */
#define ARG(i)  M32(R.esp + 4 + 4 * (i))
#define ARGP(i) ((char *)PTR(ARG(i)))
#define RET_STD(n, v) do { R.eax = (uint32_t)(v); R.esp += 4 + 4 * (n); return; } while (0)
#define RET_CDECL(v)  do { R.eax = (uint32_t)(v); R.esp += 4; return; } while (0)
#define HLE(dll, name) void imp_##dll##_##name(void)

uint32_t thunk_register(gfn_t fn, const char *name);
const char *thunk_name(uint32_t addr);
void dispatch_init(void);
uint32_t call_guest(uint32_t addr, int nargs, const uint32_t *args);
#define CALL_GUEST(addr, ...) ({ uint32_t a_[] = {__VA_ARGS__}; call_guest((addr), sizeof(a_) / 4, a_); })

/* ---- guest memory ---- */
void mem_init(void);
uint32_t heap_alloc(uint32_t size, int zero);
void heap_free(uint32_t p);
uint32_t heap_realloc(uint32_t p, uint32_t size, int zero);
uint32_t heap_size(uint32_t p);
uint32_t va_alloc(uint32_t addr, uint32_t size, uint32_t type);
int va_free(uint32_t addr, uint32_t size, uint32_t type);
uint32_t guest_strdup(const char *s);
uint32_t guest_alloc_static(uint32_t size);   /* never freed, zeroed */

/* ---- loader ---- */
void load_image(const char *exe_path);
uint32_t map_pe(const char *path, uint32_t *entry_out, uint32_t *size_out);
uint32_t pe_export(uint32_t base, const char *name);
void setup_process(const char *cmdline);
extern uint32_t image_entry;

/* ---- COM helper: build a guest object whose vtable points at native thunks ---- */
uint32_t com_make_vtbl(const gfn_t *methods, const char *const *names, int n);
uint32_t com_new(uint32_t vtbl, uint32_t extra_bytes);

/* ---- platform ---- */
enum { EV_NONE, EV_MOUSEMOVE, EV_MOUSEDOWN, EV_MOUSEUP, EV_KEYDOWN, EV_KEYUP, EV_CHAR, EV_QUIT, EV_WHEEL };
typedef struct { int type, x, y, button, key; } plat_event;
void plat_init(void);
uint32_t plat_ticks_ms(void);
uint64_t plat_ticks_us(void);
void plat_sleep(uint32_t ms);
int  plat_poll_event(plat_event *ev);
void plat_set_mode(int w, int h);
enum { PLAT_RGBA, PLAT_RGB565 };
/* pixels: w*h tightly packed, PLAT_RGBA = 0xAABBGGRR little-endian RGBA bytes, PLAT_RGB565 = 16-bit 5:6:5
 * (the game's own format: the display converts it, which is cheaper than doing it here) */
void plat_present(const void *pixels, int w, int h, int fmt);
void plat_log(const char *s);
void plat_set_cursor(const char *cur_path);   /* NULL = hidden, "" = default arrow */
void plat_show_cursor(int show);
void plat_exit(int code) __attribute__((noreturn));
/* files: host handle >= 0 or -1.  Paths are already resolved, relative to the game root, '/' separated */
int  plat_open(const char *path, int write, int create, int truncate);
int  plat_read(int fd, void *buf, uint32_t n);
int  plat_write(int fd, const void *buf, uint32_t n);
int64_t plat_seek(int fd, int64_t off, int whence);
int64_t plat_size(int fd);
void plat_close(int fd);
int  plat_unlink(const char *path);
int  plat_mkdir(const char *path);
int  plat_rename(const char *from, const char *to);
/* directory listing: returns number of entries written to names (NUL separated), is_dir flags */
int  plat_listdir(const char *path, char *names, int cap, uint8_t *is_dir, int maxn);
int  plat_stat(const char *path, int *is_dir, uint32_t *size);
/* network: one message-oriented connection to the multiplayer relay (WebSocket, see relay.py).
 * url NULL = platform default.  state: 0 closed, 1 connecting, 2 open, 3 failed/lost */
void plat_net_open(const char *url);
int  plat_net_state(void);
int  plat_net_send(const void *buf, int len);
int  plat_net_recv(void *buf, int cap);    /* length of the next message, 0 if none */
void plat_net_close(void);
/* audio: interleaved stereo s16 at 22050/44100 Hz */
void plat_audio_open(int rate);
int  plat_audio_queued(void);   /* frames queued but not yet played */
void plat_audio_push(const int16_t *frames, int n);

/* ---- subsystems ---- */
void ddraw_present_if_dirty(void);
void input_pump(void);         /* pulls platform events into the message queue / dinput state */
void audio_update(void);
uint32_t vfs_resolve(const char *winpath, char *out, int cap);
extern const char *game_exe_name;   /* main.c: the game executable in the game folder (found by CRC) */   /* windows path -> host relative */
extern char exe_dir_win[260];
/* multiplayer (dplay.c): DirectPlay 8 over the relay, plus the game's network threads as cooperative tasks */
uint32_t dplay_create(uint32_t clsid, uint32_t iid, uint32_t ppv);
uint32_t video_create(uint32_t clsid, uint32_t iid, uint32_t ppv);   /* video.c: FMV playback */
void bg_pump(void);                       /* network + background tasks; call from message pump/waits */
int  coop_thread_create(uint32_t proc, uint32_t param);   /* -1 if proc is not a known game thread */
int  coop_thread_wait(int id, uint32_t timeout_ms);        /* 1 = finished, 0 = timeout */
