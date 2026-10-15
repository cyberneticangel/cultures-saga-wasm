/* DirectPlay 8 (IDirectPlay8Peer / IDirectPlay8Address) on top of the WebSocket relay.
 *
 * The game's network layer (CDPNet) uses a peer session: one player hosts, the others
 * find the session with EnumHosts and join with a synchronous Connect.  Here every game
 * keeps one connection to the relay (relay.py); the host is the authority for the player
 * list and DPNIDs, and data goes directly to the target client through the relay.
 *
 * DirectPlay calls its message handler from worker threads.  This runtime is single
 * threaded, so messages are queued and delivered from bg_pump() (called from the message
 * pump, Sleep and waits) and at the end of the synchronous Host/Connect/Close calls.
 *
 * bg_pump() also runs the game's own network threads (a timer and the savegame transfer
 * manager), which are simple polling loops, as cooperative tasks: see the end of the file. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>

#define S_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_NOTIMPL 0x80004001u
#define DPNSUCCESS_PENDING 0x0015800eu
#define DPNERR_GENERIC 0x80004005u
#define DPNERR_INVALIDPARAM 0x80070057u
#define DPNERR_BUFFERTOOSMALL 0x80158100u
#define DPNERR_CONNECTIONLOST 0x80158160u
#define DPNERR_DOESNOTEXIST 0x80158180u
#define DPNERR_HOSTREJECTEDCONNECTION 0x80158260u
#define DPNERR_HOSTTERMINATEDSESSION 0x80158270u
#define DPNERR_INVALIDHANDLE 0x80158360u
#define DPNERR_INVALIDPLAYER 0x80158420u
#define DPNERR_NOCONNECTION 0x80158480u
#define DPNERR_NOTHOST 0x80158530u
#define DPNERR_SESSIONFULL 0x80158610u
#define DPNERR_TIMEDOUT 0x801585e0u
#define DPNERR_UNINITIALIZED 0x80158640u
#define DPNERR_USERCANCEL 0x80158650u

enum {   /* DPN_MSGID_* = 0xFFFF0000 + n */
    M_ASYNC_OP_COMPLETE = 3, M_CONNECT_COMPLETE = 5, M_CREATE_PLAYER = 7, M_DESTROY_PLAYER = 9,
    M_ENUM_HOSTS_QUERY = 10, M_ENUM_HOSTS_RESPONSE = 11, M_INDICATE_CONNECT = 14, M_PEER_INFO = 16,
    M_RECEIVE = 17, M_SEND_COMPLETE = 20, M_TERMINATE_SESSION = 22,
};
#define DPNSEND_SYNC 0x80000000u
#define DPNSEND_NOCOMPLETE 0x0002u
#define DPNSEND_NOLOOPBACK 0x0020u
#define DPNPLAYER_LOCAL 0x2u
#define DPNPLAYER_HOST 0x4u
#define REASON_NORMAL 1u
#define REASON_CONNECTIONLOST 2u
#define REASON_SESSIONTERMINATED 3u
#define REASON_HOSTDESTROYEDPLAYER 4u

/* relay control ops and peer protocol ops (first payload byte) */
enum { R_WELCOME = 1, R_HOST_ON = 2, R_HOST_OFF = 3, R_LIST = 4, R_GONE = 5 };
enum { P_ENUM_Q = 0x10, P_ENUM_R, P_CONN_REQ, P_CONN_ACK, P_PLAYER_NEW, P_PLAYER_GONE, P_DATA,
       P_PEER_INFO, P_KICK, P_LEAVE, P_END, P_PING, P_PONG };

static const uint8_t CLSID_Peer[16] = {0x4d,0x48,0x6f,0x28,0x5e,0x37,0x58,0x44,0xa2,0x72,0xb1,0x38,0xe2,0xf8,0x0a,0x6a};
static const uint8_t CLSID_Address[16] = {0x23,0x95,0x4a,0x93,0xca,0xa3,0xc5,0x4b,0xad,0xa0,0xd6,0xd9,0x5d,0x97,0x94,0x21};
static const uint8_t SP_TCPIP[16] = {0xa0,0x7b,0xfe,0xeb,0x8d,0x62,0xd2,0x11,0xae,0x0f,0x00,0x60,0x97,0xb0,0x14,0x11};

/* ------------------------------------------------------------------ byte buffers */
typedef struct { uint8_t *p; uint32_t n, cap; } buf_t;
static void b_put(buf_t *b, const void *d, uint32_t n) {
    if (b->n + n > b->cap) { b->cap = (b->n + n) * 2 + 256; b->p = realloc(b->p, b->cap); }
    memcpy(b->p + b->n, d, n); b->n += n;
}
static void b_u8(buf_t *b, uint8_t v) { b_put(b, &v, 1); }
static void b_u32(buf_t *b, uint32_t v) { b_put(b, &v, 4); }
static void b_blob(buf_t *b, const void *d, uint32_t n) { b_u32(b, n); b_put(b, d, n); }
typedef struct { const uint8_t *p; uint32_t n, o; int bad; } rd_t;
static uint32_t r_u32(rd_t *r) { uint32_t v = 0; if (r->o + 4 > r->n) { r->bad = 1; return 0; } memcpy(&v, r->p + r->o, 4); r->o += 4; return v; }
static uint8_t r_u8(rd_t *r) { if (r->o + 1 > r->n) { r->bad = 1; return 0; } return r->p[r->o++]; }
static const uint8_t *r_blob(rd_t *r, uint32_t *n) {
    *n = r_u32(r);
    if (r->bad || r->o + *n > r->n) { r->bad = 1; *n = 0; return NULL; }
    const uint8_t *p = r->p + r->o; r->o += *n; return p;
}

static uint32_t gdup(const void *p, uint32_t n) {   /* copy into guest heap */
    uint32_t g = heap_alloc(n ? n : 1, 1);
    if (n) memcpy(PTR(g), p, n);
    return g;
}
static uint32_t wbytes(uint32_t gp) {   /* size of a guest UTF-16 string incl. terminator, in bytes */
    if (!gp) return 0;
    uint32_t n = 0;
    while (M16(gp + 2 * n)) n++;
    return 2 * (n + 1);
}

/* ------------------------------------------------------------------ state */
#define MAXP 16
typedef struct {
    uint32_t id, cid, flags, ctx, rtt, announced, leave_at, reason;
    uint8_t name[128]; uint32_t namelen;   /* UTF-16 bytes incl. terminator (0 = no name) */
    uint8_t data[256]; uint32_t datalen;
    int used;
} player_t;
typedef struct {
    uint32_t flags, maxp;
    uint8_t inst[16], app[16];
    uint8_t name[128]; uint32_t namelen;
    uint8_t res[1024]; uint32_t reslen;
} sess_t;

static struct {
    int inited;
    uint32_t handler, uctx;
    int role;                 /* 0 none, 1 host, 2 joined */
    uint32_t my_id, host_id, host_cid, next_id;
    player_t pl[MAXP];
    sess_t sess;
    player_t local;           /* SetPeerInfo before a session */
    int enum_on; uint32_t enum_handle, enum_ctx, enum_last; uint8_t enum_app[16];
    uint32_t next_handle;
    /* pending synchronous connect */
    int conn_wait, conn_done; uint32_t conn_token, conn_result;
    uint8_t conn_reply[1024]; uint32_t conn_replylen;
    uint32_t ping_last;
} D;

static uint32_t my_cid;       /* relay client id, 0 = not connected */

/* ------------------------------------------------------------------ relay transport */
static void net_send(uint32_t dst, const buf_t *b) {
    buf_t m = {0};
    b_u32(&m, dst); b_put(&m, b->p, b->n);
    plat_net_send(m.p, (int)m.n);
    free(m.p);
}
static void net_send_op(uint32_t dst, uint8_t op) { buf_t b = {0}; b_u8(&b, op); net_send(dst, &b); free(b.p); }
static void dp_poll(void);
static int net_ensure(void) {
    int st = plat_net_state();
    if (st == 2 && my_cid) return 1;
    if (st != 1 && st != 2) { my_cid = 0; plat_net_open(NULL); LOG("dplay: connecting to relay\n"); }
    uint32_t t0 = plat_ticks_ms();
    while (plat_ticks_ms() - t0 < 8000) {
        dp_poll();
        st = plat_net_state();
        if (st == 2 && my_cid) return 1;
        if (st == 3 || st == 0) break;
        plat_sleep(5);
    }
    LOG("dplay: relay not reachable\n");
    return 0;
}

