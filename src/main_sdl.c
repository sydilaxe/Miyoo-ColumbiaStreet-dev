/* Miyoo Mini (Plus) front-end: SDL 1.2 as shipped with OnionOS. */
#include <SDL/SDL.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "aclock.h"
#include "breaks.h"
#include "engine.h"
#include "gfx.h"
#include "net.h"
#include "sync.h"
#include "ui.h"
#include "update.h"

#define SAVE_PATH "columbia.sav"
#define BREAK_DIR "breaks"
#define BASS_DIR "bass"
#define CA_BUNDLE "cacert.pem"

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

static volatile sig_atomic_t got_signal;
static void on_signal(int s) { got_signal = s; }

static void lock(void) { SDL_LockAudio(); }
static void unlock(void) { SDL_UnlockAudio(); }

static void audio_cb(void *ud, Uint8 *stream, int len)
{
    (void)ud;
    aclock_request(now_us(), len / 4);
    engine_render((int16_t *)stream, len / 4);
    aclock_made(len / 4, engine_position(), engine_steps_per_frame());
}

static int map_key(SDLKey k)
{
    switch (k) {
    case SDLK_UP: return BTN_UP;
    case SDLK_DOWN: return BTN_DOWN;
    case SDLK_LEFT: return BTN_LEFT;
    case SDLK_RIGHT: return BTN_RIGHT;
    case SDLK_SPACE: return BTN_A;
    case SDLK_LCTRL: return BTN_B;
    case SDLK_LSHIFT: return BTN_X;
    case SDLK_LALT: return BTN_Y;
    case SDLK_e: return BTN_L1;
    case SDLK_t: return BTN_R1;
    case SDLK_TAB: return BTN_L2;
    case SDLK_BACKSPACE: return BTN_R2;
    case SDLK_RCTRL: return BTN_SELECT;
    case SDLK_RETURN: return BTN_START;
    case SDLK_ESCAPE: return BTN_MENU;
    default: return -1;
    }
}

static SDL_Surface *video, *screen;

static void present(void (*draw)(uint32_t *, const char *), const char *msg)
{
    if (SDL_MUSTLOCK(screen))
        SDL_LockSurface(screen);
    if (screen->pitch == SCREEN_W * 4)
        draw((uint32_t *)screen->pixels, msg);
    else {
        static uint32_t fb[SCREEN_W * SCREEN_H];
        draw(fb, msg);
        for (int y = 0; y < SCREEN_H; y++)
            memcpy((uint8_t *)screen->pixels + y * screen->pitch, fb + y * SCREEN_W, SCREEN_W * 4);
    }
    if (SDL_MUSTLOCK(screen))
        SDL_UnlockSurface(screen);
    SDL_BlitSurface(screen, NULL, video, NULL);
    SDL_Flip(video);
}

static void install_update(void) { update_install_start("."); }

/* Ask GitHub for the latest release 5 s after starting (so Wi-Fi is up), then
 * every minute until it answers, five tries in all. */
static void update_watch(Uint32 now, Uint32 *next_check, int *tries)
{
    static int logged;
    UpdateStatus *u = update_status();
    if ((u->state == UP_CURRENT || u->state == UP_AVAILABLE) && !logged) {
        logged = 1;
        fprintf(stderr, "update: the latest release is v%s, this is v%s\n", u->latest, CS_VERSION);
    }
    if ((u->state == UP_IDLE || u->state == UP_CHECK_FAILED) && *tries < 5 && now >= *next_check) {
        if (u->state == UP_CHECK_FAILED)
            fprintf(stderr, "%s\n", u->err);
        ++*tries;
        *next_check = now + 60000;
        update_check_start();
    }
}

