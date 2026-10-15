/* Music engine replacing DirectMusic's runtime: plays DirectMusic segments (.sgt)
 * made of tempo / time-signature / band / pattern / sequence tracks through a small
 * DLS (Downloadable Sounds) wavetable synthesizer.
 *
 * Only what Cultures' music uses is supported: pattern notes in fixed play mode
 * (plain MIDI note values), pattern variations, CC/pitch-bend curves, bands with
 * DLS instruments.  Output is 44.1 kHz stereo mixed into the DirectSound mixer. */
#include "runtime.h"
#include "dmusic_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define RATE 44100
#define PPQ 768
#define MAXVOICES 64
#define NCHAN 256

uint8_t *dm_read_file(const char *name, uint32_t *len);    /* dmusic.c */

#define R16(p) (*(const u16u *)(p))
#define R32(p) (*(const u32u *)(p))
#define S16(p) ((int16_t)R16(p))
#define S32(p) ((int32_t)R32(p))

static uint32_t rng_state = 0x12345678;
static uint32_t rnd(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }

/* ------------------------------------------------------------------ RIFF */
typedef struct { const uint8_t *p; uint32_t len; char id[5]; char type[5]; } Chunk;
/* iterate chunks inside [p, p+len): returns 1 and fills c, advancing *off */
static int next_chunk(const uint8_t *p, uint32_t len, uint32_t *off, Chunk *c) {
    if (*off + 8 > len) return 0;
    const uint8_t *h = p + *off;
    uint32_t sz = R32(h + 4);
    if (*off + 8 + sz > len) sz = len - *off - 8;
    memcpy(c->id, h, 4); c->id[4] = 0;
    c->type[0] = 0;
    if ((!memcmp(c->id, "RIFF", 4) || !memcmp(c->id, "LIST", 4)) && sz >= 4) {
        memcpy(c->type, h + 8, 4); c->type[4] = 0;
        c->p = h + 12; c->len = sz - 4;
    } else {
        c->p = h + 8; c->len = sz;
    }
    *off += 8 + sz + (sz & 1);
    return 1;
}
static int is_list(const Chunk *c, const char *type) {
    return (!strcmp(c->id, "RIFF") || !strcmp(c->id, "LIST")) && !strcmp(c->type, type);
}
static void wcopy(const uint8_t *p, uint32_t len, char *out, int cap) {
    int i = 0;
    for (uint32_t k = 0; k + 1 < len && i < cap - 1; k += 2) {
        uint16_t ch = R16(p + k);
        if (!ch) break;
        out[i++] = ch < 128 ? (char)ch : '_';
    }
    out[i] = 0;
}

/* ------------------------------------------------------------------ DLS */
typedef struct {
    float delay, attack, hold, decay, release;   /* seconds */
    float sustain;                               /* linear 0..1 */
    float pan;                                   /* -0.5..0.5 */
    float atten_db;                              /* constant attenuation */
    float pitch_cents;
    float vel_atten_db;                          /* attenuation at velocity 0 (negative = quieter) */
} Art;
typedef struct { int16_t *data; uint32_t len, rate; int unity; int fine; float atten_db; int looped; uint32_t ls, ll; } Wave;
typedef struct {
    uint16_t klo, khi, vlo, vhi;
    int wave;
    int has_wsmp, unity, fine, looped; uint32_t ls, ll; float atten_db;
    Art art;
} Region;
typedef struct { uint32_t bank, program; int drum; int nreg; Region *reg; } Instrument;
typedef struct { char name[64]; int ninst; Instrument *inst; int nwave; Wave *wave; } Collection;

#define MAXCOLL 128
static Collection colls[MAXCOLL];
static int ncolls;

static void art_default(Art *a) {
    a->delay = 0; a->attack = 0.001f; a->hold = 0; a->decay = 0.001f; a->release = 0.001f;
    a->sustain = 1.0f; a->pan = 0; a->atten_db = 0; a->pitch_cents = 0; a->vel_atten_db = -96.0f;
}
static float tc2sec(int32_t v) {
    if (v == (int32_t)0x80000000) return 0;
    return (float)pow(2.0, (double)v / 65536.0 / 1200.0);
}
static void parse_art(const uint8_t *p, uint32_t len, Art *a) {
    /* art1/art2: cbSize, cConnectionBlocks, then blocks {usSource, usControl, usDestination, usTransform, lScale} */
    if (len < 8) return;
    uint32_t cb = R32(p), n = R32(p + 4);
    const uint8_t *b = p + cb;
    for (uint32_t i = 0; i < n && (uint32_t)(b - p) + 12 <= len; i++, b += 12) {
        uint16_t src = R16(b), ctl = R16(b + 2), dst = R16(b + 4);
        int32_t sc = S32(b + 8);
        if (ctl != 0) continue;
        if (src == 0) {
            switch (dst) {
            case 0x206: a->attack = tc2sec(sc); break;
            case 0x207: a->decay = tc2sec(sc); break;
            case 0x209: a->release = tc2sec(sc); break;
            case 0x20a: a->sustain = (float)(sc / 65536.0 / 1000.0); break;
            case 0x20b: a->delay = tc2sec(sc); break;
            case 0x20c: a->hold = tc2sec(sc); break;
            case 0x004: a->pan = (float)(sc / 65536.0 / 1000.0); break;
            case 0x001: a->atten_db = (float)(sc / 65536.0 / 10.0); break;
            case 0x003: a->pitch_cents = (float)(sc / 65536.0); break;
            }
        } else if (src == 2 && dst == 0x001) {
            a->vel_atten_db = (float)(sc / 65536.0 / 10.0);
        }
    }
    if (a->sustain < 0) a->sustain = 0;
    if (a->sustain > 1) a->sustain = 1;
}
static void parse_wsmp(const uint8_t *p, uint32_t len, int *unity, int *fine, float *att, int *looped, uint32_t *ls, uint32_t *ll) {
    if (len < 20) return;
    uint32_t cb = R32(p);
    *unity = R16(p + 4); *fine = S16(p + 6);
    *att = (float)(S32(p + 8) / 65536.0 / 10.0);
    uint32_t nloops = R32(p + 16);
    *looped = 0;
    if (nloops && cb + 16 <= len) {
        const uint8_t *l = p + cb;
        *ls = R32(l + 8); *ll = R32(l + 12);
        *looped = *ll > 0;
    }
}