/* ------------------------------------------------------------------ message queue -> game handler */
typedef struct { uint32_t type, ptr, a, b, c; } ev_t;
static ev_t *evq; static int evn, evcap, ev_head;
static void ev_push3(uint32_t type, uint32_t ptr, uint32_t a, uint32_t b, uint32_t c) {
    if (evn == evcap) { evcap = evcap * 2 + 32; evq = realloc(evq, sizeof *evq * (size_t)evcap); }
    evq[evn++] = (ev_t){type, ptr, a, b, c};
}
static void ev_push(uint32_t type, uint32_t ptr, uint32_t a, uint32_t b) { ev_push3(type, ptr, a, b, 0); }
static uint32_t pending_bufs[256]; static int npending;

static player_t *pl_find(uint32_t id) {
    for (int i = 0; i < MAXP; i++) if ((D.pl[i].used == 1 || D.pl[i].used == 3) && D.pl[i].id == id) return &D.pl[i];
    return NULL;
}
static player_t *pl_by_cid(uint32_t cid) {
    for (int i = 0; i < MAXP; i++) if (D.pl[i].used == 1 && D.pl[i].cid == cid && D.pl[i].id != D.my_id) return &D.pl[i];
    return NULL;
}
static player_t *pl_add(void) {
    for (int i = 0; i < MAXP; i++) if (!D.pl[i].used) { memset(&D.pl[i], 0, sizeof D.pl[i]); D.pl[i].used = 1; return &D.pl[i]; }
    return NULL;
}
static int pl_count(void) { int n = 0; for (int i = 0; i < MAXP; i++) n += D.pl[i].used == 1; return n; }

static uint32_t addr_new(uint32_t cid);
static void addr_release(uint32_t obj);

/* struct builders */
static void q_create_player(player_t *p) {
    if (p->announced) return;
    p->announced = 1;
    uint32_t m = heap_alloc(12, 1);
    M32(m) = 12; M32(m + 4) = p->id; M32(m + 8) = p->ctx;
    ev_push(M_CREATE_PLAYER, m, p->id, 0);
}
static void q_destroy_player(player_t *p, uint32_t reason) {
    uint32_t m = heap_alloc(16, 1);
    M32(m) = 16; M32(m + 4) = p->id; M32(m + 8) = p->ctx; M32(m + 12) = reason;
    ev_push(M_DESTROY_PLAYER, m, 0, 0);
}
static void q_terminate(uint32_t hr) {
    uint32_t m = heap_alloc(16, 1);
    M32(m) = 16; M32(m + 4) = hr;
    ev_push(M_TERMINATE_SESSION, m, 0, 0);
}
static void q_async_complete(uint32_t h, uint32_t ctx, uint32_t hr) {
    uint32_t m = heap_alloc(16, 1);
    M32(m) = 16; M32(m + 4) = h; M32(m + 8) = ctx; M32(m + 12) = hr;
    ev_push(M_ASYNC_OP_COMPLETE, m, 0, 0);
}
static void q_receive(uint32_t from, const uint8_t *d, uint32_t n) {
    player_t *p = pl_find(from);
    uint32_t data = gdup(d, n), m = heap_alloc(0x18, 1);
    M32(m) = 0x18; M32(m + 4) = from; M32(m + 8) = p ? p->ctx : 0;
    M32(m + 12) = data; M32(m + 16) = n; M32(m + 20) = data;
    ev_push(M_RECEIVE, m, data, 0);
}
static uint32_t appdesc_guest(const sess_t *s, uint32_t curp) {
    uint32_t g = heap_alloc(0x48 + s->namelen + s->reslen + 4, 1);
    M32(g) = 0x48; M32(g + 4) = s->flags;
    memcpy(PTR(g + 8), s->inst, 16); memcpy(PTR(g + 0x18), s->app, 16);
    M32(g + 0x28) = s->maxp; M32(g + 0x2c) = curp;
    if (s->namelen) { M32(g + 0x30) = g + 0x48; memcpy(PTR(g + 0x48), s->name, s->namelen); }
    if (s->reslen) { M32(g + 0x40) = g + 0x48 + s->namelen; M32(g + 0x44) = s->reslen; memcpy(PTR(g + 0x48 + s->namelen), s->res, s->reslen); }
    return g;
}
static void sess_from_guest(sess_t *s, uint32_t g) {
    memset(s, 0, sizeof *s);
    s->flags = M32(g + 4);
    memcpy(s->inst, PTR(g + 8), 16); memcpy(s->app, PTR(g + 0x18), 16);
    s->maxp = M32(g + 0x28);
    uint32_t nm = M32(g + 0x30), nb = wbytes(nm);
    if (nb > sizeof s->name) nb = sizeof s->name;
    if (nm) { memcpy(s->name, PTR(nm), nb); s->namelen = nb; }
    if (s->namelen >= 2) { s->name[s->namelen - 2] = 0; s->name[s->namelen - 1] = 0; }
    uint32_t rl = M32(g + 0x44);
    if (M32(g + 0x40) && rl && rl <= sizeof s->res) { memcpy(s->res, PTR(M32(g + 0x40)), rl); s->reslen = rl; }
}
static void b_sess(buf_t *b, const sess_t *s) {
    b_u32(b, s->flags); b_u32(b, s->maxp); b_put(b, s->inst, 16); b_put(b, s->app, 16);
    b_blob(b, s->name, s->namelen); b_blob(b, s->res, s->reslen);
}
static void r_sess(rd_t *r, sess_t *s) {
    memset(s, 0, sizeof *s);
    s->flags = r_u32(r); s->maxp = r_u32(r);
    for (int i = 0; i < 16; i++) s->inst[i] = r_u8(r);
    for (int i = 0; i < 16; i++) s->app[i] = r_u8(r);
    uint32_t n; const uint8_t *p = r_blob(r, &n);
    if (p && n <= sizeof s->name) { memcpy(s->name, p, n); s->namelen = n; }
    p = r_blob(r, &n);
    if (p && n <= sizeof s->res) { memcpy(s->res, p, n); s->reslen = n; }
}
static void b_player(buf_t *b, const player_t *p) {
    b_u32(b, p->id); b_u32(b, p->cid); b_u32(b, p->flags & DPNPLAYER_HOST);
    b_blob(b, p->name, p->namelen); b_blob(b, p->data, p->datalen);
}
static void r_player(rd_t *r, player_t *p) {
    p->id = r_u32(r); p->cid = r_u32(r); p->flags = r_u32(r);
    uint32_t n; const uint8_t *d = r_blob(r, &n);
    if (d && n <= sizeof p->name) { memcpy(p->name, d, n); p->namelen = n; }
    d = r_blob(r, &n);
    if (d && n <= sizeof p->data) { memcpy(p->data, d, n); p->datalen = n; }
}
static void info_from_guest(player_t *p, uint32_t gi) {   /* DPN_PLAYER_INFO */
    uint32_t fl = M32(gi + 4);
    if (fl & 1) {
        uint32_t nm = M32(gi + 8), nb = wbytes(nm);
        if (nb > sizeof p->name) nb = sizeof p->name;
        p->namelen = nb;
        if (nb) { memcpy(p->name, PTR(nm), nb); p->name[nb - 2] = p->name[nb - 1] = 0; }
    }
    if (fl & 2) {
        uint32_t n = M32(gi + 16);
        if (n > sizeof p->data) n = sizeof p->data;
        p->datalen = n;
        if (n) memcpy(p->data, PTR(M32(gi + 12)), n);
    }
}

/* ------------------------------------------------------------------ session lifetime */
static void session_reset(void) {
    if (D.role == 1 && my_cid) net_send_op(0, R_HOST_OFF);
    for (int i = 0; i < MAXP; i++) if (D.pl[i].used != 3) D.pl[i].used = 0;   /* leaving players go after their DESTROY_PLAYER */
    D.role = 0; D.my_id = D.host_id = D.host_cid = 0;
}
static void broadcast(const buf_t *b) {
    for (int i = 0; i < MAXP; i++)
        if (D.pl[i].used && D.pl[i].id != D.my_id) net_send(D.pl[i].cid, b);
}
/* the session ended under us: destroy every player, then TERMINATE_SESSION */
static void session_lost(uint32_t hr) {
    if (!D.role) return;
    LOG("dplay: session terminated (%08x)\n", hr);
    for (int i = 0; i < MAXP; i++)
        if ((D.pl[i].used == 1 || (D.pl[i].used == 3 && D.pl[i].leave_at)) && D.pl[i].id != D.my_id) {
            D.pl[i].leave_at = 0;
            q_destroy_player(&D.pl[i], REASON_SESSIONTERMINATED);
        }
    player_t *me = pl_find(D.my_id);
    if (me) q_destroy_player(me, REASON_SESSIONTERMINATED);
    for (int i = 0; i < MAXP; i++) if (D.pl[i].used == 1) D.pl[i].used = 3;
    q_terminate(hr);
    session_reset();
}
/* A remote player left.  DirectPlay reports a disconnect only after its own handshake, so the
 * game has processed the player's last messages (e.g. its "leaving" notice, which it resolves
 * to a name) before DESTROY_PLAYER arrives; the short delay reproduces that ordering. */
