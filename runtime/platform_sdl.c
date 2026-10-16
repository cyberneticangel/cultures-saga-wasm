/* Native (Linux) platform layer using SDL2 - used for development/debugging. */
#include "runtime.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <signal.h>

static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *tex;
static int tex_w, tex_h;
static SDL_AudioDeviceID adev;
static int headless;
static int frame_dump;   /* dump every Nth presented frame to frames/NNNN.ppm */
static int frame_no;
static uint32_t *last_frame; static int last_w, last_h;
static char shot_path[512];
static void save_ppm(const char *path, const uint32_t *rgba, int w, int h);

void plat_init(void) {
    headless = getenv("HEADLESS") != NULL;
    if (getenv("DUMP_FRAMES")) frame_dump = atoi(getenv("DUMP_FRAMES"));
    if (SDL_Init(headless ? SDL_INIT_TIMER : SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_TIMER) != 0)
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
}
uint32_t plat_ticks_ms(void) { return SDL_GetTicks(); }
uint64_t plat_ticks_us(void) { return SDL_GetPerformanceCounter() * 1000000ull / SDL_GetPerformanceFrequency(); }
void plat_sleep(uint32_t ms) { SDL_Delay(ms); }
void plat_log(const char *s) { fputs(s, stderr); }
void plat_exit(int code) { fflush(stderr); exit(code); }
static void on_term(int sig) { (void)sig; exit(0); }

void plat_set_mode(int w, int h) {
    if (headless) return;
    if (!win) {
        win = SDL_CreateWindow("Cultures - Die Saga", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, SDL_WINDOW_RESIZABLE);
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED);
    } else {
        SDL_SetWindowSize(win, w, h);
    }
    SDL_RenderSetLogicalSize(ren, w, h);
}

void plat_present(const uint32_t *rgba, int w, int h) {
    if (getenv("CTL_FILE")) {
        if (last_w != w || last_h != h) { free(last_frame); last_frame = malloc((size_t)w * h * 4); last_w = w; last_h = h; }
        memcpy(last_frame, rgba, (size_t)w * h * 4);
        if (shot_path[0]) { save_ppm(shot_path, rgba, w, h); shot_path[0] = 0; }
    }
    if (frame_dump && (frame_no++ % frame_dump) == 0) {
        char name[600]; snprintf(name, sizeof name, "%s/%05d.ppm", getenv("FRAMES_DIR") ? getenv("FRAMES_DIR") : "/tmp", frame_no / frame_dump);
        FILE *f = fopen(name, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", w, h);
            for (int i = 0; i < w * h; i++) { uint32_t p = rgba[i]; fputc(p & 0xff, f); fputc((p >> 8) & 0xff, f); fputc((p >> 16) & 0xff, f); }
            fclose(f);
        }
    }
    if (headless) return;
    if (!win) plat_set_mode(w, h);
    if (!tex || tex_w != w || tex_h != h) {
        if (tex) SDL_DestroyTexture(tex);
        tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STREAMING, w, h);
        tex_w = w; tex_h = h;
    }
    SDL_UpdateTexture(tex, NULL, rgba, w * 4);
    SDL_RenderClear(ren);
    SDL_RenderCopy(ren, tex, NULL, NULL);
    SDL_RenderPresent(ren);
}