static int load_collection(const char *name) {
    for (int i = 0; i < ncolls; i++) if (!strcasecmp(colls[i].name, name)) return i;
    if (ncolls >= MAXCOLL) return -1;
    uint32_t len;
    uint8_t *d = dm_read_file(name, &len);
    if (!d) { LOG("music: cannot load instrument collection %s\n", name); return -1; }
    Collection *C = &colls[ncolls];
    memset(C, 0, sizeof *C);
    snprintf(C->name, sizeof C->name, "%s", name);
    uint32_t off = 0; Chunk top;
    if (!next_chunk(d, len, &off, &top) || !is_list(&top, "DLS ")) { free(d); LOG("music: %s is not DLS\n", name); return -1; }
    const uint8_t *ptbl = NULL; uint32_t ptbl_len = 0;
    const uint8_t *wvpl = NULL; uint32_t wvpl_len = 0;
    Chunk lins = {0};
    off = 0; Chunk c;
    while (next_chunk(top.p, top.len, &off, &c)) {
        if (!strcmp(c.id, "ptbl")) { ptbl = c.p; ptbl_len = c.len; }
        else if (is_list(&c, "wvpl")) { wvpl = c.p; wvpl_len = c.len; }
        else if (is_list(&c, "lins")) lins = c;
    }
    /* wave pool */
    if (ptbl && wvpl) {
        uint32_t cb = R32(ptbl), n = R32(ptbl + 4);
        C->nwave = (int)n;
        C->wave = calloc(n ? n : 1, sizeof(Wave));
        for (uint32_t i = 0; i < n && cb + 4 * i + 4 <= ptbl_len; i++) {
            uint32_t woff = R32(ptbl + cb + 4 * i);
            /* woff is relative to the start of the wvpl list data (after 'wvpl') */
            uint32_t o2 = woff; Chunk wv;
            if (!next_chunk(wvpl, wvpl_len, &o2, &wv) || !is_list(&wv, "wave")) continue;
            Wave *W = &C->wave[i];
            W->unity = 60; W->fine = 0; W->atten_db = 0;
            uint32_t o3 = 0; Chunk sc;
            int fmt_bits = 16, chans = 1; uint32_t rate = 22050;
            const uint8_t *pcm = NULL; uint32_t pcmlen = 0;
            while (next_chunk(wv.p, wv.len, &o3, &sc)) {
                if (!strcmp(sc.id, "fmt ") && sc.len >= 16) { chans = R16(sc.p + 2); rate = R32(sc.p + 4); fmt_bits = R16(sc.p + 14); }
                else if (!strcmp(sc.id, "data")) { pcm = sc.p; pcmlen = sc.len; }
                else if (!strcmp(sc.id, "wsmp")) parse_wsmp(sc.p, sc.len, &W->unity, &W->fine, &W->atten_db, &W->looped, &W->ls, &W->ll);
            }
            if (!pcm) continue;
            uint32_t frames = pcmlen / (uint32_t)(chans * fmt_bits / 8);
            W->data = malloc(sizeof(int16_t) * (frames + 4));
            for (uint32_t k = 0; k < frames; k++) {
                int v = fmt_bits == 16 ? S16(pcm + 2 * k * chans) : ((int)pcm[k * chans] - 128) << 8;
                W->data[k] = (int16_t)v;
            }
            for (int k = 0; k < 4; k++) W->data[frames + k] = frames ? W->data[frames - 1] : 0;
            W->len = frames; W->rate = rate;
        }
    }
    /* instruments */
    if (lins.p) {
        uint32_t o4 = 0; Chunk ins;
        int cap = 16;
        C->inst = calloc(cap, sizeof(Instrument));
        while (next_chunk(lins.p, lins.len, &o4, &ins)) {
            if (!is_list(&ins, "ins ")) continue;
            if (C->ninst == cap) { cap *= 2; C->inst = realloc(C->inst, cap * sizeof(Instrument)); }
            Instrument *I = &C->inst[C->ninst++];
            memset(I, 0, sizeof *I);
            Art iart; art_default(&iart);
            Chunk lrgn = {0};
            uint32_t o5 = 0; Chunk ic;
            while (next_chunk(ins.p, ins.len, &o5, &ic)) {
                if (!strcmp(ic.id, "insh") && ic.len >= 12) {
                    uint32_t bank = R32(ic.p + 4);
                    I->drum = (bank & 0x80000000u) != 0;
                    I->bank = bank & 0x7fff;
                    I->program = R32(ic.p + 8) & 0x7f;
                } else if (is_list(&ic, "lrgn")) lrgn = ic;
                else if (is_list(&ic, "lart") || is_list(&ic, "lar2")) {
                    uint32_t o6 = 0; Chunk a;
                    while (next_chunk(ic.p, ic.len, &o6, &a))
                        if (!strcmp(a.id, "art1") || !strcmp(a.id, "art2")) parse_art(a.p, a.len, &iart);
                }
            }
            if (!lrgn.p) continue;
            int rcap = 8;
            I->reg = calloc(rcap, sizeof(Region));
            uint32_t o7 = 0; Chunk rg;
            while (next_chunk(lrgn.p, lrgn.len, &o7, &rg)) {
                if (!is_list(&rg, "rgn ") && !is_list(&rg, "rgn2")) continue;
                if (I->nreg == rcap) { rcap *= 2; I->reg = realloc(I->reg, rcap * sizeof(Region)); }
                Region *Rg = &I->reg[I->nreg];
                memset(Rg, 0, sizeof *Rg);
                Rg->art = iart; Rg->wave = -1; Rg->khi = 127; Rg->vhi = 127;
                uint32_t o8 = 0; Chunk rc;
                while (next_chunk(rg.p, rg.len, &o8, &rc)) {
                    if (!strcmp(rc.id, "rgnh") && rc.len >= 8) { Rg->klo = R16(rc.p); Rg->khi = R16(rc.p + 2); Rg->vlo = R16(rc.p + 4); Rg->vhi = R16(rc.p + 6); }
                    else if (!strcmp(rc.id, "wsmp")) { Rg->has_wsmp = 1; parse_wsmp(rc.p, rc.len, &Rg->unity, &Rg->fine, &Rg->atten_db, &Rg->looped, &Rg->ls, &Rg->ll); }
                    else if (!strcmp(rc.id, "wlnk") && rc.len >= 12) Rg->wave = (int)R32(rc.p + 8);
                    else if (is_list(&rc, "lart") || is_list(&rc, "lar2")) {
                        uint32_t o9 = 0; Chunk a;
                        while (next_chunk(rc.p, rc.len, &o9, &a))
                            if (!strcmp(a.id, "art1") || !strcmp(a.id, "art2")) parse_art(a.p, a.len, &Rg->art);
                    }
                }
                if (Rg->vhi == 0) Rg->vhi = 127;
                I->nreg++;
            }
        }
    }
    free(d);
    LOG("music: loaded %s (%d instruments, %d waves)\n", name, C->ninst, C->nwave);
    return ncolls++;
}

static Instrument *find_instrument(int coll, uint32_t patch) {
    int drum = (patch & 0x80000000u) != 0;
    uint32_t prog = patch & 0x7f, msb = (patch >> 16) & 0x7f, lsb = (patch >> 8) & 0x7f;
    uint32_t bank = (msb << 8) | lsb;
    for (int pass = 0; pass < 3; pass++)
        for (int ci = coll >= 0 ? coll : 0; ci < (coll >= 0 ? coll + 1 : ncolls); ci++) {
            Collection *C = &colls[ci];
            for (int i = 0; i < C->ninst; i++) {
                Instrument *I = &C->inst[i];
                if (pass == 0 && I->program == prog && I->bank == bank && I->drum == drum) return I;
                if (pass == 1 && I->program == prog && I->drum == drum) return I;
                if (pass == 2 && C->ninst == 1) return I;
            }
        }
    return NULL;
}

