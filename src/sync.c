#include "sync.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <math.h>
#include <net/if.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#include "aclock.h"
#include "engine.h"
#include "ui.h"

#define NPEER 8
#define PEER_TTL_US 3000000ULL
#define SEND_US 100000ULL
#define NERR 8
#define NLAG 64                  /* recent packets' travel times kept per unit, each way */
#define LAG_WINDOW_US 8000000ULL /* the fastest of these, lately, is the baseline */
#define ECHO_MAX_US 1000000ULL   /* echo only packets heard this recently */

typedef struct {
    int64_t v[NLAG];
    uint64_t at[NLAG];
    int n, i;
} Lags;

typedef struct {
    uint32_t id;
    uint64_t last_us;
    int playing, role;
    float bpm;
    double pos;
    uint64_t sent_us;  /* the clock in its last packet (its own clock) */
    uint64_t heard_us; /* when we heard that packet (ours) */
    /* its packets to us: heard minus sent, the clocks' difference plus the
     * trip. The clocks' part holds still, so the fastest of these marks a
     * quick trip, and a packet slower than that was held up by that much
     * more (Wi-Fi queues, broadcasts held for a power-saving unit's beacon) */
    Lags down;
    /* ours to it, from its echoes: the same the other way, with the clocks'
     * difference the other way round. The fastest each way, added, cancel
     * the clocks out and leave two quick trips */
    Lags up;
} Peer;

static SyncStatus st;
static Peer peers[NPEER];
static int sock = -1, echo_next;
static uint64_t last_send_us, last_if_us;
static struct sockaddr_in bcast[8];
static int nbcast;
static double errs[NERR];
static int nerrs, erri;

/* ------------------------------------------------------------ protocol */

static void put32(uint8_t *b, uint32_t v) { memcpy(b, &v, 4); }
static uint32_t get32(const uint8_t *b) { uint32_t v; memcpy(&v, b, 4); return v; }
static uint64_t get64(const uint8_t *b) { uint64_t v; memcpy(&v, b, 8); return v; }

/* the position being heard at time t (call with the audio locked) */
static double own_position(uint64_t t)
{
    double p;
    return aclock_position(t, &p) ? p : engine_position();
}

void sync_core_init(uint32_t id)
{
    memset(&st, 0, sizeof st);
    memset(peers, 0, sizeof peers);
    st.id = id;
    st.delay_ms = -1.f;
    nerrs = erri = echo_next = 0;
}

int sync_make_packet(uint8_t *b, uint64_t now_us)
{
    memcpy(b, "HI5S", 4);
    b[4] = 1;
    b[5] = (uint8_t)!!live.playing;
    b[6] = (uint8_t)song.sync_mode;
    b[7] = 0;
    put32(b + 8, st.id);
    float bpm = song.bpm;
    memcpy(b + 12, &bpm, 4);
    ui_lock();
    double pos = own_position(now_us) + (live.playing ? SYNC_WIRE_LEAD_US * song.bpm / 15e6 : 0.0);
    ui_unlock();
    memcpy(b + 16, &pos, 8);
    memcpy(b + 24, &now_us, 8);
    /* echo one unit heard lately, each in turn, so it can time the trip */
    memset(b + 32, 0, SYNC_PKT_LEN - 32);
    for (int k = 0; k < NPEER; k++) {
        int i = (echo_next + k) % NPEER;
        const Peer *p = &peers[i];
        if (!p->last_us || now_us < p->heard_us || now_us - p->heard_us > ECHO_MAX_US)
            continue;
        b[7] = 1;
        put32(b + 32, p->id);
        memcpy(b + 36, &p->sent_us, 8);
        put32(b + 44, (uint32_t)(now_us - p->heard_us));
        echo_next = (i + 1) % NPEER;
        break;
    }
    return SYNC_PKT_LEN;
}