static void player_left(player_t *p, uint32_t reason) {
    LOG("dplay: player %08x left (reason %u)\n", p->id, reason);
    p->used = 3; p->reason = reason; p->leave_at = plat_ticks_ms() + 300;
}

/* ------------------------------------------------------------------ incoming messages */
typedef struct { uint32_t src; uint8_t *p; uint32_t n; } defer_t;
static defer_t deferred[64]; static int ndeferred;   /* data from players we do not know yet */

static void handle_peer(uint32_t src, const uint8_t *p, uint32_t n);
static void handle_relay(const uint8_t *p, uint32_t n) {
    rd_t r = {p, n, 1, 0};
    switch (p[0]) {
    case R_WELCOME: my_cid = r_u32(&r); LOG("dplay: relay client id %u\n", my_cid); break;
    case R_LIST: {
        uint32_t k = r_u32(&r);
        for (uint32_t i = 0; i < k && !r.bad; i++) {
            uint32_t h = r_u32(&r);
            buf_t b = {0}; b_u8(&b, P_ENUM_Q); b_u32(&b, D.enum_handle); b_u32(&b, plat_ticks_ms());
            net_send(h, &b); free(b.p);
        }
        break;
    }
    case R_GONE: {
        uint32_t cid = r_u32(&r);
        if (!D.role) break;
        if (D.role == 2 && cid == D.host_cid) { session_lost(DPNERR_CONNECTIONLOST); break; }
        for (int i = 0; i < MAXP; i++) if (D.pl[i].used == 2 && D.pl[i].cid == cid) D.pl[i].used = 0;
        player_t *pp = pl_by_cid(cid);
        if (pp) player_left(pp, REASON_CONNECTIONLOST);
        break;
    }
    }
}

static void handle_peer(uint32_t src, const uint8_t *p, uint32_t n) {
    rd_t r = {p, n, 1, 0};
    switch (p[0]) {
    case P_ENUM_Q: {
        if (D.role != 1) break;
        uint32_t tok = r_u32(&r), t = r_u32(&r);
        uint32_t m = heap_alloc(0x24, 1);
        M32(m) = 0x24; M32(m + 4) = addr_new(src); M32(m + 8) = addr_new(0);
        M32(m + 0x14) = 1000;    /* dwMaxResponseDataSize */
        ev_push3(M_ENUM_HOSTS_QUERY, m, src, tok, t);   /* answered after the handler accepted it */
        break;
    }
    case P_ENUM_R: {
        if (!D.enum_on) break;
        uint32_t tok = r_u32(&r), t = r_u32(&r), curp = r_u32(&r);
        sess_t s; r_sess(&r, &s);
        if (r.bad || tok != D.enum_handle) break;
        uint32_t m = heap_alloc(0x20, 1);
        M32(m) = 0x20; M32(m + 4) = addr_new(src); M32(m + 8) = addr_new(0);
        M32(m + 12) = appdesc_guest(&s, curp);
        M32(m + 24) = D.enum_ctx; M32(m + 28) = plat_ticks_ms() - t;
        ev_push(M_ENUM_HOSTS_RESPONSE, m, 0, 0);
        break;
    }
    case P_CONN_REQ: {
        uint32_t tok = r_u32(&r);
        player_t np = {0}; np.used = 1;
        uint32_t k; const uint8_t *d = r_blob(&r, &k);
        if (d && k <= sizeof np.name) { memcpy(np.name, d, k); np.namelen = k; }
        d = r_blob(&r, &k);
        if (d && k <= sizeof np.data) { memcpy(np.data, d, k); np.datalen = k; }
        uint32_t ucn; const uint8_t *ucd = r_blob(&r, &ucn);
        if (r.bad) break;
        if (D.role != 1 || pl_count() >= (int)(D.sess.maxp ? D.sess.maxp : MAXP) || pl_count() >= MAXP - 1) {
            buf_t b = {0}; b_u8(&b, P_CONN_ACK); b_u32(&b, tok); b_u32(&b, D.role == 1 ? DPNERR_SESSIONFULL : DPNERR_NOTHOST);
            net_send(src, &b); free(b.p);
            break;
        }
        uint32_t m = heap_alloc(0x24 + ucn + 4, 1);
        M32(m) = 0x24;
        if (ucn) { M32(m + 4) = m + 0x24; M32(m + 8) = ucn; memcpy(PTR(m + 0x24), ucd, ucn); }
        M32(m + 0x1c) = addr_new(src); M32(m + 0x20) = addr_new(0);
        /* the joining player's record travels in a host-side side table until the handler decides */
        player_t *slot = pl_add();
        if (!slot) { heap_free(m); break; }
        *slot = np; slot->used = 2;   /* 2 = pending (indicated, not yet created) */
        slot->cid = src; slot->id = 0;
        ev_push(M_INDICATE_CONNECT, m, src, tok);
        break;
    }
    case P_CONN_ACK: {
        uint32_t tok = r_u32(&r), res = r_u32(&r);
        if (!D.conn_wait || tok != D.conn_token) break;
        D.conn_result = res;
        if (res == S_OK) {
            D.my_id = r_u32(&r); D.host_id = r_u32(&r);
            r_sess(&r, &D.sess);
            uint32_t k = r_u32(&r);
            memset(D.pl, 0, sizeof D.pl);
            for (uint32_t i = 0; i < k && i < MAXP && !r.bad; i++) { D.pl[i].used = 1; r_player(&r, &D.pl[i]); }
            player_t *me = pl_find(D.my_id);
            if (me) { me->flags |= DPNPLAYER_LOCAL; me->cid = my_cid; }
            D.role = 2;
        }
        uint32_t rn; const uint8_t *rp = r_blob(&r, &rn);
        D.conn_replylen = 0;
        if (rp && rn <= sizeof D.conn_reply) { memcpy(D.conn_reply, rp, rn); D.conn_replylen = rn; }
        if (res == S_OK && r.bad) D.conn_result = DPNERR_GENERIC;
        D.host_cid = src;
        D.conn_done = 1;
        break;
    }
    case P_PLAYER_NEW: {
        if (D.role != 2 || src != D.host_cid) break;
        player_t np = {0}; r_player(&r, &np);
        if (r.bad || pl_find(np.id)) break;
        player_t *pp = pl_add();
        if (!pp) break;
        np.used = 1; *pp = np;
        q_create_player(pp);
        for (int i = 0; i < ndeferred; i++)   /* data that raced ahead of the announcement */
            if (deferred[i].src == pp->cid) {
                handle_peer(deferred[i].src, deferred[i].p, deferred[i].n);
                free(deferred[i].p);
                deferred[i--] = deferred[--ndeferred];
            }
        break;
    }
    case P_PLAYER_GONE: {
        if (src != D.host_cid) break;
        uint32_t id = r_u32(&r), reason = r_u32(&r);
        player_t *pp = pl_find(id);
        if (pp && id != D.my_id) player_left(pp, reason);
        break;
    }
    case P_DATA: {
        if (!D.role) break;
        uint32_t from = r_u32(&r);
        if (r.bad) break;
        player_t *pp = pl_find(from);
        if (!pp || pp->used != 1) {
            if (ndeferred < 64) { deferred[ndeferred].src = src; deferred[ndeferred].p = malloc(n); memcpy(deferred[ndeferred].p, p, n); deferred[ndeferred++].n = n; }
            break;
        }
        q_receive(from, p + r.o, n - r.o);
        break;
    }
    case P_PEER_INFO: {
        uint32_t id = r_u32(&r);
        player_t tmp = {0}; uint32_t k; const uint8_t *d = r_blob(&r, &k);
        if (d && k <= sizeof tmp.name) { memcpy(tmp.name, d, k); tmp.namelen = k; }
        d = r_blob(&r, &k);
        if (d && k <= sizeof tmp.data) { memcpy(tmp.data, d, k); tmp.datalen = k; }
        player_t *pp = pl_find(id);
        if (!pp || r.bad) break;
        memcpy(pp->name, tmp.name, sizeof tmp.name); pp->namelen = tmp.namelen;
        memcpy(pp->data, tmp.data, sizeof tmp.data); pp->datalen = tmp.datalen;
        uint32_t m = heap_alloc(12, 1);
        M32(m) = 12; M32(m + 4) = id; M32(m + 8) = pp->ctx;
        ev_push(M_PEER_INFO, m, 0, 0);
        break;
    }
    case P_KICK:
        if (D.role == 2 && src == D.host_cid) session_lost(DPNERR_HOSTTERMINATEDSESSION);
        break;
    case P_LEAVE: {
        player_t *pp = pl_by_cid(src);
        if (pp) player_left(pp, REASON_NORMAL);
        break;
    }
    case P_END:
        if (D.role == 2 && src == D.host_cid) session_lost(DPNERR_HOSTTERMINATEDSESSION);
        break;
    case P_PING: {
        buf_t b = {0}; b_u8(&b, P_PONG); b_u32(&b, r_u32(&r));
        net_send(src, &b); free(b.p);
        break;
    }
    case P_PONG: {
        player_t *pp = pl_by_cid(src);
        if (pp) pp->rtt = plat_ticks_ms() - r_u32(&r);
        break;
    }
    }
}

