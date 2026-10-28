#include "engine.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "aclock.h"
#include "breaks.h"

#define SONG_MAGIC 0x31545343u /* "CST1" */
/* a v0.1 save: the same song without the output settings at the end */
#define SONG_SIZE_V1 offsetof(Song, out_mode)

Song song;
Live live = {.fx = -1, .queued_slot = -1, .cur_slot = -1};

const char *const voice_names[NV] = {"BRK1", "BRK2", "BRK3", "BRK4", "BASS"};

static float sr = 48000.f;

static int clampi(int x, int lo, int hi) { return x < lo ? lo : x > hi ? hi : x; }

/* ---------------------------------------------------------------- params */

static const ParamDef brk_defs[NP] = {
    {"BREAK", 0, 0, 0},   {"SLICE", 0, 0, 0},  {"MODE", 0, NMODE - 1, MODE_FIX},
    {"RANGE", 1, 16, 4},  {"PITCH", -24, 24, 0}, {"FIT", 0, 1, 1},
    {"LENGTH", 5, 100, 100}, {"ROLL", 0, 4, 1}, {"ALT", 0, NALT - 1, ALT_REV},
    {"FILTER", -50, 50, 0}, {"BITS", 1, 24, 24}, {"RATE", 0, 40, 0},
    {"COMP", 0, 100, 0},
};
static const ParamDef bass_defs[NP] = {
    {"WAVE", 0, 0, WAVE_SUB}, {"ROOT", 0, 11, 5},   {"OCTAVE", 0, 3, 1},
    {"RIFF", 0, 0, 0},        {"LENGTH", 1, 17, 6}, {"GLIDE", 0, 100, 20},
    {"CUTOFF", 0, 100, 60},   {"RESO", 0, 100, 20}, {"WIDTH", 0, 100, 40},
    {"DRIVE", 0, 100, 20},    {"ROLL", 0, 4, 0},    {"ALT", 0, NBALT - 1, BALT_OCTUP},
    {"", 0, 0, 0},
};

static const char *const mode_names[NMODE] = {"FIX", "WALK", "GRID", "RAND"};
static const char *const alt_names[NALT] = {"REV", "OCT+", "OCT-", "NEXT", "RAND", "STOP"};
static const char *const balt_names[NBALT] = {"OCT+", "FIFTH", "OCT-", "SLIDE", "REST"};
static const char *const wave_names[NWAVE_SYNTH] = {"SUB", "REESE", "808", "GROWL"};
static const char *const note_names[12] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static const int roll_counts[5] = {2, 3, 4, 6, 8};

/* bass riffs in semitones from the root; a hit plays the next note, and the
 * riff starts again every two bars */
typedef struct {
    const char *name;
    int n;
    int8_t note[8];
} Riff;
static const Riff riffs[] = {
    {"ROOT", 1, {0}},
    {"OCTAVE", 2, {0, 12}},
    {"DUB", 4, {0, 0, 7, 5}},
    {"MINOR", 4, {0, 3, 7, 3}},
    {"DROP", 4, {0, -2, -5, -7}},
    {"RISE", 4, {0, 3, 5, 7}},
    {"SIREN", 4, {0, 10, 7, 3}},
    {"DREAD", 4, {0, 1, 0, -2}},
    {"FIFTHS", 4, {0, 7, 0, -5}},
    {"ROLLER", 8, {0, 0, 12, 0, 10, 0, 7, 5}},
};
#define NRIFF ((int)(sizeof riffs / sizeof riffs[0]))
const char *riff_name(int i) { return riffs[clampi(i, 0, NRIFF - 1)].name; }
int riff_len(int i) { return riffs[clampi(i, 0, NRIFF - 1)].n; }
int riff_note(int i, int k) { const Riff *r = &riffs[clampi(i, 0, NRIFF - 1)]; return r->note[k % r->n]; }

/* the L2 sweep: how long the filter takes to close */
static const char *const sweep_names[] = {"1/4", "1/2", "1BAR", "2BAR"};
static const float sweep_steps[] = {4.f, 8.f, 16.f, 32.f};
const char *sweep_rate_name(int i) { return sweep_names[clampi(i, 0, 3)]; }
int nsweep(void) { return 4; }

const ParamDef *param_def(int v, int p) { return v == V_BASS ? &bass_defs[p] : &brk_defs[p]; }
int nparam(int v) { return v == V_BASS ? NBASS_P : NP; }
int param_min(int v, int p) { return param_def(v, p)->min; }

static const Bank *chan_bank(int v)
{
    int b = song.pat.param[v][B_BANK];
    return nbanks ? &banks[clampi(b, 0, nbanks - 1)] : NULL;
}

int param_max(int v, int p)
{
    if (v == V_BASS) {
        if (p == S_WAVE)
            return NWAVE_SYNTH + nbass - 1;
        if (p == S_RIFF)
            return NRIFF - 1;
        return bass_defs[p].max;
    }
    if (p == B_BANK)
        return nbanks > 0 ? nbanks - 1 : 0;
    if (p == B_SLICE) {
        const Bank *b = chan_bank(v);
        return b && b->nslices > 0 ? b->nslices - 1 : 0;
    }
    return brk_defs[p].max;
}

void param_set(int v, int p, int x)
{
    song.pat.param[v][p] = clampi(x, param_min(v, p), param_max(v, p));
    if (v < NBRK && p == B_BANK)
        song.pat.param[v][B_SLICE] = clampi(song.pat.param[v][B_SLICE], 0, param_max(v, B_SLICE));
}

static float rate_khz(int x) { return sr / 1000.f / powf(2.f, x / 8.f); }

void param_format(int v, int p, char *b, int n)
{
    int x = song.pat.param[v][p];
    if (v == V_BASS) {
        switch (p) {
        case S_WAVE:
            if (x < NWAVE_SYNTH)
                snprintf(b, n, "%s", wave_names[x]);
            else if (x - NWAVE_SYNTH < nbass)
                snprintf(b, n, "%s", bass_smp[x - NWAVE_SYNTH].name);
            else
                snprintf(b, n, "?");
            return;
        case S_ROOT: snprintf(b, n, "%s", note_names[clampi(x, 0, 11)]); return;
        case S_RIFF: snprintf(b, n, "%s", riff_name(x)); return;
        case S_LEN:
            if (x >= 17)
                snprintf(b, n, "HOLD");
            else
                snprintf(b, n, "%d/16", x);
            return;
        case S_ROLL: snprintf(b, n, "x%d", roll_counts[clampi(x, 0, 4)]); return;
        case S_ALT: snprintf(b, n, "%s", balt_names[clampi(x, 0, NBALT - 1)]); return;
        default: snprintf(b, n, "%d", x); return;
        }
    }
    switch (p) {
    case B_BANK: snprintf(b, n, "%s", nbanks ? banks[clampi(x, 0, nbanks - 1)].name : "-"); return;
    case B_SLICE: snprintf(b, n, "%d", x + 1); return;
    case B_MODE: snprintf(b, n, "%s", mode_names[clampi(x, 0, NMODE - 1)]); return;
    case B_PITCH: snprintf(b, n, "%+d", x); return;
    case B_FIT: snprintf(b, n, "%s", x ? "ON" : "OFF"); return;
    case B_LEN: snprintf(b, n, "%d%%", x); return;
    case B_ROLL: snprintf(b, n, "x%d", roll_counts[clampi(x, 0, 4)]); return;
    case B_ALT: snprintf(b, n, "%s", alt_names[clampi(x, 0, NALT - 1)]); return;
    case B_TONE:
        if (x == 0)
            snprintf(b, n, "OPEN");
        else
            snprintf(b, n, "%s %d", x < 0 ? "LP" : "HP", x < 0 ? -x : x);
        return;
    case B_BITS: snprintf(b, n, "%d BIT", x); return;
    case B_RATE:
        if (x == 0)
            snprintf(b, n, "FULL");
        else
            snprintf(b, n, "%.1fK", rate_khz(x));
        return;
    case B_COMP:
        if (x == 0)
            snprintf(b, n, "OFF");
        else
            snprintf(b, n, "%d", x);
        return;
    default: snprintf(b, n, "%d", x); return;
    }
}

