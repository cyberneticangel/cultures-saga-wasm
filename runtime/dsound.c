/* DirectSound 8 emulation with a software mixer feeding the platform audio queue. */
#include "runtime.h"
#include "dmusic_int.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define DS_OK 0
#define DSERR_INVALIDPARAM 0x80070057u
#define E_NOINTERFACE 0x80004002u
#define MIX_RATE 44100

typedef struct {
    uint32_t obj;
    uint32_t data, size;          /* guest buffer */
    int rate, chans, bits, align;
    int playing, looping, primary, used;
    double pos;                   /* in sample frames */
    int freq;                     /* playback frequency (0 = original) */
    int32_t volume, pan;          /* hundredths of dB */
    float gl, gr;
    int refs;
    int shared_owner;             /* duplicate shares data */
} Buf;

#define MAXB 4096
static Buf bufs[MAXB];
static uint32_t vt_ds, vt_dsb, ds_obj;
static int audio_opened;

static Buf *B(uint32_t obj) {
    uint32_t i = M32(obj + 4);
    if (i >= MAXB || bufs[i].obj != obj) fatal("bad sound buffer %08x", obj);
    return &bufs[i];
}

static void update_gain(Buf *b) {
    double v = b->volume <= -10000 ? 0 : pow(10.0, b->volume / 2000.0);
    double l = v, r = v;
    if (b->pan > 0) l *= pow(10.0, -b->pan / 2000.0);
    if (b->pan < 0) r *= pow(10.0, b->pan / 2000.0);
    b->gl = (float)l; b->gr = (float)r;
}

static int new_buf(void) {
    for (int i = 0; i < MAXB; i++) if (!bufs[i].used) {
        memset(&bufs[i], 0, sizeof bufs[i]);
        bufs[i].used = 1; bufs[i].refs = 1;
        bufs[i].obj = guest_alloc_static(16);
        M32(bufs[i].obj) = vt_dsb; M32(bufs[i].obj + 4) = (uint32_t)i;
        bufs[i].gl = bufs[i].gr = 1;
        return i;
    }
    fatal("out of sound buffers");
}

/* ------------------------------------------------------------- mixer */
static int32_t mixbuf[2 * 4096];
static int16_t outbuf[2 * 4096];

static inline void frame_at(const Buf *b, const uint8_t *d, int p, float *l, float *r) {
    if (b->bits == 16) {
        const int16_t *s = (const int16_t *)(d + (size_t)p * b->align);
        *l = s[0]; *r = b->chans == 2 ? s[1] : s[0];
    } else {
        const uint8_t *s = d + (size_t)p * b->align;
        *l = (float)((s[0] - 128) << 8); *r = b->chans == 2 ? (float)((s[1] - 128) << 8) : *l;
    }
}

/* Peak limiter on the final mix: the gain drops at once when a frame would exceed the
 * threshold and recovers over ~150 ms, so many simultaneous sounds get quieter instead of
 * clipping.  A soft clip catches anything left. */
#define LIM_THRESHOLD 29000.0f
static float lim_gain = 1.0f;
static inline int16_t soft_clip(float x) {
    const float k = 30000.0f;
    if (x > k) x = k + (32767.0f - k) * tanhf((x - k) / (32767.0f - k));
    else if (x < -k) x = -k - (32767.0f - k) * tanhf((-x - k) / (32767.0f - k));
    return (int16_t)lrintf(x);
}

void video_audio_render(int32_t *acc, int frames);   /* video.c */

