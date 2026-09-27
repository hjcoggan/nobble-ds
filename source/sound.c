// Music and sound effects on the DS sound hardware (16 channels, stereo).
//   ch 0      bass: a looped triangle-wave sample
//   ch 8      lead melody, a little to the left
//   ch 9      echo of the lead, three steps late and to the right
//   ch 10     kick and toms: a square with a falling pitch
//   ch 11-12  sound effects (peg pops alternate so they ring into each other)
//   ch 14     drums: snare and hi-hat noise
//   ch 15     noisy sound effects
#include <nds.h>
#include "sound.h"

#define CH_BASS 0
#define CH_LEAD 8
#define CH_ECHO 9
#define CH_KICK 10
#define CH_SFX 11
#define CH_SFX2 12
#define CH_DRUM 14
#define CH_NOISE 15

#define ECHO_STEPS 3

// ---------------------------------------------------------------- the song
// Each song is 8 bars of 16 steps (16th notes).
// Tokens: note like A4 or C#5, "--" holds the previous note, ".." is silence.
// Drums: K kick, S snare, H hi-hat, T low tom, - nothing.
#define STEPS_PER_BAR 16
#define BARS 8
#define SONG_STEPS (BARS * STEPS_PER_BAR)

typedef struct {
    const char *lead[BARS];
    const char *bass[BARS];
    const char *drums[BARS];
    uint8_t step_frames;      // tempo: frames per 16th note
    uint8_t duty;             // lead square duty: 0 12.5%, 1 25%, 2 50%
    uint8_t lead_vol;         // lead envelope start volume (0-15)
    uint8_t lead_decay;       // lead envelope step time (bigger = longer notes)
    uint8_t bass_vol;         // wave channel volume: 1 100%, 2 50%, 3 25%
} Song;

