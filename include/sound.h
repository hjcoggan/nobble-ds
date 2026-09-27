// Music and sound effects on the DS's 16 stereo sound channels.
#ifndef SOUND_H
#define SOUND_H

void sound_init(void);
void sound_update(void);     // call once per frame

enum { SONG_FACTORY, SONG_SHOP, SONG_OVERTIME, SONG_ASSEMBLY, SONG_MELTDOWN, SONG_BOSS, NUM_SONGS };

void music_play(int song);   // start a song from the top
void music_stop(void);       // silence, keeping the song position
void music_resume(void);

enum { SFX_POP, SFX_POP_GONE };

void sfx_peg(int hits, int kind, int x);   // hits so far this launch; x pans it
void sfx_spring(void);
void sfx_wall(int x);
void sfx_launch(void);
void sfx_item(void);         // an item fired
void sfx_move(void);
void sfx_buy(void);
void sfx_deny(void);
void sfx_clear(void);
void sfx_over(void);
void sfx_laser(void);        // boss laser fires
void sfx_armor(void);        // an armoured peg cracks

#endif
