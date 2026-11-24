#ifndef SYSTEM_AUDIO_GAMESOUND_H
#define SYSTEM_AUDIO_GAMESOUND_H

// The sound effects of the games under More (Tower, Hopper).
//
// The sounds are made here, a few tones and a burst of noise each, so there
// are no files to load. A thread mixes whatever is sounding into the external
// PCM (audio.h) and writes silence in between.
//
// Music comes first. With a track or a station playing when a game is opened
// the games stay silent and the music plays on; and when something starts
// playing while a game has the output, the game gives it up and is silent from
// then until it is opened again.

typedef enum {
	GAMESOUND_DROP,	  // a floor set down on the tower
	GAMESOUND_EXACT,  // set down squarely
	GAMESOUND_BOUNCE, // off a ledge
	GAMESOUND_SPRING, // off a spring
	GAMESOUND_BREAK,  // a ledge giving way
	GAMESOUND_OVER,	  // the end of a game
	GAMESOUND_COUNT,
} gamesound_t;

// Starts the mixer's thread and returns at once. When the output cannot be had
// the game runs silently and gamesound_play() does nothing.
void gamesound_start(void);

// Starts `sound` from its beginning, over whatever else is sounding. Any thread.
void gamesound_play(gamesound_t sound);

// Stops the mixer and gives the output back.
void gamesound_stop(void);

#endif /* SYSTEM_AUDIO_GAMESOUND_H */