/* ------------------------------------------------------------------ segments */
typedef struct { uint8_t bpm, beat; uint16_t grids; } TimeSig;
typedef struct { int32_t time; double tempo; } TempoEv;
typedef struct { uint32_t patch, pchannel, flags; uint8_t pan, vol; int16_t transpose; char dls[64]; } BandInst;
typedef struct { int32_t time; int n; BandInst *inst; } BandEv;
typedef struct { int32_t grid; uint32_t var; int32_t dur; int16_t toff; uint16_t value; uint8_t vel, trange, drange, vrange; } PNote;
typedef struct { int32_t grid; uint32_t var; int32_t dur; int16_t toff, start, end; uint8_t type, shape, cc; } PCurve;
typedef struct { uint8_t guid[16]; TimeSig ts; uint32_t choices[32]; uint16_t measures; int nnotes, ncurves; PNote *notes; PCurve *curves; int nvar; } Part;
typedef struct { uint8_t guid[16]; uint8_t lock, randvar; uint32_t pchannel; Part *part; } PartRef;
typedef struct { TimeSig ts; uint16_t measures; int nparts, nrefs; Part *parts; PartRef *refs; } Pattern;
typedef struct { int32_t time, dur; uint32_t pch; int16_t off; uint8_t status, b1, b2; } SeqEv;
typedef struct { int32_t time, dur; uint32_t pch; int16_t off, start, end; uint8_t type, shape, cc; } SeqCurve;

typedef struct {
    char name[64];
    uint32_t length, repeats, loop_start, loop_end;
    int ntempo, ntimesig, nbands, npatterns, nseq, nseqc;
    TempoEv *tempo; struct { int32_t time; TimeSig ts; } *timesig;
    BandEv *bands; Pattern *patterns; SeqEv *seq; SeqCurve *seqc;
} Segment;

#define MAXSEG 512
static Segment segs[MAXSEG];
static int nsegs;

#define GROW(arr, n, cap) do { if ((n) >= (cap)) { (cap) = (cap) ? (cap) * 2 : 8; (arr) = realloc((arr), sizeof(*(arr)) * (cap)); } } while (0)

static void parse_band(const uint8_t *p, uint32_t len, int32_t time, Segment *S, int *bcap) {
    /* RIFF DMBD: ..., LIST lbil -> LIST lbin [bins, LIST DMRF [... file]] */
    GROW(S->bands, S->nbands, *bcap);
    BandEv *B = &S->bands[S->nbands++];
    memset(B, 0, sizeof *B);
    B->time = time;
    int icap = 0;
    uint32_t o = 0; Chunk c;
    while (next_chunk(p, len, &o, &c)) {
        if (!is_list(&c, "lbil")) continue;
        uint32_t o2 = 0; Chunk li;
        while (next_chunk(c.p, c.len, &o2, &li)) {
            if (!is_list(&li, "lbin")) continue;
            GROW(B->inst, B->n, icap);
            BandInst *I = &B->inst[B->n];
            memset(I, 0, sizeof *I);
            int ok = 0;
            uint32_t o3 = 0; Chunk ic;
            while (next_chunk(li.p, li.len, &o3, &ic)) {
                if (!strcmp(ic.id, "bins") && ic.len >= 40) {
                    I->patch = R32(ic.p); I->pchannel = R32(ic.p + 24); I->flags = R32(ic.p + 28);
                    I->pan = ic.p[32]; I->vol = ic.p[33]; I->transpose = S16(ic.p + 34);
                    ok = 1;
                } else if (is_list(&ic, "DMRF")) {
                    uint32_t o4 = 0; Chunk rc;
                    while (next_chunk(ic.p, ic.len, &o4, &rc))
                        if (!strcmp(rc.id, "file")) wcopy(rc.p, rc.len, I->dls, sizeof I->dls);
                }
            }
            if (ok) B->n++;
        }
    }
}

static void parse_part(const uint8_t *p, uint32_t len, Part *P) {
    memset(P, 0, sizeof *P);
    uint32_t o = 0; Chunk c;
    while (next_chunk(p, len, &o, &c)) {
        if (!strcmp(c.id, "prth") && c.len >= 152) {
            P->ts.bpm = c.p[0]; P->ts.beat = c.p[1]; P->ts.grids = R16(c.p + 2);
            memcpy(P->choices, c.p + 4, 128);
            memcpy(P->guid, c.p + 132, 16);
            P->measures = R16(c.p + 148);
        } else if (!strcmp(c.id, "note") && c.len >= 4) {
            uint32_t es = R32(c.p), n = (c.len - 4) / es;
            P->notes = calloc(n ? n : 1, sizeof(PNote));
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t *e = c.p + 4 + i * es;
                PNote *N = &P->notes[P->nnotes++];
                N->grid = S32(e); N->var = R32(e + 4); N->dur = S32(e + 8); N->toff = S16(e + 12);
                N->value = R16(e + 14); N->vel = e[16]; N->trange = e[17]; N->drange = e[18]; N->vrange = e[19];
                for (int b = 31; b >= 0; b--) if (N->var & (1u << b)) { if (b + 1 > P->nvar) P->nvar = b + 1; break; }
            }
        } else if (!strcmp(c.id, "crve") && c.len >= 4) {
            uint32_t es = R32(c.p), n = (c.len - 4) / es;
            P->curves = calloc(n ? n : 1, sizeof(PCurve));
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t *e = c.p + 4 + i * es;
                PCurve *Cv = &P->curves[P->ncurves++];
                Cv->grid = S32(e); Cv->var = R32(e + 4); Cv->dur = S32(e + 8);
                Cv->toff = S16(e + 16); Cv->start = S16(e + 18); Cv->end = S16(e + 20);
                Cv->type = e[24]; Cv->shape = e[25]; Cv->cc = e[26];
            }
        }
    }
    if (P->nvar == 0) P->nvar = 1;
    if (!P->ts.beat) { P->ts.bpm = 4; P->ts.beat = 4; P->ts.grids = 4; }
}

static void parse_pattern_track(const uint8_t *p, uint32_t len, Segment *S, int *pcap) {
    /* RIFF DMPT: styh, LIST pttn [ptnh, rhtm, UNFO, LIST part..., LIST pref...] */
    uint32_t o = 0; Chunk c;
    while (next_chunk(p, len, &o, &c)) {
        if (!is_list(&c, "pttn")) continue;
        GROW(S->patterns, S->npatterns, *pcap);
        Pattern *Pt = &S->patterns[S->npatterns++];
        memset(Pt, 0, sizeof *Pt);
        int partcap = 0, refcap = 0;
        uint32_t o2 = 0; Chunk pc;
        while (next_chunk(c.p, c.len, &o2, &pc)) {
            if (!strcmp(pc.id, "ptnh") && pc.len >= 10) {
                Pt->ts.bpm = pc.p[0]; Pt->ts.beat = pc.p[1]; Pt->ts.grids = R16(pc.p + 2);
                Pt->measures = R16(pc.p + 8);
            } else if (is_list(&pc, "part")) {
                GROW(Pt->parts, Pt->nparts, partcap);
                parse_part(pc.p, pc.len, &Pt->parts[Pt->nparts++]);
            } else if (is_list(&pc, "pref")) {
                uint32_t o3 = 0; Chunk rc;
                while (next_chunk(pc.p, pc.len, &o3, &rc)) {
                    if (strcmp(rc.id, "prfc") || rc.len < 28) continue;
                    GROW(Pt->refs, Pt->nrefs, refcap);
                    PartRef *Rf = &Pt->refs[Pt->nrefs++];
                    memcpy(Rf->guid, rc.p, 16);
                    Rf->lock = rc.p[18]; Rf->randvar = rc.p[21];
                    Rf->pchannel = R32(rc.p + 24);
                    Rf->part = NULL;
                }
            }
        }
        for (int i = 0; i < Pt->nrefs; i++)
            for (int k = 0; k < Pt->nparts; k++)
                if (!memcmp(Pt->refs[i].guid, Pt->parts[k].guid, 16)) Pt->refs[i].part = &Pt->parts[k];
        if (!Pt->ts.beat) { Pt->ts.bpm = 4; Pt->ts.beat = 4; Pt->ts.grids = 4; }
    }
}

