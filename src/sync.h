/* Wi-Fi sync, the HI5S protocol shared with HI5 and Jaffedrone: each unit
 * broadcasts tempo, bar phase and transport over UDP ten times a second.
 * The lowest id (or the unit set to LEAD) leads; followers lock their
 * phase by trimming playback speed.
 *
 * Packet, little-endian:
 *   0 "HI5S"   4 version 1   5 playing   6 sync mode   7 flags (1: echo below)
 *   8 u32 unit id   12 f32 bpm   16 f64 position in 16ths at the send time:
 *     the one being heard, plus SYNC_WIRE_LEAD_US (older units send the one
 *     being made, which runs about that far ahead)
 *   24 u64 sender's clock at the send (us)
 *   32 u32 echo: the id of a unit heard   36 u64 the clock in its packet
 *   44 u32 us from hearing that packet to sending this one
 * Units from before the echo send only the first 32 bytes and ignore the
 * rest, so old and new units sync together. */
#pragma once
#include <stdint.h>

#define SYNC_PORT 5150
#define SYNC_PKT_MIN 32 /* the oldest packet, still understood */
#define SYNC_PKT_LEN 48 /* what this version sends */
#define SYNC_WIRE_LEAD_US 32000.0 /* 1.5 buffers of 1024 at 48 kHz */

typedef struct {
    uint32_t id;
    int peers;       /* other units heard in the last 3 s */
    int leading;     /* this unit is the leader */
    int have_leader; /* following someone */
    uint32_t leader_id;
    float offset_ms; /* follower: how far we trail the leader (+ = behind) */
    float delay_ms;  /* follower: the leader's packets take this long at best (-1: not measured) */
} SyncStatus;

void sync_init(void);             /* picks an id and opens the socket */
void sync_poll(uint64_t now_us);  /* main thread, every frame */
void sync_close(void);
const SyncStatus *sync_status(void);

/* protocol core, no sockets (the test harness drives these directly);
 * now_us is the time the packet was sent, or heard */
void sync_core_init(uint32_t id);
int sync_make_packet(uint8_t *buf, uint64_t now_us);
void sync_handle_packet(const uint8_t *buf, int n, uint64_t now_us);
void sync_elect(uint64_t now_us);
