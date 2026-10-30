#ifndef DM_MOVIE_H
#define DM_MOVIE_H

/* Hardware-decoded full-screen movies (see movie.c). */
int movie_request(const char *name);   /* 1 if a converted file exists and playback was queued */
int movie_playing(void);
int movie_engine_playing(void);       /* r61: JavaCallback_IsVideoPlaying - only movies the engine started */
void movie_stop(void);                 /* JavaCallback_StopVideo */
void movie_render(void);               /* rendering thread, at every buffer swap, before the swap */
void movie_check_stalled(void);        /* watchdog: give up a requested movie if nothing is being presented */

#endif