/* ------------------------------------------------------------- euclid */

int euclid_hit(const Cell *c, int step)
{
    int n = c->steps;
    if (n < 1 || c->hits < 1)
        return 0;
    int i = ((step - c->rot) % n + n) % n;
    return (i * c->hits) % n < c->hits;
}

static uint32_t grng = 0x2545f491u;
static uint32_t gen_rand(void)
{
    grng ^= grng << 13;
    grng ^= grng >> 17;
    grng ^= grng << 5;
    return grng;
}
static int rnd(int n) { return n > 0 ? (int)(gen_rand() % (uint32_t)n) : 0; }

void euclid_permute(Cell *c)
{
    static const uint8_t lens[] = {5, 6, 7, 8, 9, 11, 12, 13, 16, 16, 16, 32};
    int n = lens[rnd((int)sizeof lens)];
    int kmax = n * 2 / 3 > 1 ? n * 2 / 3 : 1;
    c->steps = (uint8_t)n;
    c->hits = (uint8_t)(1 + rnd(kmax));
    c->rot = (uint8_t)rnd(n);
    c->on = 1;
}

/* ------------------------------------------------------------------- dsp */

static uint32_t arng = 0x9e3779b9u;
static inline float frand(void)
{
    arng ^= arng << 13;
    arng ^= arng >> 17;
    arng ^= arng << 5;
    return (arng >> 8) * (1.f / 16777216.f);
}

static inline float softclip(float x)
{
    if (x > 3.f)
        return 1.f;
    if (x < -3.f)
        return -1.f;
    float x2 = x * x;
    return x * (27.f + x2) / (27.f + 9.f * x2);
}

static inline float db_to_lin(float db) { return expf(db * 0.11512925f); }
static inline float lin_to_db(float x) { return 8.6858896f * logf(x > 1e-9f ? x : 1e-9f); }

/* Zavalishin's state-variable filter */
typedef struct {
    float ic1, ic2;
} Svf;
typedef struct {
    float a1, a2, a3, k;
} SvfK;

static SvfK svf_coef(float hz, float q)
{
    SvfK c;
    float nyq = sr * 0.49f;
    hz = hz < 10.f ? 10.f : hz > nyq ? nyq : hz;
    float g = tanf((float)M_PI * hz / sr);
    c.k = 1.f / q;
    c.a1 = 1.f / (1.f + g * (g + c.k));
    c.a2 = g * c.a1;
    c.a3 = g * c.a2;
    return c;
}

/* returns the low-pass; *hp gets the high-pass */
static inline float svf_tick(Svf *s, const SvfK *c, float x, float *hp)
{
    float v3 = x - s->ic2;
    float v1 = c->a1 * s->ic1 + c->a2 * v3;
    float v2 = s->ic2 + c->a2 * s->ic1 + c->a3 * v3;
    s->ic1 = 2.f * v1 - s->ic1;
    s->ic2 = 2.f * v2 - s->ic2;
    if (hp)
        *hp = x - c->k * v1 - v2;
    return v2;
}

/* 4-point Hermite read */
static inline float hermite(const float *d, int len, double pos)
{
    int i = (int)pos;
    float f = (float)(pos - i);
    float xm = d[i > 0 ? i - 1 : 0];
    float x0 = d[i];
    float x1 = d[i + 1 < len ? i + 1 : len - 1];
    float x2 = d[i + 2 < len ? i + 2 : len - 1];
    float c1 = 0.5f * (x1 - xm);
    float c2 = xm - 2.5f * x0 + 2.f * x1 - 0.5f * x2;
    float c3 = 0.5f * (x2 - xm) + 1.5f * (x0 - x1);
    return ((c3 * f + c2) * f + c1) * f + x0;
}

/* a feed-forward compressor; amount 0..100 sets threshold and ratio together */
typedef struct {
    float env, gr_db;
} Comp;

typedef struct {
    float thr, slope, makeup, ka, kr;
} CompK;

static CompK comp_coef(int amount, float max_thr, float max_ratio, float att_ms, float rel_ms)
{
    CompK k;
    float a = amount / 100.f;
    k.thr = -max_thr * a;
    float ratio = 1.f + (max_ratio - 1.f) * a;
    k.slope = 1.f - 1.f / ratio;
    k.makeup = db_to_lin(-k.thr * k.slope * 0.7f);
    k.ka = 1.f - expf(-1.f / (att_ms * 0.001f * sr));
    k.kr = 1.f - expf(-1.f / (rel_ms * 0.001f * sr));
    return k;
}

static inline float comp_gain(Comp *c, const CompK *k, float level)
{
    c->env += (level > c->env ? k->ka : k->kr) * (level - c->env);
    float over = lin_to_db(c->env) - k->thr;
    c->gr_db = over > 0.f ? over * k->slope : 0.f;
    return db_to_lin(-c->gr_db) * k->makeup;
}

/* --------------------------------------------------------- break channels */

typedef struct {
    int on;
    const Bank *b;
    double pos, inc, end; /* end: where the slice (or its LENGTH) runs out */
    int slice;
    float att, rel, vel;  /* attack ramp, release gain (1 = not releasing) */
} Head;

typedef struct {
    Head h[2]; /* the hit, and the previous one fading out under it */
    Svf flt;
    float hold, hold_ph;
    Comp comp;
    int walk;
    /* ratchets */
    int rat_left, rat_cd, rat_int, rat_slice, rat_rev;
    float rat_semis, rat_vel;
} BrkCh;

static BrkCh brk[NBRK];

static void head_release(Head *h)
{
    if (h->on && h->rel >= 1.f)
        h->rel = 0.999f;
}

static void brk_play_ex(int v, int slice, int rev, float semis, float vel, int grid)
{
    BrkCh *c = &brk[v];
    const Bank *b = chan_bank(v);
    if (!b || b->nslices < 1)
        return;
    slice = ((slice % b->nslices) + b->nslices) % b->nslices;
    const int *P = song.pat.param[v];
    /* the old hit fades under the new one rather than clicking off */
    c->h[1] = c->h[0];
    head_release(&c->h[1]);
    Head *h = &c->h[0];
    double lo = b->start[slice], hi = grid ? bank_slice_end(b, slice) : b->start[slice + 1];
    double len = (hi - lo) * P[B_LEN] / 100.0;
    if (len < 32)
        len = 32;
    double rate = pow(2.0, semis / 12.0);
    if (P[B_FIT])
        rate *= song.bpm / b->bpm;
    h->on = 1;
    h->b = b;
    h->slice = slice;
    h->vel = vel;
    h->rel = 1.f;
    if (rev) {
        h->pos = lo + len - 1;
        h->inc = -rate;
        h->end = lo;
        h->att = 0.f; /* a reversed slice swells in from its tail */
    }
    else {
        h->pos = lo;
        h->inc = rate;
        h->end = lo + len;
        h->att = 0.2f;
    }
    live.slice[v] = slice;
}

static void brk_play(int v, int slice, int rev, float semis, float vel)
{
    brk_play_ex(v, slice, rev, semis, vel, 0);
}

