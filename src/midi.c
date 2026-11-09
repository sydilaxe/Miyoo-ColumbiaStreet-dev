/* The clock in (see midi.h), the TrimUI version's midi.c without the ports,
 * notes, CCs or clock out, and without its SONG page MIDI settings: this one
 * always follows, START and STOP included. Changes to the sequencer go
 * through ui_lock, as the buttons' do. */
#define _GNU_SOURCE
#include "midi.h"
#include "aclock.h"
#include "engine.h"
#include "ui.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

static volatile float latency_s = 0.02f;

void midi_set_latency(float seconds) { latency_s = seconds < 0.f ? 0.f : seconds; }

/* ------------------------------------------------------------- notices */

static pthread_mutex_t nmx = PTHREAD_MUTEX_INITIALIZER;
static char notices[8][48];
static int n_head, n_tail;

void midi_notice(const char *fmt, const char *arg)
{
    pthread_mutex_lock(&nmx);
    if (n_head - n_tail < 8) {
        snprintf(notices[n_head % 8], sizeof notices[0], fmt, arg);
        n_head++;
    }
    pthread_mutex_unlock(&nmx);
}

int midi_take_notice(char *buf, int n)
{
    int got = 0;
    pthread_mutex_lock(&nmx);
    if (n_tail < n_head) {
        snprintf(buf, (size_t)n, "%s", notices[n_tail % 8]);
        n_tail++;
        got = 1;
    }
    pthread_mutex_unlock(&nmx);
    return got;
}

/* engine_position read without the engine's lock: two variables, so a read
 * may catch them mid-change; the middle of three is always whole */
static double position(void)
{
    double a = engine_position(), b = engine_position(), c = engine_position();
    if (a > b) {
        double t = a;
        a = b, b = t;
    }
    return c < a ? a : c > b ? b : c;
}

static double heard_now(uint64_t t)
{
    double h;
    return aclock_position(t, &h) ? h : position() - latency_s * song.bpm / 15.0;
}

/* ------------------------------------------------------------ clock in */

static uint64_t tick_us;
static double tick_avg_us;
static int64_t ticks; /* since START (or the song position) */
static volatile int following;
static int ext_running;
static int settle; /* clocks after START before the phase is held: until the first step has played */
static uint64_t precise_us; /* the last position from a leading Brick: its tempo and phase win over the ticks' */
static double pll_i;

int midi_following(void) { return following; }

/* the last ticks' times, for the tempo: a straight line through four beats
 * of them, which averages out the jitter of USB and the thread waking */
#define TK 96
static uint64_t tk_t[TK];
static int tk_n, tk_w;

static void clock_tick(uint64_t t)
{
    if (tick_us && (t <= tick_us || t - tick_us >= 250000))
        tk_n = 0; /* a gap: the fit starts afresh */
    tk_t[tk_w] = t;
    tk_w = (tk_w + 1) % TK;
    if (tk_n < TK)
        tk_n++;
    if (tk_n >= 2) {
        uint64_t t0 = tk_t[(tk_w - tk_n + TK) % TK];
        double sx = 0.0, sy = 0.0, sxx = 0.0, sxy = 0.0, n = tk_n;
        for (int i = 0; i < tk_n; i++) {
            double y = (double)(tk_t[(tk_w - tk_n + i + TK) % TK] - t0);
            sx += i, sy += y, sxx += (double)i * i, sxy += i * y;
        }
        double slope = (n * sxy - sx * sy) / (n * sxx - sx * sx);
        if (slope > 0.0)
            tick_avg_us = slope;
    }
    tick_us = t;
    if (tick_avg_us <= 0.0)
        return;
    following = 1;
    if (precise_us && t - precise_us < 1000000) { /* a leading Brick's positions hold the tempo and phase */
        ticks++;
        return;
    }
    /* the tempo: set when it has really changed (the phase lock below takes
     * up what is left), quickly while the fit is young */
    float bpm = (float)(60e6 / (tick_avg_us * 24.0));
    bpm = bpm < 40.f ? 40.f : bpm > 250.f ? 250.f : bpm;
    if (fabsf(bpm - song.bpm) > (tk_n < 24 ? 0.05f : 0.15f))
        song.bpm = roundf(bpm * 100.f) / 100.f;
    if (!ext_running || !live.playing)
        return;
    ticks++;
    if (settle > 0) {
        settle--;
        return;
    }
    /* the phase: what is heard (the audio clock) should be where the clock
     * is; trimmed gently, jumped only when far off */
    double err = ticks / 6.0 - heard_now(t);
    if (fabs(err) > 2.0) {
        engine_set_position(position() + err);
        engine_set_rate(1.0);
        pll_i = 0.0;
    }
    else {
        pll_i = fmax(-0.004, fmin(0.004, pll_i + err * 0.002));
        engine_set_rate(1.0 + fmax(-0.02, fmin(0.02, err * 0.05 + pll_i)));
    }
}

static void realtime(uint8_t b, uint64_t t)
{
    switch (b) {
    case 0xf8: clock_tick(t); break;
    case 0xfa: /* START: from the top */
        ticks = 0;
        ext_running = 1;
        settle = 6;
        engine_set_rate(1.0);
        engine_set_playing(0); /* the first step plays now */
        engine_set_playing(1);
        break;
    case 0xfb: /* CONTINUE: from the song position */
        ext_running = 1;
        engine_set_playing(1);
        if (ticks > 0)
            engine_set_position(ticks / 6.0 + latency_s * song.bpm / 15.0);
        else
            settle = 6;
        break;
    case 0xfc: /* STOP */
        ext_running = 0;
        engine_set_rate(1.0);
        engine_set_playing(0);
        break;
    }
}

