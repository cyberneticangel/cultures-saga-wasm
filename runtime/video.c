/* FMV playback: the DirectShow multimedia-streams objects the game uses for its MPEG-1
 * intro and story videos (datax\FMV\<lang>\*.mpg), decoded with pl_mpeg.
 *
 * The game (0x404538 and friends) does:
 *   CoCreateInstance(CLSID_AMMultiMediaStream) -> Initialize(READ) -> AddMediaStream(ddraw,
 *   PrimaryVideo) [+ AddMediaStream(NULL, PrimaryAudio, ADDDEFAULTRENDERER)] -> OpenFile ->
 *   GetMediaStream(PrimaryVideo) -> QI IDirectDrawMediaStream -> CreateSample(NULL) ->
 *   GetSurface; then Seek(0), SetState(RUN), and per frame sample->Update(0) followed by a
 *   Blt of the sample surface to its back buffer until Update stops returning S_OK.
 * It also creates a FilterGraph once just to check that DirectShow is installed.
 *
 * Update() returns the frame that is due according to the clock started by SetState(RUN),
 * waiting for it if necessary.  The soundtrack is resampled into the DirectSound mixer. */
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <strings.h>

#define PLM_NO_STDIO
#define PL_MPEG_IMPLEMENTATION
#include "third_party/pl_mpeg.h"

#define S_OK 0u
#define E_NOINTERFACE 0x80004002u
#define E_FAIL 0x80004005u
#define MS_S_ENDOFSTREAM 0x00040003u
#define MIX_RATE 44100
#define AUDIO_LATENCY 0.10   /* the mixer queues ~100 ms ahead; delay pictures to match */

uint32_t ddraw_make_surface(int w, int h, uint32_t *pixels, int *pitch, int *bpp);
uint8_t *vfs_read_all(const char *winpath, uint32_t *len);
int skip_intro;

static const uint8_t CLSID_AMMultiMediaStream[16] = {0xe5,0x7c,0xc4,0x49,0xa4,0x9b,0xd0,0x11,0x82,0x12,0x00,0xc0,0x4f,0xc3,0x2c,0x45};
static const uint8_t CLSID_FilterGraph[16] = {0xb3,0xeb,0x36,0xe4,0x4f,0x52,0xce,0x11,0x9f,0x53,0x00,0x20,0xaf,0x0b,0xa7,0x70};
static const uint8_t MSPID_PrimaryVideo[16] = {0x6a,0xf5,0x5f,0xa3,0xda,0x9f,0xd0,0x11,0x8f,0xdf,0x00,0xc0,0x4f,0xd9,0x18,0x9d};
static const uint8_t MSPID_PrimaryAudio[16] = {0x6b,0xf5,0x5f,0xa3,0xda,0x9f,0xd0,0x11,0x8f,0xdf,0x00,0xc0,0x4f,0xd9,0x18,0x9d};

static struct {
    plm_t *plm;
    int w, h;
    int want_audio, running;
    uint64_t t0_us;             /* wall clock at SetState(RUN) */
    double shown;               /* time of the frame currently in the sample */
    int ended, frames, updates;
    uint32_t surf, pixels; int pitch, bpp;
    uint8_t *rgb;
    /* soundtrack: decoded samples waiting for the mixer */
    float *aq; int aq_len, aq_cap, aq_pos; double aq_frac;
    int arate;
} V;
static uint32_t vt_amms, vt_stream, vt_sample, vt_unk;
static uint32_t obj_amms, obj_stream, obj_sample;

/* ------------------------------------------------------------------ decoding */
static void video_close(void) {
    if (V.plm) plm_destroy(V.plm);
    free(V.rgb); free(V.aq);
    uint32_t surf = V.surf;
    memset(&V, 0, sizeof V);
    V.surf = surf;   /* the surface object belongs to ddraw; the game releases it */
}

static void put_frame(plm_frame_t *f) {
    if (!V.surf || V.bpp < 16) return;
    if (!V.rgb) V.rgb = malloc((size_t)V.w * V.h * 3);
    plm_frame_to_rgb(f, V.rgb, V.w * 3);
    for (int y = 0; y < V.h; y++) {
        const uint8_t *s = V.rgb + (size_t)y * V.w * 3;
        if (V.bpp == 16) {
            uint16_t *d = (uint16_t *)PTR(V.pixels + (uint32_t)(y * V.pitch));
            for (int x = 0; x < V.w; x++, s += 3) d[x] = (uint16_t)(((s[0] >> 3) << 11) | ((s[1] >> 2) << 5) | (s[2] >> 3));
        } else {
            uint32_t *d = (uint32_t *)PTR(V.pixels + (uint32_t)(y * V.pitch));
            for (int x = 0; x < V.w; x++, s += 3) d[x] = (uint32_t)s[0] << 16 | (uint32_t)s[1] << 8 | s[2];
        }
    }
}