static void mix(int frames) {
    memset(mixbuf, 0, sizeof(int32_t) * 2 * (size_t)frames);
    for (int i = 0; i < MAXB; i++) {
        Buf *b = &bufs[i];
        if (!b->used || !b->playing || b->primary || !b->size || !b->data) continue;
        int nframes = (int)(b->size / (uint32_t)b->align);
        if (nframes <= 0) continue;
        double step = (double)(b->freq ? b->freq : b->rate) / MIX_RATE;
        double pos = b->pos;
        const uint8_t *d = (const uint8_t *)PTR(b->data);
        float gl = b->gl, gr = b->gr;
        for (int f = 0; f < frames; f++) {
            int p = (int)pos;
            if (p >= nframes) {
                if (b->looping) { pos -= nframes; p = (int)pos; if (p >= nframes) { pos = 0; p = 0; } }
                else { b->playing = 0; break; }
            }
            /* linear interpolation towards the next frame (wrapping at the loop point) */
            int q = p + 1;
            if (q >= nframes) q = b->looping ? 0 : p;
            float l0, r0, l1, r1, t = (float)(pos - p);
            frame_at(b, d, p, &l0, &r0);
            frame_at(b, d, q, &l1, &r1);
            mixbuf[2 * f] += (int32_t)((l0 + (l1 - l0) * t) * gl);
            mixbuf[2 * f + 1] += (int32_t)((r0 + (r1 - r0) * t) * gr);
            pos += step;
        }
        b->pos = pos;
    }
    dm_render(mixbuf, frames);
    video_audio_render(mixbuf, frames);
    const float release = 1.0f / (0.150f * MIX_RATE);
    for (int f = 0; f < frames; f++) {
        float l = (float)mixbuf[2 * f], r = (float)mixbuf[2 * f + 1];
        float peak = fmaxf(fabsf(l), fabsf(r));
        float want = peak * lim_gain > LIM_THRESHOLD ? LIM_THRESHOLD / peak : 1.0f;
        if (want < lim_gain) lim_gain = want;
        else lim_gain += (1.0f - lim_gain) * (release * 4.6f);   /* ~-40 dB of the gap per 150 ms */
        outbuf[2 * f] = soft_clip(l * lim_gain);
        outbuf[2 * f + 1] = soft_clip(r * lim_gain);
    }
    plat_audio_push(outbuf, frames);
}

void audio_update(void) {
    if (!audio_opened) return;
    int target = MIX_RATE / 10;    /* keep ~100 ms queued */
    int q = plat_audio_queued();
    while (q < target) {
        int n = target - q;
        if (n > 1024) n = 1024;
        mix(n);
        q += n;
    }
}

/* ------------------------------------------------------------- IDirectSound8 */
static void ds_QueryInterface(void) { M32(ARG(2)) = ARG(0); RET_STD(3, DS_OK); }
static void ds_AddRef(void) { RET_STD(1, 1); }
static void ds_Release(void) { RET_STD(1, 0); }
static void ds_CreateSoundBuffer(void) {
    uint32_t desc = ARG(1), pp = ARG(2);
    uint32_t flags = M32(desc + 4), bytes = M32(desc + 8), wfx = M32(desc + 16);
    int i = new_buf();
    Buf *b = &bufs[i];
    if (flags & 1) {
        b->primary = 1; b->rate = MIX_RATE; b->chans = 2; b->bits = 16; b->align = 4;
        b->size = 4096 * 4; b->data = heap_alloc(b->size, 1);
    } else {
        if (!wfx) RET_STD(4, DSERR_INVALIDPARAM);
        b->chans = M16(wfx + 2); b->rate = (int)M32(wfx + 4); b->bits = M16(wfx + 14);
        b->align = M16(wfx + 12);
        if (!b->align) b->align = b->chans * b->bits / 8;
        b->size = bytes;
        b->data = heap_alloc(bytes + 16, 1);
        if (b->bits == 8) memset(PTR(b->data), 0x80, bytes);
    }
    TRACE("CreateSoundBuffer flags=%x bytes=%u %dHz %dch %dbit -> %d\n", flags, bytes, b->rate, b->chans, b->bits, i);
    M32(pp) = b->obj;
    RET_STD(4, DS_OK);
}
static void ds_GetCaps(void) {
    uint32_t c = ARG(1);
    memset(PTR(c), 0, 96); M32(c) = 96;
    M32(c + 4) = 0x0f00 | 0x400;   /* DSCAPS_SECONDARY* | CONTINUOUSRATE */
    M32(c + 8) = 100; M32(c + 12) = 100000;
    M32(c + 20) = 64; M32(c + 24) = 64;
    RET_STD(2, DS_OK);
}
static void ds_DuplicateSoundBuffer(void) {
    Buf *src = B(ARG(1));
    int i = new_buf();
    Buf *b = &bufs[i];
    uint32_t obj = b->obj;
    *b = *src;
    b->obj = obj; b->playing = 0; b->pos = 0; b->refs = 1; b->shared_owner = 1;
    M32(ARG(2)) = obj;
    RET_STD(3, DS_OK);
}
static void ds_SetCooperativeLevel(void) {
    if (!audio_opened) { plat_audio_open(MIX_RATE); audio_opened = 1; }
    RET_STD(3, DS_OK);
}
static void ds_Compact(void) { RET_STD(1, DS_OK); }
static void ds_GetSpeakerConfig(void) { M32(ARG(1)) = 4; RET_STD(2, DS_OK); }
static void ds_SetSpeakerConfig(void) { RET_STD(2, DS_OK); }
static void ds_Initialize(void) { RET_STD(2, DS_OK); }
static void ds_VerifyCertification(void) { M32(ARG(1)) = 0; RET_STD(2, DS_OK); }

