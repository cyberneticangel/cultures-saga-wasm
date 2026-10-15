/* DirectMusic 8 emulation for gedx8musicdrv.dll (recompiled): loader, performance,
 * segments and audio paths.  Playback itself lives in dmsynth.c. */
#include "runtime.h"
#include "dmusic_int.h"
#include <stdio.h>
#include <stdlib.h>

#define S_OK 0
#define S_FALSE 1
#define E_NOTIMPL 0x80004001u
#define E_NOINTERFACE 0x80004002u
#define E_FAIL 0x80004005u
#define DMUS_E_LOADER_FAILEDOPEN 0x88781190u

static void stub_log(const char *name) { LOG("dmusic: unimplemented %s\n", name); }
#define STUB(pfx, name, n) static void pfx##_##name(void) { stub_log(#pfx "::" #name); RET_STD(n, E_NOTIMPL); }

static void guid_str(uint32_t g, char *out) {
    sprintf(out, "%08x-%04x-%04x-%02x%02x%02x%02x%02x%02x%02x%02x", M32(g), M16(g + 4), M16(g + 6),
            M8(g + 8), M8(g + 9), M8(g + 10), M8(g + 11), M8(g + 12), M8(g + 13), M8(g + 14), M8(g + 15));
}
static int guid_is(uint32_t g, uint32_t d1, uint16_t d2) { return M32(g) == d1 && M16(g + 4) == d2; }

/* ---- object registry: guest object = [vtbl][kind][index] ---- */
enum { K_PERF = 1, K_LOADER, K_SEG, K_SEGSTATE, K_PATH, K_DMUSIC, K_UNK, K_COLL };
static uint32_t mkobj(uint32_t vtbl, int kind, int idx) {
    uint32_t o = com_new(vtbl, 12);
    M32(o + 4) = (uint32_t)kind; M32(o + 8) = (uint32_t)idx;
    return o;
}
#define OBJIDX(o) ((int)M32((o) + 8))

static uint32_t vt_unk, vt_perf, vt_loader, vt_seg, vt_segstate, vt_path, vt_dmusic;

/* ------------------------------------------------------------- generic IUnknown */
static void unk_QueryInterface(void) { M32(ARG(2)) = ARG(0); RET_STD(3, S_OK); }
static void unk_AddRef(void) { RET_STD(1, 2); }
static void unk_Release(void) { RET_STD(1, 1); }
static const gfn_t unk_m[] = {unk_QueryInterface, unk_AddRef, unk_Release};
static const char *const unk_n[] = {"unk::QueryInterface", "unk::AddRef", "unk::Release"};

/* ------------------------------------------------------------- IDirectMusicPerformance8 */
static int master_volume = 0;      /* hundredths of dB */