static void draw_main(uint32_t *fb, const char *msg)
{
    (void)msg;
    ui_draw(fb);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    srand((unsigned)time(NULL));
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    fprintf(stderr, "columbia street v%s\n", CS_VERSION);

    int rate = getenv("CS_RATE") ? atoi(getenv("CS_RATE")) : 48000;
    int bufsz = getenv("CS_BUFFER") ? atoi(getenv("CS_BUFFER")) : 1024;

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    SDL_ShowCursor(SDL_DISABLE);
    video = SDL_SetVideoMode(SCREEN_W, SCREEN_H, 32, SDL_HWSURFACE);
    screen = SDL_CreateRGBSurface(SDL_HWSURFACE, SCREEN_W, SCREEN_H, 32, 0x00ff0000, 0x0000ff00, 0x000000ff, 0);
    if (!video || !screen) {
        fprintf(stderr, "video: %s\n", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    present(ui_draw_loading, "LOADING BREAKS...");

    engine_init(rate);
    int nb = breaks_load_dir(BREAK_DIR);
    int ns = bass_load_dir(BASS_DIR);
    if (!nbanks) {
        breaks_fallback();
        fprintf(stderr, "no breaks in %s/: using the fallback kit\n", BREAK_DIR);
    }
    fprintf(stderr, "%d break banks, %d bass samples\n", nb, ns);
    song_default();
    if (song_load(SAVE_PATH))
        fprintf(stderr, "restored %s\n", SAVE_PATH);

    SDL_AudioSpec want;
    memset(&want, 0, sizeof want);
    want.freq = rate;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = (Uint16)bufsz;
    want.callback = audio_cb;
    /* the driver holds about a buffer before it's heard; CS_SYNC_LATENCY
     * (frames) lines this unit up with other apps by ear if needed */
    aclock_init(rate, getenv("CS_SYNC_LATENCY") ? atoi(getenv("CS_SYNC_LATENCY")) : bufsz);
    int have_audio = SDL_OpenAudio(&want, NULL) == 0;
    if (have_audio) {
        fprintf(stderr, "audio %d Hz, %d frames/buffer\n", rate, bufsz);
        ui_lock = lock;
        ui_unlock = unlock;
        SDL_PauseAudio(0);
    }
    else
        fprintf(stderr, "SDL_OpenAudio: %s\n", SDL_GetError());

    ui_init();
    sync_init();
    int update_auto = getenv("CS_UPDATE") ? atoi(getenv("CS_UPDATE")) != 0 : 1, update_tries = 0;
    if (update_auto) {
        net_init(CA_BUNDLE);
        ui_set_update(update_status(), install_update);
    }
    fprintf(stderr, "update check %s\n", update_auto ? "on" : "off");
    Uint32 last = SDL_GetTicks(), last_save = last, update_next = last + 5000, relaunch_at = 0;
    while (!ui_quit_requested() && !got_signal) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_KEYDOWN || e.type == SDL_KEYUP)
                ui_button(map_key(e.key.keysym.sym), e.type == SDL_KEYDOWN);
            else if (e.type == SDL_QUIT)
                got_signal = 1;
        }
        Uint32 now = SDL_GetTicks();
        ui_tick((int)(now - last));
        last = now;
        sync_poll(now_us());
        if (update_auto)
            update_watch(now, &update_next, &update_tries);
        if (update_status()->state == UP_INSTALLED && !relaunch_at) {
            fprintf(stderr, "update: v%s installed, relaunching\n", update_status()->latest);
            relaunch_at = now + 1200; /* long enough to read "restarting" */
        }
        if (relaunch_at && now >= relaunch_at)
            break;
        present(draw_main, NULL);

        if (now - last_save > 15000) {
            last_save = now;
            if (ui_take_changed())
                song_save(SAVE_PATH);
        }
        Uint32 spent = SDL_GetTicks() - now;
        if (spent < 33)
            SDL_Delay(33 - spent);
    }

    song_save(SAVE_PATH);
    sync_close();
    if (have_audio)
        SDL_CloseAudio();
    SDL_Quit();
    breaks_free();
    return update_status()->state == UP_INSTALLED ? RELAUNCH_STATUS : 0; /* launch.sh starts the new version */
}
