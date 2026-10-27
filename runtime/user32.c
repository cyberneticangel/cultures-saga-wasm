/* USER32 / GDI32: a single main window, a message queue fed by platform events. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

#define HWND_MAIN 0x00010010u
static uint32_t wndproc, wnd_style, wnd_exstyle, wnd_userdata;
static int wnd_w = 800, wnd_h = 600;
int mouse_x, mouse_y;
static uint32_t mouse_buttons;    /* MK_LBUTTON=1 MK_RBUTTON=2 MK_MBUTTON=0x10 */
uint8_t key_state[256];           /* VK indexed, bit 7 = down */
static int cursor_visible = 1;
static int quit_posted;

/* ---- message queue ---- */
#define QSIZE 512
static struct { uint32_t hwnd, msg, wp, lp, time; int x, y; } q[QSIZE];
static int qhead, qtail;

void post_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    int n = (qtail + 1) % QSIZE;
    if (n == qhead) return;          /* drop when full */
    q[qtail].hwnd = hwnd; q[qtail].msg = msg; q[qtail].wp = wp; q[qtail].lp = lp;
    q[qtail].time = plat_ticks_ms(); q[qtail].x = mouse_x; q[qtail].y = mouse_y;
    qtail = n;
}

uint32_t send_message(uint32_t hwnd, uint32_t msg, uint32_t wp, uint32_t lp) {
    if (!wndproc) return 0;
    return CALL_GUEST(wndproc, hwnd, msg, wp, lp);
}

/* dinput hooks */
void dinput_mouse_event(int dx, int dy, int button, int down, int wheel);
void dinput_key_event(int vk, int down);

static int vk_to_scancode(int vk);

void input_pump(void) {
    plat_event ev;
    while (plat_poll_event(&ev)) {
        switch (ev.type) {
        case EV_QUIT:
            post_message(HWND_MAIN, 0x0010 /* WM_CLOSE */, 0, 0);
            break;
        case EV_MOUSEMOVE: {
            int dx = ev.x - mouse_x, dy = ev.y - mouse_y;
            mouse_x = ev.x; mouse_y = ev.y;
            post_message(HWND_MAIN, 0x0200, mouse_buttons, (uint32_t)(mouse_x & 0xffff) | ((uint32_t)mouse_y << 16));
            dinput_mouse_event(dx, dy, -1, 0, 0);
            break;
        }
        case EV_MOUSEDOWN: case EV_MOUSEUP: {
            int down = ev.type == EV_MOUSEDOWN;
            static const uint32_t mk[3] = {1, 2, 0x10};
            static const uint32_t msgs[3][2] = {{0x202, 0x201}, {0x205, 0x204}, {0x208, 0x207}};
            static const int vks[3] = {1, 2, 4};
            int b = ev.button;
            if (b < 0 || b > 2) break;
            mouse_x = ev.x; mouse_y = ev.y;
            if (down) mouse_buttons |= mk[b]; else mouse_buttons &= ~mk[b];
            key_state[vks[b]] = down ? 0x80 : 0;
            post_message(HWND_MAIN, msgs[b][down], mouse_buttons, (uint32_t)(mouse_x & 0xffff) | ((uint32_t)mouse_y << 16));
            dinput_mouse_event(0, 0, b, down, 0);
            break;
        }
        case EV_WHEEL:
            post_message(HWND_MAIN, 0x020A, (uint32_t)((ev.y * 120) << 16) | mouse_buttons, (uint32_t)(mouse_x & 0xffff) | ((uint32_t)mouse_y << 16));
            dinput_mouse_event(0, 0, -1, 0, ev.y * 120);
            break;
        case EV_KEYDOWN: case EV_KEYUP: {
            int down = ev.type == EV_KEYDOWN;
            int vk = ev.key & 0xff;
            int was = key_state[vk] & 0x80;
            key_state[vk] = down ? 0x80 : 0;
            /* modifiers: also set generic VK_SHIFT/CONTROL/MENU */
            if (vk == 0xA0 || vk == 0xA1) key_state[0x10] = (key_state[0xA0] | key_state[0xA1]) & 0x80;
            if (vk == 0xA2 || vk == 0xA3) key_state[0x11] = (key_state[0xA2] | key_state[0xA3]) & 0x80;
            if (vk == 0xA4 || vk == 0xA5) key_state[0x12] = (key_state[0xA4] | key_state[0xA5]) & 0x80;
            int gvk = vk >= 0xA0 && vk <= 0xA5 ? 0x10 + (vk - 0xA0) / 2 : vk;
            uint32_t lp = 1u | ((uint32_t)vk_to_scancode(gvk) << 16) | (down ? (was ? 0x40000000u : 0) : 0xC0000000u);
            post_message(HWND_MAIN, down ? 0x0100 : 0x0101, (uint32_t)gvk, lp);
            dinput_key_event(gvk, down);
            break;
        }
        case EV_CHAR:
            post_message(HWND_MAIN, 0x0102, (uint32_t)ev.key, 1);
            break;
        }
    }
}