static const Song songs[NUM_SONGS] = {
    {   // 0: Factory Funk - E minor, syncopated, ~128 BPM
        .lead = {
            "E5 .. E5 G5 .. A5 -- .. B5 -- A5 G5 E5 -- .. ..",
            "D5 .. D5 E5 .. G5 -- .. A5 -- G5 E5 D5 -- .. ..",
            "E5 .. E5 G5 .. A5 -- .. B5 -- D6 -- B5 A5 G5 --",
            "A5 -- G5 -- E5 -- D5 -- E5 -- -- -- .. .. .. ..",
            "B5 -- B5 .. A5 G5 A5 -- G5 -- E5 -- D5 -- E5 --",
            "G5 -- G5 .. E5 D5 E5 -- D5 -- B4 -- A4 -- B4 --",
            "E5 .. G5 .. A5 .. B5 .. D6 -- B5 -- A5 -- G5 --",
            "A5 -- B5 -- -- -- .. .. E5 -- -- -- .. .. .. ..",
        },
        .bass = {
            "E2 .. E3 .. .. E2 E3 .. E2 .. E3 .. D3 .. B2 ..",
            "D2 .. D3 .. .. D2 D3 .. D2 .. D3 .. C3 .. A2 ..",
            "E2 .. E3 .. .. E2 E3 .. E2 .. E3 .. D3 .. B2 ..",
            "C2 .. C3 .. .. C2 C3 .. D2 .. D3 .. B2 .. D3 ..",
            "E2 .. E3 .. .. E2 E3 .. E2 .. E3 .. D3 .. B2 ..",
            "C2 .. C3 .. .. C2 C3 .. D2 .. D3 .. D2 .. D3 ..",
            "E2 .. E3 .. .. E2 E3 .. E2 .. E3 .. D3 .. B2 ..",
            "C2 .. C3 .. D2 .. D3 .. E2 -- -- -- .. .. .. ..",
        },
        .drums = {
            "K-H-S-HKH-KHS-H-", "K-H-S-HKH-KHS-H-", "K-H-S-HKH-KHS-H-", "K-H-S-HKH-KHS-SS",
            "K-H-S-HKH-KHS-H-", "K-H-S-HKH-KHS-H-", "K-H-S-HKH-KHS-H-", "K-H-S-HKS-SST-TT",
        },
        .step_frames = 7, .duty = 2, .lead_vol = 10, .lead_decay = 2, .bass_vol = 1,
    },
    {   // 1: Shop Break - C major lounge, ~100 BPM
        .lead = {
            "E5 -- -- G5 -- -- C6 -- B5 -- A5 -- G5 -- -- --",
            "F5 -- -- A5 -- -- D6 -- C6 -- B5 -- A5 -- -- --",
            "E5 -- -- G5 -- -- C6 -- D6 -- E6 -- C6 -- -- --",
            "B5 -- -- -- G5 -- -- -- D5 -- -- -- -- -- .. ..",
            "E5 -- -- G5 -- -- C6 -- B5 -- A5 -- G5 -- -- --",
            "F5 -- -- A5 -- -- D6 -- C6 -- B5 -- A5 -- -- --",
            "A5 -- -- C6 -- -- E6 -- D6 -- C6 -- A5 -- -- --",
            "G5 -- -- -- B5 -- -- -- C6 -- -- -- -- -- .. ..",
        },
        .bass = {
            "C3 -- -- -- G2 -- -- -- E3 -- -- -- G2 -- -- --",
            "D3 -- -- -- A2 -- -- -- F3 -- -- -- A2 -- -- --",
            "C3 -- -- -- G2 -- -- -- A2 -- -- -- E3 -- -- --",
            "G2 -- -- -- D3 -- -- -- B2 -- -- -- D3 -- -- --",
            "C3 -- -- -- G2 -- -- -- E3 -- -- -- G2 -- -- --",
            "D3 -- -- -- A2 -- -- -- F3 -- -- -- A2 -- -- --",
            "A2 -- -- -- E3 -- -- -- C3 -- -- -- E3 -- -- --",
            "G2 -- -- -- D3 -- -- -- C3 -- -- -- -- -- -- --",
        },
        .drums = {
            "K---H-H-S---H-H-", "K---H-H-S---H-H-", "K---H-H-S---H-H-", "K---H-H-S---H-H-",
            "K---H-H-S---H-H-", "K---H-H-S---H-H-", "K---H-H-S---H-H-", "K---H-H-S---T-T-",
        },
        .step_frames = 9, .duty = 0, .lead_vol = 9, .lead_decay = 5, .bass_vol = 2,
    },
    {   // 2: Overtime - A minor, fast, for the later rounds, ~150 BPM
        .lead = {
            "A5 A5 .. A5 G5 -- E5 -- A5 A5 .. A5 C6 -- B5 --",
            "G5 G5 .. G5 E5 -- D5 -- G5 G5 .. G5 B5 -- A5 --",
            "F5 F5 .. F5 E5 -- C5 -- F5 F5 .. F5 A5 -- G5 --",
            "E5 -- G#5 -- B5 -- E6 -- D6 -- B5 -- G#5 -- E5 --",
            "C6 -- B5 -- A5 -- E5 -- C6 -- B5 -- A5 -- E5 --",
            "B5 -- A5 -- G5 -- D5 -- B5 -- A5 -- G5 -- D5 --",
            "A5 -- G5 -- F5 -- C5 -- A5 -- G5 -- F5 -- A5 --",
            "G#5 -- -- -- B5 -- -- -- E6 -- -- -- E5 -- -- --",
        },
        .bass = {
            "A1 .. A2 .. A1 .. A2 .. A1 .. A2 .. A1 .. A2 ..",
            "G1 .. G2 .. G1 .. G2 .. G1 .. G2 .. G1 .. G2 ..",
            "F1 .. F2 .. F1 .. F2 .. F1 .. F2 .. F1 .. F2 ..",
            "E1 .. E2 .. E1 .. E2 .. E1 .. E2 .. E1 .. E2 ..",
            "A1 .. A2 .. A1 .. A2 .. A1 .. A2 .. A1 .. A2 ..",
            "G1 .. G2 .. G1 .. G2 .. G1 .. G2 .. G1 .. G2 ..",
            "F1 .. F2 .. F1 .. F2 .. F1 .. F2 .. F1 .. F2 ..",
            "E1 .. E2 .. E1 .. E2 .. E1 .. E2 .. E1 .. E2 ..",
        },
        .drums = {
            "K-HHS-HHK-HHS-HK", "K-HHS-HHK-HHS-HK", "K-HHS-HHK-HHS-HK", "K-HHS-HHK-HHSSSS",
            "K-HHS-HHK-HHS-HK", "K-HHS-HHK-HHS-HK", "K-HHS-HHK-HHS-HK", "K-HHS-HHKSSSTTTT",
        },
        .step_frames = 6, .duty = 1, .lead_vol = 9, .lead_decay = 2, .bass_vol = 1,
    },
    {   // 3: Assembly Line - D dorian bounce, ~120 BPM
        .lead = {
            "D5 .. F5 .. A5 -- G5 .. F5 .. E5 -- D5 -- .. ..",
            "C5 .. E5 .. G5 -- F5 .. E5 .. D5 -- C5 -- .. ..",
            "D5 .. F5 .. A5 -- C6 .. B5 .. A5 -- G5 -- F5 --",
            "E5 -- -- -- G5 -- -- -- A5 -- -- -- .. .. .. ..",
            "A5 .. A5 B5 C6 -- A5 .. G5 .. G5 A5 B5 -- G5 ..",
            "F5 .. F5 G5 A5 -- F5 .. E5 .. E5 F5 G5 -- E5 ..",
            "D5 .. F5 .. A5 .. D6 -- C6 -- A5 -- G5 -- E5 --",
            "D5 -- -- -- -- -- .. .. A4 .. D5 .. .. .. .. ..",
        },
        .bass = {
            "D2 .. D3 .. A2 .. D3 .. D2 .. D3 .. A2 .. C3 ..",
            "C2 .. C3 .. G2 .. C3 .. C2 .. C3 .. G2 .. B2 ..",
            "D2 .. D3 .. A2 .. D3 .. F2 .. F3 .. C3 .. F3 ..",
            "G2 .. G3 .. D3 .. G3 .. A2 .. A3 .. E3 .. A2 ..",
            "A2 .. A3 .. E3 .. A3 .. G2 .. G3 .. D3 .. G3 ..",
            "F2 .. F3 .. C3 .. F3 .. E2 .. E3 .. B2 .. E3 ..",
            "D2 .. D3 .. A2 .. D3 .. C2 .. C3 .. G2 .. C3 ..",
            "D2 -- -- -- .. .. .. .. A2 .. A2 .. D2 .. .. ..",
        },
        .drums = {
            "K--HS-H-K-KHS-H-", "K--HS-H-K-KHS-H-", "K--HS-H-K-KHS-H-", "K--HS-H-K-KHS-TT",
            "K--HS-H-K-KHS-H-", "K--HS-H-K-KHS-H-", "K--HS-H-K-KHS-H-", "K---S---K-K-SSTT",
        },
        .step_frames = 7, .duty = 1, .lead_vol = 10, .lead_decay = 3, .bass_vol = 1,
    },
    {   // 4: Meltdown - G minor, driving, ~180 BPM
        .lead = {
            "G5 .. G5 A#5 D6 .. C6 A#5 A5 .. G5 .. D5 .. G5 ..",
            "F5 .. F5 A5 C6 .. A#5 A5 G5 .. F5 .. C5 .. F5 ..",
            "D#5 .. D#5 G5 A#5 .. A5 G5 F5 .. D#5 .. D5 .. C5 ..",
            "D5 -- -- -- F#5 -- -- -- A5 -- -- -- D6 -- -- --",
            "G6 -- F6 -- D6 -- A#5 -- G5 -- A#5 -- D6 -- F6 --",
            "D#6 -- D6 -- C6 -- A5 -- F5 -- A5 -- C6 -- D#6 --",
            "D6 -- C6 -- A#5 -- A5 -- G5 -- F5 -- D#5 -- D5 --",
            "D5 .. F#5 .. A5 .. D6 .. G5 -- -- -- .. .. .. ..",
        },
        .bass = {
            "G1 G2 G1 G2 G1 G2 G1 G2 G1 G2 G1 G2 G1 G2 G1 G2",
            "F1 F2 F1 F2 F1 F2 F1 F2 F1 F2 F1 F2 F1 F2 F1 F2",
            "D#1 D#2 D#1 D#2 D#1 D#2 D#1 D#2 D#1 D#2 D#1 D#2 D#1 D#2 D#1 D#2",
            "D1 D2 D1 D2 D1 D2 D1 D2 D1 D2 D1 D2 D1 D2 D1 D2",
            "G1 G2 G1 G2 G1 G2 G1 G2 G1 G2 G1 G2 G1 G2 G1 G2",
            "C2 C3 C2 C3 C2 C3 C2 C3 F1 F2 F1 F2 F1 F2 F1 F2",
            "D#1 D#2 D#1 D#2 D#1 D#2 D#1 D#2 D1 D2 D1 D2 D1 D2 D1 D2",
            "D1 D2 D1 D2 D1 D2 D1 D2 G1 -- -- -- .. .. .. ..",
        },
        .drums = {
            "KHHHSHHHKHKHSHHH", "KHHHSHHHKHKHSHHH", "KHHHSHHHKHKHSHHH", "KHHHSHHHKKSSTTTT",
            "KHHHSHHHKHKHSHHH", "KHHHSHHHKHKHSHHH", "KHHHSHHHKHKHSHHH", "KHHHSHHHKKSSTTTT",
        },
        .step_frames = 5, .duty = 2, .lead_vol = 9, .lead_decay = 1, .bass_vol = 1,
    },
    {   // 5: Boss - C minor, menacing, ~150 BPM
        .lead = {
            "C5 .. C5 .. D#5 .. C5 .. F#5 -- F5 -- D#5 -- C5 --",
            "C5 .. C5 .. D#5 .. C5 .. G5 -- F#5 -- F5 -- D#5 --",
            "G#5 -- G5 -- F5 -- D#5 -- F5 -- D#5 -- D5 -- B4 --",
            "C5 -- -- -- .. .. C6 .. B5 .. A#5 .. A5 .. G#5 ..",
            "C6 .. C6 .. B5 .. C6 .. D#6 -- D6 -- C6 -- G5 --",
            "G#5 .. G#5 .. G5 .. G#5 .. C6 -- B5 -- G#5 -- G5 --",
            "F5 -- G5 -- G#5 -- B5 -- C6 -- D6 -- D#6 -- F#6 --",
            "G6 -- -- -- F#6 -- -- -- G6 -- -- -- .. .. .. ..",
        },
        .bass = {
            "C2 C2 .. C2 C2 .. C2 .. C2 C2 .. C2 F#2 .. G2 ..",
            "C2 C2 .. C2 C2 .. C2 .. C2 C2 .. C2 F#2 .. G2 ..",
            "G#1 G#1 .. G#1 G#1 .. G#1 .. G1 G1 .. G1 G1 .. B1 ..",
            "C2 C2 .. C2 C2 .. C2 .. C2 C2 .. C2 G1 .. G#1 ..",
            "C2 C2 .. C2 C2 .. C2 .. C2 C2 .. C2 F#2 .. G2 ..",
            "G#1 G#1 .. G#1 G#1 .. G#1 .. F1 F1 .. F1 F1 .. G1 ..",
            "F1 .. F2 .. G1 .. G2 .. G#1 .. G#2 .. B1 .. B2 ..",
            "C2 -- -- -- B1 -- -- -- C2 -- -- -- G1 .. G1 ..",
        },
        .drums = {
            "K--K--S-K--K--S-", "K--K--S-K--K--S-", "K--K--S-K--K--S-", "K--K--S-K-K-SSSS",
            "K--K--S-K--K--S-", "K--K--S-K--K--S-", "K--K--S-K--K--S-", "K--K--S-TTTTSSSS",
        },
        .step_frames = 6, .duty = 0, .lead_vol = 11, .lead_decay = 2, .bass_vol = 1,
    },
};