static void parse_seq_track(const uint8_t *p, uint32_t len, Segment *S) {
    uint32_t o = 0; Chunk c;
    int scap = 0, ccap = 0;
    while (next_chunk(p, len, &o, &c)) {
        if (!strcmp(c.id, "evtl") && c.len >= 4) {
            uint32_t es = R32(c.p), n = (c.len - 4) / es;
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t *e = c.p + 4 + i * es;
                GROW(S->seq, S->nseq, scap);
                SeqEv *E = &S->seq[S->nseq++];
                E->time = S32(e); E->dur = S32(e + 4); E->pch = R32(e + 8); E->off = S16(e + 12);
                E->status = e[14]; E->b1 = e[15]; E->b2 = e[16];
            }
        } else if (!strcmp(c.id, "curl") && c.len >= 4) {
            uint32_t es = R32(c.p), n = (c.len - 4) / es;
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t *e = c.p + 4 + i * es;
                GROW(S->seqc, S->nseqc, ccap);
                SeqCurve *E = &S->seqc[S->nseqc++];
                E->time = S32(e); E->dur = S32(e + 4); E->pch = R32(e + 12); E->off = S16(e + 16);
                E->start = S16(e + 18); E->end = S16(e + 20); E->type = e[24]; E->shape = e[25]; E->cc = e[26];
            }
        }
    }
}

int dm_load_segment(const uint8_t *d, uint32_t len, const char *name) {
    uint32_t off = 0; Chunk top;
    if (!next_chunk(d, len, &off, &top) || !is_list(&top, "DMSG")) { LOG("music: %s is not a segment\n", name); return -1; }
    /* reuse an already loaded segment with the same name */
    for (int i = 0; i < nsegs; i++) if (name[0] && !strcasecmp(segs[i].name, name)) return i;
    if (nsegs >= MAXSEG) return -1;
    Segment *S = &segs[nsegs];
    memset(S, 0, sizeof *S);
    snprintf(S->name, sizeof S->name, "%s", name);
    int tcap = 0, tscap = 0, bcap = 0, pcap = 0;
    off = 0; Chunk c;
    while (next_chunk(top.p, top.len, &off, &c)) {
        if (!strcmp(c.id, "segh") && c.len >= 24) {
            S->repeats = R32(c.p); S->length = R32(c.p + 4);
            S->loop_start = R32(c.p + 12); S->loop_end = R32(c.p + 16);
        } else if (is_list(&c, "trkl")) {
            uint32_t o2 = 0; Chunk tk;
            while (next_chunk(c.p, c.len, &o2, &tk)) {
                if (!is_list(&tk, "DMTK")) continue;
                uint32_t o3 = 0; Chunk tc;
                while (next_chunk(tk.p, tk.len, &o3, &tc)) {
                    if (!strcmp(tc.id, "tetr") && tc.len >= 4) {
                        uint32_t es = R32(tc.p), n = (tc.len - 4) / es;
                        for (uint32_t i = 0; i < n; i++) {
                            GROW(S->tempo, S->ntempo, tcap);
                            const uint8_t *e = tc.p + 4 + i * es;
                            S->tempo[S->ntempo].time = S32(e);
                            double t; memcpy(&t, e + (es >= 16 ? 8 : 4), 8);
                            S->tempo[S->ntempo++].tempo = t;
                        }
                    } else if (is_list(&tc, "TIMS")) {
                        uint32_t o4 = 0; Chunk ts;
                        while (next_chunk(tc.p, tc.len, &o4, &ts)) {
                            if (strcmp(ts.id, "tims") || ts.len < 4) continue;
                            uint32_t es = R32(ts.p), n = (ts.len - 4) / es;
                            for (uint32_t i = 0; i < n; i++) {
                                GROW(S->timesig, S->ntimesig, tscap);
                                const uint8_t *e = ts.p + 4 + i * es;
                                S->timesig[S->ntimesig].time = S32(e);
                                S->timesig[S->ntimesig].ts.bpm = e[4]; S->timesig[S->ntimesig].ts.beat = e[5];
                                S->timesig[S->ntimesig].ts.grids = R16(e + 6);
                                S->ntimesig++;
                            }
                        }
                    } else if (is_list(&tc, "DMBT")) {
                        uint32_t o4 = 0; Chunk bl;
                        while (next_chunk(tc.p, tc.len, &o4, &bl)) {
                            if (!is_list(&bl, "lbdl")) continue;
                            uint32_t o5 = 0; Chunk bn;
                            while (next_chunk(bl.p, bl.len, &o5, &bn)) {
                                if (!is_list(&bn, "lbnd")) continue;
                                int32_t bt = 0;
                                uint32_t o6 = 0; Chunk bc;
                                while (next_chunk(bn.p, bn.len, &o6, &bc)) {
                                    if ((!strcmp(bc.id, "bd2h") || !strcmp(bc.id, "bdih")) && bc.len >= 4) bt = S32(bc.p);
                                    else if (is_list(&bc, "DMBD")) parse_band(bc.p, bc.len, bt, S, &bcap);
                                }
                            }
                        }
                    } else if (is_list(&tc, "DMPT")) {
                        parse_pattern_track(tc.p, tc.len, S, &pcap);
                    } else if (!strcmp(tc.id, "seqt") || is_list(&tc, "seqt")) {
                        parse_seq_track(tc.p, tc.len, S);
                    }
                }
            }
        }
    }
    if (!S->length) {
        /* derive from patterns */
        for (int i = 0; i < S->npatterns; i++) {
            Pattern *P = &S->patterns[i];
            uint32_t ml = (uint32_t)(PPQ * 4 / P->ts.beat) * P->ts.bpm;
            if (ml * P->measures > S->length) S->length = ml * P->measures;
        }
        if (!S->length) S->length = PPQ * 4;
    }
    TRACE("music: segment %s: len %u, %d patterns, %d bands, %d tempo, %d seq\n", name, S->length, S->npatterns, S->nbands, S->ntempo, S->nseq);
    return nsegs++;
}

/* ------------------------------------------------------------------ events */
enum { EV_TEMPO, EV_BAND, EV_CC, EV_BEND, EV_NOTEOFF, EV_NOTEON };
typedef struct { int32_t time; uint8_t type, key, vel, cc; uint32_t pch; int32_t value; int32_t dur; const BandEv *band; double tempo; } Ev;