static int vk_to_scancode(int vk) {
    static const uint8_t letters[26] = {0x1e,0x30,0x2e,0x20,0x12,0x21,0x22,0x23,0x17,0x24,0x25,0x26,0x32,0x31,0x18,0x19,0x10,0x13,0x1f,0x14,0x16,0x2f,0x11,0x2d,0x15,0x2c};
    if (vk >= 'A' && vk <= 'Z') return letters[vk - 'A'];
    if (vk >= '1' && vk <= '9') return 2 + vk - '1';
    if (vk == '0') return 0x0b;
    if (vk >= 0x70 && vk <= 0x79) return 0x3b + vk - 0x70;   /* F1-F10 */
    if (vk == 0x7A) return 0x57; if (vk == 0x7B) return 0x58;
    switch (vk) {
    case 0x1B: return 0x01; case 0x08: return 0x0e; case 0x09: return 0x0f; case 0x0D: return 0x1c;
    case 0x11: return 0x1d; case 0x10: return 0x2a; case 0x12: return 0x38; case 0x20: return 0x39;
    case 0x25: return 0xcb; case 0x26: return 0xc8; case 0x27: return 0xcd; case 0x28: return 0xd0;
    case 0x2D: return 0xd2; case 0x2E: return 0xd3; case 0x24: return 0xc7; case 0x23: return 0xcf;
    case 0x21: return 0xc9; case 0x22: return 0xd1; case 0x13: return 0xc5;
    case 0x6B: return 0x4e; case 0x6D: return 0x4a; case 0xBB: return 0x0d; case 0xBD: return 0x0c;
    }
    return 0;
}
int vk_scancode(int vk) { return vk_to_scancode(vk); }

