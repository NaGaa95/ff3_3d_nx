/* movie_player.h -- FF3 3D intro movie playback */

#ifndef __MOVIE_PLAYER_H__
#define __MOVIE_PLAYER_H__

int movie_play(const char *path, int skippable);
void movie_stop(void);
void movie_skip(void);
void movie_pause(void);
void movie_resume(void);
int movie_is_playing(void);
unsigned int movie_swap_buffers(void *display, void *surface);
void movie_main_loop_tick(void);

#endif