static double clock_now(void) {
    if (!V.running) return 0;
    return (double)(plat_ticks_us() - V.t0_us) / 1e6 - AUDIO_LATENCY;
}

/* Called from the DirectSound mixer: adds the soundtrack, resampled to 44.1 kHz. */
void video_audio_render(int32_t *acc, int frames) {
    if (!V.plm || !V.running || !V.want_audio || !V.arate) return;
    double step = (double)V.arate / MIX_RATE;
    for (int f = 0; f < frames; f++) {
        while (V.aq_pos + 1 >= V.aq_len) {   /* need two frames to interpolate */
            plm_samples_t *s = plm_decode_audio(V.plm);
            if (!s) return;
            int keep = V.aq_len - V.aq_pos;
            if (keep > 0) memmove(V.aq, V.aq + 2 * V.aq_pos, sizeof(float) * 2 * (size_t)keep);
            V.aq_len = keep > 0 ? keep : 0; V.aq_pos = 0;
            if (V.aq_len + (int)s->count > V.aq_cap) { V.aq_cap = V.aq_len + (int)s->count + 4096; V.aq = realloc(V.aq, sizeof(float) * 2 * (size_t)V.aq_cap); }
            memcpy(V.aq + 2 * V.aq_len, s->interleaved, sizeof(float) * 2 * s->count);
            V.aq_len += (int)s->count;
        }
        float t = (float)V.aq_frac;
        const float *a = V.aq + 2 * V.aq_pos;
        acc[2 * f] += (int32_t)((a[0] + (a[2] - a[0]) * t) * 26000.0f);
        acc[2 * f + 1] += (int32_t)((a[1] + (a[3] - a[1]) * t) * 26000.0f);
        V.aq_frac += step;
        int adv = (int)V.aq_frac;
        V.aq_pos += adv; V.aq_frac -= adv;
    }
}

/* ------------------------------------------------------------------ IUnknown helpers */
static void u_AddRef(void) { RET_STD(1, 1); }
static void u_Release(void) { RET_STD(1, 0); }
static void u_QueryInterface(void) { M32(ARG(2)) = ARG(0); RET_STD(3, S_OK); }
static void amms_Release(void) {
    if (V.plm) LOG("video: closed after %d of %d frames\n", V.updates, V.frames);
    video_close();
    RET_STD(1, 0);
}

