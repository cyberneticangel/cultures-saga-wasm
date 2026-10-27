/* Conveniences built on the game's own code: a default autosave interval, game speed through the
 * game's speed command, and quickload.
 *
 * - Autosave: the game saves to user_autosave_00..09 every N logic ticks (12 per second at normal
 *   speed) once the interval is set, but its options window (Speichern) starts at "Aus" every time.
 *   With a default set by the player, the interval variable is set whenever it is 0.
 * - Speed: the 'p' and 'l' keys send a command {speed} (0x87; 0x86 in Northland) through the game's command queue, which
 *   keeps multiplayer games in step; 12 = normal, 0 = paused.  The same command sets any speed the
 *   page asks for, and the current speed is reported back to the page.
 * - Quickload: F9 / Ctrl+S save user_quicksave, but the game has no key to load it.  This calls the
 *   game's load routine (the one behind the load window) with that name.
 *
 * Everything runs from the game's message pump (PeekMessageA), between frames.  The routines and
 * variables are found by their code, since each game version places them elsewhere.
 * Pattern tokens: hex byte, "??" any byte, "B" a byte to capture, "A" an absolute address to
 * capture (4 bytes), "R" a rel32 call target to capture, "J" a rel32 jump to follow. */
#include "runtime.h"
#include <stdio.h>

typedef struct { const char *pat; int want; } pattern;

/* setter (interval) and per-tick check: captures &interval, &last, &tick */
static const char *const autosave_pats[] = {
    "8b 44 24 04 85 c0 75 07 21 05 A c3 83 25 ?? ?? ?? ?? 00 a3 ?? ?? ?? ?? ?? ?? ?? ?? ?? "
    "a1 ?? ?? ?? ?? 85 c0 74 15 8b 0d A 03 c8 39 0d A 72",
    NULL};
/* the 'p' key: if (speed != 0) send 0 else send previous: captures &speed (double) */
static const char *const speed_pats[] = {
    "dd 05 A dc 1d ?? ?? ?? ?? df e0 f6 c4 05 7a ?? dd 05 A",   /* Saga, Northland */
    "dd 05 A dc 1d ?? ?? ?? ?? df e0 9e 73 ?? dd 05 A",         /* 8th Wonder */
    NULL};
/* the speed command: CommandQueue::Send(&cmd, 0, 1) on *queue: captures the command id, &queue, Send */
static const char *const send_pats[] = {
    "c6 45 e8 B 89 75 ec 6a 01 e9 J 6a 00 8b 0d A 8d 45 e8 50 e8 R",   /* Saga, Northland: the 'l' key */
    "c6 45 ec B 89 7d f0 6a 00 8d 45 ec e9 J 8b 0d A 50 e8 R",         /* 8th Wonder: the 'p' key */
    NULL};
/* Load(name, 0) */
static const char *const load_pats[] = {
    "55 8b ec 81 ec 04 01 00 00 33 c0 33 c9 38 45 0c 50 0f 95 c1 a2 ?? ?? ?? ?? 41 51 8b 0d",
    NULL};

/* match at addr; captures go to out[]; returns the number captured or -1 */
static int match(uint32_t addr, const char *pat, uint32_t *out) {
    int n = 0;
    for (const char *p = pat; *p; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (addr < IMAGE_BASE || addr >= IMAGE_BASE + 0x200000) return -1;
        if (*p == 'B') { out[n++] = M8(addr); addr++; p++; }
        else if (*p == 'A') { out[n++] = M32(addr); addr += 4; p++; }
        else if (*p == 'R') { out[n++] = addr + 4 + M32(addr); addr += 4; p++; }
        else if (*p == 'J') { addr = addr + 4 + M32(addr); p++; }
        else if (*p == '?') { addr++; p += 2; }
        else {
            unsigned v;
            if (sscanf(p, "%2x", &v) != 1 || M8(addr) != v) return -1;
            addr++; p += 2;
        }
    }
    return n;
}
/* first match of any of pats in the code section; returns its address or 0 */
static uint32_t find(const char *const *pats, uint32_t *out, uint32_t lo, uint32_t hi) {
    for (int i = 0; pats[i]; i++) {
        unsigned first;
        if (sscanf(pats[i], "%2x", &first) != 1) continue;
        for (uint32_t a = lo; a < hi; a++)
            if (M8(a) == first && match(a, pats[i], out) >= 0) return a;
    }
    return 0;
}

