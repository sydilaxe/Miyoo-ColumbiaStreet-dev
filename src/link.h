/* The clock link over USB, from the TrimUI version: a Brick leads, with a
 * hub in its top (host) port, and this unit's USB-C port goes to the same
 * hub. The port is presented as a "Columbia Street clock link" device (USB
 * gadget mode through configfs and FunctionFS: a vendor interface with one
 * bulk OUT endpoint), with the same ids a following Brick uses, so the
 * leader finds it the same way and sends it its clock, START, STOP and where
 * its sound is. What arrives is fed to midi.c as if from a MIDI port.
 *
 * Only following: leading takes a USB host port.
 *
 * It needs a kernel with a USB device controller (/sys/class/udc), configfs
 * and FunctionFS. Whether the Miyoo Mini Plus's has them isn't known; when
 * it hasn't, the start-up log (log.txt) and the SONG page say which part is
 * missing, and nothing else changes. CS_LINK=0 in launch.sh leaves the port
 * alone. */
#pragma once
#include <stdint.h>

#define LINK_VID 0x1d6b /* the Linux Foundation's composite gadget id */
#define LINK_PID 0x0104
#define LINK_SUBCLASS 0x42 /* 'B' */
#define LINK_PROTOCOL 0x4c /* 'L' */

/* set up the USB port (1) or not (0); and give it back */
int link_follow_start(void);
void link_follow_stop(void);
/* 0 not set up, 1 waiting for a leader, 2 a leader sending (in the last 2 s) */
int link_follow_state(void);
/* when not set up, why, for the screen ("NO DEVICE MODE", ...) */
const char *link_why(void);

/* what the follower writes to FunctionFS, for the tests: the length */
int link_descriptors(uint8_t *buf, int max);
int link_strings(uint8_t *buf, int max);