uint32_t *cursor_decode(const char *host_path, int *w, int *h, int *hx, int *hy);
static struct { char path[260]; SDL_Cursor *c; } curs[16];
static int ncurs, cursor_shown = 1;
void plat_set_cursor(const char *path) {
    if (headless) return;
    if (!path) { SDL_ShowCursor(SDL_DISABLE); return; }
    SDL_ShowCursor(cursor_shown ? SDL_ENABLE : SDL_DISABLE);
    if (!path[0]) { SDL_SetCursor(SDL_GetDefaultCursor()); return; }
    for (int i = 0; i < ncurs; i++) if (strcmp(curs[i].path, path) == 0) { SDL_SetCursor(curs[i].c); return; }
    int w, h, hx, hy;
    uint32_t *px = cursor_decode(path, &w, &h, &hx, &hy);
    if (!px || ncurs >= 16) return;
    SDL_Surface *sf = SDL_CreateRGBSurfaceWithFormatFrom(px, w, h, 32, w * 4, SDL_PIXELFORMAT_ABGR8888);
    SDL_Cursor *c = SDL_CreateColorCursor(sf, hx, hy);
    SDL_FreeSurface(sf); free(px);
    snprintf(curs[ncurs].path, 260, "%s", path); curs[ncurs].c = c; ncurs++;
    SDL_SetCursor(c);
}
void plat_show_cursor(int show) { cursor_shown = show; if (!headless) SDL_ShowCursor(show ? SDL_ENABLE : SDL_DISABLE); }

static int sdl_to_vk(SDL_Keycode k, SDL_Scancode sc) {
    if (k >= 'a' && k <= 'z') return (int)(k - 'a' + 'A');
    if (k >= '0' && k <= '9') return (int)k;
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return 0x70 + (sc - SDL_SCANCODE_F1);
    switch (k) {
    case SDLK_ESCAPE: return 0x1B; case SDLK_RETURN: return 0x0D; case SDLK_BACKSPACE: return 0x08;
    case SDLK_TAB: return 0x09; case SDLK_SPACE: return 0x20;
    case SDLK_LEFT: return 0x25; case SDLK_UP: return 0x26; case SDLK_RIGHT: return 0x27; case SDLK_DOWN: return 0x28;
    case SDLK_INSERT: return 0x2D; case SDLK_DELETE: return 0x2E; case SDLK_HOME: return 0x24; case SDLK_END: return 0x23;
    case SDLK_PAGEUP: return 0x21; case SDLK_PAGEDOWN: return 0x22; case SDLK_PAUSE: return 0x13;
    case SDLK_LSHIFT: return 0xA0; case SDLK_RSHIFT: return 0xA1; case SDLK_LCTRL: return 0xA2; case SDLK_RCTRL: return 0xA3;
    case SDLK_LALT: return 0xA4; case SDLK_RALT: return 0xA5;
    case SDLK_KP_PLUS: return 0x6B; case SDLK_KP_MINUS: return 0x6D; case SDLK_PLUS: return 0xBB; case SDLK_MINUS: return 0xBD;
    case SDLK_KP_0: case SDLK_KP_1: case SDLK_KP_2: case SDLK_KP_3: case SDLK_KP_4:
    case SDLK_KP_5: case SDLK_KP_6: case SDLK_KP_7: case SDLK_KP_8: case SDLK_KP_9:
        return 0x60 + (k == SDLK_KP_0 ? 0 : (int)(k - SDLK_KP_1) + 1);
    }
    return 0;
}

/* ---- scripted control (CTL_FILE): lines "move x y", "click x y [b]", "rclick x y",
 * "key VK", "text str", "shot path.ppm" ---- */
