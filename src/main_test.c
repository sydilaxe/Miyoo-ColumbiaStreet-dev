/* Headless host harness: plays a scripted button sequence over the real
 * samples, writes a WAV of the audio and a screenshot of each page, then
 * checks the engine: straight break playback, bit depth, compressor,
 * pattern generation, save / load and DSP time.
 *
 * usage: cs_test OUTDIR [APP_DIR]   (APP_DIR holds breaks/ and bass/) */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "breaks.h"
#include "engine.h"
#include "gfx.h"
#include "sync.h"
#include "ui.h"
#include "update.h"

#define RATE 48000

typedef struct {
    int ms, btn, down;
    const char *shot;
} Ev;

#define PRESS(t, b) {t, b, 1, NULL}, {t + 60, b, 0, NULL}
#define SHOT(t, name) {t, -1, 0, name}

static const Ev script[] = {
    PRESS(0, BTN_START),
    SHOT(3000, "pattern"),
    /* punch-in effects: repeat 1/16, reverse, tape stop */
    {4000, BTN_R2, 1, NULL},
    {4050, BTN_RIGHT, 1, NULL},
    SHOT(4600, "fx_repeat"),
    {5200, BTN_RIGHT, 0, NULL},
    {5300, BTN_LEFT, 1, NULL},
    {6300, BTN_LEFT, 0, NULL},
    {6400, BTN_DOWN, 1, NULL},
    {7600, BTN_DOWN, 0, NULL},
    {7650, BTN_R2, 0, NULL},
    /* L2 sweep */
    {8000, BTN_L2, 1, NULL},
    SHOT(9000, "filter"),
    {10000, BTN_L2, 0, NULL},
    /* pattern edits: BRK3 ALT cell, more hits, B+right rotates, B tap re-rolls */
    PRESS(10500, BTN_DOWN), PRESS(10600, BTN_DOWN), PRESS(10700, BTN_RIGHT), PRESS(10800, BTN_RIGHT),
    PRESS(10900, BTN_X),
    {11000, BTN_B, 1, NULL}, PRESS(11050, BTN_RIGHT), {11200, BTN_B, 0, NULL},
    SHOT(11400, "pattern_edit"),
    /* CHOP page: BRK2, bits down to 6, compressor up, filter */
    PRESS(12000, BTN_R1), PRESS(12100, BTN_RIGHT),
    PRESS(12200, BTN_UP), PRESS(12300, BTN_UP), PRESS(12400, BTN_UP), /* -> BITS */
    {12500, BTN_Y, 1, NULL}, {13600, BTN_Y, 0, NULL},
    PRESS(13700, BTN_DOWN), PRESS(13800, BTN_DOWN), /* -> COMP */
    {13900, BTN_X, 1, NULL}, {15200, BTN_X, 0, NULL},
    SHOT(16000, "chop"),
    /* BASS page: riff up */
    PRESS(16500, BTN_R1), PRESS(16600, BTN_DOWN), PRESS(16700, BTN_DOWN), PRESS(16800, BTN_DOWN),
    PRESS(16900, BTN_X),
    SHOT(18000, "bass"),
    /* MIX page: master glue up */
    PRESS(18500, BTN_R1), PRESS(18600, BTN_LEFT), PRESS(18700, BTN_DOWN),
    PRESS(18800, BTN_X), PRESS(18900, BTN_X),
    SHOT(20000, "mix"),
    /* SONG page: save to A, a new beat, save to B, queue A */
    PRESS(20500, BTN_R1), PRESS(20600, BTN_X),
    PRESS(21000, BTN_SELECT), PRESS(21100, BTN_RIGHT), PRESS(21200, BTN_X),
    PRESS(21400, BTN_LEFT), PRESS(21500, BTN_A),
    SHOT(21600, "song"),
    /* the generated beat on the pattern page */
    PRESS(23000, BTN_R1),
    PRESS(23200, BTN_SELECT),
    SHOT(25000, "pattern_generated"),
    PRESS(29900, BTN_START),
};
#define NSCRIPT ((int)(sizeof script / sizeof script[0]))
#define TOTAL_MS 30000