static void perf_Init(void) { if (ARG(1)) M32(ARG(1)) = mkobj(vt_dmusic, K_DMUSIC, 0); RET_STD(4, S_OK); }
static void perf_PlaySegment(void) {
    uint32_t seg = ARG(1), flags = ARG(2), pss = ARG(5);
    int st = dm_play(OBJIDX(seg), flags, 0);
    if (pss) M32(pss) = mkobj(vt_segstate, K_SEGSTATE, st);
    RET_STD(6, S_OK);
}
static void perf_Stop(void) {
    dm_stop(ARG(1) ? OBJIDX(ARG(1)) : -1, ARG(4));
    RET_STD(5, S_OK);
}
STUB(perf, GetSegmentState, 3)
static void perf_SetPrepareTime(void) { RET_STD(2, S_OK); }
static void perf_GetPrepareTime(void) { M32(ARG(1)) = 1000; RET_STD(2, S_OK); }
static void perf_SetBumperLength(void) { RET_STD(2, S_OK); }
static void perf_GetBumperLength(void) { M32(ARG(1)) = 50; RET_STD(2, S_OK); }
STUB(perf, SendPMsg, 2)
STUB(perf, MusicToReferenceTime, 3)
STUB(perf, ReferenceToMusicTime, 4)
static void perf_IsPlaying(void) {
    int seg = ARG(1) ? OBJIDX(ARG(1)) : -1, st = ARG(2) ? OBJIDX(ARG(2)) : -1;
    RET_STD(3, dm_is_playing(seg, st) ? S_OK : S_FALSE);
}
static void perf_GetTime(void) {
    uint64_t rt = dm_reftime();
    if (ARG(1)) M64(ARG(1)) = rt;
    if (ARG(2)) M32(ARG(2)) = dm_musictime();
    RET_STD(3, S_OK);
}
STUB(perf, AllocPMsg, 3)
STUB(perf, FreePMsg, 2)
STUB(perf, GetGraph, 2)
STUB(perf, SetGraph, 2)
static void perf_SetNotificationHandle(void) { RET_STD(4, S_OK); }
static void perf_GetNotificationPMsg(void) { RET_STD(2, S_FALSE); }
static void perf_AddNotificationType(void) { RET_STD(2, S_OK); }
static void perf_RemoveNotificationType(void) { RET_STD(2, S_OK); }
STUB(perf, AddPort, 2)
STUB(perf, RemovePort, 2)
STUB(perf, AssignPChannelBlock, 4)
STUB(perf, AssignPChannel, 5)
STUB(perf, PChannelInfo, 5)
STUB(perf, DownloadInstrument, 9)
STUB(perf, Invalidate, 3)
STUB(perf, GetParam, 7)
STUB(perf, SetParam, 6)
static void perf_GetGlobalParam(void) {
    char g[64]; guid_str(ARG(1), g);
    if (guid_is(ARG(1), 0xd2ac28b1u, 0xb39b)) { M32(ARG(2)) = (uint32_t)master_volume; RET_STD(4, S_OK); }   /* GUID_PerfMasterVolume */
    LOG("dmusic: GetGlobalParam %s\n", g);
    RET_STD(4, E_FAIL);
}
static void perf_SetGlobalParam(void) {
    char g[64]; guid_str(ARG(1), g);
    if (guid_is(ARG(1), 0xd2ac28b1u, 0xb39b)) { master_volume = (int32_t)M32(ARG(2)); dm_set_master_volume(master_volume); RET_STD(4, S_OK); }
    LOG("dmusic: SetGlobalParam %s\n", g);
    RET_STD(4, S_OK);
}
static void perf_GetLatencyTime(void) { M64(ARG(1)) = dm_reftime(); RET_STD(2, S_OK); }
static void perf_GetQueueTime(void) { M64(ARG(1)) = dm_reftime(); RET_STD(2, S_OK); }
STUB(perf, AdjustTime, 3)
static void perf_CloseDown(void) { dm_stop(-1, 0); RET_STD(1, S_OK); }
STUB(perf, GetResolvedTime, 5)
STUB(perf, MIDIToMusic, 6)
STUB(perf, MusicToMIDI, 6)
STUB(perf, TimeToRhythm, 7)
STUB(perf, RhythmToTime, 7)
static void perf_InitAudio(void) {
    if (ARG(1)) M32(ARG(1)) = mkobj(vt_dmusic, K_DMUSIC, 0);
    if (ARG(2) && !M32(ARG(2))) M32(ARG(2)) = 0;
    LOG("dmusic: InitAudio pathtype=%u pchannels=%u flags=%x\n", ARG(4), ARG(5), ARG(6));
    dm_init();
    RET_STD(8, S_OK);
}
static void perf_PlaySegmentEx(void) {
    uint32_t src = ARG(1), flags = ARG(4), pss = ARG(7);
    if (!src) RET_STD(10, E_FAIL);
    int st = dm_play(OBJIDX(src), flags, M32(src + 4) == K_SEG ? 0 : 0);
    if (pss) M32(pss) = mkobj(vt_segstate, K_SEGSTATE, st);
    RET_STD(10, S_OK);
}
static void perf_StopEx(void) {
    uint32_t o = ARG(1);
    int seg = -1;
    if (o && M32(o + 4) == K_SEG) seg = OBJIDX(o);
    else if (o && M32(o + 4) == K_SEGSTATE) seg = dm_state_segment(OBJIDX(o));
    dm_stop(seg, ARG(4));
    RET_STD(5, S_OK);
}
STUB(perf, ClonePMsg, 3)
static void perf_CreateAudioPath(void) { M32(ARG(3)) = mkobj(vt_path, K_PATH, 0); RET_STD(4, S_OK); }
static void perf_CreateStandardAudioPath(void) { M32(ARG(4)) = mkobj(vt_path, K_PATH, 0); RET_STD(5, S_OK); }
static void perf_SetDefaultAudioPath(void) { RET_STD(2, S_OK); }
static void perf_GetDefaultAudioPath(void) { M32(ARG(1)) = mkobj(vt_path, K_PATH, 0); RET_STD(2, S_OK); }
STUB(perf, GetParamEx, 8)
STUB(perf, SetParamEx, 7)