/* ---- window class / creation ---- */
HLE(USER32, RegisterClassA) {
    uint32_t wc = ARG(0);
    wndproc = M32(wc + 4);
    LOG("RegisterClassA(%s) wndproc=%08x\n", (char *)PTR(M32(wc + 36)), wndproc);
    RET_STD(1, 0xC001);
}
HLE(USER32, RegisterWindowMessageA) { static uint32_t n = 0xC100; RET_STD(1, n++); }
HLE(USER32, CreateWindowExA) {
    wnd_exstyle = ARG(0); wnd_style = ARG(3);
    LOG("CreateWindowExA(%s, %d,%d %dx%d)\n", ARG(2) ? ARGP(2) : "", ARG(4), ARG(5), ARG(6), ARG(7));
    uint32_t esp = R.esp;
    /* WM_CREATE with a minimal CREATESTRUCT */
    uint32_t cs = guest_alloc_static(48);
    M32(cs + 28) = ARG(6); M32(cs + 24) = ARG(7);
    send_message(HWND_MAIN, 0x0001, 0, cs);
    R.esp = esp;
    RET_STD(12, HWND_MAIN);
}
HLE(USER32, DefWindowProcA) {
    uint32_t msg = ARG(1);
    if (msg == 0x0010) { /* WM_CLOSE -> destroy */
        uint32_t esp = R.esp;
        send_message(HWND_MAIN, 0x0002, 0, 0);
        R.esp = esp;
    }
    RET_STD(4, 0);
}
HLE(USER32, ShowWindow) {
    uint32_t esp = R.esp;
    send_message(HWND_MAIN, 0x0006 /* WM_ACTIVATE */, 1, 0);
    send_message(HWND_MAIN, 0x001C /* WM_ACTIVATEAPP */, 1, 0);
    send_message(HWND_MAIN, 0x0007 /* WM_SETFOCUS */, 0, 0);
    R.esp = esp;
    RET_STD(2, 0);
}
HLE(USER32, UpdateWindow) { RET_STD(1, 1); }
HLE(USER32, SetWindowPos) { RET_STD(7, 1); }
HLE(USER32, GetWindowLongA) {
    int32_t idx = (int32_t)ARG(1);
    uint32_t v = idx == -16 ? wnd_style : idx == -20 ? wnd_exstyle : idx == -4 ? wndproc : idx == -21 ? wnd_userdata : 0;
    RET_STD(2, v);
}
HLE(USER32, SetWindowLongA) {
    int32_t idx = (int32_t)ARG(1); uint32_t v = ARG(2), old = 0;
    if (idx == -16) { old = wnd_style; wnd_style = v; }
    else if (idx == -20) { old = wnd_exstyle; wnd_exstyle = v; }
    else if (idx == -4) { old = wndproc; wndproc = v; }
    else if (idx == -21) { old = wnd_userdata; wnd_userdata = v; }
    RET_STD(3, old);
}
void user32_set_size(int w, int h) { wnd_w = w; wnd_h = h; }
HLE(USER32, GetClientRect) {
    uint32_t r = ARG(1);
    M32(r) = 0; M32(r + 4) = 0; M32(r + 8) = wnd_w; M32(r + 12) = wnd_h;
    RET_STD(2, 1);
}
HLE(USER32, GetWindowRect) {
    uint32_t r = ARG(1);
    M32(r) = 0; M32(r + 4) = 0; M32(r + 8) = wnd_w; M32(r + 12) = wnd_h;
    RET_STD(2, 1);
}
HLE(USER32, AdjustWindowRectEx) { RET_STD(4, 1); }
HLE(USER32, GetSystemMetrics) {
    int i = (int)ARG(0);
    uint32_t v = i == 0 ? wnd_w : i == 1 ? wnd_h : i == 43 ? 3 : 0;   /* SM_CXSCREEN, SM_CYSCREEN, SM_CMOUSEBUTTONS */
    if (i == 19) v = 1;   /* SM_MOUSEPRESENT */
    RET_STD(1, v);
}
HLE(USER32, GetMenu) { RET_STD(1, 0); }
HLE(USER32, InvalidateRect) { RET_STD(3, 1); }
HLE(USER32, BeginPaint) { memset(ARGP(1), 0, 64); RET_STD(2, 0x00020020u); }
HLE(USER32, EndPaint) { RET_STD(2, 1); }
HLE(USER32, GetDC) { RET_STD(1, 0x00020020u); }
HLE(USER32, ReleaseDC) { RET_STD(2, 1); }
HLE(USER32, MessageBoxA) {
    logf_("MessageBox: [%s] %s\n", ARG(2) ? ARGP(2) : "", ARG(1) ? ARGP(1) : "");
    RET_STD(4, 1);   /* IDOK */
}