#define HOLD 1
#define REST 0

static const Song *song = &songs[0];
static uint8_t lead[SONG_STEPS], bass[SONG_STEPS];
static char drums[SONG_STEPS];

// ---------------------------------------------------------------- pitch

static uint32_t hz_x16[128];     // frequency of each MIDI note, x16

static void build_rates(void)
{
    // 16x the frequency of C8..B8 (MIDI 108-119); lower octaves shift right
    static const uint32_t top[12] = { 66976, 70959, 75178, 79648, 84385, 89402,
                                      94719, 100351, 106318, 112640, 119338, 126434 };
    for (int n = 12; n < 120; n++) hz_x16[n] = top[n % 12] >> (9 - n / 12);
}

static unsigned psg_timer(int note) { return soundTimerFromHz(hz_x16[note] / 2); }       // 8 steps a cycle
static unsigned pcm_timer(int note) { return soundTimerFromHz(hz_x16[note] * 2); }       // 32 samples a cycle

static void parse_notes(const char *const *src, uint8_t *out)
{
    static const int8_t semis[7] = { 9, 11, 0, 2, 4, 5, 7 };   // A..G
    int k = 0;
    for (int bar = 0; bar < BARS; bar++) {
        const char *s = src[bar];
        for (int step = 0; step < STEPS_PER_BAR; step++) {
            while (*s == ' ') s++;
            if (s[0] == '-') {
                out[k] = HOLD;
                s += 2;
            } else if (s[0] == '.') {
                out[k] = REST;
                s += 2;
            } else {
                int n = semis[s[0] - 'A'];
                s++;
                if (*s == '#') {
                    n++;
                    s++;
                }
                out[k] = (uint8_t)((*s - '0' + 1) * 12 + n);
                s++;
            }
            k++;
        }
    }
}