static uint8_t rxbuf[1 << 20];
static void dp_poll(void) {
    int st = plat_net_state();
    if (st == 0) return;
    int n;
    while ((n = plat_net_recv(rxbuf, sizeof rxbuf)) > 0) {
        if (n < 5) continue;
        uint32_t src; memcpy(&src, rxbuf, 4);
        if (src == 0) handle_relay(rxbuf + 4, (uint32_t)n - 4);
        else handle_peer(src, rxbuf + 4, (uint32_t)n - 4);
    }
    if (plat_net_state() == 3 && my_cid) {
        LOG("dplay: lost connection to the relay\n");
        my_cid = 0;
        session_lost(DPNERR_CONNECTIONLOST);
        D.enum_on = 0;
    }
    uint32_t now = plat_ticks_ms();
    if (D.enum_on && my_cid && now - D.enum_last >= 1000) {
        D.enum_last = now;
        buf_t b = {0}; b_u8(&b, R_LIST); b_put(&b, D.enum_app, 16);
        net_send(0, &b); free(b.p);
    }
    for (int i = 0; i < MAXP; i++)
        if (D.pl[i].used == 3 && D.pl[i].leave_at && (int32_t)(now - D.pl[i].leave_at) >= 0) {
            D.pl[i].leave_at = 0;
            q_destroy_player(&D.pl[i], D.pl[i].reason);
        }
    if (D.role && now - D.ping_last >= 2000) {
        D.ping_last = now;
        buf_t b = {0}; b_u8(&b, P_PING); b_u32(&b, now);
        broadcast(&b); free(b.p);
    }
}

/* deliver queued messages to the game's handler, with the per-message follow-up */
static void ev_clear(void) {
    for (int i = ev_head; i < evn; i++) {
        if (evq[i].type == M_RECEIVE && evq[i].a) heap_free(evq[i].a);
        if (evq[i].ptr) heap_free(evq[i].ptr);
    }
    evn = ev_head = 0;
}
static int dispatching;
static void dp_dispatch(void) {
    if (dispatching) return;
    dispatching = 1;
    while (ev_head < evn) {
        ev_t e = evq[ev_head++];
        uint32_t hr = 0;
        if (getenv("DPLAY_TRACE") && e.type != M_RECEIVE && e.type != M_SEND_COMPLETE) LOG("dplay: -> handler msg %u\n", e.type);
        if (D.handler) hr = CALL_GUEST(D.handler, D.uctx, 0xFFFF0000u + e.type, e.ptr);
        uint32_t m = e.ptr;
        switch (e.type) {
        case M_CREATE_PLAYER: { player_t *p = pl_find(M32(m + 4)); if (p) p->ctx = M32(m + 8); break; }
        case M_DESTROY_PLAYER:
            for (int i = 0; i < MAXP; i++) if (D.pl[i].used == 3 && D.pl[i].id == M32(m + 4)) D.pl[i].used = 0;
            break;
        case M_RECEIVE:
            if (hr == DPNSUCCESS_PENDING && npending < 256) { pending_bufs[npending++] = e.a; e.a = 0; }
            if (e.a) heap_free(e.a);
            break;
        case M_ENUM_HOSTS_QUERY: {
            if (hr == S_OK && D.role == 1) {
                buf_t b = {0}; b_u8(&b, P_ENUM_R); b_u32(&b, e.b); b_u32(&b, e.c); b_u32(&b, (uint32_t)pl_count());
                b_sess(&b, &D.sess);
                net_send(e.a, &b); free(b.p);
            }
            addr_release(M32(m + 4)); addr_release(M32(m + 8));
            break;
        }
        case M_ENUM_HOSTS_RESPONSE:
            addr_release(M32(m + 4)); addr_release(M32(m + 8));
            heap_free(M32(m + 12));
            break;
        case M_INDICATE_CONNECT: {
            uint32_t cid = e.a, tok = e.b;
            player_t *np = NULL;
            for (int i = 0; i < MAXP; i++) if (D.pl[i].used == 2 && D.pl[i].cid == cid) { np = &D.pl[i]; break; }
            buf_t b = {0}; b_u8(&b, P_CONN_ACK); b_u32(&b, tok);
            uint32_t rn = M32(m + 0x10), rp = M32(m + 0xc);
            if (!np || D.role != 1 || hr != S_OK) {
                b_u32(&b, DPNERR_HOSTREJECTEDCONNECTION);
                if (np) np->used = 0;
            } else {
                np->used = 1; np->id = D.next_id++; np->ctx = M32(m + 0x18);
                b_u32(&b, S_OK); b_u32(&b, np->id); b_u32(&b, D.host_id); b_sess(&b, &D.sess);
                b_u32(&b, (uint32_t)pl_count());
                for (int i = 0; i < MAXP; i++) if (D.pl[i].used == 1) b_player(&b, &D.pl[i]);
                LOG("dplay: player %08x joined (client %u)\n", np->id, cid);
                buf_t a = {0}; b_u8(&a, P_PLAYER_NEW); b_player(&a, np);
                for (int i = 0; i < MAXP; i++)
                    if (D.pl[i].used == 1 && D.pl[i].id != D.my_id && D.pl[i].id != np->id) net_send(D.pl[i].cid, &a);
                free(a.p);
                q_create_player(np);
            }
            b_blob(&b, rp && rn ? PTR(rp) : NULL, rp && rn <= 1024 ? rn : 0);
            net_send(cid, &b); free(b.p);
            addr_release(M32(m + 0x1c)); addr_release(M32(m + 0x20));
            break;
        }
        }
        heap_free(m);
    }
    evn = ev_head = 0;
    dispatching = 0;
}