static int brk_pick(int v, int step)
{
    const int *P = song.pat.param[v];
    const Bank *b = chan_bank(v);
    int n = b ? b->nslices : 1;
    switch (P[B_MODE]) {
    case MODE_WALK: return P[B_SLICE] + (brk[v].walk++ % P[B_RANGE]);
    case MODE_GRID: return b ? bank_slice_at(b, step) : 0;
    case MODE_RAND: return P[B_SLICE] + (int)(frand() * P[B_RANGE]) % (P[B_RANGE] < n ? P[B_RANGE] : n);
    default: return P[B_SLICE];
    }
}

static float sps; /* samples per 16th */

static void brk_trigger(int v, int flags, int step)
{
    BrkCh *c = &brk[v];
    const int *P = song.pat.param[v];
    const Bank *b = chan_bank(v);
    if (!b)
        return;
    int slice = brk_pick(v, step), rev = 0;
    float semis = (float)P[B_PITCH];
    if (flags & TRIG_ALT) {
        switch (P[B_ALT]) {
        case ALT_REV: rev = 1; break;
        case ALT_OCTUP: semis += 12.f; break;
        case ALT_OCTDN: semis -= 12.f; break;
        case ALT_NEXT: slice++; break;
        case ALT_RAND: slice = (int)(frand() * b->nslices); break;
        case ALT_STOP:
            head_release(&c->h[0]);
            head_release(&c->h[1]);
            c->rat_left = 0;
            live.trig_count[v]++;
            return;
        }
    }
    /* GRID with a plain hit lets the slice that is already playing run on
     * when no new one starts on this step, so a full HIT cell plays the
     * break straight */
    slice = ((slice % b->nslices) + b->nslices) % b->nslices;
    int grid = P[B_MODE] == MODE_GRID;
    if (grid && flags == TRIG_HIT && c->h[0].on && c->h[0].rel >= 1.f && c->h[0].b == b && c->h[0].inc > 0 &&
        (c->h[0].slice == slice || !bank_slice_exact(b, step)))
        return;
    float vel = flags & TRIG_ROLL ? 0.7f : 1.f;
    brk_play_ex(v, slice, rev, semis, vel, grid);
    c->rat_left = 0;
    if (flags & TRIG_ROLL) {
        int r = roll_counts[clampi(P[B_ROLL], 0, 4)];
        c->rat_left = r - 1;
        c->rat_int = (int)(sps / r);
        if (c->rat_int < 32)
            c->rat_int = 32;
        c->rat_cd = c->rat_int;
        c->rat_slice = slice;
        c->rat_rev = rev;
        c->rat_semis = semis;
        c->rat_vel = vel;
    }
    live.trig_count[v]++;
}

static inline float head_tick(Head *h)
{
    if (!h->on)
        return 0.f;
    const Bank *b = h->b;
    double remain = (h->end - h->pos) / h->inc; /* output samples left */
    if (remain <= 0.0 || h->pos < 0 || h->pos >= b->len) {
        h->on = 0;
        return 0.f;
    }
    float x = hermite(b->data, b->len, h->pos);
    h->pos += h->inc;
    float g = h->vel;
    float tail = (float)(remain / (0.003 * sr)); /* 3 ms fade at the slice end */
    if (tail < 1.f)
        g *= tail;
    if (h->att < 1.f) {
        g *= h->att;
        h->att += h->inc > 0 ? 1.f / (0.0005f * sr) : 1.f / (0.004f * sr);
    }
    if (h->rel < 1.f) {
        g *= h->rel;
        h->rel *= 0.9965f; /* ~3 ms at 48 kHz */
        if (h->rel < 1e-3f)
            h->on = 0;
    }
    return x * g;
}

/* --------------------------------------------------------------- bass */

typedef struct {
    int gate;
    long gate_left;     /* samples until the note lets go, -1 for HOLD */
    float env;
    float lf, lf_target, glide_k; /* frequency in log2 Hz */
    double ph[3];
    float pitch_env;
    double spos, srate;
    const BassSample *smp;
    float lfo;
    Svf flt;
    int riff_pos;
    int rat_left, rat_cd, rat_int;
    int rat_note;
} BassV;

static BassV bass;

static float note_hz(int note) { return 440.f * powf(2.f, (note - 69) / 12.f); }

static void bass_note(int note, int legato_ok, int slide)
{
    const int *P = song.pat.param[V_BASS];
    float hz = note_hz(note);
    float lt = log2f(hz);
    float glide_ms = slide ? 120.f : P[S_GLIDE] * 1.5f;
    int legato = legato_ok && bass.gate && bass.env > 0.05f && glide_ms > 0.f;
    bass.lf_target = lt;
    if (legato)
        bass.glide_k = 1.f - expf(-1.f / (glide_ms * 0.001f * sr));
    else {
        bass.lf = lt;
        bass.glide_k = 1.f;
        bass.pitch_env = 1.f;
    }
    int w = P[S_WAVE];
    if (w >= NWAVE_SYNTH && w - NWAVE_SYNTH < nbass) {
        const BassSample *s = &bass_smp[w - NWAVE_SYNTH];
        if (!legato || bass.smp != s) {
            bass.smp = s;
            bass.spos = 0.0;
        }
    }
    else
        bass.smp = NULL;
    bass.gate = 1;
    bass.gate_left = P[S_LEN] >= 17 ? -1 : (long)(P[S_LEN] * sps);
    live.note = note;
}

static void bass_trigger(int flags, int step)
{
    const int *P = song.pat.param[V_BASS];
    (void)step;
    int note = 12 * (P[S_OCT] + 1) + P[S_ROOT] + riff_note(P[S_RIFF], bass.riff_pos);
    bass.riff_pos++;
    int slide = 0;
    if (flags & TRIG_ALT) {
        switch (P[S_ALT]) {
        case BALT_OCTUP: note += 12; break;
        case BALT_FIFTH: note += 7; break;
        case BALT_OCTDN: note -= 12; break;
        case BALT_SLIDE: slide = 1; break;
        case BALT_REST:
            bass.gate = 0;
            bass.rat_left = 0;
            live.trig_count[V_BASS]++;
            return;
        }
    }
    if (note < 12)
        note += 12;
    bass_note(note, 1, slide);
    bass.rat_left = 0;
    if (flags & TRIG_ROLL) {
        int r = roll_counts[clampi(P[S_ROLL], 0, 4)];
        bass.rat_left = r - 1;
        bass.rat_int = (int)(sps / r);
        if (bass.rat_int < 32)
            bass.rat_int = 32;
        bass.rat_cd = bass.rat_int;
        bass.rat_note = note;
        if (P[S_LEN] < 17)
            bass.gate_left = bass.rat_int * 3 / 4;
    }
    live.trig_count[V_BASS]++;
}

static inline float polyblep(double t, double dt)
{
    if (t < dt) {
        t /= dt;
        return (float)(t + t - t * t - 1.0);
    }
    if (t > 1.0 - dt) {
        t = (t - 1.0) / dt;
        return (float)(t * t + t + t + 1.0);
    }
    return 0.f;
}

static inline float saw(double *ph, double dt)
{
    float y = (float)(2.0 * *ph - 1.0) - polyblep(*ph, dt);
    *ph += dt;
    if (*ph >= 1.0)
        *ph -= 1.0;
    return y;
}

static inline float sine_ph(double *ph, double dt)
{
    float y = sinf(2.f * (float)M_PI * (float)*ph);
    *ph += dt;
    if (*ph >= 1.0)
        *ph -= 1.0;
    return y;
}

typedef struct {
    SvfK flt;
    float width, drive, drive_norm, att_k, rel_k, lfo_inc;
    int wave;
} BassK;