// ---------------------------------------------------------------- voices
// Every channel we use has a simple volume envelope and pitch slide,
// updated once a frame.

typedef struct {
    int vol, decay;          // volume (0-127) x 16, and how much it drops a frame
    int timer, slide;        // sound timer and how much it changes a frame
    int on;
} Voice;

static Voice voice[16];
static const uint16_t tri_wave[16] __attribute__((aligned(4))) = {
    // 32 signed 8-bit samples, two per halfword
    0x9080, 0xB0A0, 0xD0C0, 0xF0E0, 0x1000, 0x3020, 0x5040, 0x7060,
    0x607F, 0x4050, 0x2030, 0x0010, 0xE0F0, 0xC0D0, 0xA0B0, 0x8090,
};

static void voice_psg(int ch, int note, int vol, int decay, int pan, SoundDuty duty)
{
    Voice *v = &voice[ch];
    v->vol = vol * 16;
    v->decay = decay;
    v->timer = psg_timer(note);
    v->slide = 0;
    v->on = 1;
    soundPreparePsg(ch | SOUND_START, v->vol, pan, v->timer, duty);
}

static void voice_noise(int ch, int hz, int vol, int decay, int pan)
{
    Voice *v = &voice[ch];
    v->vol = vol * 16;
    v->decay = decay;
    v->timer = soundTimerFromHz(hz);
    v->slide = 0;
    v->on = 1;
    soundPreparePsg(ch | SOUND_START, v->vol, pan, v->timer, SoundDuty_50);
}