static FILE *ctl_f;
static plat_event pending[64];
static int npending, pending_pos;
static void save_ppm(const char *path, const uint32_t *rgba, int w, int h) {
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int i = 0; i < w * h; i++) { uint32_t p = rgba[i]; fputc(p & 0xff, f); fputc((p >> 8) & 0xff, f); fputc((p >> 16) & 0xff, f); }
    fclose(f);
}
static void q(int type, int x, int y, int b, int k) {
    if (npending < 64) { plat_event *e = &pending[npending++]; e->type = type; e->x = x; e->y = y; e->button = b; e->key = k; }
}
static uint32_t ctl_hold_until;
static int ctl_poll(plat_event *ev) {
    if (pending_pos < npending) {
        if (plat_ticks_ms() < ctl_hold_until) return 0;
        *ev = pending[pending_pos++];
        if (ev->type == EV_MOUSEDOWN || ev->type == EV_KEYDOWN) ctl_hold_until = plat_ticks_ms() + 120;
        if (pending_pos == npending) npending = pending_pos = 0;
        return 1;
    }
    if (!ctl_f) {
        const char *p = getenv("CTL_FILE");
        if (!p || !(ctl_f = fopen(p, "r"))) return 0;
    }
    char line[512];
    clearerr(ctl_f);
    if (!fgets(line, sizeof line, ctl_f)) return 0;
    int x, y, b = 0, k;
    char str[480];
    if (sscanf(line, "click %d %d %d", &x, &y, &b) >= 2) {
        q(EV_MOUSEMOVE, x, y, 0, 0); q(EV_MOUSEDOWN, x, y, b, 0); q(EV_MOUSEUP, x, y, b, 0);
    } else if (sscanf(line, "rclick %d %d", &x, &y) == 2) {
        q(EV_MOUSEMOVE, x, y, 0, 0); q(EV_MOUSEDOWN, x, y, 1, 0); q(EV_MOUSEUP, x, y, 1, 0);
    } else if (sscanf(line, "move %d %d", &x, &y) == 2) {
        q(EV_MOUSEMOVE, x, y, 0, 0);
    } else if (sscanf(line, "down %d %d %d", &x, &y, &b) >= 2) {
        q(EV_MOUSEMOVE, x, y, 0, 0); q(EV_MOUSEDOWN, x, y, b, 0);
    } else if (sscanf(line, "up %d %d %d", &x, &y, &b) >= 2) {
        q(EV_MOUSEMOVE, x, y, 0, 0); q(EV_MOUSEUP, x, y, b, 0);
    } else if (sscanf(line, "key %i", &k) == 1) {
        q(EV_KEYDOWN, 0, 0, 0, k); q(EV_KEYUP, 0, 0, 0, k);
    } else if (sscanf(line, "text %479[^\n]", str) == 1) {
        for (char *c = str; *c; c++) q(EV_CHAR, 0, 0, 0, (unsigned char)*c);
    } else if (sscanf(line, "shot %479s", str) == 1) {
        if (last_frame) save_ppm(str, last_frame, last_w, last_h);
        else snprintf(shot_path, sizeof shot_path, "%s", str);
    } else if (strncmp(line, "quit", 4) == 0) {
        q(EV_QUIT, 0, 0, 0, 0);
    }
    return ctl_poll(ev);
}

int plat_poll_event(plat_event *ev) {
    if (ctl_poll(ev)) return 1;
    if (headless) return 0;
    SDL_Event e;
    while (SDL_PollEvent(&e)) {
        memset(ev, 0, sizeof *ev);
        switch (e.type) {
        case SDL_QUIT: ev->type = EV_QUIT; return 1;
        case SDL_MOUSEMOTION: ev->type = EV_MOUSEMOVE; ev->x = e.motion.x; ev->y = e.motion.y; return 1;
        case SDL_MOUSEBUTTONDOWN: case SDL_MOUSEBUTTONUP:
            ev->type = e.type == SDL_MOUSEBUTTONDOWN ? EV_MOUSEDOWN : EV_MOUSEUP;
            ev->x = e.button.x; ev->y = e.button.y;
            ev->button = e.button.button == SDL_BUTTON_LEFT ? 0 : e.button.button == SDL_BUTTON_RIGHT ? 1 : 2;
            return 1;
        case SDL_MOUSEWHEEL: ev->type = EV_WHEEL; ev->y = e.wheel.y; return 1;
        case SDL_KEYDOWN: case SDL_KEYUP: {
            int vk = sdl_to_vk(e.key.keysym.sym, e.key.keysym.scancode);
            if (!vk) continue;
            ev->type = e.type == SDL_KEYDOWN ? EV_KEYDOWN : EV_KEYUP; ev->key = vk;
            return 1;
        }
        case SDL_TEXTINPUT: {
            unsigned char c = (unsigned char)e.text.text[0];
            if (c >= 0x80) {   /* UTF-8 -> Latin-1 for the common two-byte case */
                if ((c & 0xe0) == 0xc0) c = (unsigned char)(((c & 0x1f) << 6) | (e.text.text[1] & 0x3f)); else continue;
            }
            ev->type = EV_CHAR; ev->key = c;
            return 1;
        }
        }
    }
    return 0;
}