static inline float bass_tick(const BassK *k)
{
    /* envelope: 2 ms attack, 40 ms release (808 and samples ring out longer) */
    if (bass.gate && bass.gate_left >= 0 && --bass.gate_left < 0)
        bass.gate = 0;
    float target = bass.gate ? 1.f : 0.f;
    bass.env += (target - bass.env) * (bass.gate ? k->att_k : k->rel_k);
    if (bass.env < 1e-4f && !bass.gate)
        return 0.f;
    bass.lf += (bass.lf_target - bass.lf) * bass.glide_k;
    float hz = exp2f(bass.lf);
    double dt = hz / sr;
    float x;
    switch (k->wave) {
    case WAVE_SUB:
        x = sine_ph(&bass.ph[0], dt);
        x += k->width * 0.35f * sinf(4.f * (float)M_PI * (float)bass.ph[0]);
        break;
    case WAVE_REESE: {
        double d = k->width * 0.012;
        x = 0.5f * saw(&bass.ph[0], dt * (1.0 + d)) + 0.5f * saw(&bass.ph[1], dt * (1.0 - d));
        x += 0.6f * sine_ph(&bass.ph[2], dt);
        break;
    }
    case WAVE_808: {
        float pe = bass.pitch_env;
        bass.pitch_env *= 0.9993f;
        x = sine_ph(&bass.ph[0], dt * (1.0 + 1.5 * pe * pe));
        x = softclip(x * (1.5f + 2.f * k->width));
        break;
    }
    case WAVE_GROWL: {
        bass.lfo += k->lfo_inc;
        if (bass.lfo >= 1.f)
            bass.lfo -= 1.f;
        float w = 0.5f + 0.5f * sinf(2.f * (float)M_PI * bass.lfo);
        float idx = 0.5f + 4.f * k->width * w;
        float m = sine_ph(&bass.ph[1], dt * 2.0);
        double ph = bass.ph[0] + idx * m / (2.0 * M_PI);
        x = sinf(2.f * (float)M_PI * (float)ph);
        bass.ph[0] += dt;
        if (bass.ph[0] >= 1.0)
            bass.ph[0] -= 1.0;
        break;
    }
    default: {
        const BassSample *s = bass.smp;
        if (!s || bass.spos >= s->len - 1) {
            bass.gate = 0;
            bass.env = 0.f;
            return 0.f;
        }
        float root = s->root_hz > 0.f ? s->root_hz : 55.f;
        x = hermite(s->data, s->len, bass.spos);
        bass.spos += hz / root;
        break;
    }
    }
    x = svf_tick(&bass.flt, &k->flt, x, NULL);
    x = softclip(x * k->drive) * k->drive_norm;
    return x * bass.env * 0.5f;
}

/* ---------------------------------------------------------------- reverb */

#define NCOMB 4
static const int comb_len[NCOMB] = {1116, 1188, 1277, 1356};
static const int ap_len[2] = {556, 441};
typedef struct {
    float *buf;
    int n, i;
    float z;
} Line;
static Line combs[2][NCOMB], aps[2][2];

static void line_init(Line *l, int n)
{
    l->n = n;
    l->i = 0;
    l->z = 0.f;
    free(l->buf);
    l->buf = calloc(n, sizeof(float));
}

static inline float comb_tick(Line *l, float x, float fb, float damp)
{
    float y = l->buf[l->i];
    l->z = y + (l->z - y) * damp;
    l->buf[l->i] = x + l->z * fb;
    if (++l->i >= l->n)
        l->i = 0;
    return y;
}

static inline float ap_tick(Line *l, float x)
{
    float y = l->buf[l->i];
    l->buf[l->i] = x + y * 0.5f;
    if (++l->i >= l->n)
        l->i = 0;
    return y - x;
}

/* ------------------------------------------------------ punch-in effects */

#define RB_BITS 18
#define RB_N (1 << RB_BITS)
#define RB_MASK (RB_N - 1)
static float rbL[RB_N], rbR[RB_N];
static uint32_t wpos, step_wpos;
static int fx_cur = -1;
static uint32_t fx_anchor;
static double fx_t, fx_speed;
static float fx_mix;
static int fx_kind = -1;

static void fx_begin(int fx)
{
    fx_kind = fx;
    fx_t = 0.0;
    fx_speed = 1.0;
    switch (fx) {
    case FX_REPEAT8:
    case FX_REPEAT16:
        /* loop the beat from the last step line; reading starts where the
         * live signal is, so the effect enters without a jump */
        fx_anchor = step_wpos;
        fx_t = (double)((wpos - step_wpos) & RB_MASK);
        break;
    default: fx_anchor = wpos; break;
    }
}

static inline void fx_read(float *L, float *R)
{
    uint32_t p;
    switch (fx_kind) {
    case FX_REPEAT8:
    case FX_REPEAT16: {
        double len = sps * (fx_kind == FX_REPEAT8 ? 2.0 : 1.0);
        double t = fmod(fx_t, len);
        p = (fx_anchor + (uint32_t)t) & RB_MASK;
        fx_t += 1.0;
        /* short fades at the loop seam */
        float e = (float)fminf((float)t, (float)(len - t)) / (0.002f * sr);
        float g = e < 1.f ? e : 1.f;
        *L = rbL[p] * g;
        *R = rbR[p] * g;
        return;
    }
    case FX_REVERSE:
        p = (fx_anchor - (uint32_t)fx_t) & RB_MASK;
        fx_t += 1.0;
        if (fx_t > RB_N - sr)
            fx_t = 0.0;
        *L = rbL[p];
        *R = rbR[p];
        return;
    case FX_TAPESTOP: {
        double stop = sps * 4.0; /* a beat to wind down */
        p = (fx_anchor + (uint32_t)fx_t) & RB_MASK;
        uint32_t p2 = (p + 1) & RB_MASK;
        float f = (float)(fx_t - floor(fx_t));
        *L = (rbL[p] + (rbL[p2] - rbL[p]) * f) * (float)fx_speed;
        *R = (rbR[p] + (rbR[p2] - rbR[p]) * f) * (float)fx_speed;
        fx_t += fx_speed;
        fx_speed -= 1.0 / stop;
        if (fx_speed < 0.0)
            fx_speed = 0.0;
        return;
    }
    }
    *L = *R = 0.f;
}

/* --------------------------------------------------------------- transport */

static double step_phase, sync_rate = 1.0;
static int next_step;
static float meter_acc[NV + 1];
static Comp glue;
static Svf sweep[2];
static float sweep_cut = 1.f; /* 1 = open */
static SvfK sweep_k;

static void pattern_load_now(int i)
{
    song.pat = song.slot[i];
    song_clamp();
    live.cur_slot = i;
}

static void fire_step(void)
{
    int s = next_step++;
    live.step = s;
    step_wpos = wpos;
    if (s % 16 == 0 && live.queued_slot >= 0) {
        pattern_load_now(live.queued_slot);
        live.queued_slot = -1;
    }
    if (s % 32 == 0)
        bass.riff_pos = 0;
    for (int v = 0; v < NV; v++) {
        int flags = 0;
        for (int r = 0; r < NROW; r++)
            if (song.pat.cell[v][r].on && euclid_hit(&song.pat.cell[v][r], s))
                flags |= 1 << r;
        if (!flags)
            continue;
        if (v == V_BASS)
            bass_trigger(flags, s);
        else
            brk_trigger(v, flags, s);
    }
}

