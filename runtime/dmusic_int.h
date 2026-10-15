/* Interface between the DirectMusic COM layer (dmusic.c) and the music engine (dmsynth.c). */
#pragma once
#include <stdint.h>

void dm_init(void);
int  dm_load_segment(const uint8_t *data, uint32_t len, const char *name);   /* -> segment index or -1 */
int  dm_play(int seg, uint32_t flags, int unused);                           /* -> segment state index */
void dm_stop(int seg, uint32_t flags);                                       /* seg -1: everything */
int  dm_is_playing(int seg, int state);
uint64_t dm_reftime(void);          /* 100 ns units */
uint32_t dm_musictime(void);
void dm_set_master_volume(int hundredths_db);
void dm_set_path_volume(int hundredths_db, uint32_t fade_ms);
uint32_t dm_seg_length(int seg);
void dm_seg_set_length(int seg, uint32_t len);
uint32_t dm_seg_repeats(int seg);
void dm_seg_set_repeats(int seg, uint32_t n);
void dm_seg_set_loop(int seg, uint32_t start, uint32_t end);
void dm_seg_download(int seg);
void dm_seg_timesig(int seg, uint8_t *bpm, uint8_t *beat, uint16_t *grids);
double dm_seg_tempo(int seg);
int  dm_state_segment(int st);
uint32_t dm_state_start(int st);
uint32_t dm_state_seek(int st);
/* mixes `frames` stereo frames of music (44.1 kHz) into acc (interleaved int32) */
void dm_render(int32_t *acc, int frames);