/* ---- messages ---- */
static void write_msg(uint32_t m, int i) {
    M32(m) = q[i].hwnd; M32(m + 4) = q[i].msg; M32(m + 8) = q[i].wp; M32(m + 12) = q[i].lp;
    M32(m + 16) = q[i].time; M32(m + 20) = (uint32_t)q[i].x; M32(m + 24) = (uint32_t)q[i].y;
}
static int peek(uint32_t m, uint32_t lo, uint32_t hi, int remove) {
    for (int i = qhead; i != qtail; i = (i + 1) % QSIZE) {
        if ((lo || hi) && (q[i].msg < lo || q[i].msg > hi)) continue;
        write_msg(m, i);
        if (remove) {
            /* remove entry i, shifting the rest */
            for (int j = i; (j + 1) % QSIZE != qtail; j = (j + 1) % QSIZE) q[j] = q[(j + 1) % QSIZE];
            qtail = (qtail - 1 + QSIZE) % QSIZE;
        }
        return 1;
    }
    return 0;
}
HLE(USER32, PeekMessageA) {
    qol_frame();
    uint32_t m = ARG(0), lo = ARG(2), hi = ARG(3), rm = ARG(4);
    ddraw_present_if_dirty();
    input_pump();
    audio_update();
    bg_pump();
    if (quit_posted == 1 && qhead == qtail) {
        quit_posted = 2;
        memset(PTR(m), 0, 28); M32(m + 4) = 0x12;
        RET_STD(5, 1);
    }
    RET_STD(5, peek(m, lo, hi, rm & 1));
}
HLE(USER32, GetMessageA) {
    uint32_t m = ARG(0), lo = ARG(2), hi = ARG(3);
    for (;;) {
        ddraw_present_if_dirty();
        input_pump();
        audio_update();
        bg_pump();
        if (quit_posted) { memset(PTR(m), 0, 28); M32(m + 4) = 0x12; RET_STD(4, 0); }
        if (peek(m, lo, hi, 1)) RET_STD(4, 1);
        plat_sleep(5);
    }
}
HLE(USER32, TranslateMessage) { RET_STD(1, 0); }
HLE(USER32, DispatchMessageA) {
    uint32_t m = ARG(0);
    uint32_t r = 0;
    uint32_t esp = R.esp;
    if (M32(m + 4) == 0x0113 && M32(m + 12)) {       /* WM_TIMER with callback */
        r = CALL_GUEST(M32(m + 12), M32(m), 0x113, M32(m + 8), plat_ticks_ms());
    } else {
        r = send_message(M32(m), M32(m + 4), M32(m + 8), M32(m + 12));
    }
    R.esp = esp;
    RET_STD(1, r);
}
HLE(USER32, PostMessageA) { post_message(ARG(0), ARG(1), ARG(2), ARG(3)); RET_STD(4, 1); }
HLE(USER32, SendMessageA) {
    uint32_t h = ARG(0), msg = ARG(1), wp = ARG(2), lp = ARG(3);
    uint32_t esp = R.esp;
    uint32_t r = send_message(h, msg, wp, lp);
    R.esp = esp;
    RET_STD(4, r);
}
HLE(USER32, PostQuitMessage) { quit_posted = 1; RET_STD(1, 0); }

/* ---- input ---- */
HLE(USER32, GetAsyncKeyState) { input_pump(); RET_STD(1, key_state[ARG(0) & 0xff] & 0x80 ? 0x8000u : 0); }
HLE(USER32, GetKeyState) { RET_STD(1, key_state[ARG(0) & 0xff] & 0x80 ? 0xff80u : 0); }
HLE(USER32, GetCursorPos) { M32(ARG(0)) = (uint32_t)mouse_x; M32(ARG(0) + 4) = (uint32_t)mouse_y; RET_STD(1, 1); }
HLE(USER32, SetCursorPos) { mouse_x = (int)ARG(0); mouse_y = (int)ARG(1); RET_STD(2, 1); }
HLE(USER32, ScreenToClient) { RET_STD(2, 1); }
HLE(USER32, ClipCursor) { RET_STD(1, 1); }
HLE(USER32, ShowCursor) {
    cursor_visible += ARG(0) ? 1 : -1;
    plat_show_cursor(cursor_visible > 0);
    RET_STD(1, (uint32_t)(cursor_visible - 1));
}
static char cursor_files[16][260];
static int ncursors;
static uint32_t cur_cursor;
HLE(USER32, SetCursor) {
    uint32_t h = ARG(0), old = cur_cursor;
    if (h != cur_cursor) {
        cur_cursor = h;
        int i = (int)(h - 0x00030010u);
        if (h == 0) plat_set_cursor(NULL);
        else if (i >= 0 && i < ncursors) plat_set_cursor(cursor_files[i]);
        else plat_set_cursor("");      /* system arrow */
    }
    RET_STD(1, old);
}
HLE(USER32, LoadCursorA) { RET_STD(2, 0x00030001u); }
HLE(USER32, LoadCursorFromFileA) {
    char host[520];
    if (!vfs_resolve(ARGP(0), host, sizeof host) || ncursors >= 16) RET_STD(1, 0);
    for (int i = 0; i < ncursors; i++) if (strcmp(cursor_files[i], host) == 0) RET_STD(1, 0x00030010u + (uint32_t)i);
    snprintf(cursor_files[ncursors], 260, "%s", host);
    RET_STD(1, 0x00030010u + (uint32_t)ncursors++);
}
HLE(USER32, LoadIconA) { RET_STD(2, 0x00030003u); }
HLE(USER32, OpenClipboard) { RET_STD(1, 0); }
HLE(USER32, CloseClipboard) { RET_STD(0, 1); }
HLE(USER32, GetClipboardData) { RET_STD(1, 0); }