static const gfn_t perf_m[] = {unk_QueryInterface, unk_AddRef, unk_Release,
    perf_Init, perf_PlaySegment, perf_Stop, perf_GetSegmentState, perf_SetPrepareTime, perf_GetPrepareTime,
    perf_SetBumperLength, perf_GetBumperLength, perf_SendPMsg, perf_MusicToReferenceTime, perf_ReferenceToMusicTime,
    perf_IsPlaying, perf_GetTime, perf_AllocPMsg, perf_FreePMsg, perf_GetGraph, perf_SetGraph,
    perf_SetNotificationHandle, perf_GetNotificationPMsg, perf_AddNotificationType, perf_RemoveNotificationType,
    perf_AddPort, perf_RemovePort, perf_AssignPChannelBlock, perf_AssignPChannel, perf_PChannelInfo,
    perf_DownloadInstrument, perf_Invalidate, perf_GetParam, perf_SetParam, perf_GetGlobalParam, perf_SetGlobalParam,
    perf_GetLatencyTime, perf_GetQueueTime, perf_AdjustTime, perf_CloseDown, perf_GetResolvedTime, perf_MIDIToMusic,
    perf_MusicToMIDI, perf_TimeToRhythm, perf_RhythmToTime, perf_InitAudio, perf_PlaySegmentEx, perf_StopEx,
    perf_ClonePMsg, perf_CreateAudioPath, perf_CreateStandardAudioPath, perf_SetDefaultAudioPath,
    perf_GetDefaultAudioPath, perf_GetParamEx, perf_SetParamEx};
static const char *const perf_n[] = {"perf::QueryInterface", "perf::AddRef", "perf::Release",
    "perf::Init", "perf::PlaySegment", "perf::Stop", "perf::GetSegmentState", "perf::SetPrepareTime", "perf::GetPrepareTime",
    "perf::SetBumperLength", "perf::GetBumperLength", "perf::SendPMsg", "perf::MusicToReferenceTime", "perf::ReferenceToMusicTime",
    "perf::IsPlaying", "perf::GetTime", "perf::AllocPMsg", "perf::FreePMsg", "perf::GetGraph", "perf::SetGraph",
    "perf::SetNotificationHandle", "perf::GetNotificationPMsg", "perf::AddNotificationType", "perf::RemoveNotificationType",
    "perf::AddPort", "perf::RemovePort", "perf::AssignPChannelBlock", "perf::AssignPChannel", "perf::PChannelInfo",
    "perf::DownloadInstrument", "perf::Invalidate", "perf::GetParam", "perf::SetParam", "perf::GetGlobalParam", "perf::SetGlobalParam",
    "perf::GetLatencyTime", "perf::GetQueueTime", "perf::AdjustTime", "perf::CloseDown", "perf::GetResolvedTime", "perf::MIDIToMusic",
    "perf::MusicToMIDI", "perf::TimeToRhythm", "perf::RhythmToTime", "perf::InitAudio", "perf::PlaySegmentEx", "perf::StopEx",
    "perf::ClonePMsg", "perf::CreateAudioPath", "perf::CreateStandardAudioPath", "perf::SetDefaultAudioPath",
    "perf::GetDefaultAudioPath", "perf::GetParamEx", "perf::SetParamEx"};

/* ------------------------------------------------------------- IDirectMusicLoader8 */
static char search_dir[520] = "";

static void wstr(uint32_t p, char *out, int cap) {
    int i = 0;
    for (; i < cap - 1 && M16(p + 2 * i); i++) { uint16_t c = M16(p + 2 * i); out[i] = c < 256 ? (char)c : '?'; }
    out[i] = 0;
}