void engine_set_playing(int on)
{
    if (on && !live.playing) {
        step_phase = 0.0;
        next_step = 0;
        bass.riff_pos = 0;
        for (int v = 0; v < NBRK; v++)
            brk[v].walk = 0;
    }
    if (!on) {
        bass.gate = 0;
        for (int v = 0; v < NBRK; v++) {
            brk[v].rat_left = 0;
            /* let the hits ring out briefly rather than cut */
        }
    }
    live.playing = on;
}

void engine_audition(int v)
{
    sps = sr * 15.f / song.bpm;
    if (v == V_BASS) {
        const int *P = song.pat.param[V_BASS];
        bass_note(12 * (P[S_OCT] + 1) + P[S_ROOT], 0, 0);
        if (P[S_LEN] >= 17)
            bass.gate_left = (long)(4 * sps);
        live.trig_count[v]++;
        return;
    }
    const int *P = song.pat.param[v];
    brk_play(v, P[B_SLICE], 0, (float)P[B_PITCH], 1.f);
    live.trig_count[v]++;
}

static const int ppqn_values[NPPQN] = {1, 2, 4, 8, 16, 24, 32};
int clk_ppqn_value(int i) { return ppqn_values[i < 0 ? 0 : i >= NPPQN ? NPPQN - 1 : i]; }

double engine_position(void)
{
    if (!live.playing)
        return (double)next_step;
    double frac = 1.0 - step_phase / sps;
    return (double)live.step + (frac < 0.0 ? 0.0 : frac > 1.0 ? 1.0 : frac);
}

void engine_set_position(double steps)
{
    double was = engine_position();
    if (steps < 0.0)
        steps = 0.0;
    int s = (int)steps;
    double frac = steps - s;
    live.step = s;
    next_step = s + 1;
    step_phase = sps * (1.0 - frac);
    if (step_phase < 1.0)
        step_phase = 1.0;
    aclock_shift(engine_position() - was);
}

double engine_steps_per_frame(void) { return live.playing && sps > 0.f ? sync_rate / sps : 0.0; }

void engine_set_rate(double r) { sync_rate = r < 0.9 ? 0.9 : r > 1.1 ? 1.1 : r; }

/* ---------------------------------------------------------------- render */

typedef struct {
    SvfK flt;
    int flt_on, hp;
    float hold;   /* sample-and-hold period, 1 = off */
    float q, iq;  /* bit quantiser, q = 0 for 24 bit */
    int one_bit;
    CompK comp;
    int comp_on;
    float gL, gR, send;
} BrkK;