static void put32(FILE *f, uint32_t v) { fwrite(&v, 4, 1, f); }
static void put16(FILE *f, uint16_t v) { fwrite(&v, 2, 1, f); }

static void write_wav(const char *path, const int16_t *pcm, int frames)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fwrite("RIFF", 1, 4, f);
    put32(f, 36 + frames * 4);
    fwrite("WAVEfmt ", 1, 8, f);
    put32(f, 16);
    put16(f, 1);
    put16(f, 2);
    put32(f, RATE);
    put32(f, RATE * 4);
    put16(f, 4);
    put16(f, 16);
    fwrite("data", 1, 4, f);
    put32(f, frames * 4);
    fwrite(pcm, 4, frames, f);
    fclose(f);
}

static void write_ppm(const char *path, const uint32_t *fb)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return;
    fprintf(f, "P6\n%d %d\n255\n", SCREEN_W, SCREEN_H);
    for (int i = 0; i < SCREEN_W * SCREEN_H; i++) {
        uint8_t px[3] = {(uint8_t)(fb[i] >> 16), (uint8_t)(fb[i] >> 8), (uint8_t)fb[i]};
        fwrite(px, 1, 3, f);
    }
    fclose(f);
}

static uint32_t fb[SCREEN_W * SCREEN_H];

/* silence every channel but v, with plain settings */
static void solo_channel(int v)
{
    for (int i = 0; i < NV; i++) {
        song.pat.mute[i] = i != v;
        song.solo[i] = 0;
        for (int r = 0; r < NROW; r++)
            song.pat.cell[i][r].on = 0;
    }
    song.send[v] = 0;
    song.pan[v] = 0;
    song.vol[v] = 100;
    song.master = 77; /* gain = 0.77^2 * 1.3 ~ 0.77 */
    song.master_drive = 0;
    song.glue = 0;
    song.verb_time = 0;
}

static double corr(const float *a, const float *b, int n)
{
    double ab = 0, aa = 1e-12, bb = 1e-12;
    for (int i = 0; i < n; i++) {
        ab += a[i] * b[i];
        aa += a[i] * a[i];
        bb += b[i] * b[i];
    }
    return ab / sqrt(aa * bb);
}

/* render n frames to a mono float buffer (left channel) */
static UpdateStatus fake_update; /* the update prompt, as if v9.9.9 were out */
static void fake_install(void)
{
    fake_update.state = UP_INSTALLING;
    fake_update.progress = 42;
}

static void render_mono(float *dst, int n)
{
    static int16_t buf[2048 * 2];
    for (int done = 0; done < n;) {
        int k = n - done < 1024 ? n - done : 1024;
        engine_render(buf, k);
        for (int i = 0; i < k; i++)
            dst[done + i] = buf[2 * i] / 32768.f;
        done += k;
    }
}