static void voice_off(int ch)
{
    voice[ch].on = 0;
    soundStop(1u << ch);
}

static void voices_update(void)
{
    for (int ch = 0; ch < 16; ch++) {
        Voice *v = &voice[ch];
        if (!v->on) continue;
        if (v->decay) {
            v->vol -= v->decay;
            if (v->vol <= 0) {
                voice_off(ch);
                continue;
            }
            soundChSetVolume(ch, v->vol);
        }
        if (v->slide) {
            v->timer += v->slide;
            if (v->timer < 64) v->timer = 64;
            soundChSetTimer(ch, v->timer);
        }
    }
}

// ---------------------------------------------------------------- music

static int music_on, step, step_timer;
static int lead_note;

static void play_drum(char d)
{
    switch (d) {
    case 'K':
        voice_psg(CH_KICK, 43, 120, 90, 64, SoundDuty_50);   // thump: G2 sliding down
        voice[CH_KICK].slide = 700;
        break;
    case 'T':
        voice_psg(CH_KICK, 50, 100, 60, 56, SoundDuty_50);
        voice[CH_KICK].slide = 300;
        break;
    case 'S':
        voice_noise(CH_DRUM, 20000, 90, 110, 70);
        break;
    case 'H':
        voice_noise(CH_DRUM, 120000, 40, 160, 84);
        break;
    }
}

