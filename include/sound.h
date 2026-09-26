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

enum { SFX_PEG_PLUS, SFX_PEG_MULT, SFX_PEG_COIN };

void sfx_peg(int hits, int kind);   // hits: pegs hit so far this drop
void sfx_bumper(void);
void sfx_land(int bucket_mult);
void sfx_move(void);
void sfx_buy(void);
void sfx_deny(void);
void sfx_clear(void);
void sfx_over(void);

#endif