/* resolve a file name relative to the search directory to a host path */
static int resolve_music_file(const char *name, char *host, int cap) {
    char win[600];
    if (strchr(name, ':') || name[0] == '\\') snprintf(win, sizeof win, "%s", name);
    else snprintf(win, sizeof win, "%s%s%s", search_dir, search_dir[0] ? "\\" : "", name);
    return vfs_resolve(win, host, cap);
}

static uint8_t *read_host_file(const char *host, uint32_t *len) {
    int fd = plat_open(host, 0, 0, 0);
    if (fd < 0) return NULL;
    int64_t n = plat_size(fd);
    uint8_t *b = malloc((size_t)n + 1);
    plat_read(fd, b, (uint32_t)n);
    plat_close(fd);
    *len = (uint32_t)n;
    return b;
}

/* read an IStream completely (guest COM object): Seek to 0, Read until done */
static uint8_t *read_stream(uint32_t stream, uint32_t *len) {
    uint32_t vt = M32(stream);
    uint32_t tmp = heap_alloc(16, 1);
    /* IStream::Stat would give the size; read in chunks instead */
    uint32_t cap = 1 << 16, n = 0;
    uint8_t *buf = malloc(cap);
    uint32_t chunk = heap_alloc(65536, 0);
    /* Seek(0, STREAM_SEEK_SET, NULL): args this, LARGE_INTEGER(8), origin, pnew */
    CALL_GUEST(M32(vt + 20), stream, 0, 0, 0, 0);
    for (;;) {
        M32(tmp) = 0;
        CALL_GUEST(M32(vt + 12), stream, chunk, 65536, tmp);   /* Read(pv, cb, pcbRead) */
        uint32_t got = M32(tmp);
        if (!got) break;
        if (n + got > cap) { while (n + got > cap) cap *= 2; buf = realloc(buf, cap); }
        memcpy(buf + n, PTR(chunk), got); n += got;
        if (got < 65536) break;
    }
    heap_free(chunk); heap_free(tmp);
    *len = n;
    return buf;
}

/* used by the synth to load DLS collections referenced from bands */
uint8_t *dm_read_file(const char *name, uint32_t *len) {
    char host[600];
    if (!resolve_music_file(name, host, sizeof host)) return NULL;
    return read_host_file(host, len);
}