/* ------------------------------------------------------------- IDirectSoundBuffer8 */
static void dsb_QueryInterface(void) {
    uint32_t iid = M32(ARG(1));
    if (iid == 0x6825a449u || iid == 0x279afa85u) {     /* IDirectSoundBuffer8 / IDirectSoundBuffer */
        M32(ARG(2)) = ARG(0); RET_STD(3, DS_OK);
    }
    LOG("IDirectSoundBuffer::QueryInterface %08x -> E_NOINTERFACE\n", iid);
    M32(ARG(2)) = 0;
    RET_STD(3, E_NOINTERFACE);
}
static void dsb_AddRef(void) { RET_STD(1, ++B(ARG(0))->refs); }
static void dsb_Release(void) {
    Buf *b = B(ARG(0));
    int r = --b->refs;
    if (r <= 0) {
        if (!b->shared_owner) {
            /* free data only when no duplicate still references it */
            int shared = 0;
            for (int i = 0; i < MAXB; i++) if (&bufs[i] != b && bufs[i].used && bufs[i].data == b->data) shared = 1;
            if (!shared) heap_free(b->data);
        } else {
            int alive = 0;
            for (int i = 0; i < MAXB; i++) if (&bufs[i] != b && bufs[i].used && bufs[i].data == b->data) alive = 1;
            if (!alive) heap_free(b->data);
        }
        M32(b->obj + 4) = 0xffffffffu;
        b->used = 0; b->obj = 0;
        RET_STD(1, 0);
    }
    RET_STD(1, r);
}
static void dsb_GetCaps(void) {
    Buf *b = B(ARG(0));
    uint32_t c = ARG(1);
    memset(PTR(c), 0, 20); M32(c) = 20; M32(c + 4) = 0x8 | 0xe0; M32(c + 8) = b->size;
    RET_STD(2, DS_OK);
}
static void dsb_GetCurrentPosition(void) {
    Buf *b = B(ARG(0));
    uint32_t play = (uint32_t)b->pos * (uint32_t)b->align;
    if (b->size) play %= b->size;
    if (ARG(1)) M32(ARG(1)) = play;
    if (ARG(2)) {
        uint32_t w = play + (uint32_t)(b->rate / 100) * (uint32_t)b->align;   /* write cursor ~10 ms ahead */
        M32(ARG(2)) = b->size ? w % b->size : 0;
    }
    RET_STD(3, DS_OK);
}
static void dsb_GetFormat(void) {
    Buf *b = B(ARG(0));
    uint32_t w = ARG(1);
    if (w) {
        M16(w) = 1; M16(w + 2) = (uint16_t)b->chans; M32(w + 4) = (uint32_t)b->rate;
        M32(w + 8) = (uint32_t)(b->rate * b->align); M16(w + 12) = (uint16_t)b->align; M16(w + 14) = (uint16_t)b->bits;
        if (ARG(2) >= 18) M16(w + 16) = 0;
    }
    if (ARG(3)) M32(ARG(3)) = 18;
    RET_STD(4, DS_OK);
}
static void dsb_GetVolume(void) { M32(ARG(1)) = (uint32_t)B(ARG(0))->volume; RET_STD(2, DS_OK); }
static void dsb_GetPan(void) { M32(ARG(1)) = (uint32_t)B(ARG(0))->pan; RET_STD(2, DS_OK); }
static void dsb_GetFrequency(void) { Buf *b = B(ARG(0)); M32(ARG(1)) = (uint32_t)(b->freq ? b->freq : b->rate); RET_STD(2, DS_OK); }
static void dsb_GetStatus(void) {
    Buf *b = B(ARG(0));
    M32(ARG(1)) = b->playing ? (1u | (b->looping ? 4u : 0u)) : 0u;
    RET_STD(2, DS_OK);
}
static void dsb_Initialize(void) { RET_STD(3, DS_OK); }
static void dsb_Lock(void) {
    Buf *b = B(ARG(0));
    uint32_t off = ARG(1), n = ARG(2), p1 = ARG(3), l1 = ARG(4), p2 = ARG(5), l2 = ARG(6), flags = ARG(7);
    if (flags & 2) { off = 0; n = b->size; }
    if (flags & 1) { uint32_t play = (uint32_t)b->pos * (uint32_t)b->align; off = b->size ? play % b->size : 0; }
    if (n > b->size) n = b->size;
    if (off >= b->size) off = 0;
    uint32_t first = n;
    if (off + n > b->size) first = b->size - off;
    M32(p1) = b->data + off; M32(l1) = first;
    if (p2) M32(p2) = first < n ? b->data : 0;
    if (l2) M32(l2) = n - first;
    RET_STD(8, DS_OK);
}
static void dsb_Play(void) {
    Buf *b = B(ARG(0));
    b->playing = 1; b->looping = ARG(3) & 1;
    if (!audio_opened) { plat_audio_open(MIX_RATE); audio_opened = 1; }
    RET_STD(4, DS_OK);
}
static void dsb_SetCurrentPosition(void) {
    Buf *b = B(ARG(0));
    b->pos = b->align ? (double)(ARG(1) / (uint32_t)b->align) : 0;
    RET_STD(2, DS_OK);
}
static void dsb_SetFormat(void) { RET_STD(2, DS_OK); }
static void dsb_SetVolume(void) { Buf *b = B(ARG(0)); b->volume = (int32_t)ARG(1); update_gain(b); RET_STD(2, DS_OK); }
static void dsb_SetPan(void) { Buf *b = B(ARG(0)); b->pan = (int32_t)ARG(1); update_gain(b); RET_STD(2, DS_OK); }
static void dsb_SetFrequency(void) { B(ARG(0))->freq = (int)ARG(1); RET_STD(2, DS_OK); }
static void dsb_Stop(void) { B(ARG(0))->playing = 0; RET_STD(1, DS_OK); }
static void dsb_Unlock(void) { RET_STD(5, DS_OK); }
static void dsb_Restore(void) { RET_STD(1, DS_OK); }
static void dsb_SetFX(void) { RET_STD(4, 0x80004001u); }
static void dsb_AcquireResources(void) { RET_STD(4, DS_OK); }
static void dsb_GetObjectInPath(void) { RET_STD(5, 0x80004001u); }