void engine_render(int16_t *out, int frames)
{
    sps = sr * 15.f / song.bpm;
    if (step_phase > sps)
        step_phase = sps;

    int any_solo = 0;
    for (int v = 0; v < NV; v++)
        any_solo |= song.solo[v];
    /* mono + clock: the sound in mono on one side, clock pulses on the other */
    int clk_out = song.out_mode == OUT_MONO_CLOCK, clk_left = song.clk_side;
    int clk_ppqn = clk_ppqn_value(song.clk_ppqn);
    double clk_spp = sps * 4.0 / clk_ppqn; /* samples per pulse */
    double clk_w = fmin(song.clk_width * 0.001 * sr, clk_spp * 0.5);
    float gain[NV], gL[NV], gR[NV];
    for (int v = 0; v < NV; v++) {
        float g = song.vol[v] / 100.f;
        g *= g;
        if (song.pat.mute[v] || (any_solo && !song.solo[v]))
            g = 0.f;
        gain[v] = g;
        float pan = clk_out ? 0.f : song.pan[v] / 100.f; /* mono + clock: from the centre */
        gL[v] = g * sqrtf(0.5f * (1.f - pan));
        gR[v] = g * sqrtf(0.5f * (1.f + pan));
    }

    BrkK bk[NBRK];
    for (int v = 0; v < NBRK; v++) {
        const int *P = song.pat.param[v];
        BrkK *k = &bk[v];
        int t = P[B_TONE];
        k->flt_on = t != 0;
        k->hp = t > 0;
        if (t < 0)
            k->flt = svf_coef(18000.f * powf(150.f / 18000.f, -t / 50.f), 0.9f);
        else if (t > 0)
            k->flt = svf_coef(20.f * powf(4000.f / 20.f, t / 50.f), 0.9f);
        k->hold = P[B_RATE] ? powf(2.f, P[B_RATE] / 8.f) : 1.f;
        k->one_bit = P[B_BITS] <= 1;
        k->q = P[B_BITS] >= 24 ? 0.f : (float)(1 << (P[B_BITS] - 1));
        k->iq = k->q > 0.f ? 1.f / k->q : 0.f;
        k->comp_on = P[B_COMP] > 0;
        k->comp = comp_coef(P[B_COMP], 36.f, 8.f, 2.f, 80.f);
        k->gL = gL[v];
        k->gR = gR[v];
        k->send = song.send[v] / 100.f;
    }
    BassK bq;
    {
        const int *P = song.pat.param[V_BASS];
        bq.wave = P[S_WAVE];
        bq.width = P[S_WIDTH] / 100.f;
        bq.drive = 1.f + P[S_DRIVE] * 0.03f;
        bq.drive_norm = 1.f / softclip(bq.drive);
        bq.flt = svf_coef(40.f * powf(2.f, P[S_CUT] / 100.f * 7.5f), 0.6f + P[S_RES] * 0.08f);
        bq.att_k = 1.f - expf(-1.f / (0.002f * sr));
        int ring = bq.wave == WAVE_808 || bq.wave >= NWAVE_SYNTH;
        bq.rel_k = 1.f - expf(-1.f / ((ring ? 0.12f : 0.04f) * sr));
        bq.lfo_inc = song.bpm / 60.f * 2.f / sr; /* 1/8 note wobble */
    }
    float bass_send = song.send[V_BASS] / 100.f;

    CompK gk = comp_coef(song.glue, 20.f, 4.f, 10.f, 120.f);
    float mdrive = 1.f + song.master_drive * 0.06f, mnorm = 1.f / softclip(mdrive);
    float master = song.master / 100.f;
    master *= master * 1.3f;
    float fb = 0.7f + song.verb_time * 0.0028f, damp = 0.6f - song.verb_tone * 0.005f;

    /* the L2 sweep closes over the chosen time and reopens in a 16th */
    float close_k = 1.f / (sweep_steps[clampi(song.sweep_rate, 0, 3)] * sps);
    float open_k = 1.f / sps;

    if (live.fx != fx_cur) {
        fx_cur = live.fx;
        if (fx_cur >= 0)
            fx_begin(fx_cur);
    }
    float fx_ramp = 1.f / (0.004f * sr);

    for (int n = 0; n < frames; n++) {
        if (live.playing) {
            step_phase -= sync_rate;
            if (step_phase <= 0.0) {
                step_phase += sps;
                fire_step();
            }
        }
        float L = 0.f, R = 0.f, verb_in = 0.f;
        for (int v = 0; v < NBRK; v++) {
            BrkCh *c = &brk[v];
            const BrkK *k = &bk[v];
            if (c->rat_left > 0 && --c->rat_cd <= 0) {
                c->rat_left--;
                c->rat_cd = c->rat_int;
                c->rat_vel += (1.f - c->rat_vel) * 0.35f;
                brk_play(v, c->rat_slice, c->rat_rev, c->rat_semis, c->rat_vel);
            }
            if (!c->h[0].on && !c->h[1].on && c->comp.env < 1e-5f) {
                continue;
            }
            float x = head_tick(&c->h[0]) + head_tick(&c->h[1]);
            if (k->flt_on) {
                float hp, lp = svf_tick(&c->flt, &k->flt, x, &hp);
                x = k->hp ? hp : lp;
            }
            if (k->hold > 1.f) {
                c->hold_ph += 1.f;
                if (c->hold_ph >= k->hold) {
                    c->hold_ph -= k->hold;
                    c->hold = x;
                }
                x = c->hold;
            }
            if (k->one_bit)
                x = x > 0.002f ? 0.5f : x < -0.002f ? -0.5f : 0.f;
            else if (k->q > 0.f)
                x = floorf(x * k->q + 0.5f) * k->iq;
            if (k->comp_on)
                x *= comp_gain(&c->comp, &k->comp, fabsf(x));
            L += x * k->gL;
            R += x * k->gR;
            verb_in += x * gain[v] * k->send;
            float a = fabsf(x * gain[v]);
            if (a > meter_acc[v])
                meter_acc[v] = a;
        }
        if (bass.rat_left > 0 && --bass.rat_cd <= 0) {
            bass.rat_left--;
            bass.rat_cd = bass.rat_int;
            bass_note(bass.rat_note, 0, 0);
            if (song.pat.param[V_BASS][S_LEN] < 17)
                bass.gate_left = bass.rat_int * 3 / 4;
        }
        float b = bass_tick(&bq);
        L += b * gL[V_BASS];
        R += b * gR[V_BASS];
        verb_in += b * gain[V_BASS] * bass_send;
        if (fabsf(b * gain[V_BASS]) > meter_acc[V_BASS])
            meter_acc[V_BASS] = fabsf(b * gain[V_BASS]);

        /* reverb */
        float vin = verb_in * 0.12f, wl = 0.f, wr = 0.f;
        for (int i = 0; i < NCOMB; i++) {
            wl += comb_tick(&combs[0][i], vin, fb, damp);
            wr += comb_tick(&combs[1][i], vin, fb, damp);
        }
        for (int i = 0; i < 2; i++) {
            wl = ap_tick(&aps[0][i], wl);
            wr = ap_tick(&aps[1][i], wr);
        }
        L += wl;
        R += wr;

        L *= 0.7f; /* headroom for four breaks and a sub */
        R *= 0.7f;
        /* glue compressor, stereo-linked */
        if (song.glue > 0) {
            float g = comp_gain(&glue, &gk, fmaxf(fabsf(L), fabsf(R)));
            L *= g;
            R *= g;
        }
        else
            glue.gr_db = 0.f;

        /* L2 sweep */
        float tgt = live.filter_held ? 0.f : 1.f;
        if (sweep_cut != tgt) {
            if (tgt < sweep_cut)
                sweep_cut = fmaxf(tgt, sweep_cut - close_k);
            else
                sweep_cut = fminf(tgt, sweep_cut + open_k);
        }
        if (sweep_cut < 0.999f) {
            if ((n & 15) == 0) /* coefficients every 16 samples */
                sweep_k = svf_coef(200.f * powf(90.f, sweep_cut), 2.2f);
            L = svf_tick(&sweep[0], &sweep_k, L, NULL);
            R = svf_tick(&sweep[1], &sweep_k, R, NULL);
        }
        else
            sweep[0].ic1 = sweep[0].ic2 = sweep[1].ic1 = sweep[1].ic2 = 0.f;

        /* punch-in effects read back what was just played */
        rbL[wpos] = L;
        rbR[wpos] = R;
        float tm = fx_cur >= 0 ? 1.f : 0.f;
        if (fx_mix != tm)
            fx_mix = tm > fx_mix ? fminf(tm, fx_mix + fx_ramp) : fmaxf(tm, fx_mix - fx_ramp);
        if (fx_mix > 0.f && fx_kind >= 0) {
            float fl, fr;
            fx_read(&fl, &fr);
            L += (fl - L) * fx_mix;
            R += (fr - R) * fx_mix;
        }
        wpos = (wpos + 1) & RB_MASK;

        L = softclip(L * mdrive) * mnorm * master;
        R = softclip(R * mdrive) * mnorm * master;
        L = softclip(L);
        R = softclip(R);
        float m = fmaxf(fabsf(L), fabsf(R));
        if (m > meter_acc[NV])
            meter_acc[NV] = m;
        if (clk_out) {
            float mono = 0.5f * (L + R), c = 0.f;
            if (live.playing) {
                /* the position in 16ths at the start of this sample */
                double pos = live.step + 1.0 - (step_phase + sync_rate) / sps, ph = fmod(pos * clk_ppqn / 4.0, 1.0);
                if ((ph < 0.0 ? ph + 1.0 : ph) * clk_spp < clk_w)
                    c = 0.95f;
            }
            L = clk_left ? c : mono;
            R = clk_left ? mono : c;
        }
        int li = (int)(L * 32767.f), ri = (int)(R * 32767.f);
        out[2 * n] = (int16_t)(li > 32767 ? 32767 : li < -32768 ? -32768 : li);
        out[2 * n + 1] = (int16_t)(ri > 32767 ? 32767 : ri < -32768 ? -32768 : ri);
    }

    for (int v = 0; v <= NV; v++) {
        live.meter[v] = meter_acc[v];
        meter_acc[v] = 0.f;
    }
    for (int v = 0; v < NBRK; v++) {
        live.gr[v] = brk[v].comp.gr_db;
        const Head *h = &brk[v].h[0];
        live.play_pos[v] = h->on && h->b ? (float)(h->pos / h->b->len) : -1.f;
    }
    live.gr[NV] = glue.gr_db;
    live.cutoff_norm = sweep_cut;
}

/* ------------------------------------------------------------ song state */

static int find_bank(const char *name)
{
    for (int i = 0; i < nbanks; i++)
        if (!strcmp(banks[i].name, name))
            return i;
    return -1;
}

static int pick_tag(const Bank *b, int tag, int fallback)
{
    int n = bank_count_tag(b, tag);
    if (!n)
        return fallback < b->nslices ? fallback : 0;
    return bank_find_tag(b, tag, rnd(n));
}

static void cell(Cell *c, int on, int hits, int steps, int rot)
{
    c->on = (uint8_t)on;
    c->hits = (uint8_t)hits;
    c->steps = (uint8_t)steps;
    c->rot = (uint8_t)(rot % steps);
}

static void defaults_for(Pattern *p)
{
    memset(p, 0, sizeof *p);
    for (int v = 0; v < NV; v++) {
        for (int q = 0; q < NP; q++)
            p->param[v][q] = param_def(v, q)->def;
        for (int r = 0; r < NROW; r++)
            cell(&p->cell[v][r], 0, r == ROW_HIT ? 4 : 1, 16, 0);
    }
}

/* A jungle pattern: kick chops, a backbeat snare with a roll and a
 * reversed hit, a walking ghost/hat channel, the break running straight
 * underneath, and a sub line. Every choice is drawn from seed. */