typedef struct {
    int used, seg, secondary;
    double start_mt;          /* performance music time when the current pass started */
    uint32_t pass_len;
    uint32_t repeats_left;    /* 0xffffffff = infinite */
    Ev *ev; int nev, cap, pos;
    int stopping;
    double stop_at;
    uint32_t first_start;
} State;
#define MAXSTATES 16
static State states[MAXSTATES];

static void add_ev(State *s, Ev e) {
    if (s->nev >= s->cap) { s->cap = s->cap ? s->cap * 2 : 1024; s->ev = realloc(s->ev, sizeof(Ev) * s->cap); }
    s->ev[s->nev++] = e;
}
static int ev_cmp(const void *a, const void *b) {
    const Ev *x = a, *y = b;
    if (x->time != y->time) return x->time < y->time ? -1 : 1;
    return (int)x->type - (int)y->type;     /* tempo, band, cc, bend, note-off before note-on */
}
static int32_t grid_time(const TimeSig *ts, int32_t grid) {
    int32_t beat = PPQ * 4 / (ts->beat ? ts->beat : 4);
    int32_t g = ts->grids ? ts->grids : 4;
    int32_t q = grid / g, r = grid % g;
    if (r < 0) { r += g; q--; }
    return q * beat + r * (beat / g);
}
static int32_t measure_len(const TimeSig *ts) { return PPQ * 4 / (ts->beat ? ts->beat : 4) * (ts->bpm ? ts->bpm : 4); }

static void add_curve(State *s, int32_t t0, int32_t dur, int type, int cc, uint32_t pch, int sv, int evv, int shape, int32_t lim) {
    int steps = dur > 0 ? dur / 24 + 1 : 1;
    if (steps > 128) steps = 128;
    for (int i = 0; i <= steps; i++) {
        double f = steps ? (double)i / steps : 1.0;
        switch (shape) {
        case 1: f = 1.0; break;                         /* instant */
        case 2: f = f * f; break;                       /* exp */
        case 3: f = sqrt(f); break;                     /* log */
        case 4: f = 0.5 - 0.5 * cos(f * M_PI); break;   /* sine */
        }
        int32_t t = t0 + (int32_t)((double)dur * i / (steps ? steps : 1));
        if (t >= lim) break;
        Ev e = {0};
        e.time = t; e.pch = pch;
        e.value = (int32_t)lround(sv + (evv - sv) * f);
        if (type == 3) { e.type = EV_BEND; }
        else if (type == 4) { e.type = EV_CC; e.cc = (uint8_t)cc; }
        else return;
        add_ev(s, e);
        if (shape == 1) break;
    }
}

/* Expand one pass of a segment into a sorted event list. */
static void gen_pass(State *s) {
    Segment *S = &segs[s->seg];
    s->nev = 0; s->pos = 0;
    int32_t L = (int32_t)s->pass_len;
    for (int i = 0; i < S->ntempo; i++) if (S->tempo[i].time < L) { Ev e = {0}; e.time = S->tempo[i].time < 0 ? 0 : S->tempo[i].time; e.type = EV_TEMPO; e.tempo = S->tempo[i].tempo; add_ev(s, e); }
    for (int i = 0; i < S->nbands; i++) if (S->bands[i].time < L) { Ev e = {0}; e.time = S->bands[i].time < 0 ? 0 : S->bands[i].time; e.type = EV_BAND; e.band = &S->bands[i]; add_ev(s, e); }
    for (int pi = 0; pi < S->npatterns; pi++) {
        Pattern *Pt = &S->patterns[pi];
        int32_t plen = measure_len(&Pt->ts) * (Pt->measures ? Pt->measures : 1);
        int rep = 0;
        for (int32_t pstart = 0; pstart < L; pstart += plen, rep++) {
            int lockvar[256];
            for (int k = 0; k < 256; k++) lockvar[k] = -1;
            for (int ri = 0; ri < Pt->nrefs; ri++) {
                PartRef *Rf = &Pt->refs[ri];
                Part *P = Rf->part;
                if (!P) continue;
                /* choose a variation: random among enabled ones, shared per lock id */
                int v;
                if (Rf->lock && lockvar[Rf->lock] >= 0) v = lockvar[Rf->lock];
                else {
                    int cand[32], nc = 0;
                    for (int k = 0; k < P->nvar && k < 32; k++) if (P->choices[k] & 0x0fffffffu) cand[nc++] = k;
                    if (!nc) { cand[0] = 0; nc = 1; }
                    v = Rf->randvar == 0 && nc > 0 ? cand[rep % nc] : cand[rnd() % nc];
                    if (Rf->lock) lockvar[Rf->lock] = v;
                }
                uint32_t vbit = 1u << v;
                int32_t partlen = measure_len(&P->ts) * (P->measures ? P->measures : 1);
                int32_t pend = pstart + plen < L ? pstart + plen : L;
                for (int32_t qstart = pstart; qstart < pend; qstart += partlen) {
                    for (int ni = 0; ni < P->nnotes; ni++) {
                        PNote *N = &P->notes[ni];
                        if (!(N->var & vbit)) continue;
                        int32_t t = qstart + grid_time(&P->ts, N->grid) + N->toff;
                        if (t < qstart || t >= pend) continue;
                        Ev e = {0};
                        e.time = t; e.type = EV_NOTEON; e.pch = Rf->pchannel;
                        e.key = (uint8_t)(N->value & 0x7f);
                        int vel = N->vel + (N->vrange ? (int)(rnd() % (N->vrange + 1)) - N->vrange / 2 : 0);
                        e.vel = (uint8_t)(vel < 1 ? 1 : vel > 127 ? 127 : vel);
                        e.dur = N->dur > 0 ? N->dur : 1;
                        add_ev(s, e);
                    }
                    for (int ci = 0; ci < P->ncurves; ci++) {
                        PCurve *Cv = &P->curves[ci];
                        if (!(Cv->var & vbit)) continue;
                        int32_t t = qstart + grid_time(&P->ts, Cv->grid) + Cv->toff;
                        if (t < qstart || t >= pend) continue;
                        add_curve(s, t, Cv->dur, Cv->type, Cv->cc, Rf->pchannel, Cv->start, Cv->end, Cv->shape, pend);
                    }
                }
            }
        }
    }
    for (int i = 0; i < S->nseq; i++) {
        SeqEv *E = &S->seq[i];
        int32_t t = E->time + E->off;
        if (t < 0 || t >= L) continue;
        Ev e = {0}; e.time = t; e.pch = E->pch;
        uint8_t st = E->status & 0xf0;
        if (st == 0x90 && E->b2) { e.type = EV_NOTEON; e.key = E->b1 & 0x7f; e.vel = E->b2; e.dur = E->dur > 0 ? E->dur : 1; add_ev(s, e); }
        else if (st == 0xB0) { e.type = EV_CC; e.cc = E->b1; e.value = E->b2; add_ev(s, e); }
        else if (st == 0xE0) { e.type = EV_BEND; e.value = (E->b2 << 7) | E->b1; add_ev(s, e); }
    }
    for (int i = 0; i < S->nseqc; i++) {
        SeqCurve *Cv = &S->seqc[i];
        add_curve(s, Cv->time + Cv->off, Cv->dur, Cv->type, Cv->cc, Cv->pch, Cv->start, Cv->end, Cv->shape, L);
    }
    /* note-offs */
    int n = s->nev;
    for (int i = 0; i < n; i++) if (s->ev[i].type == EV_NOTEON) {
        Ev e = s->ev[i];
        e.type = EV_NOTEOFF; e.time = s->ev[i].time + s->ev[i].dur;
        add_ev(s, e);
    }
    qsort(s->ev, (size_t)s->nev, sizeof(Ev), ev_cmp);
}