static struct {
    int searched;
    uint32_t as_interval, as_last, tick;   /* autosave */
    uint32_t speed;                       /* double: current game speed */
    uint32_t queue, send, speed_cmd;      /* command queue (pointer variable), its Send, the speed command */
    uint32_t load;
    int as_set;                           /* the interval we last set, 0 = none */
} Q;

static void search(void) {
    Q.searched = 1;
    /* the code section: the first section header */
    uint32_t pe = IMAGE_BASE + M32(IMAGE_BASE + 0x3c);
    uint32_t sec = pe + 24 + M16(pe + 20);
    uint32_t lo = IMAGE_BASE + M32(sec + 12), hi = lo + M32(sec + 8);
    if (hi > IMAGE_BASE + 0x200000) hi = IMAGE_BASE + 0x200000;
    uint32_t c[4] = {0};
    if (find(autosave_pats, c, lo, hi)) { Q.as_interval = c[0]; Q.as_last = c[1]; Q.tick = c[2]; }
    if (find(speed_pats, c, lo, hi)) Q.speed = c[0];
    if (find(send_pats, c, lo, hi) && (c[0] == 0x86 || c[0] == 0x87)) { Q.speed_cmd = c[0]; Q.queue = c[1]; Q.send = c[2]; }
    Q.load = find(load_pats, c, lo, hi);
    LOG("qol: autosave %s, speed %s, command queue %s, load %s\n",
        Q.as_interval ? "found" : "-", Q.speed ? "found" : "-", Q.send ? "found" : "-", Q.load ? "found" : "-");
}

static int busy;
static void send_speed(int v) {
    uint32_t queue = M32(Q.queue);
    if (!queue) return;
    uint32_t esp = R.esp;
    R.esp -= 0x20;
    uint32_t cmd = R.esp;
    memset(PTR(cmd), 0, 0x14);
    M8(cmd) = (uint8_t)Q.speed_cmd; M32(cmd + 4) = (uint32_t)v;
    R.ecx = queue;
    CALL_GUEST(Q.send, cmd, 0, 1);
    R.esp = esp;
}
static void quickload(void) {
    int is_dir; uint32_t size;
    if (plat_stat("Saves/singleplayer/savegames/user_quicksave.sav", &is_dir, &size) != 0) {
        LOG("qol: no quicksave to load\n");
        return;
    }
    uint32_t esp = R.esp;
    R.esp -= 0x20;
    uint32_t name = R.esp;
    strcpy(PTR(name), "user_quicksave");
    CALL_GUEST(Q.load, name, 0);
    R.esp = esp;
    LOG("qol: quickload\n");
}

void qol_frame(void) {
    if (busy) return;
    busy = 1;
    if (!Q.searched) search();
    uint32_t eax = R.eax, ecx = R.ecx, edx = R.edx;

    /* autosave default (seconds; the game counts 12 ticks per second) */
    int secs = plat_qol_get(QOL_AUTOSAVE);
    if (Q.as_interval) {
        uint32_t cur = M32(Q.as_interval);
        if (secs > 0 && (cur == 0 || (int)cur == Q.as_set)) {
            uint32_t want = (uint32_t)secs * 12;
            if (cur != want) { M32(Q.as_interval) = want; M32(Q.as_last) = M32(Q.tick); Q.as_set = (int)want; }
        } else if (secs <= 0 && Q.as_set && (int)cur == Q.as_set) {
            M32(Q.as_interval) = 0; Q.as_set = 0;   /* our default was switched off */
        }
        if (M32(Q.tick) < M32(Q.as_last)) M32(Q.as_last) = M32(Q.tick);   /* a new game restarted the clock */
    }

    /* speed: report the current one, apply a requested one */
    if (Q.speed && Q.send) {
        double d; memcpy(&d, PTR(Q.speed), 8);
        plat_qol_set(QOL_SPEED_NOW, (int)(d + 0.5));
        int want = plat_qol_get(QOL_SPEED_REQ);
        if (want >= 0) { plat_qol_set(QOL_SPEED_REQ, -1); send_speed(want); }
    }

    /* quickload (single player only) */
    if (plat_qol_get(QOL_QUICKLOAD)) {
        plat_qol_set(QOL_QUICKLOAD, 0);
        if (Q.load && !plat_net_state()) quickload();
    }
    R.eax = eax; R.ecx = ecx; R.edx = edx;
    busy = 0;
}
