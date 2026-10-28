/* Audio clock: where the music is at the speaker, at any moment.
 *
 * The engine's own position moves a whole buffer at a time (when the buffer
 * is made, every 21-43 ms), and that buffer is heard later still. Read at a
 * random moment it can be a buffer or more off, and Wi-Fi sync would chase
 * that as if it were real drift. This keeps a smooth timeline instead: a
 * delay-locked loop on the audio device's requests says which frame is being
 * played at any instant, and the position noted with each buffer made turns
 * that frame into steps.
 *
 * Shared with the sibling apps; keep the copies the same. */
#pragma once
#include <stdint.h>

/* rate in Hz; latency = frames between the device asking for a frame and
 * the speaker playing it (the driver's queue) */
void aclock_init(int rate, int latency_frames);
/* the audio device's callback: call first thing, with the frames it wants */
void aclock_request(uint64_t now_us, int frames);
/* the device played `frames` of silence nobody made (a dropout) */
void aclock_silence(int frames);
/* after the engine makes `frames`: its position after them, and its speed */
void aclock_made(int frames, double pos_end, double steps_per_frame);
/* the position was moved by `steps` (call with the audio locked) */
void aclock_shift(double steps);
/* the position heard at time t_us; 0 when no audio is running */
int aclock_position(uint64_t t_us, double *pos);