/* ---- wsprintfA (cdecl, varargs on the guest stack) ---- */
int guest_vsprintf(char *out, int cap, const char *fmt, uint32_t argp);
HLE(USER32, wsprintfA) {
    char buf[1100];
    int n = guest_vsprintf(buf, 1024, ARGP(1), R.esp + 12);
    strcpy(ARGP(0), buf);
    RET_CDECL(n);
}

int guest_vsprintf(char *out, int cap, const char *fmt, uint32_t ap) {
    int o = 0;
    for (const char *p = fmt; *p && o < cap - 1; p++) {
        if (*p != '%') { out[o++] = *p; continue; }
        char spec[32]; int sl = 0;
        spec[sl++] = *p++;
        while (*p && strchr("-+ #0123456789.lhI", *p) && sl < 28) {
            if (*p == 'l' || *p == 'h') { p++; continue; }
            spec[sl++] = *p++;
        }
        if (!*p) break;
        char c = *p;
        char tmp[512];
        if (c == '%') { out[o++] = '%'; continue; }
        spec[sl++] = c; spec[sl] = 0;
        if (c == 's') { const char *s = M32(ap) ? (char *)PTR(M32(ap)) : "(null)"; ap += 4; snprintf(tmp, sizeof tmp, spec, s); }
        else if (c == 'c') { int v = (int)M32(ap); ap += 4; snprintf(tmp, sizeof tmp, spec, v); }
        else if (strchr("eEfgG", c)) { double v = MF64(ap); ap += 8; snprintf(tmp, sizeof tmp, spec, v); }
        else { uint32_t v = M32(ap); ap += 4; snprintf(tmp, sizeof tmp, spec, v); }
        for (char *t = tmp; *t && o < cap - 1; t++) out[o++] = *t;
    }
    out[o] = 0;
    return o;
}

/* ---- GDI ---- */
HLE(GDI32, GetDeviceCaps) {
    int i = (int)ARG(1);
    uint32_t v = 0;
    switch (i) {
    case 8: v = (uint32_t)wnd_w; break;      /* HORZRES */
    case 10: v = (uint32_t)wnd_h; break;     /* VERTRES */
    case 12: v = 16; break;                  /* BITSPIXEL */
    case 14: v = 1; break;                   /* PLANES */
    case 24: v = 256; break;                 /* NUMCOLORS */
    case 38: v = 0; break;                   /* RASTERCAPS */
    case 104: v = 256; break;                /* SIZEPALETTE */
    case 116: v = 60; break;                 /* VREFRESH */
    }
    RET_STD(2, v);
}
HLE(GDI32, CreatePalette) { RET_STD(1, 0x00040001u); }
HLE(GDI32, SelectPalette) { RET_STD(3, 0x00040002u); }
HLE(GDI32, RealizePalette) { RET_STD(1, 0); }
HLE(GDI32, DeleteObject) { RET_STD(1, 1); }