/* ---- files ----
 * The game directory is never written to: writes go to an overlay directory (env USERDATA,
 * default port/userdata-native), copy-on-write.  Reads prefer the overlay. */
static const char *overlay(void) {
    static const char *o;
    if (!o) {
        o = getenv("USERDATA");
        if (!o || !*o) {   /* default: one folder per game version */
            static char def[300];
            snprintf(def, sizeof def, "port/userdata-native/%s", recomp_variant);
            o = def;
        }
    }
    return o;
}
/* The game uses cp1252 (treated as Latin-1) names; the host file system uses UTF-8. */
static const char *hp(const char *p, char *out, size_t n) {
    size_t o = 0;
    for (; *p && o + 3 < n; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x80) out[o++] = (char)c;
        else { out[o++] = (char)(0xc0 | (c >> 6)); out[o++] = (char)(0x80 | (c & 0x3f)); }
    }
    out[o] = 0;
    return out;
}
static void from_utf8(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 1 < n; p++) {
        if ((p[0] & 0xe0) == 0xc0 && (p[1] & 0xc0) == 0x80) { out[o++] = (char)(((p[0] & 0x1f) << 6) | (p[1] & 0x3f)); p++; }
        else out[o++] = (char)*p;
    }
    out[o] = 0;
}
#define HP(p) hp(p, (char[1024]){0}, 1024)
static void ov_path(const char *p, char *out, size_t n) { snprintf(out, n, "%s/%s", overlay(), HP(p)); }
static int ov_exists(const char *p) { char o[1024]; struct stat st; ov_path(p, o, sizeof o); return stat(o, &st) == 0; }
static void mkdirs(const char *dir) {   /* mkdir -p */
    char t[1024]; snprintf(t, sizeof t, "%s", dir);
    for (char *c = t + 1; *c; c++) if (*c == '/') { *c = 0; mkdir(t, 0755); *c = '/'; }
    mkdir(t, 0755);
}
static void ov_parent(const char *p) {
    char o[1024]; ov_path(p, o, sizeof o);
    char *sl = strrchr(o, '/'); if (sl) { *sl = 0; mkdirs(o); }
}
static int copy_up(const char *p) {     /* copy a base file into the overlay */
    if (ov_exists(p)) return 0;
    int in = open(HP(p), O_RDONLY); if (in < 0) return -1;
    ov_parent(p);
    char o[1024]; ov_path(p, o, sizeof o);
    int out = open(o, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (out < 0) { close(in); return -1; }
    char buf[65536]; ssize_t n;
    while ((n = read(in, buf, sizeof buf)) > 0) if (write(out, buf, (size_t)n) != n) break;
    close(in); close(out);
    return 0;
}
int plat_open(const char *path, int write, int create, int truncate) {
    char o[1024]; ov_path(path, o, sizeof o);
    if (!write) return open(ov_exists(path) ? o : HP(path), O_RDONLY);
    struct stat st;
    if (!ov_exists(path)) {
        int base = stat(HP(path), &st) == 0;
        if (!base && !create) return -1;
        if (base && !truncate) copy_up(path);
        ov_parent(path);
    }
    int fl = O_RDWR;
    if (create) fl |= O_CREAT;
    if (truncate) fl |= O_TRUNC;
    return open(o, fl, 0644);
}
int plat_read(int fd, void *buf, uint32_t n) { return (int)read(fd, buf, n); }
int plat_write(int fd, const void *buf, uint32_t n) { return (int)write(fd, buf, n); }
int64_t plat_seek(int fd, int64_t off, int whence) { return lseek(fd, off, whence); }
int64_t plat_size(int fd) { struct stat st; return fstat(fd, &st) == 0 ? st.st_size : -1; }
void plat_close(int fd) { close(fd); }
/* only overlay files can be deleted or renamed; base files stay untouched */
int plat_unlink(const char *p) { char o[1024]; ov_path(p, o, sizeof o); return unlink(o); }
int plat_mkdir(const char *p) {
    struct stat st;
    if (stat(HP(p), &st) == 0 || ov_exists(p)) return -1;
    char o[1024]; ov_path(p, o, sizeof o); ov_parent(p);
    return mkdir(o, 0755);
}
int plat_rename(const char *a, const char *b) {
    if (copy_up(a) != 0) return -1;
    char oa[1024], ob[1024]; ov_path(a, oa, sizeof oa); ov_path(b, ob, sizeof ob); ov_parent(b);
    return rename(oa, ob);
}
int plat_stat(const char *p, int *is_dir, uint32_t *size) {
    struct stat st; char o[1024]; ov_path(p, o, sizeof o);
    if (stat(o, &st) != 0 && stat(HP(p), &st) != 0) return -1;
    *is_dir = S_ISDIR(st.st_mode); *size = (uint32_t)st.st_size;
    return 0;
}

static int listdir1(const char *path, char *names, int *o, int cap, uint8_t *is_dir, int n, int maxn) {
    DIR *d = opendir(path);
    if (!d) return n;
    int n0 = n;
    struct dirent *e;
    while ((e = readdir(d)) && n < maxn) {
        char nm[256]; from_utf8(e->d_name, nm, sizeof nm);
        int dup = 0;   /* skip names the overlay already listed */
        for (int i = 0, q = 0; i < n0; i++, q += (int)strlen(names + q) + 1)
            if (strcmp(names + q, nm) == 0) { dup = 1; break; }
        if (dup) continue;
        int l = (int)strlen(nm) + 1;
        if (*o + l > cap) break;
        memcpy(names + *o, nm, (size_t)l); *o += l;
        char full[1024]; snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        struct stat st;
        is_dir[n++] = stat(full, &st) == 0 && S_ISDIR(st.st_mode);
    }
    closedir(d);
    return n;
}
int plat_listdir(const char *path, char *names, int cap, uint8_t *is_dir, int maxn) {
    char o[1024]; ov_path(path, o, sizeof o);
    struct stat st;
    const char *h = HP(path);
    if (stat(h, &st) != 0 && stat(o, &st) != 0) return -1;
    int off = 0, n = listdir1(o, names, &off, cap, is_dir, 0, maxn);
    return listdir1(h, names, &off, cap, is_dir, n, maxn);
}

/* ---- network: minimal WebSocket client for the multiplayer relay (env NET_URL, default ws://127.0.0.1:8000/net) ---- */
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
static int net_fd = -1, net_st;
static uint8_t *net_in; static size_t net_in_len, net_in_cap;

void plat_net_close(void) {
    if (net_fd >= 0) close(net_fd);
    net_fd = -1; net_st = 0; net_in_len = 0;
}
static int send_all(const void *p, size_t n) {
    const uint8_t *b = p;
    while (n) {
        ssize_t k = send(net_fd, b, n, MSG_NOSIGNAL);
        if (k < 0 && (errno == EAGAIN || errno == EINTR)) { SDL_Delay(1); continue; }
        if (k <= 0) { net_st = 3; return -1; }
        b += k; n -= (size_t)k;
    }
    return 0;
}
void plat_net_open(const char *url) {
    plat_net_close();
    if (!url || !*url) url = getenv("NET_URL");
    if (!url || !*url) url = "ws://127.0.0.1:8000/net";
    char host[256] = "127.0.0.1", port[16] = "80", path[256] = "/";
    const char *h = strstr(url, "://"); h = h ? h + 3 : url;
    const char *sl = strchr(h, '/'), *co = strchr(h, ':');
    size_t hl = (size_t)((co && (!sl || co < sl)) ? co - h : sl ? sl - h : (long)strlen(h));
    snprintf(host, sizeof host, "%.*s", (int)hl, h);
    if (co && (!sl || co < sl)) snprintf(port, sizeof port, "%.*s", (int)((sl ? sl : co + strlen(co)) - co - 1), co + 1);
    if (sl) snprintf(path, sizeof path, "%s", sl);
    struct addrinfo hints = {0}, *ai;
    hints.ai_socktype = SOCK_STREAM;
    net_st = 3;
    if (getaddrinfo(host, port, &hints, &ai) != 0) return;
    net_fd = socket(ai->ai_family, SOCK_STREAM, 0);
    if (net_fd < 0 || connect(net_fd, ai->ai_addr, ai->ai_addrlen) != 0) { freeaddrinfo(ai); plat_net_close(); net_st = 3; return; }
    freeaddrinfo(ai);
    int one = 1; setsockopt(net_fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    char req[768];
    int n = snprintf(req, sizeof req, "GET %s HTTP/1.1\r\nHost: %s:%s\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                     "Sec-WebSocket-Key: Q3VsdHVyZXNTYWdhMjAwNQ==\r\nSec-WebSocket-Version: 13\r\n\r\n", path, host, port);
    send(net_fd, req, (size_t)n, MSG_NOSIGNAL);
    char resp[1024]; size_t got = 0;   /* read the handshake reply byte by byte (it is short) */
    while (got < sizeof resp - 1) {
        if (recv(net_fd, resp + got, 1, 0) != 1) break;
        got++; resp[got] = 0;
        if (got >= 4 && memcmp(resp + got - 4, "\r\n\r\n", 4) == 0) break;
    }
    if (got < 12 || memcmp(resp + 9, "101", 3) != 0) { plat_net_close(); net_st = 3; return; }
    fcntl(net_fd, F_SETFL, fcntl(net_fd, F_GETFL) | O_NONBLOCK);
    net_st = 2;
}
int plat_net_state(void) { return net_st; }
int plat_net_send(const void *buf, int len) {
    if (net_st != 2) return -1;
    uint8_t hdr[14]; int hn = 0;
    hdr[hn++] = 0x82;
    if (len < 126) hdr[hn++] = (uint8_t)(0x80 | len);
    else if (len < 65536) { hdr[hn++] = 0x80 | 126; hdr[hn++] = (uint8_t)(len >> 8); hdr[hn++] = (uint8_t)len; }
    else { hdr[hn++] = 0x80 | 127; for (int i = 7; i >= 0; i--) hdr[hn++] = (uint8_t)((uint64_t)len >> (8 * i)); }
    uint8_t mask[4] = {0x12, 0x34, 0x56, 0x78};
    memcpy(hdr + hn, mask, 4); hn += 4;
    uint8_t *m = malloc((size_t)hn + (size_t)len);
    memcpy(m, hdr, (size_t)hn);
    for (int i = 0; i < len; i++) m[hn + i] = ((const uint8_t *)buf)[i] ^ mask[i & 3];
    int r = send_all(m, (size_t)hn + (size_t)len);
    free(m);
    return r;
}
int plat_net_recv(void *buf, int cap) {
    if (net_fd < 0) return 0;
    for (;;) {
        if (net_in_cap - net_in_len < 65536) { net_in_cap = net_in_cap * 2 + 65536; net_in = realloc(net_in, net_in_cap); }
        ssize_t k = recv(net_fd, net_in + net_in_len, net_in_cap - net_in_len, 0);
        if (k > 0) { net_in_len += (size_t)k; continue; }
        if (k == 0 || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { if (net_st == 2) net_st = 3; }
        break;
    }
    for (;;) {   /* parse one complete frame */
        if (net_in_len < 2) return 0;
        uint8_t *p = net_in;
        int op = p[0] & 15; uint64_t n = p[1] & 127; size_t h = 2;
        if (n == 126) { if (net_in_len < 4) return 0; n = (uint64_t)p[2] << 8 | p[3]; h = 4; }
        else if (n == 127) { if (net_in_len < 10) return 0; n = 0; for (int i = 0; i < 8; i++) n = n << 8 | p[2 + i]; h = 10; }
        if (net_in_len < h + n) return 0;
        int len = (int)n;
        if (op == 2 && len > cap) len = cap;
        if (op == 2) memcpy(buf, p + h, (size_t)len);
        else if (op == 9) { uint8_t pong[2] = {0x8a, 0x80}; uint8_t z[4] = {0}; send_all(pong, 2); send_all(z, 4); }
        else if (op == 8) net_st = 3;
        memmove(net_in, net_in + h + n, net_in_len - h - n);
        net_in_len -= h + n;
        if (op == 2) return len > 0 ? len : 0;
    }
}

/* ---- audio ---- */
/* AUDIO_DUMP=file.wav: write the mixed output to a WAV file at real-time pace (headless testing) */
static FILE *adump; static uint64_t adump_frames; static uint32_t adump_t0; static int adump_rate;
static void adump_finish(void) {
    if (!adump) return;
    uint32_t bytes = (uint32_t)(adump_frames * 4);
    fseek(adump, 4, SEEK_SET); uint32_t v = 36 + bytes; fwrite(&v, 4, 1, adump);
    fseek(adump, 40, SEEK_SET); fwrite(&bytes, 4, 1, adump);
    fclose(adump); adump = NULL;
}
void plat_audio_open(int rate) {
    if (getenv("AUDIO_DUMP") && !adump) {
        adump = fopen(getenv("AUDIO_DUMP"), "wb");
        adump_rate = rate; adump_t0 = SDL_GetTicks();
        uint8_t h[44] = "RIFF\0\0\0\0WAVEfmt \x10\0\0\0\x01\0\x02\0";
        *(uint32_t *)(h + 24) = (uint32_t)rate; *(uint32_t *)(h + 28) = (uint32_t)rate * 4;
        h[32] = 4; h[34] = 16; memcpy(h + 36, "data", 4);
        fwrite(h, 1, 44, adump);
        atexit(adump_finish);
        return;
    }
    if (headless || adev) return;
    SDL_AudioSpec want = {0}, have;
    want.freq = rate; want.format = AUDIO_S16SYS; want.channels = 2; want.samples = 1024;
    adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (adev) SDL_PauseAudioDevice(adev, 0);
}
int plat_audio_queued(void) {
    if (adump) {
        uint64_t played = (uint64_t)(SDL_GetTicks() - adump_t0) * (uint64_t)adump_rate / 1000;
        return played >= adump_frames ? 0 : (int)(adump_frames - played);
    }
    return adev ? (int)(SDL_GetQueuedAudioSize(adev) / 4) : 1 << 30;
}
void plat_audio_push(const int16_t *f, int n) {
    if (adump) { fwrite(f, 4, (size_t)n, adump); adump_frames += (uint64_t)n; return; }
    if (adev) SDL_QueueAudio(adev, f, (uint32_t)n * 4);
}

static void on_segv(int sig, siginfo_t *si, void *ctx) {
    (void)ctx;
    fprintf(stderr, "signal %d at address %p\n", sig, si->si_addr);
    fatal("crash");
}

void game_start(void);
int main(int argc, char **argv) {
    if (argc > 1 && chdir(argv[1]) != 0) { perror("chdir"); return 1; }
    if (getenv("LOGLEVEL")) log_level = atoi(getenv("LOGLEVEL"));
    struct sigaction sa = {0};
    sa.sa_sigaction = on_segv; sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    signal(SIGTERM, on_term);
    { extern uint32_t trace_calls; if (getenv("TRACE_CALLS")) trace_calls = 1; }
    if (getenv("ALARM")) alarm((unsigned)atoi(getenv("ALARM")));
    plat_init();
    if (!headless) SDL_StartTextInput();
    game_start();
    return 0;
}
