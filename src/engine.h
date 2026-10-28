/* Columbia Street audio engine: four break channels that play slices of
 * sampled drum breaks and one bass channel, each sequenced by three
 * polymetric Euclidean cells (HIT, ROLL, ALT). Per break channel: pitch,
 * filter, bit depth (1-24), sample-rate reduction and a compressor. On the
 * bus: reverb, a glue compressor, drive, the L2 filter sweep and punch-in
 * repeat / reverse / tape-stop effects. */
#pragma once
#include <stddef.h>
#include <stdint.h>

#define CS_VERSION "0.3.0" /* the release tag is v<this>: the app offers any newer release (CI checks the tag) */

#define NV 5    /* channels */
#define NBRK 4  /* break channels 0..3; channel 4 is the bass */
#define V_BASS 4
#define NROW 3
#define NP 13   /* params per channel */
#define MAX_STEPS 32
#define NSLOT 8 /* pattern memory A..H */
#define NFXSLOT 4

enum { ROW_HIT, ROW_ROLL, ROW_ALT };
enum { TRIG_HIT = 1, TRIG_ROLL = 2, TRIG_ALT = 4 };

/* break channel params */
enum {
    B_BANK, B_SLICE, B_MODE, B_RANGE, B_PITCH, B_FIT, B_LEN,
    B_ROLL, B_ALT, B_TONE, B_BITS, B_RATE, B_COMP
};
/* how a hit picks its slice */
enum { MODE_FIX, MODE_WALK, MODE_GRID, MODE_RAND, NMODE };
/* what the ALT cell does to a hit */
enum { ALT_REV, ALT_OCTUP, ALT_OCTDN, ALT_NEXT, ALT_RAND, ALT_STOP, NALT };

/* bass params */
enum {
    S_WAVE, S_ROOT, S_OCT, S_RIFF, S_LEN, S_GLIDE, S_CUT, S_RES,
    S_WIDTH, S_DRIVE, S_ROLL, S_ALT
};
#define NBASS_P 12
enum { WAVE_SUB, WAVE_REESE, WAVE_808, WAVE_GROWL, NWAVE_SYNTH }; /* then the bass/ samples */
enum { BALT_OCTUP, BALT_FIFTH, BALT_OCTDN, BALT_SLIDE, BALT_REST, NBALT };

enum { SYNC_OFF, SYNC_AUTO, SYNC_LEAD, SYNC_FOLLOW, NSYNCMODE };
/* the output, as on HI5: stereo, or the sound in mono on one side and clock
 * pulses on the other (clk_side 0 = clock on the right), clk_ppqn pulses per
 * quarter note (an index into 1, 2, 4, 8, 16, 24, 32), clk_width ms wide.
 * In mono + clock every channel plays from the centre (the pans are kept). */
enum { OUT_STEREO, OUT_MONO_CLOCK };
#define NPPQN 7
/* punch-in effects on R2 + up / right / left / down */
enum { FX_REPEAT8, FX_REPEAT16, FX_REVERSE, FX_TAPESTOP };

typedef struct {
    uint8_t on, steps, hits, rot;
} Cell;

typedef struct {
    const char *name;
    int min, max, def;
} ParamDef;

/* one pattern: what the slots A..H store and recall */
typedef struct {
    Cell cell[NV][NROW];
    int param[NV][NP];
    uint8_t mute[NV];
} Pattern;

/* Everything the user edits; saved to disk as-is. */
typedef struct {
    uint32_t magic, size;
    float bpm;
    Pattern pat;
    int vol[NV], pan[NV], send[NV];
    uint8_t solo[NV];
    int master, glue, master_drive, verb_time, verb_tone, sweep_rate;
    int sync_mode;
    Pattern slot[NSLOT];
    uint8_t slot_used[NSLOT];
    /* v0.2: new fields go at the end, so an older save still loads */
    int out_mode, clk_side, clk_ppqn, clk_width;
} Song;

/* Shared between UI and audio thread. The UI writes these only while
 * holding the audio lock; the audio thread publishes the rest. */
typedef struct {
    int playing;
    int filter_held;
    int fx;                /* punch-in effect held, or -1 */
    int queued_slot;       /* pattern slot loading at the next bar, or -1 */
    int cur_slot;          /* slot last loaded or saved, or -1 */
    volatile int step;
    volatile uint32_t trig_count[NV];
    volatile int slice[NBRK];   /* slice each break channel last played */
    volatile float play_pos[NBRK]; /* playhead, 0..1 of the bank, -1 when silent */
    volatile int note;          /* MIDI note the bass last played */
    volatile float meter[NV + 1];
    volatile float gr[NV + 1];  /* compressor gain reduction in dB (bass slot unused, last = glue) */
    volatile float cutoff_norm;
} Live;

extern Song song;
extern Live live;
extern const char *const voice_names[NV];

const ParamDef *param_def(int v, int p);
int nparam(int v);
int param_min(int v, int p);
int param_max(int v, int p);
void param_format(int v, int p, char *buf, int n);
void param_set(int v, int p, int x); /* clamps */
const char *riff_name(int i);
int riff_len(int i);
int riff_note(int i, int k);
const char *sweep_rate_name(int i);
int clk_ppqn_value(int i); /* clk_ppqn index -> pulses per quarter note */
int nsweep(void);

void engine_init(int sample_rate);
void engine_render(int16_t *out, int frames);
void engine_audition(int v);
void engine_set_playing(int on);
/* transport position in 16th steps with fraction, a hard jump, and a
 * playback speed trim for the Wi-Fi sync (1.0 = none); all under the lock */
double engine_position(void);
void engine_set_position(double steps);
void engine_set_rate(double r);
double engine_steps_per_frame(void); /* how far the position moves each frame */

int euclid_hit(const Cell *c, int step);
void euclid_permute(Cell *c);
void song_default(void);
void pattern_generate(Pattern *p, unsigned seed); /* a fresh jungle pattern */
void song_clamp(void); /* after the banks change, keep params in range */
int song_save(const char *path);
int song_load(const char *path);

void slot_save(int i);
void slot_load(int i); /* now when stopped, at the next bar when playing */
void slot_clear(int i);