static Peer *peer_slot(uint32_t id, uint64_t now_us)
{
    Peer *free = NULL;
    for (int i = 0; i < NPEER; i++) {
        if (peers[i].id == id && peers[i].last_us)
            return &peers[i];
        if (!free && (!peers[i].last_us || now_us - peers[i].last_us > PEER_TTL_US))
            free = &peers[i];
    }
    if (free)
        memset(free, 0, sizeof *free);
    return free;
}

static double wrap16(double x)
{
    x = fmod(x, 16.0);
    if (x > 8.0)
        x -= 16.0;
    if (x <= -8.0)
        x += 16.0;
    return x;
}

static double median_err(void)
{
    double a[NERR];
    int n = nerrs < NERR ? nerrs : NERR;
    memcpy(a, errs, n * sizeof a[0]);
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0 && a[j - 1] > a[j]; j--) {
            double t = a[j];
            a[j] = a[j - 1];
            a[j - 1] = t;
        }
    return n ? a[n / 2] : 0.0;
}

static void lags_add(Lags *l, int64_t v, uint64_t now_us)
{
    l->v[l->i] = v;
    l->at[l->i] = now_us;
    l->i = (l->i + 1) % NLAG;
    if (l->n < NLAG)
        l->n++;
}

static int lags_min(const Lags *l, uint64_t now_us, int64_t *min)
{
    int found = 0;
    for (int i = 0; i < l->n; i++)
        if (now_us - l->at[i] <= LAG_WINDOW_US && (!found || l->v[i] < *min)) {
            *min = l->v[i];
            found = 1;
        }
    return found;
}

/* the quickest trip one way, in us: -1 until the unit has echoed us */
static double quick_us(const Peer *p, uint64_t now_us)
{
    int64_t down, up;
    if (!lags_min(&p->down, now_us, &down) || !lags_min(&p->up, now_us, &up))
        return -1.0;
    double q = (double)(down + up) / 2.0;
    return q < 0.0 ? 0.0 : q;
}

/* how long the packet just heard from p took to arrive, in us */
static double trip_us(const Peer *p, uint64_t now_us)
{
    int64_t last = p->down.v[(p->down.i + NLAG - 1) % NLAG], fast = last;
    lags_min(&p->down, now_us, &fast);
    double quick = quick_us(p, now_us);
    double t = (double)(last - fast) + (quick < 0.0 ? 0.0 : quick); /* old units can't echo: count it as no time */
    return t > 1e6 ? 1e6 : t;
}

/* a packet from the unit we follow: match tempo and transport, then phase.
 * Both sides are compared at the moment the packet was heard: the leader's
 * position carried forward by the packet's trip, ours as heard then. */
static void follow(const Peer *p, uint64_t now_us)
{
    double spu = p->playing ? p->bpm / 15e6 : 0.0; /* its 16ths per us */
    double sent = p->pos + trip_us(p, now_us) * spu, lead = sent - SYNC_WIRE_LEAD_US * spu;
    double quick = quick_us(p, now_us);
    st.delay_ms = quick < 0.0 ? -1.f : (float)(quick / 1000.0);
    ui_lock();
    if (fabsf(p->bpm - song.bpm) > 0.01f && p->bpm >= 40.f && p->bpm <= 250.f)
        song.bpm = p->bpm;
    if (p->playing != live.playing) {
        engine_set_playing(p->playing);
        if (p->playing)
            engine_set_position(sent); /* the engine's own position runs about as far ahead */
        nerrs = erri = 0;
    }
    double rate = 1.0;
    if (live.playing) {
        double err = wrap16(lead - own_position(now_us));
        errs[erri] = err;
        erri = (erri + 1) % NERR;
        if (nerrs < NERR)
            nerrs++;
        double med = median_err();
        if (nerrs >= 3 && fabs(med) > 0.5) {
            engine_set_position(engine_position() + med);
            nerrs = erri = 0;
            med = 0.0;
        }
        else if (nerrs >= 3) {
            double k = med * 0.15;
            rate = 1.0 + (k > 0.03 ? 0.03 : k < -0.03 ? -0.03 : k);
        }
        st.offset_ms = (float)(med * 15000.0 / song.bpm);
    }
    engine_set_rate(rate);
    ui_unlock();
}

