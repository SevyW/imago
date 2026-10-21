// Background-music preview: decodes a theme's .at9 with the Vita's audio
// decoder on its own thread and loops it until stopped.
#pragma once

typedef enum { MUSIC_IDLE, MUSIC_LOADING, MUSIC_PLAYING, MUSIC_ERROR } MusicState;

void music_init(void);
void music_shutdown(void);
// Starts playing path (stopping anything else).
void music_play(const char *path);
void music_stop(void);
// State of the given track: anything but `path` counts as MUSIC_IDLE.
MusicState music_state(const char *path);