/* ------------------------------------------------------------------ IAMMultiMediaStream */
static void amms_GetInformation(void) { if (ARG(1)) M32(ARG(1)) = 0; if (ARG(2)) M32(ARG(2)) = 0; RET_STD(3, S_OK); }
static void amms_GetMediaStream(void) {
    if (memcmp(PTR(ARG(1)), MSPID_PrimaryVideo, 16) != 0) RET_STD(3, 0x80040120u /* MS_E_NOSTREAM */);
    M32(ARG(2)) = obj_stream;
    RET_STD(3, S_OK);
}
static void amms_EnumMediaStreams(void) { RET_STD(3, 1u /* S_FALSE */); }
static void amms_GetState(void) { M32(ARG(1)) = (uint32_t)V.running; RET_STD(2, S_OK); }
static void amms_SetState(void) {
    int run = ARG(1) == 1;
    if (run && !V.running) {
        V.running = 1;
        V.t0_us = plat_ticks_us() - (uint64_t)(V.shown * 1e6);
    } else if (!run) {
        V.running = 0;
    }
    RET_STD(2, S_OK);
}
static void amms_GetTime(void) {
    uint64_t t = (uint64_t)(clock_now() * 1e7);
    M32(ARG(1)) = (uint32_t)t; M32(ARG(1) + 4) = (uint32_t)(t >> 32);
    RET_STD(2, S_OK);
}
static void amms_GetDuration(void) {
    uint64_t t = V.plm ? (uint64_t)(plm_get_duration(V.plm) * 1e7) : 0;
    M32(ARG(1)) = (uint32_t)t; M32(ARG(1) + 4) = (uint32_t)(t >> 32);
    RET_STD(2, S_OK);
}
static void amms_Seek(void) {   /* (this, STREAM_TIME as two dwords) */
    uint64_t t = (uint64_t)ARG(1) | (uint64_t)ARG(2) << 32;
    if (V.plm && t == 0) {
        plm_rewind(V.plm);
        V.shown = 0; V.ended = 0; V.aq_len = V.aq_pos = 0; V.aq_frac = 0;
        if (V.running) V.t0_us = plat_ticks_us();
    }
    RET_STD(3, S_OK);
}
static void amms_GetEndOfStreamEventHandle(void) { M32(ARG(1)) = 0; RET_STD(2, E_FAIL); }
static void amms_Initialize(void) { RET_STD(4, S_OK); }
static void amms_GetFilterGraph(void) { M32(ARG(1)) = 0; RET_STD(2, S_OK); }
static void amms_GetFilter(void) { M32(ARG(1)) = 0; RET_STD(2, S_OK); }
static void amms_AddMediaStream(void) {   /* (this, pStreamObject, PurposeId, dwFlags, ppNewStream) */
    if (memcmp(PTR(ARG(2)), MSPID_PrimaryAudio, 16) == 0) V.want_audio = 1;
    if (ARG(4)) M32(ARG(4)) = memcmp(PTR(ARG(2)), MSPID_PrimaryVideo, 16) == 0 ? obj_stream : 0;
    RET_STD(5, S_OK);
}
static void amms_OpenFile(void) {
    char path[260]; int n = 0;
    for (uint32_t p = ARG(1); M16(p) && n < 259; p += 2) { uint16_t c = M16(p); path[n++] = c < 256 ? (char)c : '?'; }
    path[n] = 0;
    int want_audio = V.want_audio;
    video_close();
    V.want_audio = want_audio;
    uint32_t len;
    uint8_t *data = vfs_read_all(path, &len);
    if (!data) { LOG("video: %s not found\n", path); RET_STD(3, 0x80070002u); }
    V.plm = plm_create_with_memory(data, len, 1);
    if (!V.plm || !plm_has_headers(V.plm)) { LOG("video: %s is not an MPEG-1 stream\n", path); video_close(); RET_STD(3, E_FAIL); }
    V.w = plm_get_width(V.plm); V.h = plm_get_height(V.plm);
    V.arate = plm_get_num_audio_streams(V.plm) ? plm_get_samplerate(V.plm) : 0;
    plm_set_audio_enabled(V.plm, V.want_audio && V.arate);
    plm_set_loop(V.plm, 0);
    LOG("video: %s %dx%d %.2f fps, %.1f s, audio %d Hz\n", path, V.w, V.h, plm_get_framerate(V.plm), plm_get_duration(V.plm), V.arate);
    /* the start-up video is FMV\<lang>\intro.mpg in every version.  Only its first showing is skipped,
     * so the menu's "Introsequenz" still plays it; skipping ends the stream at once, the same path as a
     * video that has played out */
    static int intro_seen;
    const char *base = path;
    for (const char *c = path; *c; c++) if (*c == '\\' || *c == '/') base = c + 1;
    if (strcasecmp(base, "intro.mpg") == 0 && !intro_seen++ && skip_intro) { LOG("video: skipped\n"); V.ended = 1; }
    RET_STD(3, S_OK);
}
static void amms_OpenMoniker(void) { RET_STD(4, E_FAIL); }
static void amms_Render(void) { RET_STD(2, S_OK); }

/* ------------------------------------------------------------------ IDirectDrawMediaStream */
static void st_GetMultiMediaStream(void) { M32(ARG(1)) = obj_amms; RET_STD(2, S_OK); }
static void st_GetInformation(void) {
    if (ARG(1)) memcpy(PTR(ARG(1)), MSPID_PrimaryVideo, 16);
    if (ARG(2)) M32(ARG(2)) = 0;
    RET_STD(3, S_OK);
}
static void st_SetSameFormat(void) { RET_STD(3, S_OK); }
static void st_AllocateSample(void) { RET_STD(3, E_FAIL); }
static void st_CreateSharedSample(void) { RET_STD(4, E_FAIL); }
static void st_SendEndOfStream(void) { RET_STD(2, S_OK); }
static void st_GetFormat(void) {   /* (this, pDDSDCurrent, ppDDPalette, pDDSDDesired, pdwFlags) */
    uint32_t d = ARG(1);
    if (d) { M32(d + 4) |= 6; M32(d + 8) = (uint32_t)V.h; M32(d + 12) = (uint32_t)V.w; }
    if (ARG(2)) M32(ARG(2)) = 0;
    if (ARG(4)) M32(ARG(4)) = 0;
    RET_STD(5, S_OK);
}
static void st_SetFormat(void) { RET_STD(3, S_OK); }
static void st_GetDirectDraw(void) { M32(ARG(1)) = 0; RET_STD(2, E_FAIL); }
static void st_SetDirectDraw(void) { RET_STD(2, S_OK); }
static void st_CreateSample(void) {   /* (this, pSurface, pRect, dwFlags, ppSample) */
    if (!V.plm) RET_STD(5, E_FAIL);
    V.surf = ddraw_make_surface(V.w, V.h, &V.pixels, &V.pitch, &V.bpp);
    M32(ARG(4)) = obj_sample;
    RET_STD(5, S_OK);
}
static void st_GetTimePerFrame(void) {
    uint64_t t = V.plm ? (uint64_t)(1e7 / plm_get_framerate(V.plm)) : 400000;
    M32(ARG(1)) = (uint32_t)t; M32(ARG(1) + 4) = (uint32_t)(t >> 32);
    RET_STD(2, S_OK);
}