/* ------------------------------------------------------------------ synth */
typedef struct {
    Instrument *inst;
    float vol, expr, pan;     /* linear CC7/CC11 gains, pan -1..1 */
    int bend, bendrange;      /* bend 0..16383, range semitones */
    int transpose;
    uint8_t sustain;
    float reverb;             /* CC91 send level 0..1 */
} Channel;
static Channel chans[NCHAN];

typedef struct {
    int active, released, held;
    uint32_t pch; uint8_t key;
    const Wave *w; const Region *rg;
    double pos, inc;
    int looped; uint32_t ls, le;
    int stage; double env, env_db, t;    /* env: linear for attack, dB afterwards */
    float gain_l, gain_r;
    float send;                           /* reverb send (channel CC91) */
    double base_cents;
    int owner;                            /* state index */
} Voice;
static Voice voices[MAXVOICES];

static double perf_mt;          /* music time in ticks */
static double tempo = 120.0;
static uint64_t perf_samples;
static float master_gain = 1.0f;
static float path_gain = 1.0f, path_gain_target = 1.0f, path_gain_step = 0;
static int dm_ready;

static float db2lin(double db) { return db <= -96 ? 0 : (float)pow(10.0, db / 20.0); }

void dm_init(void) {
    if (dm_ready) return;
    dm_ready = 1;
    for (int i = 0; i < NCHAN; i++) { chans[i].vol = 1; chans[i].expr = 1; chans[i].pan = 0; chans[i].bend = 8192; chans[i].bendrange = 2; chans[i].reverb = 40 / 127.0f; }
}

static void apply_band(const BandEv *B) {
    for (int i = 0; i < B->n; i++) {
        const BandInst *I = &B->inst[i];
        Channel *ch = &chans[I->pchannel % NCHAN];
        int coll = I->dls[0] ? load_collection(I->dls) : -1;
        if (I->flags & 0x1) {   /* DMUS_IO_INST_PATCH */
            Instrument *ins = find_instrument(coll, I->patch);
            if (!ins && coll >= 0) ins = find_instrument(-1, I->patch);
            ch->inst = ins;
        }
        if (I->flags & 0x20) ch->pan = ((float)I->pan - 64.0f) / 64.0f;
        if (I->flags & 0x40) { float v = I->vol / 127.0f; ch->vol = v * v; }
        if (I->flags & 0x80) ch->transpose = I->transpose;
    }
}

static void voice_gain(Voice *v) {
    Channel *ch = &chans[v->pch % NCHAN];
    float pan = ch->pan + v->rg->art.pan * 2.0f;
    if (pan < -1) pan = -1;
    if (pan > 1) pan = 1;
    double a = (pan + 1) * M_PI / 4;
    float g = ch->vol * ch->expr;
    v->gain_l = g * (float)cos(a);
    v->gain_r = g * (float)sin(a);
    v->send = ch->reverb;
}
static void voice_pitch(Voice *v) {
    Channel *ch = &chans[v->pch % NCHAN];
    double cents = v->base_cents + (ch->bend - 8192) / 8192.0 * ch->bendrange * 100.0;
    v->inc = pow(2.0, cents / 1200.0) * (double)v->w->rate / RATE;
}

static void note_on(int owner, uint32_t pch, int key, int vel) {
    Channel *ch = &chans[pch % NCHAN];
    Instrument *I = ch->inst;
    if (!I) return;
    int k = key + (I->drum ? 0 : ch->transpose);
    if (k < 0 || k > 127) return;
    const Region *rg = NULL;
    for (int i = 0; i < I->nreg; i++)
        if (k >= I->reg[i].klo && k <= I->reg[i].khi && vel >= I->reg[i].vlo && vel <= I->reg[i].vhi) { rg = &I->reg[i]; break; }
    if (!rg || rg->wave < 0) return;
    Collection *C = NULL;
    for (int i = 0; i < ncolls; i++) if (I >= colls[i].inst && I < colls[i].inst + colls[i].ninst) C = &colls[i];
    if (!C || rg->wave >= C->nwave || !C->wave[rg->wave].data) return;
    const Wave *w = &C->wave[rg->wave];
    /* pick a voice: free, else the quietest released, else the oldest */
    int vi = -1;
    for (int i = 0; i < MAXVOICES; i++) if (!voices[i].active) { vi = i; break; }
    if (vi < 0) for (int i = 0; i < MAXVOICES; i++) if (voices[i].released) { vi = i; break; }
    if (vi < 0) vi = (int)(rnd() % MAXVOICES);
    Voice *v = &voices[vi];
    memset(v, 0, sizeof *v);
    v->active = 1; v->pch = pch; v->key = (uint8_t)key; v->w = w; v->rg = rg; v->owner = owner;
    int unity = rg->has_wsmp ? rg->unity : w->unity;
    int fine = rg->has_wsmp ? rg->fine : w->fine;
    v->looped = rg->has_wsmp ? rg->looped : w->looped;
    v->ls = rg->has_wsmp ? rg->ls : w->ls;
    v->le = v->ls + (rg->has_wsmp ? rg->ll : w->ll);
    if (v->le > w->len) v->le = w->len;
    if (v->ls >= v->le) v->looped = 0;
    v->base_cents = (k - unity) * 100.0 + fine + rg->art.pitch_cents;
    voice_pitch(v);
    double vel_db = rg->art.vel_atten_db * (1.0 - (vel / 127.0) * (vel / 127.0));
    if (vel_db < -96) vel_db = -96;
    double att = (rg->has_wsmp ? rg->atten_db : w->atten_db) + rg->art.atten_db + vel_db;
    v->env_db = att;       /* constant attenuation folded into the envelope's 0 dB point */
    v->stage = rg->art.delay > 0 ? 0 : 1;
    v->env = 0; v->t = 0;
    voice_gain(v);
}

static void note_off(int owner, uint32_t pch, int key) {
    for (int i = 0; i < MAXVOICES; i++) {
        Voice *v = &voices[i];
        if (v->active && !v->released && v->pch == pch && v->key == key && v->owner == owner) {
            if (chans[pch % NCHAN].sustain) v->held = 1;
            else { v->released = 1; v->stage = 5; }
            break;
        }
    }
}

static void all_notes_off(int owner) {
    for (int i = 0; i < MAXVOICES; i++)
        if (voices[i].active && (owner < 0 || voices[i].owner == owner)) { voices[i].released = 1; voices[i].stage = 5; }
}

