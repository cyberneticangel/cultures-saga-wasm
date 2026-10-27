/* Browser platform layer: thin wrappers over JS imports (see web/worker.js). */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>

#define IMPORT(name) __attribute__((import_module("env"), import_name(#name)))

IMPORT(js_ticks_ms) double js_ticks_ms(void);
IMPORT(js_sleep) void js_sleep(int ms);
IMPORT(js_game_speed) int js_game_speed(void);
IMPORT(js_qol_get) int js_qol_get(int slot);
IMPORT(js_qol_set) void js_qol_set(int slot, int v);
IMPORT(js_log) void js_log(const char *s, int len);
IMPORT(js_exit) void js_exit(int code);
IMPORT(js_set_mode) void js_set_mode(int w, int h);
IMPORT(js_present) void js_present(const void *pixels, int w, int h, int fmt);
IMPORT(js_poll_event) int js_poll_event(int *ev);
IMPORT(js_open) int js_open(const char *path, int write, int create, int trunc);
IMPORT(js_read) int js_read(int fd, void *buf, int n);
IMPORT(js_write) int js_write(int fd, const void *buf, int n);
IMPORT(js_seek) double js_seek(int fd, double off, int whence);
IMPORT(js_size) double js_size(int fd);
IMPORT(js_close) void js_close(int fd);
IMPORT(js_unlink) int js_unlink(const char *path);
IMPORT(js_mkdir) int js_mkdir(const char *path);
IMPORT(js_rename) int js_rename(const char *a, const char *b);
IMPORT(js_listdir) int js_listdir(const char *path, char *names, int cap, uint8_t *is_dir, int maxn);
IMPORT(js_stat) int js_stat(const char *path, int *is_dir, uint32_t *size);
IMPORT(js_set_cursor) void js_set_cursor(int id, const uint32_t *rgba, int w, int h, int hx, int hy);
IMPORT(js_audio_open) void js_audio_open(int rate);
IMPORT(js_audio_queued) int js_audio_queued(void);
IMPORT(js_audio_push) void js_audio_push(const int16_t *frames, int n);
IMPORT(js_net_open) void js_net_open(const char *url);
IMPORT(js_net_state) int js_net_state(void);
IMPORT(js_net_send) int js_net_send(const void *buf, int n);
IMPORT(js_net_recv) int js_net_recv(void *buf, int cap);
IMPORT(js_net_close) void js_net_close(void);

void plat_init(void) {}
uint32_t *cursor_decode(const char *host_path, int *w, int *h, int *hx, int *hy);
static char cur_paths[16][260];
static int ncur, cur_shown = 1, cur_id = -2;
void plat_set_cursor(const char *path) {
    if (!path) { cur_id = -1; js_set_cursor(-1, 0, 0, 0, 0, 0); return; }
    if (!path[0]) { cur_id = -2; if (cur_shown) js_set_cursor(-2, 0, 0, 0, 0, 0); return; }
    for (int i = 0; i < ncur; i++)
        if (strcmp(cur_paths[i], path) == 0) { cur_id = i; if (cur_shown) js_set_cursor(i, 0, 0, 0, 0, 0); return; }
    int w, h, hx, hy;
    uint32_t *px = cursor_decode(path, &w, &h, &hx, &hy);
    if (!px || ncur >= 16) return;
    snprintf(cur_paths[ncur], 260, "%s", path);
    cur_id = ncur++;
    js_set_cursor(cur_id, px, w, h, hx, hy);   /* registers + selects */
    if (!cur_shown) js_set_cursor(-1, 0, 0, 0, 0, 0);
    free(px);
}
void plat_show_cursor(int show) {
    cur_shown = show;
    js_set_cursor(show ? cur_id : -1, 0, 0, 0, 0, 0);
}
uint32_t plat_ticks_ms(void) { return (uint32_t)(uint64_t)js_ticks_ms(); }
uint64_t plat_ticks_us(void) { return (uint64_t)(js_ticks_ms() * 1000.0); }
void plat_sleep(uint32_t ms) { js_sleep((int)ms); }
int plat_game_speed(void) { return js_game_speed(); }
int plat_qol_get(int slot) { return js_qol_get(slot); }
void plat_qol_set(int slot, int v) { js_qol_set(slot, v); }
void plat_log(const char *s) { js_log(s, (int)strlen(s)); }
void plat_exit(int code) { js_exit(code); __builtin_trap(); }
void plat_set_mode(int w, int h) { js_set_mode(w, h); }
void plat_present(const void *pixels, int w, int h, int fmt) { js_present(pixels, w, h, fmt); }
int plat_poll_event(plat_event *ev) {
    int e[5];
    if (!js_poll_event(e)) return 0;
    ev->type = e[0]; ev->x = e[1]; ev->y = e[2]; ev->button = e[3]; ev->key = e[4];
    return 1;
}
int plat_open(const char *p, int w, int c, int t) { return js_open(p, w, c, t); }
int plat_read(int fd, void *b, uint32_t n) { return js_read(fd, b, (int)n); }
int plat_write(int fd, const void *b, uint32_t n) { return js_write(fd, b, (int)n); }
int64_t plat_seek(int fd, int64_t off, int whence) { return (int64_t)js_seek(fd, (double)off, whence); }
int64_t plat_size(int fd) { return (int64_t)js_size(fd); }
void plat_close(int fd) { js_close(fd); }
int plat_unlink(const char *p) { return js_unlink(p); }
int plat_mkdir(const char *p) { return js_mkdir(p); }
int plat_rename(const char *a, const char *b) { return js_rename(a, b); }
int plat_listdir(const char *p, char *names, int cap, uint8_t *is_dir, int maxn) { return js_listdir(p, names, cap, is_dir, maxn); }
int plat_stat(const char *p, int *is_dir, uint32_t *size) { return js_stat(p, is_dir, size); }
void plat_audio_open(int rate) { js_audio_open(rate); }
int plat_audio_queued(void) { return js_audio_queued(); }
void plat_audio_push(const int16_t *f, int n) { js_audio_push(f, n); }
void plat_net_open(const char *url) { js_net_open(url ? url : ""); }
int plat_net_state(void) { return js_net_state(); }
int plat_net_send(const void *b, int n) { return js_net_send(b, n); }
int plat_net_recv(void *b, int cap) { return js_net_recv(b, cap); }
void plat_net_close(void) { js_net_close(); }

void game_start(void);
/* flags: bit 0 = skip the intro videos */
__attribute__((export_name("game_main"))) int game_main(int loglevel, int flags) {
    log_level = loglevel;
    skip_intro = flags & 1;
    {
        extern unsigned char __heap_base, __data_end, __global_base;
        int local;
        LOG("wasm layout: global_base=%p data_end=%p heap_base=%p stack~%p\n",
            (void *)&__global_base, (void *)&__data_end, (void *)&__heap_base, (void *)&local);
    }
    game_start();
    return 0;
}