void sync_handle_packet(const uint8_t *b, int n, uint64_t now_us)
{
    if (n < SYNC_PKT_MIN || memcmp(b, "HI5S", 4) || b[4] != 1)
        return;
    uint32_t id = get32(b + 8);
    if (id == st.id || song.sync_mode == SYNC_OFF)
        return;
    Peer *p = peer_slot(id, now_us);
    if (!p)
        return;
    p->id = id;
    p->last_us = now_us;
    p->playing = b[5];
    p->role = b[6];
    memcpy(&p->bpm, b + 12, 4);
    memcpy(&p->pos, b + 16, 8);
    if (p->bpm != p->bpm || p->pos != p->pos) { /* NaN: ignore it */
        p->last_us = 0;
        return;
    }
    p->sent_us = get64(b + 24);
    p->heard_us = now_us;
    lags_add(&p->down, (int64_t)(now_us - p->sent_us), now_us);
    if (n >= SYNC_PKT_LEN && (b[7] & 1) && get32(b + 32) == st.id) {
        /* it echoes one of ours: when it heard it (its clock: this packet's
         * send, less the time it held it) minus when we sent it (ours) */
        int64_t up = (int64_t)(p->sent_us - get32(b + 44) - get64(b + 36));
        lags_add(&p->up, up, now_us);
    }
    sync_elect(now_us);
    if (st.have_leader && st.leader_id == id)
        follow(p, now_us);
}

/* leader: a unit set to LEAD beats one on AUTO, then the lowest id wins;
 * units set to FOLLOW never lead */
void sync_elect(uint64_t now_us)
{
    int alive = 0, best_rank = 3;
    uint32_t best_id = 0;
    int self_rank = song.sync_mode == SYNC_LEAD ? 0 : song.sync_mode == SYNC_AUTO ? 1 : 3;
    if (self_rank < 3) {
        best_rank = self_rank;
        best_id = st.id;
    }
    for (int i = 0; i < NPEER; i++) {
        Peer *p = &peers[i];
        if (!p->last_us || now_us - p->last_us > PEER_TTL_US)
            continue;
        alive++;
        int rank = p->role == SYNC_LEAD ? 0 : p->role == SYNC_AUTO ? 1 : 3;
        if (rank < best_rank || (rank == best_rank && rank < 3 && p->id < best_id)) {
            best_rank = rank;
            best_id = p->id;
        }
    }
    st.peers = alive;
    int leading = song.sync_mode != SYNC_OFF && best_rank < 3 && best_id == st.id;
    int have_leader = song.sync_mode != SYNC_OFF && best_rank < 3 && best_id != st.id;
    if (!have_leader && st.have_leader) {
        ui_lock();
        engine_set_rate(1.0);
        ui_unlock();
        nerrs = erri = 0;
        st.offset_ms = 0.f;
        st.delay_ms = -1.f;
    }
    if (have_leader && (!st.have_leader || st.leader_id != best_id))
        nerrs = erri = 0;
    st.leading = leading;
    st.have_leader = have_leader;
    st.leader_id = have_leader ? best_id : 0;
}

const SyncStatus *sync_status(void) { return &st; }

/* ------------------------------------------------------------- sockets */

static void refresh_bcast(void)
{
    struct ifaddrs *ifa, *p;
    nbcast = 0;
    if (getifaddrs(&ifa) != 0)
        return;
    for (p = ifa; p && nbcast < 8; p = p->ifa_next) {
        if (!p->ifa_addr || p->ifa_addr->sa_family != AF_INET || !(p->ifa_flags & IFF_BROADCAST) ||
            (p->ifa_flags & IFF_LOOPBACK) || !p->ifa_broadaddr)
            continue;
        bcast[nbcast] = *(struct sockaddr_in *)p->ifa_broadaddr;
        bcast[nbcast].sin_port = htons(SYNC_PORT);
        nbcast++;
    }
    freeifaddrs(ifa);
}