static void do_event(int owner, const Ev *e) {
    Channel *ch = &chans[e->pch % NCHAN];
    switch (e->type) {
    case EV_TEMPO: if (e->tempo > 1 && !states[owner].secondary) tempo = e->tempo; break;
    case EV_BAND: apply_band(e->band); break;
    case EV_NOTEON: note_on(owner, e->pch, e->key, e->vel); break;
    case EV_NOTEOFF: note_off(owner, e->pch, e->key); break;
    case EV_BEND:
        ch->bend = e->value < 0 ? 0 : e->value > 16383 ? 16383 : e->value;
        for (int i = 0; i < MAXVOICES; i++) if (voices[i].active && voices[i].pch == e->pch) voice_pitch(&voices[i]);
        break;
    case EV_CC: {
        int v = e->value < 0 ? 0 : e->value > 127 ? 127 : e->value;
        float f = v / 127.0f;
        if (e->cc == 7) ch->vol = f * f;
        else if (e->cc == 11) ch->expr = f * f;
        else if (e->cc == 10) ch->pan = (v - 64) / 64.0f;
        else if (e->cc == 91) ch->reverb = v / 127.0f;
        else if (e->cc == 64) {
            ch->sustain = v >= 64;
            if (!ch->sustain)
                for (int i = 0; i < MAXVOICES; i++) if (voices[i].active && voices[i].held && voices[i].pch == e->pch) { voices[i].held = 0; voices[i].released = 1; voices[i].stage = 5; }
        } else if (e->cc == 121) { ch->vol = 1; ch->expr = 1; ch->bend = 8192; ch->reverb = 40 / 127.0f; }
        else if (e->cc == 123) all_notes_off(owner);
        if (e->cc == 7 || e->cc == 11 || e->cc == 10 || e->cc == 91)
            for (int i = 0; i < MAXVOICES; i++) if (voices[i].active && voices[i].pch == e->pch) voice_gain(&voices[i]);
        break;
    }
    }
}

/* envelope: stage 0 delay, 1 attack, 2 hold, 3 decay, 4 sustain, 5 release */
static inline float env_step(Voice *v, double dt) {
    const Art *a = &v->rg->art;
    v->t += dt;
    switch (v->stage) {
    case 0: if (v->t >= a->delay) { v->stage = 1; v->t = 0; } return 0;
    case 1:
        if (a->attack <= 0.0005 || v->t >= a->attack) { v->stage = 2; v->t = 0; v->env = 1; }
        else v->env = v->t / a->attack;
        return (float)v->env * db2lin(v->env_db);
    case 2: if (v->t >= a->hold) { v->stage = 3; v->t = 0; v->env = 0; } return db2lin(v->env_db);
    case 3: {
        double sus_db = a->sustain <= 0.0001 ? -96 : 20 * log10(a->sustain);
        double rate = 96.0 / (a->decay > 0.0005 ? a->decay : 0.0005);
        v->env -= rate * dt;      /* env holds dB below full */
        if (v->env <= sus_db) { v->env = sus_db; v->stage = 4; }
        if (v->env <= -96) { v->active = 0; return 0; }
        return db2lin(v->env_db + v->env);
    }
    case 4:
        if (v->env <= -96) { v->active = 0; return 0; }
        return db2lin(v->env_db + v->env);
    case 5: default: {
        /* release from the current level */
        if (v->env > 0) v->env = 0;
        double rate = 96.0 / (a->release > 0.0005 ? a->release : 0.0005);
        v->env -= rate * dt;
        if (v->env <= -96) { v->active = 0; return 0; }
        return db2lin(v->env_db + v->env);
    }
    }
}

/* ------------------------------------------------------------------ reverb
 * DirectMusic's default audio path sends music through the synth's reverb.  This is a
 * Freeverb-style stereo reverb (8 parallel damped combs + 4 allpasses per side) fed by the
 * per-channel CC91 sends, tuned to a medium hall like the DirectX "music" preset. */
#define RV_COMBS 8
#define RV_APS 4
static const int rv_comb_len[RV_COMBS] = {1116, 1188, 1277, 1356, 1422, 1491, 1557, 1617};
static const int rv_ap_len[RV_APS] = {556, 441, 341, 225};
#define RV_SPREAD 23
typedef struct { float *buf; int len, idx; float store; } rv_comb;
typedef struct { float *buf; int len, idx; } rv_ap;
static rv_comb rv_c[2][RV_COMBS];
static rv_ap rv_a[2][RV_APS];
static float rev_in[2 * 4096];
static int rv_ready;
#define RV_FEEDBACK 0.80f    /* room size: ~1.6 s decay */
#define RV_DAMP 0.35f
#define RV_WET 0.40f      /* wet level ~14 dB below the dry music */
static void reverb_init(void) {
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < RV_COMBS; i++) {
            rv_c[c][i].len = rv_comb_len[i] + c * RV_SPREAD;
            rv_c[c][i].buf = calloc((size_t)rv_c[c][i].len, sizeof(float));
        }
        for (int i = 0; i < RV_APS; i++) {
            rv_a[c][i].len = rv_ap_len[i] + c * RV_SPREAD;
            rv_a[c][i].buf = calloc((size_t)rv_a[c][i].len, sizeof(float));
        }
    }
    rv_ready = 1;
}
static void reverb_process(int32_t *acc, int frames) {
    if (!rv_ready) reverb_init();
    for (int f = 0; f < frames; f++) {
        float in = (rev_in[2 * f] + rev_in[2 * f + 1]) * 0.015f;   /* Freeverb's fixed input gain */
        for (int c = 0; c < 2; c++) {
            float out = 0;
            for (int i = 0; i < RV_COMBS; i++) {
                rv_comb *k = &rv_c[c][i];
                float y = k->buf[k->idx];
                k->store = y * (1 - RV_DAMP) + k->store * RV_DAMP;
                k->buf[k->idx] = in + k->store * RV_FEEDBACK;
                if (++k->idx >= k->len) k->idx = 0;
                out += y;
            }
            for (int i = 0; i < RV_APS; i++) {
                rv_ap *a = &rv_a[c][i];
                float b = a->buf[a->idx];
                a->buf[a->idx] = out + b * 0.5f;
                out = b - out;
                if (++a->idx >= a->len) a->idx = 0;
            }
            acc[2 * f + c] += (int32_t)(out * RV_WET * 3.0f);
        }
        rev_in[2 * f] = rev_in[2 * f + 1] = 0;
    }
}

static void render_voices(int32_t *acc, int frames) {
    const double dt = 1.0 / RATE;
    float out_gain = master_gain * 0.6f;
    for (int i = 0; i < MAXVOICES; i++) {
        Voice *v = &voices[i];
        if (!v->active) continue;
        const int16_t *d = v->w->data;
        uint32_t len = v->w->len;
        float eg = 0;
        for (int f = 0; f < frames; f++) {
            /* envelope updated every 16 samples */
            if ((f & 15) == 0) {
                eg = env_step(v, dt * 16);
                if (!v->active) break;
            }
            uint32_t p = (uint32_t)v->pos;
            if (p >= len) { v->active = 0; break; }
            double fr = v->pos - p;
            float s = (float)(d[p] + (d[p + 1] - d[p]) * fr);
            float g = eg * out_gain * path_gain;
            acc[2 * f] += (int32_t)(s * g * v->gain_l);
            acc[2 * f + 1] += (int32_t)(s * g * v->gain_r);
            if (v->send > 0) { float sg = s * g * v->send; rev_in[2 * f] += sg * v->gain_l; rev_in[2 * f + 1] += sg * v->gain_r; }
            v->pos += v->inc;
            if (v->looped && v->pos >= v->le) v->pos -= (v->le - v->ls);
        }
    }
}