static void music_step(void)
{
    uint8_t n = lead[step];
    int decay = 16 * 12 / (song->lead_decay + 1);
    if (n == REST) {
        voice_off(CH_LEAD);
    } else if (n != HOLD) {
        voice_psg(CH_LEAD, n, song->lead_vol * 7, decay, 44, (SoundDuty)(song->duty + 1));
        lead_note = n;
    }
    // the echo replays the lead a few steps later, quieter and to the right
    uint8_t e = lead[(step + SONG_STEPS - ECHO_STEPS) % SONG_STEPS];
    if (e > HOLD) voice_psg(CH_ECHO, e, song->lead_vol * 3, decay, 92, (SoundDuty)(song->duty + 1));

    n = bass[step];
    if (n == REST) {
        voice_off(CH_BASS);
    } else if (n != HOLD) {
        Voice *v = &voice[CH_BASS];
        v->vol = (song->bass_vol == 1 ? 110 : 80) * 16;
        v->decay = 12;
        v->slide = 0;
        v->on = 1;
        v->timer = pcm_timer(n);
        soundPreparePcm(CH_BASS | SOUND_START, v->vol, 64, v->timer, SoundMode_Repeat, SoundFmt_Pcm8,
                        tri_wave, 0, sizeof(tri_wave) / 4);
    }
    play_drum(drums[step]);
}

void sound_init(void)
{
    soundEnable();
    soundSetMixerVolume(127);
    build_rates();
}

void music_play(int index)
{
    song = &songs[index % NUM_SONGS];
    parse_notes(song->lead, lead);
    parse_notes(song->bass, bass);
    for (int bar = 0; bar < BARS; bar++)
        for (int i = 0; i < STEPS_PER_BAR; i++)
            drums[bar * STEPS_PER_BAR + i] = song->drums[bar][i];
    music_on = 1;
    step = 0;
    step_timer = 0;
}

void music_stop(void)
{
    music_on = 0;
    voice_off(CH_LEAD);
    voice_off(CH_ECHO);
    voice_off(CH_BASS);
    voice_off(CH_KICK);
    voice_off(CH_DRUM);
}

void music_resume(void)
{
    music_on = 1;
    step_timer = 0;
}

// ---------------------------------------------------------------- sound effects

// sequencer for ch 11: pairs of (midi note, frames), 0-terminated
static const uint8_t *sfx_seq;
static int sfx_timer;
static int pop_ch;