void sync_init(void)
{
    uint32_t id = (uint32_t)time(NULL) ^ (uint32_t)getpid() * 2654435761u;
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        uint32_t r = 0;
        if (fread(&r, 1, 4, f) == 4)
            id ^= r;
        fclose(f);
    }
    if (!id)
        id = 1;
    sync_core_init(id);
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock < 0)
        return;
    int one = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    struct sockaddr_in a = {0};
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    a.sin_port = htons(SYNC_PORT);
    if (bind(sock, (struct sockaddr *)&a, sizeof a) != 0) {
        fprintf(stderr, "sync: bind failed: %s\n", strerror(errno));
        close(sock);
        sock = -1;
        return;
    }
    fcntl(sock, F_SETFL, fcntl(sock, F_GETFL, 0) | O_NONBLOCK);
    /* the kernel notes when each packet arrived, so time spent waiting for
     * the next frame to read it doesn't count as being out of step */
    setsockopt(sock, SOL_SOCKET, SO_TIMESTAMPNS, &one, sizeof one);
    /* ask Wi-Fi for its voice queue, which goes first */
    int tos = 0xc0, prio = 6;
    setsockopt(sock, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    setsockopt(sock, SOL_SOCKET, SO_PRIORITY, &prio, sizeof prio);
    refresh_bcast();
}

static uint64_t clock_us(clockid_t c)
{
    struct timespec ts;
    clock_gettime(c, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
}

void sync_poll(uint64_t now_us)
{
    if (sock < 0)
        return;
    uint8_t b[64];
    uint64_t mono = clock_us(CLOCK_MONOTONIC), real = clock_us(CLOCK_REALTIME);
    for (;;) {
        union {
            char buf[CMSG_SPACE(sizeof(struct timespec)) + 64];
            struct cmsghdr align;
        } ctl;
        struct iovec iov = {b, sizeof b};
        struct msghdr m = {0};
        m.msg_iov = &iov;
        m.msg_iovlen = 1;
        m.msg_control = ctl.buf;
        m.msg_controllen = sizeof ctl.buf;
        ssize_t n = recvmsg(sock, &m, 0);
        if (n <= 0)
            break;
        /* when it arrived: the kernel's stamp is wall-clock time, so turn
         * its age into our steady clock */
        uint64_t heard = mono;
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&m); c; c = CMSG_NXTHDR(&m, c))
            if (c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_TIMESTAMPNS) {
                struct timespec ts;
                memcpy(&ts, CMSG_DATA(c), sizeof ts);
                uint64_t at = (uint64_t)ts.tv_sec * 1000000ULL + (uint64_t)ts.tv_nsec / 1000;
                if (at <= real && real - at < 1000000ULL)
                    heard = mono - (real - at);
            }
        sync_handle_packet(b, (int)n, heard);
    }
    sync_elect(now_us);
    if (song.sync_mode == SYNC_OFF)
        return;
    if (now_us - last_if_us > 5000000ULL) {
        last_if_us = now_us;
        refresh_bcast();
    }
    if (now_us - last_send_us < SEND_US)
        return;
    last_send_us = now_us;
    int n = sync_make_packet(b, clock_us(CLOCK_MONOTONIC));
    for (int i = 0; i < nbcast; i++)
        sendto(sock, b, n, 0, (struct sockaddr *)&bcast[i], sizeof bcast[i]);
    if (!nbcast) {
        struct sockaddr_in a = {0};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_BROADCAST);
        a.sin_port = htons(SYNC_PORT);
        sendto(sock, b, n, 0, (struct sockaddr *)&a, sizeof a);
    }
}

void sync_close(void)
{
    if (sock >= 0)
        close(sock);
    sock = -1;
}