/* ------------------------------------------------------------------ performance */
static void start_pass(State *s) {
    Segment *S = &segs[s->seg];
    s->pass_len = S->length;
    gen_pass(s);
}

int dm_play(int seg, uint32_t flags, int unused) {
    (void)unused;
    if (seg < 0 || seg >= nsegs) return -1;
    dm_init();
    int secondary = (flags & 0x80) != 0;
    double start = perf_mt;
    if (!secondary) {
        /* align to beat/measure of the running primary segment, then stop it */
        for (int i = 0; i < MAXSTATES; i++) {
            State *o = &states[i];
            if (!o->used || o->secondary || o->stopping) continue;
            if (flags & ((1u << 13) | (1u << 12) | (1u << 11) | (1u << 14))) {
                int32_t unit = (flags & (1u << 13)) || (flags & (1u << 14)) ? PPQ * 4 : PPQ;
                double rel = perf_mt - o->start_mt;
                double next = o->start_mt + ceil(rel / unit) * unit;
                if (next > start) start = next;
            }
            o->stopping = 1; o->stop_at = start;
        }
    }
    int si = -1;
    for (int i = 0; i < MAXSTATES; i++) if (!states[i].used) { si = i; break; }
    if (si < 0) return -1;
    State *s = &states[si];
    Ev *keep = s->ev; int cap = s->cap;
    memset(s, 0, sizeof *s);
    s->ev = keep; s->cap = cap;
    s->used = 1; s->seg = seg; s->secondary = secondary;
    s->start_mt = start;
    s->first_start = (uint32_t)start;
    s->repeats_left = segs[seg].repeats;
    start_pass(s);
    LOG("music: play %s%s (repeats %d)\n", segs[seg].name, secondary ? " [secondary]" : "", (int)segs[seg].repeats);
    return si;
}

void dm_stop(int seg, uint32_t flags) {
    (void)flags;
    for (int i = 0; i < MAXSTATES; i++) {
        State *s = &states[i];
        if (!s->used || (seg >= 0 && s->seg != seg)) continue;
        s->used = 0;
        all_notes_off(i);
    }
}

int dm_is_playing(int seg, int st) {
    for (int i = 0; i < MAXSTATES; i++) {
        State *s = &states[i];
        if (!s->used) continue;
        if (st >= 0 && i != st) continue;
        if (seg >= 0 && s->seg != seg) continue;
        return 1;
    }
    return 0;
}

void dm_render(int32_t *acc, int frames) {
    if (!dm_ready) return;
    const int BLOCK = 64;
    for (int done = 0; done < frames; ) {
        int n = frames - done < BLOCK ? frames - done : BLOCK;
        double dmt = n * tempo * PPQ / (60.0 * RATE);
        double end = perf_mt + dmt;
        for (int i = 0; i < MAXSTATES; i++) {
            State *s = &states[i];
            if (!s->used) continue;
            if (s->stopping && end >= s->stop_at) { s->used = 0; all_notes_off(i); continue; }
            for (;;) {
                double rel_end = end - s->start_mt;
                while (s->pos < s->nev && s->ev[s->pos].time < rel_end) {
                    do_event(i, &s->ev[s->pos]);
                    s->pos++;
                }
                if (rel_end < s->pass_len) break;
                /* pass finished */
                if (s->repeats_left == 0) { s->used = 0; break; }
                if (s->repeats_left != 0xffffffffu) s->repeats_left--;
                s->start_mt += s->pass_len;
                start_pass(s);
            }
        }
        /* audio path volume fades */
        if (path_gain != path_gain_target) {
            path_gain += path_gain_step * n;
            if ((path_gain_step > 0 && path_gain >= path_gain_target) || (path_gain_step < 0 && path_gain <= path_gain_target) || path_gain_step == 0)
                path_gain = path_gain_target;
        }
        render_voices(acc + 2 * done, n);
        reverb_process(acc + 2 * done, n);
        perf_mt = end;
        perf_samples += (uint64_t)n;
        done += n;
    }
}

uint64_t dm_reftime(void) { return perf_samples * 10000000ull / RATE; }
uint32_t dm_musictime(void) { return (uint32_t)perf_mt; }
void dm_set_master_volume(int hb) { master_gain = db2lin(hb / 100.0); }
void dm_set_path_volume(int hb, uint32_t ms) {
    path_gain_target = db2lin(hb / 100.0);
    if (!ms) { path_gain = path_gain_target; path_gain_step = 0; }
    else path_gain_step = (path_gain_target - path_gain) / (ms * (RATE / 1000.0f));
}
uint32_t dm_seg_length(int seg) { return seg >= 0 && seg < nsegs ? segs[seg].length : 0; }
void dm_seg_set_length(int seg, uint32_t l) { if (seg >= 0 && seg < nsegs) segs[seg].length = l; }
uint32_t dm_seg_repeats(int seg) { return seg >= 0 && seg < nsegs ? segs[seg].repeats : 0; }
void dm_seg_set_repeats(int seg, uint32_t n) { if (seg >= 0 && seg < nsegs) segs[seg].repeats = n; }
void dm_seg_set_loop(int seg, uint32_t a, uint32_t b) { if (seg >= 0 && seg < nsegs) { segs[seg].loop_start = a; segs[seg].loop_end = b; } }
void dm_seg_download(int seg) {
    if (seg < 0 || seg >= nsegs) return;
    Segment *S = &segs[seg];
    for (int i = 0; i < S->nbands; i++)
        for (int k = 0; k < S->bands[i].n; k++)
            if (S->bands[i].inst[k].dls[0]) load_collection(S->bands[i].inst[k].dls);
}
int dm_state_segment(int st) { return st >= 0 && st < MAXSTATES ? states[st].seg : -1; }
uint32_t dm_state_start(int st) { return st >= 0 && st < MAXSTATES ? states[st].first_start : 0; }
uint32_t dm_state_seek(int st) { return st >= 0 && st < MAXSTATES && states[st].used ? (uint32_t)(perf_mt - states[st].start_mt) : 0; }

void dm_seg_timesig(int seg, uint8_t *bpm, uint8_t *beat, uint16_t *grids) {
    *bpm = 4; *beat = 4; *grids = 4;
    if (seg < 0 || seg >= nsegs) return;
    Segment *S = &segs[seg];
    if (S->ntimesig) { *bpm = S->timesig[0].ts.bpm; *beat = S->timesig[0].ts.beat; *grids = S->timesig[0].ts.grids; }
    else if (S->npatterns) { *bpm = S->patterns[0].ts.bpm; *beat = S->patterns[0].ts.beat; *grids = S->patterns[0].ts.grids; }
}
double dm_seg_tempo(int seg) {
    if (seg < 0 || seg >= nsegs || !segs[seg].ntempo) return 120.0;
    return segs[seg].tempo[0].tempo;
}