static void loader_GetObject(void) {
    uint32_t desc = ARG(1), ppv = ARG(3);
    uint32_t valid = M32(desc + 4);
    char cls[64], name[300] = "", fname[300] = "";
    guid_str(desc + 24, cls);
    if (valid & 4) wstr(desc + 56, name, sizeof name);
    if (valid & 0x10) wstr(desc + 312, fname, sizeof fname);
    uint8_t *data = NULL; uint32_t len = 0;
    if (valid & 0x400) {                       /* DMUS_OBJ_MEMORY */
        len = (uint32_t)M64(desc + 832);
        data = malloc(len ? len : 1);
        memcpy(data, PTR(M32(desc + 840)), len);
    } else if (valid & 0x800) {                /* DMUS_OBJ_STREAM */
        data = read_stream(M32(desc + 844), &len);
    } else if (valid & 0x10) {
        char host[600];
        if (resolve_music_file(fname, host, sizeof host)) data = read_host_file(host, &len);
    }
    if (!data) {
        LOG("dmusic: GetObject(class %s, name '%s', file '%s') failed\n", cls, name, fname);
        M32(ppv) = 0;
        RET_STD(4, DMUS_E_LOADER_FAILEDOPEN);
    }
    int idx = dm_load_segment(data, len, fname[0] ? fname : name);
    free(data);
    if (idx < 0) { M32(ppv) = 0; RET_STD(4, E_FAIL); }
    TRACE("dmusic: GetObject '%s' -> segment %d\n", fname[0] ? fname : name, idx);
    M32(ppv) = mkobj(vt_seg, K_SEG, idx);
    RET_STD(4, S_OK);
}
STUB(loader, SetObject, 2)
static void loader_SetSearchDirectory(void) {
    char w[520]; wstr(ARG(2), w, sizeof w);
    size_t l = strlen(w);
    while (l && (w[l - 1] == '\\' || w[l - 1] == '/')) w[--l] = 0;
    snprintf(search_dir, sizeof search_dir, "%s", w);
    LOG("dmusic: SetSearchDirectory(%s)\n", search_dir);
    RET_STD(4, S_OK);
}
static void loader_ScanDirectory(void) { RET_STD(4, S_OK); }
static void loader_CacheObject(void) { RET_STD(2, S_OK); }
static void loader_ReleaseObject(void) { RET_STD(2, S_OK); }
static void loader_ClearCache(void) { RET_STD(2, S_OK); }
static void loader_EnableCache(void) { RET_STD(3, S_OK); }
STUB(loader, EnumObject, 4)
static void loader_CollectGarbage(void) { RET_STD(1, S_OK); }
static void loader_ReleaseObjectByUnknown(void) { RET_STD(2, S_OK); }
static void loader_LoadObjectFromFile(void) {
    char f[520], host[600]; wstr(ARG(3), f, sizeof f);
    uint32_t len; uint8_t *data = NULL;
    if (resolve_music_file(f, host, sizeof host)) data = read_host_file(host, &len);
    if (!data) { LOG("dmusic: LoadObjectFromFile(%s) failed\n", f); M32(ARG(4)) = 0; RET_STD(5, DMUS_E_LOADER_FAILEDOPEN); }
    int idx = dm_load_segment(data, len, f);
    free(data);
    if (idx < 0) { M32(ARG(4)) = 0; RET_STD(5, E_FAIL); }
    M32(ARG(4)) = mkobj(vt_seg, K_SEG, idx);
    RET_STD(5, S_OK);
}
static const gfn_t loader_m[] = {unk_QueryInterface, unk_AddRef, unk_Release, loader_GetObject, loader_SetObject,
    loader_SetSearchDirectory, loader_ScanDirectory, loader_CacheObject, loader_ReleaseObject, loader_ClearCache,
    loader_EnableCache, loader_EnumObject, loader_CollectGarbage, loader_ReleaseObjectByUnknown, loader_LoadObjectFromFile};
static const char *const loader_n[] = {"loader::QueryInterface", "loader::AddRef", "loader::Release", "loader::GetObject",
    "loader::SetObject", "loader::SetSearchDirectory", "loader::ScanDirectory", "loader::CacheObject", "loader::ReleaseObject",
    "loader::ClearCache", "loader::EnableCache", "loader::EnumObject", "loader::CollectGarbage",
    "loader::ReleaseObjectByUnknown", "loader::LoadObjectFromFile"};