/* ------------------------------------------------------------------ IDirectPlay8Address */
#define MAXADDR 256
typedef struct { uint8_t name[64]; uint32_t type, size; uint8_t data[256]; } comp_t;
static struct { int used; uint32_t refs, cid, obj; uint8_t sp[16]; comp_t comp[8]; int ncomp; } addrs[MAXADDR];
static uint32_t vt_addr, vt_peer;
static int addr_slot(uint32_t obj) { uint32_t s = M32(obj + 4); return s < MAXADDR && addrs[s].used && addrs[s].obj == obj ? (int)s : -1; }
static uint32_t addr_new(uint32_t cid) {
    for (int i = 0; i < MAXADDR; i++) if (!addrs[i].used) {
        memset(&addrs[i], 0, sizeof addrs[i]);
        uint32_t o = heap_alloc(8, 1);
        M32(o) = vt_addr; M32(o + 4) = (uint32_t)i;
        addrs[i].used = 1; addrs[i].refs = 1; addrs[i].cid = cid; addrs[i].obj = o;
        memcpy(addrs[i].sp, SP_TCPIP, 16);
        return o;
    }
    fatal("dplay: too many address objects");
}
static void addr_release(uint32_t obj) {
    int s = obj ? addr_slot(obj) : -1;
    if (s < 0) return;
    if (--addrs[s].refs == 0) { addrs[s].used = 0; heap_free(obj); }
}
#define THIS_ADDR int s = addr_slot(ARG(0)); if (s < 0) fatal("dplay: bad address object %08x", ARG(0))
static void a_QueryInterface(void) { addrs[addr_slot(ARG(0))].refs++; M32(ARG(2)) = ARG(0); RET_STD(3, S_OK); }
static void a_AddRef(void) { THIS_ADDR; RET_STD(1, ++addrs[s].refs); }
static void a_Release(void) { THIS_ADDR; uint32_t r = addrs[s].refs - 1; addr_release(ARG(0)); RET_STD(1, r); }
static void a_Duplicate(void) {
    THIS_ADDR;
    uint32_t o = addr_new(addrs[s].cid);
    int d = addr_slot(o);
    memcpy(addrs[d].sp, addrs[s].sp, 16);
    memcpy(addrs[d].comp, addrs[s].comp, sizeof addrs[s].comp); addrs[d].ncomp = addrs[s].ncomp;
    M32(ARG(1)) = o;
    RET_STD(2, S_OK);
}
static void a_SetEqual(void) {
    THIS_ADDR; int o = addr_slot(ARG(1));
    if (o >= 0) { addrs[s].cid = addrs[o].cid; memcpy(addrs[s].sp, addrs[o].sp, 16); memcpy(addrs[s].comp, addrs[o].comp, sizeof addrs[o].comp); addrs[s].ncomp = addrs[o].ncomp; }
    RET_STD(2, S_OK);
}
static void a_IsEqual(void) { THIS_ADDR; int o = addr_slot(ARG(1)); RET_STD(2, o >= 0 && addrs[o].cid == addrs[s].cid ? 0x00158005u : 0x0015800au); }
static void a_Clear(void) { THIS_ADDR; addrs[s].ncomp = 0; addrs[s].cid = 0; RET_STD(1, S_OK); }
static void a_GetSP(void) { THIS_ADDR; memcpy(PTR(ARG(1)), addrs[s].sp, 16); RET_STD(2, S_OK); }
static void a_SetSP(void) { THIS_ADDR; memcpy(addrs[s].sp, PTR(ARG(1)), 16); RET_STD(2, S_OK); }
static void a_GetDevice(void) { memset(PTR(ARG(1)), 0, 16); RET_STD(2, S_OK); }
static void a_SetDevice(void) { RET_STD(2, S_OK); }
static void a_GetURLA(void) {
    THIS_ADDR;
    char url[96]; snprintf(url, sizeof url, "x-directplay:/provider=relay;client=%u", addrs[s].cid);
    uint32_t need = (uint32_t)strlen(url) + 1;
    if (!ARG(1) || M32(ARG(2)) < need) { M32(ARG(2)) = need; RET_STD(3, DPNERR_BUFFERTOOSMALL); }
    memcpy(ARGP(1), url, need); M32(ARG(2)) = need;
    RET_STD(3, S_OK);
}
static void a_GetURLW(void) {
    THIS_ADDR;
    char url[96]; snprintf(url, sizeof url, "x-directplay:/provider=relay;client=%u", addrs[s].cid);
    uint32_t need = (uint32_t)strlen(url) + 1;
    if (!ARG(1) || M32(ARG(2)) < need) { M32(ARG(2)) = need; RET_STD(3, DPNERR_BUFFERTOOSMALL); }
    for (uint32_t i = 0; i < need; i++) M16(ARG(1) + 2 * i) = (uint8_t)url[i];
    M32(ARG(2)) = need;
    RET_STD(3, S_OK);
}
static void a_BuildFromURL(void) { RET_STD(2, S_OK); }
static void a_GetNumComponents(void) { THIS_ADDR; M32(ARG(1)) = (uint32_t)addrs[s].ncomp; RET_STD(2, S_OK); }
static void a_AddComponent(void) {
    THIS_ADDR;
    uint32_t nm = ARG(1), nb = wbytes(nm), size = ARG(3);
    if (nb > 64 || size > 256) RET_STD(5, DPNERR_INVALIDPARAM);
    int i;
    for (i = 0; i < addrs[s].ncomp; i++) if (memcmp(addrs[s].comp[i].name, PTR(nm), nb) == 0) break;
    if (i == addrs[s].ncomp) { if (i == 8) RET_STD(5, DPNERR_GENERIC); addrs[s].ncomp++; }
    memset(&addrs[s].comp[i], 0, sizeof addrs[s].comp[i]);
    memcpy(addrs[s].comp[i].name, PTR(nm), nb);
    addrs[s].comp[i].type = ARG(4); addrs[s].comp[i].size = size;
    if (size) memcpy(addrs[s].comp[i].data, PTR(ARG(2)), size);
    RET_STD(5, S_OK);
}
static void a_GetComponentByName(void) {
    THIS_ADDR;
    uint32_t nm = ARG(1), nb = wbytes(nm);
    for (int i = 0; i < addrs[s].ncomp; i++)
        if (memcmp(addrs[s].comp[i].name, PTR(nm), nb) == 0) {
            comp_t *c = &addrs[s].comp[i];
            if (ARG(4)) M32(ARG(4)) = c->type;
            if (!ARG(2) || M32(ARG(3)) < c->size) { M32(ARG(3)) = c->size; RET_STD(5, DPNERR_BUFFERTOOSMALL); }
            memcpy(PTR(ARG(2)), c->data, c->size); M32(ARG(3)) = c->size;
            RET_STD(5, S_OK);
        }
    RET_STD(5, DPNERR_DOESNOTEXIST);
}
static void a_GetComponentByIndex(void) { RET_STD(7, DPNERR_DOESNOTEXIST); }
static void a_GetUserData(void) { M32(ARG(2)) = 0; RET_STD(3, S_OK); }
static void a_SetUserData(void) { RET_STD(3, S_OK); }
static void a_BuildFromDPADDRESS(void) { RET_STD(3, E_NOTIMPL); }

