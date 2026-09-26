// Music and sound effects on the GBA's PSG (Game Boy compatible) channels.
//   ch1 square  - sound effects
//   ch2 square  - lead melody
//   ch3 wave    - bass
//   ch4 noise   - drums
#ifndef SOUND_H
#define SOUND_H

void sound_init(void);
void sound_update(void);     // call once per frame

enum { SONG_FACTORY, SONG_SHOP, SONG_OVERTIME, NUM_SONGS };

void music_play(int song);   // start a song from the top
void music_stop(void);       // silence, keeping the song position
void music_resume(void);

enum { SFX_POP, SFX_POP_GONE };

void sfx_peg(int hits, int kind);   // hits: pegs hit so far this launch
void sfx_spring(void);
void sfx_wall(void);
void sfx_launch(void);
void sfx_item(void);         // an item fired
void sfx_move(void);
void sfx_buy(void);
void sfx_deny(void);
void sfx_clear(void);
void sfx_over(void);

#endif