/* ------------------------------------------------------------- IDirectMusicSegment8 */
static void seg_GetLength(void) { M32(ARG(1)) = dm_seg_length(OBJIDX(ARG(0))); RET_STD(2, S_OK); }
static void seg_SetLength(void) { dm_seg_set_length(OBJIDX(ARG(0)), ARG(1)); RET_STD(2, S_OK); }
static void seg_GetRepeats(void) { M32(ARG(1)) = dm_seg_repeats(OBJIDX(ARG(0))); RET_STD(2, S_OK); }
static void seg_SetRepeats(void) { dm_seg_set_repeats(OBJIDX(ARG(0)), ARG(1)); RET_STD(2, S_OK); }
static void seg_GetDefaultResolution(void) { M32(ARG(1)) = 0; RET_STD(2, S_OK); }
static void seg_SetDefaultResolution(void) { RET_STD(2, S_OK); }
STUB(seg, GetTrack, 5)
STUB(seg, GetTrackGroup, 3)
STUB(seg, InsertTrack, 3)
STUB(seg, RemoveTrack, 2)
STUB(seg, InitPlay, 4)
STUB(seg, GetGraph, 2)
STUB(seg, SetGraph, 2)
static void seg_AddNotificationType(void) { RET_STD(2, S_OK); }
static void seg_RemoveNotificationType(void) { RET_STD(2, S_OK); }
static void seg_GetParam(void) {
    /* (this, rguidType, dwGroupBits, dwIndex, mtTime, pmtNext, pParam) */
    uint32_t g = ARG(1), next = ARG(5), out = ARG(6);
    int seg = OBJIDX(ARG(0));
    if (next) M32(next) = 0;
    if (guid_is(g, 0xd2ac28a4u, 0xb39b)) {            /* GUID_TimeSignature -> DMUS_TIMESIGNATURE */
        uint8_t bpm, beat; uint16_t grids;
        dm_seg_timesig(seg, &bpm, &beat, &grids);
        M32(out) = 0; M8(out + 4) = bpm; M8(out + 5) = beat; M16(out + 6) = grids;
        RET_STD(7, S_OK);
    }
    if (guid_is(g, 0xd2ac28a5u, 0xb39b)) {            /* GUID_TempoParam -> DMUS_TEMPO_PARAM */
        M32(out) = 0; MF64(out + 8) = dm_seg_tempo(seg);
        RET_STD(7, S_OK);
    }
    if (guid_is(g, 0xd2ac289eu, 0xb39b)) {            /* GUID_ChordParam -> DMUS_CHORD_KEY: C major */
        memset(PTR(out), 0, 204);
        M16(out) = 'M';
        M8(out + 35) = 1;
        M32(out + 36) = 0x91;            /* chord pattern: root, major third, fifth */
        M32(out + 40) = 0xab5ab5;        /* scale pattern: major */
        M32(out + 44) = 0xffffff;
        M32(out + 48) = 0xffffffff;
        M32(out + 196) = 0xab5ab5;
        RET_STD(7, S_OK);
    }
    char gs[64]; guid_str(g, gs);
    LOG("dmusic: Segment::GetParam %s unsupported\n", gs);
    RET_STD(7, 0x88781187u);
}
static void seg_SetParam(void) {
    char g[64]; guid_str(ARG(1), g);
    TRACE("dmusic: Segment::SetParam %s\n", g);
    RET_STD(6, S_OK);
}
STUB(seg, Clone, 4)
static void seg_SetStartPoint(void) { RET_STD(2, S_OK); }
static void seg_GetStartPoint(void) { M32(ARG(1)) = 0; RET_STD(2, S_OK); }
static void seg_SetLoopPoints(void) { dm_seg_set_loop(OBJIDX(ARG(0)), ARG(1), ARG(2)); RET_STD(3, S_OK); }
static void seg_GetLoopPoints(void) { M32(ARG(1)) = 0; M32(ARG(2)) = 0; RET_STD(3, S_OK); }
static void seg_SetPChannelsUsed(void) { RET_STD(3, S_OK); }
static void seg_SetTrackConfig(void) { RET_STD(6, S_OK); }
static void seg_GetAudioPathConfig(void) { M32(ARG(1)) = mkobj(vt_unk, K_UNK, 0); RET_STD(2, S_OK); }   /* segments carry a DMAP config */
STUB(seg, Compose, 5)
static void seg_Download(void) { dm_seg_download(OBJIDX(ARG(0))); RET_STD(2, S_OK); }
static void seg_Unload(void) { RET_STD(2, S_OK); }
static void seg_Release(void) { RET_STD(1, 0); }
static const gfn_t seg_m[] = {unk_QueryInterface, unk_AddRef, seg_Release, seg_GetLength, seg_SetLength, seg_GetRepeats,
    seg_SetRepeats, seg_GetDefaultResolution, seg_SetDefaultResolution, seg_GetTrack, seg_GetTrackGroup,
    seg_InsertTrack, seg_RemoveTrack, seg_InitPlay, seg_GetGraph, seg_SetGraph, seg_AddNotificationType,
    seg_RemoveNotificationType, seg_GetParam, seg_SetParam, seg_Clone, seg_SetStartPoint, seg_GetStartPoint,
    seg_SetLoopPoints, seg_GetLoopPoints, seg_SetPChannelsUsed, seg_SetTrackConfig, seg_GetAudioPathConfig,
    seg_Compose, seg_Download, seg_Unload};
static const char *const seg_n[] = {"seg::QueryInterface", "seg::AddRef", "seg::Release", "seg::GetLength", "seg::SetLength",
    "seg::GetRepeats", "seg::SetRepeats", "seg::GetDefaultResolution", "seg::SetDefaultResolution", "seg::GetTrack",
    "seg::GetTrackGroup", "seg::InsertTrack", "seg::RemoveTrack", "seg::InitPlay", "seg::GetGraph", "seg::SetGraph",
    "seg::AddNotificationType", "seg::RemoveNotificationType", "seg::GetParam", "seg::SetParam", "seg::Clone",
    "seg::SetStartPoint", "seg::GetStartPoint", "seg::SetLoopPoints", "seg::GetLoopPoints", "seg::SetPChannelsUsed",
    "seg::SetTrackConfig", "seg::GetAudioPathConfig", "seg::Compose", "seg::Download", "seg::Unload"};