/* ------------------------------------------------- the leader's position */

/* A leading Brick's position over the USB clock link, about 50 times a
 * second: F0 7D 'T' 'L', where its sound is (the 16ths heard at the moment
 * it was sent, x 65536, five 7-bit bytes, low first), its exact tempo (BPM x
 * 1000, three bytes) and whether it plays, F7. The follower turns its own
 * heard position (the audio clock, aclock) toward it: a jump when more than
 * half a 16th off, else a slight change of speed. */
#define LINK_TRANSIT_S 0.0003 /* a bulk transfer through a hub and the follower's thread waking */
int midi_link_pos(uint8_t *out, int max, double heard, double bpm, int playing)
{
    if (max < 14)
        return 0;
    uint64_t p = heard > 0.0 ? (uint64_t)llround(heard * 65536.0) : 0;
    uint32_t b = (uint32_t)lround(bpm * 1000.0);
    int n = 0;
    out[n++] = 0xf0, out[n++] = 0x7d, out[n++] = 'T', out[n++] = 'L';
    for (int k = 0; k < 5; k++)
        out[n++] = (uint8_t)(p >> (7 * k) & 127);
    for (int k = 0; k < 3; k++)
        out[n++] = (uint8_t)(b >> (7 * k) & 127);
    out[n++] = playing ? 1 : 0;
    out[n++] = 0xf7;
    return n;
}

static void link_position(uint64_t t, double lead, double bpm, int lead_playing)
{
    precise_us = t;
    following = 1;
    if (bpm >= 40.0 && bpm <= 250.0 && fabs(bpm - song.bpm) > 1e-4)
        song.bpm = (float)bpm;
    if (!live.playing) {
        if (lead_playing) { /* joined mid-song: in at the leader's place */
            engine_set_playing(1);
            engine_set_position(lead + (LINK_TRANSIT_S + latency_s) * bpm / 15.0);
            ext_running = 1;
            pll_i = 0.0;
        }
        return;
    }
    if (!lead_playing) { /* the leader stopped (a leader passing on another's clock sends no STOP) */
        ext_running = 0;
        engine_set_rate(1.0);
        engine_set_playing(0);
        return;
    }
    double err = lead + LINK_TRANSIT_S * bpm / 15.0 - heard_now(t);
    if (fabs(err) > 0.5) {
        engine_set_position(position() + err);
        engine_set_rate(1.0);
        pll_i = 0.0;
        return;
    }
    pll_i = fmax(-0.003, fmin(0.003, pll_i + err * 0.02));
    engine_set_rate(1.0 + fmax(-0.02, fmin(0.02, err * 0.4 + pll_i)));
}

/* ------------------------------------------------------------- parsing */

static uint8_t status, data[2];
static int ndata, in_sysex;
static uint8_t sx[16]; /* a SysEx being read: only the Brick link's position is wanted */
static int nsx;

static void sysex_end(uint64_t t)
{
    if (nsx != 12 || sx[0] != 0x7d || sx[1] != 'T' || sx[2] != 'L')
        return;
    uint64_t p = 0;
    uint32_t b = 0;
    for (int k = 0; k < 5; k++)
        p |= (uint64_t)sx[3 + k] << (7 * k);
    for (int k = 0; k < 3; k++)
        b |= (uint32_t)sx[8 + k] << (7 * k);
    link_position(t, p / 65536.0, b / 1000.0, sx[11] & 1);
}

static int data_len(uint8_t st)
{
    switch (st & 0xf0) {
    case 0xc0:
    case 0xd0: return 1;
    case 0xf0:
        return st == 0xf2 ? 2 : st == 0xf1 || st == 0xf3 ? 1 : 0;
    default: return 2;
    }
}

static void message(void)
{
    if (status == 0xf2) { /* song position, in 16ths */
        ticks = (int64_t)((data[1] << 7) | data[0]) * 6;
        if (live.playing)
            engine_set_position(ticks / 6.0 + latency_s * song.bpm / 15.0);
    }
}

static void feed(const uint8_t *b, int n, uint64_t t)
{
    for (int i = 0; i < n; i++) {
        uint8_t c = b[i];
        if (c >= 0xf8) { /* real time: anywhere, even inside another message */
            realtime(c, t);
            continue;
        }
        if (c == 0xf0) {
            in_sysex = 1;
            nsx = 0;
            continue;
        }
        if (c == 0xf7) {
            if (in_sysex)
                sysex_end(t);
            in_sysex = 0;
            continue;
        }
        if (c & 0x80) {
            in_sysex = 0;
            status = c;
            ndata = 0;
            if (data_len(c) == 0) {
                message();
                status = 0;
            }
            continue;
        }
        if (in_sysex) {
            if (nsx < (int)sizeof sx)
                sx[nsx++] = c;
            else
                nsx = (int)sizeof sx + 1; /* too long: not ours */
            continue;
        }
        if (!status)
            continue;
        data[ndata++] = c;
        if (ndata >= data_len(status)) {
            message();
            ndata = 0; /* running status: the same kind again */
            if (status >= 0xf0)
                status = 0;
        }
    }
}

void midi_feed(const uint8_t *b, int n, uint64_t t)
{
    ui_lock();
    feed(b, n, t);
    ui_unlock();
}

void midi_reset(void)
{
    tick_us = 0;
    tick_avg_us = 0.0;
    ticks = 0;
    following = ext_running = settle = 0;
    status = 0, ndata = 0, in_sysex = 0, nsx = 0;
    precise_us = 0, pll_i = 0.0;
    tk_n = tk_w = 0;
}