/* ------------------------------------------------------------------ IDirectPlay8Peer */
static uint32_t peer_refs;
static void p_QueryInterface(void) { peer_refs++; M32(ARG(2)) = ARG(0); RET_STD(3, S_OK); }
static void p_AddRef(void) { RET_STD(1, ++peer_refs); }
static void close_session(int notify);
static void p_Release(void) {
    if (peer_refs) peer_refs--;
    if (!peer_refs) { close_session(0); D.handler = 0; D.inited = 0; ev_clear(); }
    RET_STD(1, peer_refs);
}
static void p_Initialize(void) {
    LOG("dplay: Initialize(ctx=%08x handler=%08x flags=%x)\n", ARG(1), ARG(2), ARG(3));
    D.uctx = ARG(1); D.handler = ARG(2); D.inited = 1;
    RET_STD(4, S_OK);
}
static void p_EnumServiceProviders(void) {
    /* (this, pguidSP, pguidApp, pSPInfoBuffer, pcbBufferSize, pcReturned, dwFlags) */
    static const char name[] = "Internet TCP/IP (WebSocket relay)";
    uint32_t namebytes = 2 * (uint32_t)sizeof name, need = 32 + namebytes;
    uint32_t buf = ARG(3), psize = ARG(4), pcount = ARG(5);
    if (!buf || M32(psize) < need) { M32(psize) = need; M32(pcount) = 0; RET_STD(7, DPNERR_BUFFERTOOSMALL); }
    memset(PTR(buf), 0, 32);
    memcpy(PTR(buf + 4), SP_TCPIP, 16);
    M32(buf + 20) = buf + 32;
    for (uint32_t i = 0; i < sizeof name; i++) M16(buf + 32 + 2 * i) = (uint8_t)name[i];
    M32(psize) = need; M32(pcount) = 1;
    RET_STD(7, S_OK);
}
static void p_CancelAsyncOperation(void) {
    uint32_t h = ARG(1);
    if (D.enum_on && (h == D.enum_handle || h == 0)) {
        D.enum_on = 0;
        q_async_complete(D.enum_handle, D.enum_ctx, DPNERR_USERCANCEL);
        dp_dispatch();   /* the game expects the completion before Cancel returns (it clears its handle afterwards) */
        RET_STD(3, S_OK);
    }
    RET_STD(3, h ? DPNERR_INVALIDHANDLE : S_OK);
}
static void p_EnumHosts(void) {
    /* (this, pApplicationDesc, pAddrHost, pDeviceInfo, pUserEnumData, dwUserEnumDataSize, dwEnumCount,
     *  dwRetryInterval, dwTimeOut, pvUserContext, pAsyncHandle, dwFlags) */
    if (!D.inited) RET_STD(12, DPNERR_UNINITIALIZED);
    if (!net_ensure()) RET_STD(12, DPNERR_NOCONNECTION);
    memcpy(D.enum_app, PTR(ARG(1) + 0x18), 16);
    D.enum_on = 1; D.enum_handle = ++D.next_handle | 0x40000000u; D.enum_ctx = ARG(9); D.enum_last = plat_ticks_ms() - 1000;
    if (ARG(10)) M32(ARG(10)) = D.enum_handle;
    LOG("dplay: EnumHosts started\n");
    RET_STD(12, DPNSUCCESS_PENDING);
}
static void p_Host(void) {
    /* (this, pdnAppDesc, prgpDeviceInfo, cDeviceInfo, pdnSecurity, pdnCredentials, pvPlayerContext, dwFlags) */
    if (!D.inited) RET_STD(8, DPNERR_UNINITIALIZED);
    if (!net_ensure()) RET_STD(8, DPNERR_NOCONNECTION);
    session_reset();
    memset(D.pl, 0, sizeof D.pl);
    sess_from_guest(&D.sess, ARG(1));
    for (int i = 0; i < 16; i++) D.sess.inst[i] = (uint8_t)(rand() ^ (plat_ticks_ms() >> (i & 7)));
    D.role = 1; D.next_id = 0x00100001u;
    D.my_id = D.host_id = D.next_id++; D.host_cid = my_cid;
    player_t *me = pl_add();
    *me = D.local; me->used = 1; me->id = D.my_id; me->cid = my_cid;
    me->flags = DPNPLAYER_LOCAL | DPNPLAYER_HOST; me->ctx = ARG(6);
    buf_t b = {0}; b_u8(&b, R_HOST_ON); b_put(&b, D.sess.app, 16);
    net_send(0, &b); free(b.p);
    LOG("dplay: hosting session (max %u players), player id %08x\n", D.sess.maxp, D.my_id);
    q_create_player(me);
    dp_dispatch();
    RET_STD(8, S_OK);
}
static void p_Connect(void) {
    /* (this, pdnAppDesc, pHostAddr, pDeviceInfo, pdnSecurity, pdnCredentials, pvUserConnectData,
     *  dwUserConnectDataSize, pvPlayerContext, pvAsyncContext, phAsyncHandle, dwFlags) */
    if (!D.inited) RET_STD(12, DPNERR_UNINITIALIZED);
    int s = addr_slot(ARG(2));
    uint32_t host = s >= 0 ? addrs[s].cid : 0;
    LOG("dplay: Connect to relay client %u (flags %x)\n", host, ARG(11));
    if (!host || !net_ensure()) RET_STD(12, DPNERR_NOCONNECTION);
    session_reset();
    memset(D.pl, 0, sizeof D.pl);
    D.conn_wait = 1; D.conn_done = 0; D.conn_token = ++D.next_handle;
    buf_t b = {0}; b_u8(&b, P_CONN_REQ); b_u32(&b, D.conn_token);
    b_blob(&b, D.local.name, D.local.namelen); b_blob(&b, D.local.data, D.local.datalen);
    b_blob(&b, ARG(6) ? PTR(ARG(6)) : NULL, ARG(6) ? ARG(7) : 0);
    net_send(host, &b); free(b.p);
    uint32_t t0 = plat_ticks_ms();
    while (!D.conn_done && plat_ticks_ms() - t0 < 15000 && plat_net_state() == 2) { dp_poll(); if (!D.conn_done) plat_sleep(2); }
    D.conn_wait = 0;
    uint32_t hr = D.conn_done ? D.conn_result : DPNERR_TIMEDOUT;
    if (hr == S_OK && D.role == 2) {
        player_t *me = pl_find(D.my_id);
        if (me) { me->ctx = ARG(8); q_create_player(me); }
        for (int i = 0; i < MAXP; i++) if (D.pl[i].used && D.pl[i].id != D.my_id) q_create_player(&D.pl[i]);
        LOG("dplay: joined session as %08x (%d players)\n", D.my_id, pl_count());
    } else {
        LOG("dplay: Connect failed %08x\n", hr);
        if (hr == S_OK) hr = DPNERR_CONNECTIONLOST;
        memset(D.pl, 0, sizeof D.pl); D.role = 0;
    }
    uint32_t m = heap_alloc(0x1c + D.conn_replylen + 4, 1);
    M32(m) = 0x1c; M32(m + 8) = ARG(9); M32(m + 12) = hr;
    if (D.conn_replylen) { M32(m + 16) = m + 0x1c; M32(m + 20) = D.conn_replylen; memcpy(PTR(m + 0x1c), D.conn_reply, D.conn_replylen); }
    M32(m + 24) = D.my_id;
    ev_push(M_CONNECT_COMPLETE, m, 0, 0);
    dp_dispatch();
    RET_STD(12, hr);
}
static void p_SendTo(void) {
    /* (this, dpnid, prgBufferDesc, cBufferDesc, dwTimeOut, pvAsyncContext, phAsyncHandle, dwFlags) */
    uint32_t to = ARG(1), bd = ARG(2), nb = ARG(3), fl = ARG(7);
    if (!D.role) RET_STD(8, DPNERR_NOCONNECTION);
    buf_t d = {0}; b_u8(&d, P_DATA); b_u32(&d, D.my_id);
    uint32_t hdr = d.n;
    for (uint32_t i = 0; i < nb; i++) b_put(&d, PTR(M32(bd + 8 * i + 4)), M32(bd + 8 * i));
    if (to == 0) {
        broadcast(&d);
        if (!(fl & DPNSEND_NOLOOPBACK)) q_receive(D.my_id, d.p + hdr, d.n - hdr);
    } else if (to == D.my_id) {
        q_receive(D.my_id, d.p + hdr, d.n - hdr);
    } else {
        player_t *p = pl_find(to);
        if (!p || p->used != 1) { free(d.p); RET_STD(8, DPNERR_INVALIDPLAYER); }
        net_send(p->cid, &d);
    }
    free(d.p);
    if (fl & DPNSEND_SYNC) RET_STD(8, S_OK);
    uint32_t h = ++D.next_handle | 0x20000000u;
    if (ARG(6)) M32(ARG(6)) = h;
    if (!(fl & DPNSEND_NOCOMPLETE)) {
        uint32_t m = heap_alloc(0x14, 1);
        player_t *p = to ? pl_find(to) : NULL;
        M32(m) = 0x14; M32(m + 4) = h; M32(m + 8) = ARG(5); M32(m + 12) = S_OK; M32(m + 16) = p ? p->rtt / 2 + 1 : 1;
        ev_push(M_SEND_COMPLETE, m, 0, 0);
    }
    RET_STD(8, DPNSUCCESS_PENDING);
}
static void p_GetSendQueueInfo(void) { if (ARG(2)) M32(ARG(2)) = 0; if (ARG(3)) M32(ARG(3)) = 0; RET_STD(5, S_OK); }
static void p_GetApplicationDesc(void) {
    if (!D.role) RET_STD(4, DPNERR_NOCONNECTION);
    uint32_t need = 0x48 + D.sess.namelen + D.sess.reslen;
    if (!ARG(1) || M32(ARG(2)) < need) { M32(ARG(2)) = need; RET_STD(4, DPNERR_BUFFERTOOSMALL); }
    uint32_t g = appdesc_guest(&D.sess, (uint32_t)pl_count()), o = ARG(1);
    memcpy(PTR(o), PTR(g), need);
    if (D.sess.namelen) M32(o + 0x30) = o + 0x48;
    if (D.sess.reslen) M32(o + 0x40) = o + 0x48 + D.sess.namelen;
    heap_free(g);
    RET_STD(4, S_OK);
}
static void p_SetApplicationDesc(void) {
    if (D.role != 1) RET_STD(3, DPNERR_NOTHOST);
    uint8_t inst[16]; memcpy(inst, D.sess.inst, 16);
    sess_from_guest(&D.sess, ARG(1)); memcpy(D.sess.inst, inst, 16);
    RET_STD(3, S_OK);
}
static void p_EnumPlayersAndGroups(void) {
    uint32_t arr = ARG(1), pc = ARG(2);
    uint32_t n = 0;
    for (int i = 0; i < MAXP; i++) if (D.pl[i].used == 1) n++;
    if (!(ARG(3) & 1)) { M32(pc) = 0; RET_STD(4, S_OK); }   /* groups only: none */
    if (!arr || M32(pc) < n) { M32(pc) = n; RET_STD(4, DPNERR_BUFFERTOOSMALL); }
    uint32_t k = 0;
    for (int i = 0; i < MAXP; i++) if (D.pl[i].used == 1) M32(arr + 4 * k++) = D.pl[i].id;
    if (getenv("DPLAY_TRACE")) LOG("dplay: EnumPlayersAndGroups -> %u\n", n);
    M32(pc) = n;
    RET_STD(4, S_OK);
}
static void p_SetPeerInfo(void) {
    info_from_guest(&D.local, ARG(1));
    player_t *me = D.role ? pl_find(D.my_id) : NULL;
    if (me) {
        info_from_guest(me, ARG(1));
        buf_t b = {0}; b_u8(&b, P_PEER_INFO); b_u32(&b, me->id); b_blob(&b, me->name, me->namelen); b_blob(&b, me->data, me->datalen);
        broadcast(&b); free(b.p);
    }
    RET_STD(5, S_OK);
}
static void p_GetPeerInfo(void) {
    /* (this, dpnid, pdpnPlayerInfo, pdwSize, dwFlags) */
    player_t *p = pl_find(ARG(1));
    if (getenv("DPLAY_TRACE")) LOG("dplay: GetPeerInfo(%08x) -> %s\n", ARG(1), p ? (p->used == 1 ? "ok" : "leaving") : "none");
    if (!p) RET_STD(5, DPNERR_INVALIDPLAYER);
    uint32_t need = 0x18 + p->namelen + p->datalen, o = ARG(2);
    if (!o || M32(ARG(3)) < need) { M32(ARG(3)) = need; RET_STD(5, DPNERR_BUFFERTOOSMALL); }
    M32(o + 4) = 3;
    M32(o + 8) = p->namelen ? o + 0x18 : 0;
    if (p->namelen) memcpy(PTR(o + 0x18), p->name, p->namelen);
    M32(o + 12) = p->datalen ? o + 0x18 + p->namelen : 0;
    if (p->datalen) memcpy(PTR(o + 0x18 + p->namelen), p->data, p->datalen);
    M32(o + 16) = p->datalen;
    M32(o + 20) = p->flags & (DPNPLAYER_LOCAL | DPNPLAYER_HOST);
    M32(ARG(3)) = need;
    RET_STD(5, S_OK);
}
static void p_GetPeerAddress(void) {
    player_t *p = pl_find(ARG(1));
    if (!p) RET_STD(4, DPNERR_INVALIDPLAYER);
    M32(ARG(2)) = addr_new(p->cid);
    RET_STD(4, S_OK);
}
static void p_GetLocalHostAddresses(void) {
    uint32_t pc = ARG(2);
    if (!ARG(1) || M32(pc) < 1) { M32(pc) = 1; RET_STD(4, DPNERR_BUFFERTOOSMALL); }
    M32(ARG(1)) = addr_new(my_cid); M32(pc) = 1;
    RET_STD(4, S_OK);
}
static void close_session(int notify) {
    if (D.enum_on) { D.enum_on = 0; if (notify) q_async_complete(D.enum_handle, D.enum_ctx, DPNERR_USERCANCEL); }
    if (D.role) {
        buf_t b = {0};
        if (D.role == 1) b_u8(&b, P_END); else { b_u8(&b, P_LEAVE); b_u32(&b, D.my_id); }
        broadcast(&b); free(b.p);
        if (notify) {
            for (int i = 0; i < MAXP; i++)
                if ((D.pl[i].used == 1 || (D.pl[i].used == 3 && D.pl[i].leave_at)) && D.pl[i].id != D.my_id) {
                    D.pl[i].leave_at = 0;
                    q_destroy_player(&D.pl[i], D.pl[i].used == 3 ? D.pl[i].reason : REASON_NORMAL);
                }
            player_t *me = pl_find(D.my_id);
            if (me) q_destroy_player(me, REASON_NORMAL);
        }
    }
    if (notify) dp_dispatch();
    session_reset();
}
static void p_Close(void) {
    LOG("dplay: Close\n");
    close_session(1);
    D.inited = 0;
    ev_clear();
    RET_STD(2, S_OK);
}
static void p_DestroyPeer(void) {
    /* (this, dpnidClient, pvDestroyData, dwDestroyDataSize, dwFlags) */
    if (D.role != 1) RET_STD(5, DPNERR_NOTHOST);
    player_t *p = pl_find(ARG(1));
    if (!p || p->id == D.my_id) RET_STD(5, DPNERR_INVALIDPLAYER);
    buf_t b = {0}; b_u8(&b, P_KICK); b_blob(&b, ARG(2) ? PTR(ARG(2)) : NULL, ARG(2) ? ARG(3) : 0);
    net_send(p->cid, &b); free(b.p);
    buf_t g = {0}; b_u8(&g, P_PLAYER_GONE); b_u32(&g, p->id); b_u32(&g, REASON_HOSTDESTROYEDPLAYER);
    for (int i = 0; i < MAXP; i++)
        if (D.pl[i].used == 1 && D.pl[i].id != D.my_id && D.pl[i].id != p->id) net_send(D.pl[i].cid, &g);
    free(g.p);
    player_left(p, REASON_HOSTDESTROYEDPLAYER);
    RET_STD(5, S_OK);
}
static void p_ReturnBuffer(void) {
    for (int i = 0; i < npending; i++)
        if (pending_bufs[i] == ARG(1)) { heap_free(ARG(1)); pending_bufs[i] = pending_bufs[--npending]; RET_STD(3, S_OK); }
    RET_STD(3, DPNERR_INVALIDHANDLE);
}
static void p_GetPlayerContext(void) {
    player_t *p = pl_find(ARG(1));
    if (!p) RET_STD(4, DPNERR_INVALIDPLAYER);
    M32(ARG(2)) = p->ctx;
    RET_STD(4, S_OK);
}
static void p_GetCaps(void) { RET_STD(3, S_OK); }
static void p_SetCaps(void) { RET_STD(3, S_OK); }
static void p_SPCaps(void) { RET_STD(4, S_OK); }
static void p_GetConnectionInfo(void) {
    player_t *p = pl_find(ARG(1));
    if (!p) RET_STD(4, DPNERR_INVALIDPLAYER);
    uint32_t o = ARG(2), sz = M32(o);
    if (sz >= 8) { memset(PTR(o + 4), 0, sz - 4); M32(o + 4) = p->rtt; }
    if (sz >= 12) M32(o + 8) = 1000000;   /* throughput, bytes/s */
    RET_STD(4, S_OK);
}
static void p_TerminateSession(void) {
    if (D.role != 1) RET_STD(4, DPNERR_NOTHOST);
    close_session(1);
    RET_STD(4, S_OK);
}
#define STUB(name, n) static void p_##name(void) { LOG("dplay: unimplemented IDirectPlay8Peer::" #name "\n"); RET_STD(n, E_NOTIMPL); }
STUB(CreateGroup, 6) STUB(DestroyGroup, 5) STUB(AddPlayerToGroup, 6) STUB(RemovePlayerFromGroup, 6)
STUB(SetGroupInfo, 6) STUB(GetGroupInfo, 5) STUB(EnumGroupMembers, 5) STUB(GetGroupContext, 4) STUB(RegisterLobby, 4)