/* ------------------------------------------------------------- IDirectMusicSegmentState8 */
static void ss_GetRepeats(void) { M32(ARG(1)) = 0; RET_STD(2, S_OK); }
static void ss_GetSegment(void) { M32(ARG(1)) = mkobj(vt_seg, K_SEG, dm_state_segment(OBJIDX(ARG(0)))); RET_STD(2, S_OK); }
static void ss_GetStartTime(void) { M32(ARG(1)) = dm_state_start(OBJIDX(ARG(0))); RET_STD(2, S_OK); }
static void ss_GetSeek(void) { M32(ARG(1)) = dm_state_seek(OBJIDX(ARG(0))); RET_STD(2, S_OK); }
static void ss_GetStartPoint(void) { M32(ARG(1)) = 0; RET_STD(2, S_OK); }
static void ss_SetTrackConfig(void) { RET_STD(6, S_OK); }
STUB(ss, GetObjectInPath, 8)
static const gfn_t ss_m[] = {unk_QueryInterface, unk_AddRef, unk_Release, ss_GetRepeats, ss_GetSegment, ss_GetStartTime,
    ss_GetSeek, ss_GetStartPoint, ss_SetTrackConfig, ss_GetObjectInPath};
static const char *const ss_n[] = {"ss::QueryInterface", "ss::AddRef", "ss::Release", "ss::GetRepeats", "ss::GetSegment",
    "ss::GetStartTime", "ss::GetSeek", "ss::GetStartPoint", "ss::SetTrackConfig", "ss::GetObjectInPath"};

/* ------------------------------------------------------------- IDirectMusicAudioPath8 */
static void path_GetObjectInPath(void) { M32(ARG(7)) = 0; RET_STD(8, 0x88781183u); }   /* DMUS_E_NOT_FOUND */
static void path_Activate(void) { RET_STD(2, S_OK); }
static void path_SetVolume(void) { dm_set_path_volume((int32_t)ARG(1), ARG(2)); RET_STD(3, S_OK); }
static void path_ConvertPChannel(void) { M32(ARG(2)) = ARG(1); RET_STD(3, S_OK); }
static const gfn_t path_m[] = {unk_QueryInterface, unk_AddRef, unk_Release, path_GetObjectInPath, path_Activate,
    path_SetVolume, path_ConvertPChannel};
static const char *const path_n[] = {"path::QueryInterface", "path::AddRef", "path::Release", "path::GetObjectInPath",
    "path::Activate", "path::SetVolume", "path::ConvertPChannel"};

/* ------------------------------------------------------------- IDirectMusic8 (rarely used) */
STUB(dm, EnumPort, 3)
STUB(dm, CreateMusicBuffer, 4)
STUB(dm, CreatePort, 5)
STUB(dm, EnumMasterClock, 3)
STUB(dm, GetMasterClock, 3)
STUB(dm, SetMasterClock, 2)
static void dm_Activate(void) { RET_STD(2, S_OK); }
STUB(dm, GetDefaultPort, 2)
static void dm_SetDirectSound(void) { RET_STD(3, S_OK); }
STUB(dm, SetExternalMasterClock, 2)
static const gfn_t dmo_m[] = {unk_QueryInterface, unk_AddRef, unk_Release, dm_EnumPort, dm_CreateMusicBuffer, dm_CreatePort,
    dm_EnumMasterClock, dm_GetMasterClock, dm_SetMasterClock, dm_Activate, dm_GetDefaultPort, dm_SetDirectSound,
    dm_SetExternalMasterClock};
static const char *const dmo_n[] = {"dm::QueryInterface", "dm::AddRef", "dm::Release", "dm::EnumPort", "dm::CreateMusicBuffer",
    "dm::CreatePort", "dm::EnumMasterClock", "dm::GetMasterClock", "dm::SetMasterClock", "dm::Activate",
    "dm::GetDefaultPort", "dm::SetDirectSound", "dm::SetExternalMasterClock"};