static void sfx_start(const uint8_t *seq)
{
    sfx_seq = seq;
    sfx_timer = 0;
}

void sound_update(void)
{
    if (music_on && --step_timer <= 0) {
        step_timer = song->step_frames;
        music_step();
        step = (step + 1) % SONG_STEPS;
    }
    if (sfx_seq && --sfx_timer <= 0) {
        if (sfx_seq[0] == 0) {
            sfx_seq = 0;
        } else {
            voice_psg(CH_SFX, sfx_seq[0], 100, 40, 64, SoundDuty_50);
            sfx_timer = sfx_seq[1];
            sfx_seq += 2;
        }
    }
    voices_update();
}

// Each peg hit in a launch plays the next note up a pentatonic scale, panned
// towards where the peg is.
void sfx_peg(int hits, int kind, int x)
{
    static const uint8_t scale[] = { 72, 74, 76, 79, 81, 84, 86, 88, 91, 93, 96, 98, 100, 103 };
    int n = hits - 1;
    if (n < 0) n = 0;
    if (n >= (int)sizeof(scale)) n = sizeof(scale) - 1;
    int pan = 16 + x * 96 / 256;
    pop_ch = pop_ch == CH_SFX ? CH_SFX2 : CH_SFX;
    sfx_seq = 0;
    voice_psg(pop_ch, scale[n], 100, 90, pan, SoundDuty_50);
    if (kind == SFX_POP_GONE) {
        // a peg that vanishes gets a sparkle an octave up
        static uint8_t seq[] = { 0, 3, 0, 4, 0 };
        seq[0] = scale[n];
        seq[2] = scale[n] + 12 > 107 ? 107 : scale[n] + 12;
        sfx_start(seq);
    }
}

void sfx_spring(void)
{
    sfx_seq = 0;
    voice_psg(CH_SFX, 60, 110, 50, 64, SoundDuty_50);
    voice[CH_SFX].slide = -90;          // boing
}

void sfx_wall(int x)
{
    voice_noise(CH_NOISE, 9000, 60, 150, x < 128 ? 24 : 104);
}

void sfx_launch(void)
{
    sfx_seq = 0;
    voice_psg(CH_SFX, 84, 90, 60, 64, SoundDuty_25);
    voice[CH_SFX].slide = 25;           // falling pew
}

void sfx_item(void)
{
    static const uint8_t seq[] = { 91, 2, 96, 2, 103, 4, 0 };
    sfx_start(seq);
}

void sfx_move(void)
{
    static const uint8_t seq[] = { 79, 2, 84, 3, 0 };
    sfx_start(seq);
}

void sfx_buy(void)
{
    static const uint8_t seq[] = { 84, 3, 88, 3, 91, 3, 96, 8, 0 };
    sfx_start(seq);
}

void sfx_deny(void)
{
    static const uint8_t seq[] = { 55, 6, 52, 10, 0 };
    sfx_start(seq);
}

void sfx_clear(void)
{
    static const uint8_t seq[] = { 72, 6, 76, 6, 79, 6, 84, 6, 88, 20, 0 };
    music_stop();
    sfx_start(seq);
}

void sfx_over(void)
{
    static const uint8_t seq[] = { 67, 12, 64, 12, 60, 12, 55, 30, 0 };
    music_stop();
    sfx_start(seq);
}

void sfx_laser(void)
{
    sfx_seq = 0;
    voice_psg(CH_SFX, 103, 120, 40, 64, SoundDuty_50);
    voice[CH_SFX].slide = 40;
    voice_noise(CH_NOISE, 16000, 100, 50, 64);
}

void sfx_armor(void)
{
    static const uint8_t seq[] = { 98, 2, 86, 2, 98, 3, 0 };   // metallic clank
    sfx_start(seq);
    voice_noise(CH_NOISE, 60000, 70, 200, 64);
}