void pattern_generate(Pattern *p, unsigned seed)
{
    grng = seed ? seed * 2654435761u : 0x2545f491u;
    if (!grng)
        grng = 1;
    for (int i = 0; i < 4; i++)
        gen_rand();
    defaults_for(p);
    if (!nbanks)
        return;

    /* the main break: a loop with plenty of slices, often the Amen */
    int loops[MAX_BANKS], nl = 0;
    for (int i = 0; i < nbanks; i++)
        if (banks[i].nslices >= 8 && strcmp(banks[i].name, "JUNGLE"))
            loops[nl++] = i;
    int amen = find_bank("AMEN");
    int main_b = amen >= 0 && rnd(100) < 45 ? amen : nl ? loops[rnd(nl)] : 0;
    int second = nl ? loops[rnd(nl)] : main_b;
    const Bank *mb = &banks[main_b];
    int *P;

    /* BRK1: kicks */
    P = p->param[0];
    P[B_BANK] = main_b;
    P[B_SLICE] = pick_tag(mb, TAG_KICK, 0);
    P[B_MODE] = MODE_FIX;
    static const int kick_pats[][3] = {{2, 16, 0}, {3, 16, 0}, {3, 8, 0}, {5, 16, 0}, {2, 16, 0}, {3, 16, 0}};
    const int *kp = kick_pats[rnd(6)];
    cell(&p->cell[0][ROW_HIT], 1, kp[0], kp[1], kp[2]);
    cell(&p->cell[0][ROW_ROLL], 0, 1, 16, 14);
    cell(&p->cell[0][ROW_ALT], rnd(100) < 30, 1, 16, 6 + rnd(4));
    P[B_ALT] = ALT_OCTDN;
    P[B_COMP] = 30 + rnd(40);

    /* BRK2: the backbeat snare */
    P = p->param[1];
    P[B_BANK] = main_b;
    P[B_SLICE] = pick_tag(mb, TAG_SNARE, mb->nslices > 2 ? 2 : 0);
    P[B_MODE] = MODE_FIX;
    cell(&p->cell[1][ROW_HIT], 1, 2, 16, 4);
    cell(&p->cell[1][ROW_ROLL], rnd(100) < 65, 1, rnd(2) ? 16 : 32, rnd(2) ? 15 : 14);
    P[B_ROLL] = 1 + rnd(3);
    cell(&p->cell[1][ROW_ALT], rnd(100) < 50, 1, 32, 28 + rnd(3));
    P[B_ALT] = rnd(3) ? ALT_REV : ALT_OCTUP;
    P[B_COMP] = 20 + rnd(50);

    /* BRK3: ghosts and hats walking through a run of slices */
    P = p->param[2];
    P[B_BANK] = rnd(2) ? main_b : second;
    const Bank *b3 = &banks[P[B_BANK]];
    P[B_RANGE] = 3 + rnd(6);
    P[B_SLICE] = rnd(b3->nslices > P[B_RANGE] ? b3->nslices - P[B_RANGE] : 1);
    P[B_MODE] = rnd(2) ? MODE_WALK : MODE_RAND;
    cell(&p->cell[2][ROW_HIT], 1, 5 + rnd(6), 16, rnd(16));
    cell(&p->cell[2][ROW_ROLL], rnd(100) < 25, 1, 13, rnd(13));
    cell(&p->cell[2][ROW_ALT], rnd(100) < 60, 2 + rnd(2), 7 + rnd(7), rnd(7));
    P[B_ALT] = rnd(2) ? ALT_REV : ALT_NEXT;
    P[B_TONE] = rnd(2) ? 10 + rnd(20) : 0;
    P[B_BITS] = rnd(3) ? 24 : 8 + rnd(5);

    /* BRK4: the break running straight, with hits dropped out */
    P = p->param[3];
    P[B_BANK] = second;
    P[B_MODE] = MODE_GRID;
    static const int grid_hits[] = {16, 16, 14, 12, 11};
    cell(&p->cell[3][ROW_HIT], rnd(100) < 75, grid_hits[rnd(5)], 16, rnd(4));
    cell(&p->cell[3][ROW_ROLL], 0, 1, 16, 0);
    cell(&p->cell[3][ROW_ALT], rnd(100) < 40, 1 + rnd(2), 13 + rnd(4), rnd(13));
    P[B_ALT] = rnd(2) ? ALT_REV : ALT_OCTUP;
    P[B_TONE] = rnd(2) ? 0 : 5 + rnd(15);
    P[B_RATE] = rnd(3) ? 0 : 6 + rnd(8);

    /* BASS */
    P = p->param[V_BASS];
    int r = rnd(100);
    if (nbass && r < 35)
        P[S_WAVE] = NWAVE_SYNTH + rnd(nbass);
    else
        P[S_WAVE] = r < 70 ? WAVE_SUB : r < 85 ? WAVE_REESE : r < 93 ? WAVE_808 : WAVE_GROWL;
    static const int roots[] = {2, 4, 5, 7, 9};
    P[S_ROOT] = roots[rnd(5)];
    P[S_OCT] = 1;
    P[S_RIFF] = rnd(NRIFF);
    static const int bass_pats[][3] = {{1, 16, 0}, {2, 16, 0}, {3, 16, 0}, {2, 16, 6}, {3, 16, 2}, {2, 32, 0}};
    const int *bp = bass_pats[rnd(6)];
    cell(&p->cell[V_BASS][ROW_HIT], 1, bp[0], bp[1], bp[2]);
    cell(&p->cell[V_BASS][ROW_ROLL], 0, 1, 16, 0);
    cell(&p->cell[V_BASS][ROW_ALT], rnd(100) < 35, 1, 32, 8 + rnd(20));
    P[S_ALT] = rnd(2) ? BALT_SLIDE : BALT_OCTUP;
    P[S_LEN] = 6 + rnd(12);
    P[S_GLIDE] = rnd(40);
    P[S_CUT] = 35 + rnd(40);
    P[S_WIDTH] = 20 + rnd(60);
}

void song_clamp(void)
{
    Pattern *p = &song.pat;
    for (int v = 0; v < NV; v++) {
        for (int q = 0; q < nparam(v); q++) {
            int x = p->param[v][q];
            /* the bank first, so the slice range follows it */
            p->param[v][q] = clampi(x, param_min(v, q), param_max(v, q));
        }
        for (int r = 0; r < NROW; r++) {
            Cell *c = &p->cell[v][r];
            c->steps = (uint8_t)clampi(c->steps, 1, MAX_STEPS);
            c->hits = (uint8_t)clampi(c->hits, 0, c->steps);
            c->rot = (uint8_t)(c->rot % c->steps);
        }
    }
}

