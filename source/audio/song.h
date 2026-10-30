/* Music, ambience and dialogue: the MediaPlayer side of BaseUE3JavaApp (JavaCallback_*Song*). See song.c. */
#ifndef DM_SONG_H
#define DM_SONG_H

#include <stdint.h>

void song_init(void);
void song_play(const char *name, int loop);
void song_stop(void);
void song_pause(int paused);
void song_set_base_volume(float v);
void song_set_fades(float fade_in_s, float fade_out_s);
void song_set_class_volume(float v);
void song_set_music_volume(float v);

int song_battle_music(void);   /* fight music or fight ambience playing or queued */
int song_cin_playing(void);    /* a cutscene track (cin_*) is playing */
extern volatile int song_io_busy;   /* a song is starting: other background readers keep off the card */

/* Add the current song to a 48 kHz stereo block (called by the sound-effect mixer, opensl.c). */
void song_mix(int32_t *acc, int frames);

#endif