/* ------------------------------------------------------------- IDirectMusicComposer8 */
static uint32_t vt_composer;
STUB(comp, ComposeSegmentFromTemplate, 6)
STUB(comp, ComposeSegmentFromShape, 9)
static void comp_ComposeTransition(void) { M32(ARG(7)) = 0; RET_STD(8, S_FALSE); }
static void comp_AutoTransition(void) {
    /* (this, perf, toSeg, wCommand, dwFlags, chordmap, ppTransSeg, ppToSegState, ppTransSegState) */
    uint32_t to = ARG(2), flags = ARG(4);
    int st = -1;
    if (to) st = dm_play(OBJIDX(to), flags & ~0x80u, 0);
    if (ARG(6)) M32(ARG(6)) = 0;
    if (ARG(7)) M32(ARG(7)) = st >= 0 ? mkobj(vt_segstate, K_SEGSTATE, st) : 0;
    if (ARG(8)) M32(ARG(8)) = 0;
    RET_STD(9, S_OK);
}
STUB(comp, ComposeTemplateFromShape, 7)
STUB(comp, ChangeChordMap, 4)
static const gfn_t comp_m[] = {unk_QueryInterface, unk_AddRef, unk_Release, comp_ComposeSegmentFromTemplate,
    comp_ComposeSegmentFromShape, comp_ComposeTransition, comp_AutoTransition, comp_ComposeTemplateFromShape,
    comp_ChangeChordMap};
static const char *const comp_n[] = {"comp::QueryInterface", "comp::AddRef", "comp::Release",
    "comp::ComposeSegmentFromTemplate", "comp::ComposeSegmentFromShape", "comp::ComposeTransition",
    "comp::AutoTransition", "comp::ComposeTemplateFromShape", "comp::ChangeChordMap"};

/* ------------------------------------------------------------- CoCreateInstance hook */
static void init_vtables(void) {
    if (vt_unk) return;
    vt_unk = com_make_vtbl(unk_m, unk_n, 3);
    vt_perf = com_make_vtbl(perf_m, perf_n, (int)(sizeof perf_m / sizeof *perf_m));
    vt_loader = com_make_vtbl(loader_m, loader_n, (int)(sizeof loader_m / sizeof *loader_m));
    vt_seg = com_make_vtbl(seg_m, seg_n, (int)(sizeof seg_m / sizeof *seg_m));
    vt_segstate = com_make_vtbl(ss_m, ss_n, (int)(sizeof ss_m / sizeof *ss_m));
    vt_path = com_make_vtbl(path_m, path_n, (int)(sizeof path_m / sizeof *path_m));
    vt_dmusic = com_make_vtbl(dmo_m, dmo_n, (int)(sizeof dmo_m / sizeof *dmo_m));
    vt_composer = com_make_vtbl(comp_m, comp_n, (int)(sizeof comp_m / sizeof *comp_m));
}

uint32_t dmusic_create(uint32_t clsid, uint32_t iid, uint32_t ppv) {
    (void)iid;
    init_vtables();
    uint32_t c = M32(clsid);
    if (c == 0x636b9f10u) { M32(ppv) = mkobj(vt_dmusic, K_DMUSIC, 0); return 0; }      /* CLSID_DirectMusic (DX version probe) */
    if (M16(clsid + 4) == 0xb39b && M16(clsid + 6) == 0x11d1) {
        if (c == 0xd2ac2881u) { M32(ppv) = mkobj(vt_perf, K_PERF, 0); return 0; }     /* CLSID_DirectMusicPerformance */
        if (c == 0xd2ac2892u) { M32(ppv) = mkobj(vt_loader, K_LOADER, 0); return 0; } /* CLSID_DirectMusicLoader */
        if (c == 0xd2ac2890u) { M32(ppv) = mkobj(vt_composer, K_UNK, 0); return 0; }  /* CLSID_DirectMusicComposer */
        if (c == 0xd2ac2882u) { M32(ppv) = mkobj(vt_seg, K_SEG, -1); return 0; }      /* CLSID_DirectMusicSegment */
    }
    return 0x80040154u;
}