/* The default: the Amen chopped across the four channels over a sub */
void song_default(void)
{
    memset(&song, 0, sizeof song);
    song.magic = SONG_MAGIC;
    song.size = sizeof song;
    song.bpm = 170.f;
    for (int v = 0; v < NV; v++) {
        song.vol[v] = 80;
        song.pan[v] = 0;
        song.send[v] = 0;
    }
    song.vol[2] = 62;
    song.vol[3] = 58;
    song.pan[2] = -20;
    song.pan[3] = 20;
    song.send[1] = 18;
    song.send[2] = 10;
    song.vol[V_BASS] = 78;
    song.master = 80;
    song.glue = 35;
    song.master_drive = 10;
    song.verb_time = 45;
    song.verb_tone = 40;
    song.sweep_rate = 2;
    song.sync_mode = SYNC_AUTO;
    song.out_mode = OUT_STEREO;
    song.clk_side = 0;
    song.clk_ppqn = 1; /* 2 PPQN: Pocket Operators, Volcas */
    song.clk_width = 5;

    Pattern *p = &song.pat;
    defaults_for(p);
    if (!nbanks)
        return;
    int amen = find_bank("AMEN");
    int b = amen >= 0 ? amen : 0;
    const Bank *bk = &banks[b];
    int *P;

    P = p->param[0]; /* kicks */
    P[B_BANK] = b;
    P[B_SLICE] = bank_find_tag(bk, TAG_KICK, 0) >= 0 ? bank_find_tag(bk, TAG_KICK, 0) : 0;
    P[B_COMP] = 40;
    cell(&p->cell[0][ROW_HIT], 1, 3, 16, 0);
    cell(&p->cell[0][ROW_ROLL], 0, 1, 16, 14);
    cell(&p->cell[0][ROW_ALT], 0, 1, 16, 10);
    P[B_ALT] = ALT_OCTDN;

    P = p->param[1]; /* snare */
    P[B_BANK] = b;
    P[B_SLICE] = bank_find_tag(bk, TAG_SNARE, 0) >= 0 ? bank_find_tag(bk, TAG_SNARE, 0) : 0;
    P[B_COMP] = 45;
    P[B_ROLL] = 2;
    cell(&p->cell[1][ROW_HIT], 1, 2, 16, 4);
    cell(&p->cell[1][ROW_ROLL], 1, 1, 32, 30);
    cell(&p->cell[1][ROW_ALT], 1, 1, 32, 15);
    P[B_ALT] = ALT_REV;

    P = p->param[2]; /* ghost chops */
    P[B_BANK] = b;
    P[B_SLICE] = bk->nslices > 12 ? 8 : 0;
    P[B_MODE] = MODE_WALK;
    P[B_RANGE] = 5;
    P[B_TONE] = 18;
    P[B_BITS] = 12;
    cell(&p->cell[2][ROW_HIT], 1, 7, 16, 1);
    cell(&p->cell[2][ROW_ROLL], 0, 1, 13, 5);
    cell(&p->cell[2][ROW_ALT], 1, 2, 11, 3);
    P[B_ALT] = ALT_REV;

    P = p->param[3]; /* the break straight */
    P[B_BANK] = b;
    P[B_MODE] = MODE_GRID;
    P[B_RATE] = 8;
    cell(&p->cell[3][ROW_HIT], 1, 13, 16, 0);
    cell(&p->cell[3][ROW_ROLL], 0, 1, 16, 0);
    cell(&p->cell[3][ROW_ALT], 1, 1, 15, 7);
    P[B_ALT] = ALT_OCTUP;

    P = p->param[V_BASS];
    P[S_WAVE] = WAVE_SUB;
    P[S_ROOT] = 5; /* F */
    P[S_OCT] = 1;
    P[S_RIFF] = 2; /* DUB */
    P[S_LEN] = 6;
    P[S_GLIDE] = 25;
    cell(&p->cell[V_BASS][ROW_HIT], 1, 3, 16, 0);
    cell(&p->cell[V_BASS][ROW_ROLL], 0, 1, 16, 0);
    cell(&p->cell[V_BASS][ROW_ALT], 1, 1, 32, 22);
    P[S_ALT] = BALT_SLIDE;
    song_clamp();
}

/* ------------------------------------------------- pattern slots / disk */

void slot_save(int i)
{
    song.slot[i] = song.pat;
    song.slot_used[i] = 1;
    live.cur_slot = i;
}

void slot_load(int i)
{
    if (!song.slot_used[i])
        return;
    if (live.playing)
        live.queued_slot = i;
    else
        pattern_load_now(i);
}

void slot_clear(int i)
{
    song.slot_used[i] = 0;
    if (live.cur_slot == i)
        live.cur_slot = -1;
    if (live.queued_slot == i)
        live.queued_slot = -1;
}

/* Banks and bass samples are saved by name, so adding files to breaks/
 * or bass/ doesn't move a pattern onto other sounds. */
typedef struct {
    char bank[NV][8];
} Names;

static void names_of(const Pattern *p, Names *nm)
{
    memset(nm, 0, sizeof *nm);
    for (int v = 0; v < NBRK; v++)
        if (nbanks)
            memcpy(nm->bank[v], banks[clampi(p->param[v][B_BANK], 0, nbanks - 1)].name, 8);
    int w = p->param[V_BASS][S_WAVE];
    if (w >= NWAVE_SYNTH && w - NWAVE_SYNTH < nbass)
        memcpy(nm->bank[V_BASS], bass_smp[w - NWAVE_SYNTH].name, 8);
}

static void resolve(Pattern *p, const Names *nm)
{
    for (int v = 0; v < NBRK; v++) {
        int i = find_bank(nm->bank[v]);
        if (i >= 0)
            p->param[v][B_BANK] = i;
    }
    if (nm->bank[V_BASS][0]) {
        for (int i = 0; i < nbass; i++)
            if (!strcmp(bass_smp[i].name, nm->bank[V_BASS]))
                p->param[V_BASS][S_WAVE] = NWAVE_SYNTH + i;
    }
}

int song_save(const char *path)
{
    char tmp[512];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f)
        return 0;
    Names nm[1 + NSLOT];
    names_of(&song.pat, &nm[0]);
    for (int i = 0; i < NSLOT; i++)
        names_of(&song.slot[i], &nm[1 + i]);
    song.magic = SONG_MAGIC;
    song.size = sizeof song;
    int ok = fwrite(&song, sizeof song, 1, f) == 1 && fwrite(nm, sizeof nm, 1, f) == 1;
    ok &= fclose(f) == 0;
    if (ok)
        ok = rename(tmp, path) == 0;
    return ok;
}

int song_load(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    static Song s;
    Names nm[1 + NSLOT];
    uint32_t hdr[2] = {0, 0};
    int ok = fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == SONG_MAGIC && (hdr[1] == sizeof s || hdr[1] == SONG_SIZE_V1);
    memset(&s, 0, sizeof s);
    ok = ok && fseek(f, 0, SEEK_SET) == 0 && fread(&s, hdr[1], 1, f) == 1 && fread(nm, sizeof nm, 1, f) == 1;
    fclose(f);
    if (!ok)
        return 0;
    if (hdr[1] == SONG_SIZE_V1) { /* from v0.1: stereo out, the clock as its defaults */
        s.size = sizeof s;
        s.out_mode = OUT_STEREO, s.clk_side = 0, s.clk_ppqn = 1, s.clk_width = 5;
    }
    if (!(s.bpm >= 40.f && s.bpm <= 250.f))
        s.bpm = 170.f;
    resolve(&s.pat, &nm[0]);
    for (int i = 0; i < NSLOT; i++)
        resolve(&s.slot[i], &nm[1 + i]);
    song = s;
    song_clamp();
    for (int v = 0; v < NV; v++) {
        song.vol[v] = clampi(song.vol[v], 0, 100);
        song.pan[v] = clampi(song.pan[v], -100, 100);
        song.send[v] = clampi(song.send[v], 0, 100);
    }
    song.master = clampi(song.master, 0, 100);
    song.glue = clampi(song.glue, 0, 100);
    song.master_drive = clampi(song.master_drive, 0, 100);
    song.verb_time = clampi(song.verb_time, 0, 100);
    song.verb_tone = clampi(song.verb_tone, 0, 100);
    song.sweep_rate = clampi(song.sweep_rate, 0, 3);
    song.sync_mode = clampi(song.sync_mode, 0, NSYNCMODE - 1);
    song.out_mode = clampi(song.out_mode, 0, 1);
    song.clk_side = clampi(song.clk_side, 0, 1);
    song.clk_ppqn = clampi(song.clk_ppqn, 0, NPPQN - 1);
    song.clk_width = clampi(song.clk_width, 1, 20);
    return 1;
}

void engine_init(int sample_rate)
{
    sr = (float)sample_rate;
    breaks_set_rate(sample_rate);
    sps = sr * 15.f / 170.f;
    float scale = sr / 44100.f;
    for (int c = 0; c < 2; c++) {
        for (int i = 0; i < NCOMB; i++)
            line_init(&combs[c][i], (int)((comb_len[i] + c * 23) * scale));
        for (int i = 0; i < 2; i++)
            line_init(&aps[c][i], (int)((ap_len[i] + c * 23) * scale));
    }
    memset(brk, 0, sizeof brk);
    memset(&bass, 0, sizeof bass);
    for (int v = 0; v < NBRK; v++)
        live.play_pos[v] = -1.f;
    live.fx = -1;
    live.queued_slot = -1;
    live.cur_slot = -1;
}