int main(int argc, char **argv)
{
    const char *out = argc > 1 ? argv[1] : "build/test";
    const char *app = argc > 2 ? argv[2] : "dist/ColumbiaStreet";
    char path[512];

    engine_init(RATE);
    snprintf(path, sizeof path, "%s/breaks", app);
    int nb = breaks_load_dir(path);
    snprintf(path, sizeof path, "%s/bass", app);
    int ns = bass_load_dir(path);
    if (!nbanks)
        breaks_fallback();
    printf("samples: %d banks, %d bass\n", nb, ns);
    for (int i = 0; i < nbanks; i++)
        printf("bank %-6s %5.1f BPM %2d steps %2d slices (kick %d, snare %d, hat %d)\n", banks[i].name, banks[i].bpm,
               banks[i].steps, banks[i].nslices, bank_count_tag(&banks[i], TAG_KICK),
               bank_count_tag(&banks[i], TAG_SNARE), bank_count_tag(&banks[i], TAG_HAT));

    /* ---- the scripted session */
    song_default();
    ui_init();
    sync_core_init(1);
    int frames = TOTAL_MS * RATE / 1000;
    int16_t *pcm = malloc(frames * 4);
    int ev = 0, block = RATE / 100; /* 10 ms */
    double dsp_s = 0.0;
    for (int ms = 0, pos = 0; ms < TOTAL_MS; ms += 10, pos += block) {
        while (ev < NSCRIPT && script[ev].ms <= ms) {
            const Ev *e = &script[ev++];
            if (e->shot) {
                ui_draw(fb);
                snprintf(path, sizeof path, "%s/%s.ppm", out, e->shot);
                write_ppm(path, fb);
            }
            else
                ui_button(e->btn, e->down);
        }
        ui_tick(10);
        clock_t t0 = clock();
        engine_render(pcm + pos * 2, block);
        dsp_s += (double)(clock() - t0) / CLOCKS_PER_SEC;
    }
    snprintf(path, sizeof path, "%s/session.wav", out);
    write_wav(path, pcm, frames);
    int full = 0;
    double rms = 0;
    for (int i = 0; i < frames * 2; i++) {
        full += pcm[i] >= 32767 || pcm[i] <= -32768;
        rms += (double)pcm[i] * pcm[i];
    }
    rms = sqrt(rms / (frames * 2)) / 32768.0;
    printf("session: %.0f s, rms %.3f, %d full-scale samples (want 0)\n", TOTAL_MS / 1000.0, rms, full);
    printf("dsp: %.1f%% of real time on this host\n", 100.0 * dsp_s / (TOTAL_MS / 1000.0));
    free(pcm);

    /* ---- GRID with a full HIT cell at the break's own tempo plays it straight */
    int amen = -1;
    for (int i = 0; i < nbanks; i++)
        if (!strcmp(banks[i].name, "AMEN"))
            amen = i;
    int gb = amen >= 0 ? amen : 0;
    const Bank *b = &banks[gb];
    engine_set_playing(0);
    song_default();
    solo_channel(0);
    int *P = song.pat.param[0];
    P[B_BANK] = gb;
    P[B_MODE] = MODE_GRID;
    P[B_FIT] = 1;
    P[B_LEN] = 100;
    P[B_COMP] = 0;
    song.pat.cell[0][ROW_HIT] = (Cell){.on = 1, .steps = 16, .hits = 16, .rot = 0};
    song.bpm = b->bpm;
    {
        /* every slice plays once, and the first matches the loop exactly */
        float *y = malloc(b->len * sizeof(float));
        int seen[MAX_SLICES] = {0}, played = 0, last = -1;
        static int16_t buf[512];
        engine_set_playing(1);
        for (int i = 0; i < b->len; i += 256) {
            int k = b->len - i < 256 ? b->len - i : 256;
            engine_render(buf, k);
            for (int j = 0; j < k; j++)
                y[i + j] = buf[2 * j] / 32768.f;
            if (live.slice[0] != last) {
                last = live.slice[0];
                played += !seen[last];
                seen[last] = 1;
            }
        }
        engine_set_playing(0);
        double c = corr(y + 64, b->data + 64, b->start[1] - 128);
        /* slices sharing a step play as one */
        int want = 0;
        for (int i = 0; i < b->nslices; i++)
            want += i == 0 || floorf(b->at[i] + 0.5f) != floorf(b->at[i - 1] + 0.5f);
        printf("grid straight: %s at %.0f BPM, %d of %d steps with onsets played, first slice correlation %.3f "
               "(want all and over 0.99: %d)\n",
               b->name, b->bpm, played, want, c, played == want && c > 0.99);
        free(y);
    }

    /* ---- FIT: at double the tempo the same slice plays twice as fast */
    {
        song.bpm = b->bpm * 2.f;
        P[B_MODE] = MODE_FIX;
        P[B_SLICE] = 0;
        song.pat.cell[0][ROW_HIT] = (Cell){.on = 1, .steps = 16, .hits = 1, .rot = 0};
        int n = b->start[1] - b->start[0];
        float *y = malloc(n * sizeof(float));
        engine_set_playing(1);
        render_mono(y, n);
        engine_set_playing(0);
        int last = 0;
        for (int i = 0; i < n; i++)
            if (fabsf(y[i]) > 1e-4f)
                last = i;
        printf("fit: slice of %d samples played in %d at x2 tempo (want about half: %d)\n", n, last,
               last > n * 0.4 && last < n * 0.6);
        free(y);
    }

    /* ---- bit depth: a 4-bit slice has at most 16 levels, 1 bit three */
    song.bpm = 170.f;
    for (int bits = 1; bits <= 24; bits += bits < 4 ? 3 : bits < 16 ? 4 : 8) {
        P[B_BITS] = bits;
        P[B_SLICE] = 0;
        engine_set_playing(0);
        float *y = malloc(RATE / 4 * sizeof(float));
        engine_audition(0);
        render_mono(y, RATE / 4);
        int levels = 0;
        float seen[4096];
        for (int i = 0; i < RATE / 4 && levels < 4096; i++) {
            int dup = 0;
            for (int k = 0; k < levels && !dup; k++)
                dup = seen[k] == y[i];
            if (!dup)
                seen[levels++] = y[i];
        }
        printf("bits %2d: %d output levels\n", bits, levels);
        free(y);
    }
    P[B_BITS] = 24;

    /* ---- compressor: COMP 100 pulls the peak down and reports reduction */
    {
        float peak[2], gr = 0.f;
        for (int k = 0; k < 2; k++) {
            P[B_COMP] = k ? 100 : 0;
            float *y = malloc(RATE / 2 * sizeof(float));
            engine_audition(0);
            for (int i = 0; i < RATE / 2; i += 1024) {
                render_mono(y + i, RATE / 2 - i < 1024 ? RATE / 2 - i : 1024);
                if (k && live.gr[0] > gr)
                    gr = live.gr[0];
            }
            float rmsv = 0.f;
            for (int i = 0; i < RATE / 2; i++)
                rmsv += y[i] * y[i];
            peak[k] = sqrtf(rmsv / (RATE / 2));
            free(y);
        }
        printf("comp: rms %.3f -> %.3f with COMP 100, up to %.1f dB gain reduction (want over 6: %d)\n", peak[0],
               peak[1], gr, gr > 6.f);
        P[B_COMP] = 0;
    }

    /* ---- generator: in range, and different seeds differ */
    {
        Pattern a, c;
        int same = 0, bad = 0;
        for (unsigned s = 1; s < 200; s++) {
            pattern_generate(&a, s);
            pattern_generate(&c, s + 1000);
            same += !memcmp(&a, &c, sizeof a);
            song.pat = a;
            Pattern before = song.pat;
            song_clamp();
            bad += memcmp(&before, &song.pat, sizeof before) != 0;
        }
        printf("generator: 199 seeds, %d identical pairs, %d out of range (want 0 0)\n", same, bad);
    }

    /* ---- save and load keep the pattern, banks by name */
    {
        song_default();
        song.pat.param[2][B_BITS] = 7;
        song.bpm = 163.5f;
        slot_save(3);
        snprintf(path, sizeof path, "%s/test.sav", out);
        int ok = song_save(path);
        Song before = song;
        song_default();
        ok &= song_load(path);
        printf("save/load: %d, same %d, bpm %.1f, bits %d, slot D used %d\n", ok,
               !memcmp(&before.pat, &song.pat, sizeof song.pat), song.bpm, song.pat.param[2][B_BITS],
               song.slot_used[3]);
    }

    /* ---- a v0.1 save (without the output settings) still loads, in stereo */
    {
        song_default();
        song.bpm = 151.f;
        song.out_mode = OUT_MONO_CLOCK;
        song_save(path);
        FILE *f = fopen(path, "rb");
        static unsigned char buf[sizeof(Song) + 4096];
        size_t got = fread(buf, 1, sizeof buf, f);
        fclose(f);
        uint32_t v1 = (uint32_t)offsetof(Song, out_mode);
        memcpy(buf + 4, &v1, 4);
        memmove(buf + v1, buf + sizeof(Song), got - sizeof(Song)); /* drop the new fields */
        char p1[600];
        snprintf(p1, sizeof p1, "%s/v01.sav", out);
        f = fopen(p1, "wb");
        fwrite(buf, 1, got - (sizeof(Song) - v1), f);
        fclose(f);
        song_default();
        int ok = song_load(p1);
        printf("save compat v0.1: %d, bpm %.0f, output %d, ppqn %d, pulse %d\n", ok, song.bpm, song.out_mode,
               clk_ppqn_value(song.clk_ppqn), song.clk_width);
    }

    /* ---- mono + clock: the pulses' count, spacing and width on the clock
     * side, nothing but pulses there, and the sound on the other side */
    {
        int16_t *pc = malloc(sizeof(int16_t) * 2 * 48000 * 4);
        static const int idx[4] = {1, 5, 6, 1}; /* 2, 24, 32 PPQN; the last on the left */
        for (int k = 0; k < 4; k++) {
            song_default();
            song.bpm = 120.f;
            song.pan[0] = -100, song.pan[1] = 100;
            song.out_mode = OUT_MONO_CLOCK;
            song.clk_ppqn = idx[k];
            song.clk_side = k == 3;
            engine_set_playing(0);
            engine_render(pc, 4800);
            engine_set_playing(1);
            engine_render(pc, 48000 * 4);
            int cs = k == 3 ? 0 : 1, edges = 0, prev = 0, width = 0, last = -1, other = 0;
            double gaps = 0;
            float apk = 0.f;
            for (int i = 0; i < 48000 * 4; i++) {
                int c = pc[2 * i + cs], hi = c > 16000;
                other += c != 0 && c != 31128;
                if (hi && !prev) {
                    if (last >= 0)
                        gaps += i - last;
                    last = i, edges++;
                }
                if (hi && edges == 1)
                    width++;
                prev = hi;
                if (abs(pc[2 * i + 1 - cs]) > apk)
                    apk = (float)abs(pc[2 * i + 1 - cs]);
            }
            printf("clock out: %d PPQN at 120 BPM on the %s -> %d pulses in 4 s, mean gap %.2f ms, width %.2f ms, "
                   "other values %d, sound peak %.2f\n",
                   clk_ppqn_value(idx[k]), cs ? "right" : "left", edges, gaps / (edges - 1) * 1000.0 / 48000,
                   width * 1000.0 / 48000, other, apk / 32768.f);
        }
        engine_set_playing(0);
        engine_render(pc, 24000);
        int any = 0;
        for (int i = 0; i < 24000; i++)
            any += pc[2 * i] > 16000; /* the last pass had the clock on the left */
        printf("clock out stopped: pulses %d (want 0)\n", any);
        free(pc);
        song_default();
    }

    /* ---- every bass voice sounds */
    {
        song_default();
        solo_channel(V_BASS);
        int quiet = 0;
        for (int w = 0; w < NWAVE_SYNTH + nbass; w++) {
            song.pat.param[V_BASS][S_WAVE] = w;
            song.pat.param[V_BASS][S_LEN] = 8;
            float *y = malloc(RATE / 4 * sizeof(float));
            engine_audition(V_BASS);
            render_mono(y, RATE / 4);
            float pk = 0.f;
            for (int i = 0; i < RATE / 4; i++)
                pk = fmaxf(pk, fabsf(y[i]));
            quiet += pk < 0.05f;
            free(y);
            for (int k = 0; k < 4; k++) { /* let it ring out */
                static int16_t tmp[4096];
                engine_render(tmp, 2048);
            }
        }
        printf("bass voices: %d, %d silent (want 0)\n", NWAVE_SYNTH + nbass, quiet);
    }

    /* ---- SELECT keeps the mutes and the mixer; so does its undo */
    {
        song_default();
        song.pat.mute[1] = song.pat.mute[V_BASS] = 1;
        song.solo[2] = 1;
        song.vol[0] = 33;
        song.pan[3] = -45;
        song.send[1] = 61;
        song.master = 71;
        song.glue = 12;
        song.master_drive = 27;
        song.verb_time = 64;
        song.verb_tone = 22;
        song.sweep_rate = 1;
        song.out_mode = 1;
        song.clk_ppqn = 5;
        Song mix = song;
        Pattern before = song.pat;
        ui_button(BTN_SELECT, 1);
        ui_button(BTN_SELECT, 0);
        int new_beat = memcmp(song.pat.cell, before.cell, sizeof before.cell) != 0;
        int mutes = !memcmp(song.pat.mute, mix.pat.mute, sizeof mix.pat.mute);
        int mixer = !memcmp(song.vol, mix.vol, sizeof mix.vol) && !memcmp(song.pan, mix.pan, sizeof mix.pan) &&
                    !memcmp(song.send, mix.send, sizeof mix.send) && !memcmp(song.solo, mix.solo, sizeof mix.solo) &&
                    song.master == mix.master && song.glue == mix.glue && song.master_drive == mix.master_drive &&
                    song.verb_time == mix.verb_time && song.verb_tone == mix.verb_tone &&
                    song.sweep_rate == mix.sweep_rate && song.bpm == mix.bpm && song.out_mode == mix.out_mode &&
                    song.clk_side == mix.clk_side && song.clk_ppqn == mix.clk_ppqn && song.clk_width == mix.clk_width;
        /* unmute one on the new beat, then undo: the undo keeps that too */
        song.pat.mute[1] = 0;
        ui_button(BTN_R2, 1);
        ui_button(BTN_SELECT, 1);
        ui_button(BTN_SELECT, 0);
        ui_button(BTN_R2, 0);
        int undone = !memcmp(song.pat.cell, before.cell, sizeof before.cell);
        int undo_mutes = song.pat.mute[1] == 0 && song.pat.mute[V_BASS] == 1;
        printf("select: new beat %d, mutes kept %d, mixer kept %d; undo back %d, mutes kept %d\n", new_beat, mutes,
               mixer, undone, undo_mutes);
    }

    /* ---- sync: a leader's packet sets our tempo */
    {
        song_default();
        song.sync_mode = SYNC_AUTO;
        sync_core_init(100);
        uint8_t pkt[SYNC_PKT_LEN];
        float save_bpm = song.bpm;
        song.bpm = 140.f;
        song.sync_mode = SYNC_LEAD;
        sync_core_init(5);
        sync_make_packet(pkt, 1000);
        song.bpm = save_bpm;
        song.sync_mode = SYNC_AUTO;
        sync_core_init(100);
        sync_handle_packet(pkt, sizeof pkt, 2000);
        printf("sync: bpm=%.1f following=%d\n", song.bpm, sync_status()->have_leader);
    }

    /* ---- the update prompt: A starts the install; the B that closes it doesn't roll a rhythm */
    {
        song_default();
        ui_set_page(PAGE_PATTERN);
        snprintf(fake_update.latest, sizeof fake_update.latest, "9.9.9");
        fake_update.state = UP_AVAILABLE;
        ui_set_update(&fake_update, fake_install);
        ui_tick(10);
        ui_draw(fb);
        int shown = (fb[120 * SCREEN_W + SCREEN_W / 2] & 0xffffff) == 0xff5a2a; /* the panel's orange border */
        snprintf(path, sizeof path, "%s/update.ppm", out);
        write_ppm(path, fb);
        ui_button(BTN_A, 1);
        ui_button(BTN_A, 0);
        ui_draw(fb);
        snprintf(path, sizeof path, "%s/update_install.ppm", out);
        write_ppm(path, fb);
        int started = fake_update.state == UP_INSTALLING;
        fake_update.state = UP_AVAILABLE;
        Pattern before = song.pat;
        ui_button(BTN_B, 1);
        ui_button(BTN_B, 0);
        ui_tick(10);
        ui_draw(fb);
        int closed = (fb[120 * SCREEN_W + SCREEN_W / 2] & 0xffffff) != 0xff5a2a;
        int same = !memcmp(&before, &song.pat, sizeof before);
        printf("update prompt: shown %d, A installs %d, B closes it %d, pattern untouched %d\n", shown, started, closed,
               same);
        ui_set_update(NULL, NULL);
    }
    breaks_free();
    return 0;
}
