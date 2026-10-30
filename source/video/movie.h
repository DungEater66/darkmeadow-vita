/* Full-screen movies, decoded in hardware (see movie.c). */
#ifndef DM_MOVIE_H
#define DM_MOVIE_H

int movie_request(const char *name);   /* JavaCallback_StartVideo: 1 if a converted file exists and playback was queued */
void movie_stop(void);                 /* JavaCallback_StopVideo */
int movie_playing(void);               /* any movie, the opening movie included */
int movie_engine_playing(void);        /* JavaCallback_IsVideoPlaying: only movies the engine asked for */
int movie_opening_pending(void);       /* the opening movie has not played yet */
void movie_render(void);               /* rendering thread, at every buffer swap, before the swap */
void movie_check_stalled(void);        /* watchdog: give up a requested movie if nothing is being presented */

#endif
