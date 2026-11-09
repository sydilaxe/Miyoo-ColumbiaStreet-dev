/* The clock in, from the TrimUI version's midi.c: only what a unit following
 * a leading Brick over the USB clock link (link.h) needs. What arrives on the
 * link is MIDI: clock (24 a quarter note), START, CONTINUE, STOP, song
 * position, and the leader's own position (a SysEx, see midi.c), fed here as
 * if from a MIDI port. The tempo follows it, and its phase is held by
 * trimming the playback speed, as Wi-Fi sync does.
 *
 * The names are the TrimUI version's, so its full midi.c (USB MIDI ports,
 * notes, CCs, the clock out) can take this one's place later. */
#pragma once
#include <stdint.h>

void midi_set_latency(float seconds); /* from the engine's position to the sound heard */
int midi_following(void);             /* the link's clock leads */
/* a message for the screen, any thread (fmt with one %s); taken on the main thread */
void midi_notice(const char *fmt, const char *arg);
int midi_take_notice(char *buf, int n);

/* bytes in, as if from a port, at a time in microseconds (any thread: it
 * takes ui_lock) */
void midi_feed(const uint8_t *b, int n, uint64_t now_us);
void midi_reset(void);
/* a leading Brick's position message (F0 7D 'T' 'L' ... F7), for the tests */
int midi_link_pos(uint8_t *out, int max, double heard, double bpm, int playing);