static const gfn_t peer_m[] = {
    p_QueryInterface, p_AddRef, p_Release, p_Initialize, p_EnumServiceProviders, p_CancelAsyncOperation, p_Connect,
    p_SendTo, p_GetSendQueueInfo, p_Host, p_GetApplicationDesc, p_SetApplicationDesc, p_CreateGroup, p_DestroyGroup,
    p_AddPlayerToGroup, p_RemovePlayerFromGroup, p_SetGroupInfo, p_GetGroupInfo, p_EnumPlayersAndGroups,
    p_EnumGroupMembers, p_SetPeerInfo, p_GetPeerInfo, p_GetPeerAddress, p_GetLocalHostAddresses, p_Close, p_EnumHosts,
    p_DestroyPeer, p_ReturnBuffer, p_GetPlayerContext, p_GetGroupContext, p_GetCaps, p_SetCaps, p_SPCaps, p_SPCaps,
    p_GetConnectionInfo, p_RegisterLobby, p_TerminateSession};
static const char *const peer_n[] = {
    "dp8peer::QueryInterface", "dp8peer::AddRef", "dp8peer::Release", "dp8peer::Initialize", "dp8peer::EnumServiceProviders",
    "dp8peer::CancelAsyncOperation", "dp8peer::Connect", "dp8peer::SendTo", "dp8peer::GetSendQueueInfo", "dp8peer::Host",
    "dp8peer::GetApplicationDesc", "dp8peer::SetApplicationDesc", "dp8peer::CreateGroup", "dp8peer::DestroyGroup",
    "dp8peer::AddPlayerToGroup", "dp8peer::RemovePlayerFromGroup", "dp8peer::SetGroupInfo", "dp8peer::GetGroupInfo",
    "dp8peer::EnumPlayersAndGroups", "dp8peer::EnumGroupMembers", "dp8peer::SetPeerInfo", "dp8peer::GetPeerInfo",
    "dp8peer::GetPeerAddress", "dp8peer::GetLocalHostAddresses", "dp8peer::Close", "dp8peer::EnumHosts",
    "dp8peer::DestroyPeer", "dp8peer::ReturnBuffer", "dp8peer::GetPlayerContext", "dp8peer::GetGroupContext",
    "dp8peer::GetCaps", "dp8peer::SetCaps", "dp8peer::SetSPCaps", "dp8peer::GetSPCaps", "dp8peer::GetConnectionInfo",
    "dp8peer::RegisterLobby", "dp8peer::TerminateSession"};
