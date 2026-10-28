#include "aclock.h"

#include <math.h>

/* The device side and the engine side are each written by one thread (the
 * engine side also by setting changes made with the audio locked) and read
 * by the screen's thread, so each sits behind a sequence counter: odd while
 * it's being written, and a reader that sees it change reads again. */

typedef struct {
    double t0, t1, e2;   /* loop: smoothed start of this request and the next; period */
    double f0;           /* frames requested before this request, less any silence */
    int frames, ok;
} Dev;

typedef struct {
    double made;         /* frames made, ever */
    double pos, spf;     /* position after them, steps per frame */
    int ok;
} Made;

static Dev dev;
static Made mk;
static volatile unsigned dseq, mseq;
static double requested, silent;
static int rate_ = 48000, latency_;

static void wbegin(volatile unsigned *s)
{
    __atomic_store_n(s, *s + 1, __ATOMIC_RELAXED);
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

static void wend(volatile unsigned *s) { __atomic_store_n(s, *s + 1, __ATOMIC_RELEASE); }

void aclock_init(int rate, int latency_frames)
{
    wbegin(&dseq);
    wbegin(&mseq);
    rate_ = rate > 0 ? rate : 48000;
    latency_ = latency_frames > 0 ? latency_frames : 0;
    dev.ok = 0;
    mk.ok = 0;
    mk.made = 0;
    requested = silent = 0;
    wend(&mseq);
    wend(&dseq);
}

void aclock_request(uint64_t now_us, int frames)
{
    double t = (double)now_us, per = frames * 1e6 / rate_;
    Dev d = dev;
    /* the Miyoo's driver asks for two buffers back to back, then waits: the
     * loop smooths that into an even beat (a quarter-hertz bandwidth) */
    double w = 6.283185307179586 * 0.25 * per * 1e-6, b = sqrt(2.0) * w, c = w * w;
    if (!d.ok || frames != d.frames || fabs(t - d.t1) > 8.0 * per + 100000.0) {
        d.t0 = t; /* first request, or the sound stopped: start over */
        d.t1 = t + per;
        d.e2 = per;
        d.frames = frames;
        d.ok = 1;
    }
    else {
        double e = t - d.t1;
        d.t0 = d.t1;
        d.t1 += b * e + d.e2;
        d.e2 += c * e;
    }
    d.f0 = requested - silent;
    requested += frames;
    wbegin(&dseq);
    dev = d;
    wend(&dseq);
}

void aclock_silence(int frames)
{
    wbegin(&dseq);
    silent += frames;
    dev.f0 -= frames;
    wend(&dseq);
}

void aclock_made(int frames, double pos_end, double steps_per_frame)
{
    wbegin(&mseq);
    mk.made += frames;
    mk.pos = pos_end;
    mk.spf = steps_per_frame;
    mk.ok = 1;
    wend(&mseq);
}

void aclock_shift(double steps)
{
    wbegin(&mseq);
    mk.pos += steps;
    wend(&mseq);
}

int aclock_position(uint64_t t_us, double *pos)
{
    Dev d;
    Made m;
    unsigned s;
    do {
        s = __atomic_load_n(&dseq, __ATOMIC_ACQUIRE);
        d = dev;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    } while ((s & 1) || s != __atomic_load_n(&dseq, __ATOMIC_RELAXED));
    do {
        s = __atomic_load_n(&mseq, __ATOMIC_ACQUIRE);
        m = mk;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
    } while ((s & 1) || s != __atomic_load_n(&mseq, __ATOMIC_RELAXED));
    if (!d.ok || !m.ok || d.t1 <= d.t0)
        return 0;
    double x = ((double)t_us - d.t0) / (d.t1 - d.t0); /* requests since this one */
    if (x > 25.0 || x < -25.0)
        return 0; /* the sound has stopped, or the time is far off */
    double heard = d.f0 + x * d.frames - latency_;
    *pos = m.pos - (m.made - heard) * m.spf;
    return 1;
}