/* ------------------------------------------------------------------ IDirectDrawStreamSample */
static void sm_GetMediaStream(void) { M32(ARG(1)) = obj_stream; RET_STD(2, S_OK); }
static void sm_GetSampleTimes(void) { RET_STD(4, S_OK); }
static void sm_SetSampleTimes(void) { RET_STD(3, S_OK); }
static void sm_Update(void) {   /* (this, dwFlags, hEvent, pfnAPC, dwAPCData) */
    if (!V.plm || V.ended) RET_STD(5, MS_S_ENDOFSTREAM);
    plm_frame_t *f;
    /* decode until the frame that is due; skip pictures if we fell behind */
    for (;;) {
        f = plm_decode_video(V.plm);
        if (!f) { V.ended = 1; LOG("video: end of stream after %d frames (%d shown)\n", V.frames, V.updates); RET_STD(5, MS_S_ENDOFSTREAM); }
        V.frames++;
        double next = f->time + 1.0 / plm_get_framerate(V.plm);
        if (!V.running || next > clock_now()) break;
    }
    /* wait for its presentation time, keeping sound and input flowing */
    while (V.running && f->time > clock_now()) {
        audio_update();
        input_pump();
        plat_sleep(2);
    }
    V.shown = f->time;
    V.updates++;
    put_frame(f);
    RET_STD(5, S_OK);
}
static void sm_CompletionStatus(void) { RET_STD(3, S_OK); }
static void sm_GetSurface(void) {   /* (this, ppDirectDrawSurface, pRect) */
    if (ARG(1)) M32(ARG(1)) = V.surf;
    if (ARG(2)) { M32(ARG(2)) = 0; M32(ARG(2) + 4) = 0; M32(ARG(2) + 8) = (uint32_t)V.w; M32(ARG(2) + 12) = (uint32_t)V.h; }
    RET_STD(3, V.surf ? S_OK : E_FAIL);
}
static void sm_SetRect(void) { RET_STD(2, S_OK); }

/* ------------------------------------------------------------------ class factory hook */
#define N(x) (int)(sizeof x / sizeof *x)
uint32_t video_create(uint32_t clsid, uint32_t iid, uint32_t ppv) {
    (void)iid;
    if (!vt_amms) {
        static const gfn_t amms_m[] = {u_QueryInterface, u_AddRef, amms_Release, amms_GetInformation, amms_GetMediaStream,
            amms_EnumMediaStreams, amms_GetState, amms_SetState, amms_GetTime, amms_GetDuration, amms_Seek,
            amms_GetEndOfStreamEventHandle, amms_Initialize, amms_GetFilterGraph, amms_GetFilter, amms_AddMediaStream,
            amms_OpenFile, amms_OpenMoniker, amms_Render};
        static const gfn_t st_m[] = {u_QueryInterface, u_AddRef, u_Release, st_GetMultiMediaStream, st_GetInformation,
            st_SetSameFormat, st_AllocateSample, st_CreateSharedSample, st_SendEndOfStream, st_GetFormat, st_SetFormat,
            st_GetDirectDraw, st_SetDirectDraw, st_CreateSample, st_GetTimePerFrame};
        static const gfn_t sm_m[] = {u_QueryInterface, u_AddRef, u_Release, sm_GetMediaStream, sm_GetSampleTimes,
            sm_SetSampleTimes, sm_Update, sm_CompletionStatus, sm_GetSurface, sm_SetRect};
        static const gfn_t unk_m[] = {u_QueryInterface, u_AddRef, u_Release};
        vt_amms = com_make_vtbl(amms_m, NULL, N(amms_m));
        vt_stream = com_make_vtbl(st_m, NULL, N(st_m));
        vt_sample = com_make_vtbl(sm_m, NULL, N(sm_m));
        vt_unk = com_make_vtbl(unk_m, NULL, N(unk_m));
        obj_amms = com_new(vt_amms, 4); obj_stream = com_new(vt_stream, 4); obj_sample = com_new(vt_sample, 4);
    }
    if (memcmp(PTR(clsid), CLSID_AMMultiMediaStream, 16) == 0) { video_close(); M32(ppv) = obj_amms; return S_OK; }
    if (memcmp(PTR(clsid), CLSID_FilterGraph, 16) == 0) { M32(ppv) = com_new(vt_unk, 4); return S_OK; }
    return 0x80040154u;
}