static const gfn_t ds_m[] = {ds_QueryInterface, ds_AddRef, ds_Release, ds_CreateSoundBuffer, ds_GetCaps,
                             ds_DuplicateSoundBuffer, ds_SetCooperativeLevel, ds_Compact, ds_GetSpeakerConfig,
                             ds_SetSpeakerConfig, ds_Initialize, ds_VerifyCertification};
static const char *const ds_n[] = {"ds_QueryInterface", "ds_AddRef", "ds_Release", "ds_CreateSoundBuffer", "ds_GetCaps", "ds_DuplicateSoundBuffer", "ds_SetCooperativeLevel", "ds_Compact", "ds_GetSpeakerConfig", "ds_SetSpeakerConfig", "ds_Initialize", "ds_VerifyCertification"};

static const gfn_t dsb_m[] = {dsb_QueryInterface, dsb_AddRef, dsb_Release, dsb_GetCaps, dsb_GetCurrentPosition,
                              dsb_GetFormat, dsb_GetVolume, dsb_GetPan, dsb_GetFrequency, dsb_GetStatus,
                              dsb_Initialize, dsb_Lock, dsb_Play, dsb_SetCurrentPosition, dsb_SetFormat,
                              dsb_SetVolume, dsb_SetPan, dsb_SetFrequency, dsb_Stop, dsb_Unlock, dsb_Restore,
                              dsb_SetFX, dsb_AcquireResources, dsb_GetObjectInPath};
static const char *const dsb_n[] = {"dsb_QueryInterface", "dsb_AddRef", "dsb_Release", "dsb_GetCaps", "dsb_GetCurrentPosition", "dsb_GetFormat", "dsb_GetVolume", "dsb_GetPan", "dsb_GetFrequency", "dsb_GetStatus", "dsb_Initialize", "dsb_Lock", "dsb_Play", "dsb_SetCurrentPosition", "dsb_SetFormat", "dsb_SetVolume", "dsb_SetPan", "dsb_SetFrequency", "dsb_Stop", "dsb_Unlock", "dsb_Restore", "dsb_SetFX", "dsb_AcquireResources", "dsb_GetObjectInPath"};


/* DirectSoundCreate8(LPCGUID, LPDIRECTSOUND8*, LPUNKNOWN) - imported by ordinal 11 */
void imp_DSOUND_ord11(void) {
    if (!vt_ds) {
        vt_ds = com_make_vtbl(ds_m, ds_n, 12);
        vt_dsb = com_make_vtbl(dsb_m, dsb_n, 24);
        ds_obj = com_new(vt_ds, 4);
    }
    LOG("DirectSoundCreate8\n");
    M32(ARG(1)) = ds_obj;
    RET_STD(3, DS_OK);
}