static const gfn_t addr_m[] = {
    a_QueryInterface, a_AddRef, a_Release, a_BuildFromURL, a_BuildFromURL, a_Duplicate, a_SetEqual, a_IsEqual, a_Clear,
    a_GetURLW, a_GetURLA, a_GetSP, a_GetUserData, a_SetSP, a_SetUserData, a_GetNumComponents, a_GetComponentByName,
    a_GetComponentByIndex, a_AddComponent, a_GetDevice, a_SetDevice, a_BuildFromDPADDRESS};
static const char *const addr_n[] = {
    "dp8addr::QueryInterface", "dp8addr::AddRef", "dp8addr::Release", "dp8addr::BuildFromURLW", "dp8addr::BuildFromURLA",
    "dp8addr::Duplicate", "dp8addr::SetEqual", "dp8addr::IsEqual", "dp8addr::Clear", "dp8addr::GetURLW",
    "dp8addr::GetURLA", "dp8addr::GetSP", "dp8addr::GetUserData", "dp8addr::SetSP", "dp8addr::SetUserData",
    "dp8addr::GetNumComponents", "dp8addr::GetComponentByName", "dp8addr::GetComponentByIndex",
    "dp8addr::AddComponent", "dp8addr::GetDevice", "dp8addr::SetDevice", "dp8addr::BuildFromDPADDRESS"};

/* CoCreateInstance hook: 0 on success, REGDB_E_CLASSNOTREG if the class is not DirectPlay */
uint32_t dplay_create(uint32_t clsid, uint32_t iid, uint32_t ppv) {
    (void)iid;
    if (!vt_peer) {
        vt_peer = com_make_vtbl(peer_m, peer_n, (int)(sizeof peer_m / sizeof *peer_m));
        vt_addr = com_make_vtbl(addr_m, addr_n, (int)(sizeof addr_m / sizeof *addr_m));
    }
    if (memcmp(PTR(clsid), CLSID_Peer, 16) == 0) {
        static uint32_t peer_obj;
        if (!peer_obj) { peer_obj = com_new(vt_peer, 4); }
        peer_refs++;
        M32(ppv) = peer_obj;
        return S_OK;
    }
    if (memcmp(PTR(clsid), CLSID_Address, 16) == 0) { M32(ppv) = addr_new(0); return S_OK; }
    return 0x80040154u;
}

/* ------------------------------------------------------------------ the game's network threads
 * CDPTimer: param {interval, callback, &stop}; calls callback every `interval` ms until *stop
 *   is set.  The thread frees its parameter block with the game's free().
 * CFileTransferManager: param = manager; while (!*(char *)param) manager_singleton()->Step().
 * Both are busy loops in the original, so running them from the pump is equivalent.
 *
 * The thread procedures are recognised by their code (the game versions place them at
 * different addresses and compile the loops slightly differently), and the helpers they call
 * are read from the matched call instructions.  Pattern tokens: hex byte, "??" any byte,
 * "R" a rel32 call target to capture (4 bytes). */
static const char *const timer_patterns[] = {
    "51 51 53 55 56 8b 74 24 18 33 c0 85 f6 0f 95 c0 57 68 ?? ?? ?? ?? 50 6a ?? 68 ?? ?? ?? ?? e8 ?? ?? ?? ?? "
    "8b 1d ?? ?? ?? ?? 83 c4 10 33 ed ff d3 8b 7e 08 89 44 24 1c 8b 06 89 44 24 10 8b 46 04 56 89 44 24 18 e8 R 59",
    NULL};
static const char *const ftm_patterns[] = {
    /* Saga, Northland: do { } while loop */
    "56 8b 74 24 08 33 c0 85 f6 0f 95 c0 68 ?? ?? ?? ?? 50 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? e8 ?? ?? ?? ?? 83 c4 10 "
    "eb 0c e8 R 8b c8 e8 R 80 3e 00 74 ef",
    /* 8th Wonder: while loop */
    "56 8b 74 24 08 33 c0 85 f6 0f 95 c0 68 ?? ?? ?? ?? 50 68 ?? ?? ?? ?? 68 ?? ?? ?? ?? e8 ?? ?? ?? ?? 83 c4 10 "
    "80 3e 00 75 0e e8 R 8b c8 e8 R eb ed",
    NULL};
/* match guest code at addr; captured call targets go to out[]; returns the number captured or -1 */
static int code_match(uint32_t addr, const char *pat, uint32_t *out) {
    int n = 0;
    for (const char *p = pat; *p; ) {
        while (*p == ' ') p++;
        if (!*p) break;
        if (p[0] == 'R') {
            uint32_t rel = M32(addr);
            out[n++] = addr + 4 + rel;
            addr += 4; p++;
        } else if (p[0] == '?') {
            addr++; p += 2;
        } else {
            unsigned v; if (sscanf(p, "%2x", &v) != 1 || M8(addr) != v) return -1;
            addr++; p += 2;
        }
    }
    return n;
}
enum { T_TIMER = 1, T_FTM = 2 };
typedef struct { int used, done, kind; uint32_t proc, interval, cb, stop, acc, last, getter, step; } task_t;
static task_t tasks[8];
int coop_thread_create(uint32_t proc, uint32_t param) {
    if (proc < IMAGE_BASE || proc >= IMAGE_BASE + 0x200000) return -1;
    uint32_t cap[4];
    int kind = 0;
    for (int i = 0; !kind && timer_patterns[i]; i++) if (code_match(proc, timer_patterns[i], cap) == 1) kind = T_TIMER;
    uint32_t game_free = cap[0];
    for (int i = 0; !kind && ftm_patterns[i]; i++) if (code_match(proc, ftm_patterns[i], cap) == 2) kind = T_FTM;
    if (!kind) return -1;
    for (int i = 0; i < 8; i++) if (!tasks[i].used) {
        task_t *t = &tasks[i];
        memset(t, 0, sizeof *t);
        t->used = 1; t->kind = kind; t->proc = proc; t->last = plat_ticks_ms();
        if (kind == T_TIMER) {
            t->interval = M32(param); t->cb = M32(param + 4); t->stop = M32(param + 8);
            CALL_GUEST(game_free, param);
        } else {
            t->stop = param; t->getter = cap[0]; t->step = cap[1];
        }
        LOG("thread %08x (%s) started as cooperative task %d\n", proc, kind == T_TIMER ? "network timer" : "file transfer", i);
        return i;
    }
    return -1;
}
static int task_step(task_t *t) {   /* returns 0 when the task finished */
    if (M8(t->stop)) { t->done = 1; return 0; }
    if (t->kind == T_TIMER) {
        uint32_t now = plat_ticks_ms();
        t->acc += now - t->last; t->last = now;
        if (t->acc >= t->interval) { t->acc = 0; call_guest(t->cb, 0, NULL); }
    } else {
        uint32_t t0 = plat_ticks_ms();
        do {
            uint32_t mgr = call_guest(t->getter, 0, NULL);
            if (!mgr) break;
            R.ecx = mgr;
            call_guest(t->step, 0, NULL);
        } while (!M8(t->stop) && plat_ticks_ms() - t0 < 4);
    }
    return 1;
}
/* WaitForSingleObject on a task: finished once its stop flag is set */
int coop_thread_wait(int id, uint32_t timeout) {
    if (id < 0 || id >= 8 || !tasks[id].used) return 1;
    task_t *t = &tasks[id];
    uint32_t t0 = plat_ticks_ms();
    for (;;) {
        if (t->done || M8(t->stop)) { t->done = 1; t->used = 0; return 1; }
        if (timeout != 0xffffffffu && plat_ticks_ms() - t0 >= timeout) return 0;
        bg_pump();
        plat_sleep(1);
    }
}

/* ------------------------------------------------------------------ pump */
static int pumping;
void bg_pump(void) {
    if (pumping) return;
    pumping = 1;
    dp_poll();
    if (D.inited) dp_dispatch();
    for (int i = 0; i < 8; i++)
        if (tasks[i].used && !tasks[i].done && !task_step(&tasks[i])) LOG("cooperative task %d finished\n", i);
    pumping = 0;
}
