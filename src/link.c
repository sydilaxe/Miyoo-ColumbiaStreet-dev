/* The clock link over USB, the follower (see link.h), from the TrimUI
 * version's link.c. */
#define _GNU_SOURCE
#include "link.h"
#include "midi.h"

#include <dirent.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/usb/ch9.h>
#include <linux/usb/functionfs.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define LINK_NAME "Columbia Street clock link"
#define CONFIGFS "/sys/kernel/config"
#define GADGETS CONFIGFS "/usb_gadget"
#define GADGET GADGETS "/cslink"
#define FFS_NAME "cslink"
#define FFS_DIR "/tmp/cslink-ffs"

static uint64_t now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000;
}

/* ------------------------------------------------------ descriptors */

typedef struct {
    struct usb_interface_descriptor intf;
    struct usb_endpoint_descriptor_no_audio out;
} __attribute__((packed)) LinkDescs;

int link_descriptors(uint8_t *buf, int max)
{
    struct {
        struct usb_functionfs_descs_head_v2 head;
        __le32 fs_count, hs_count;
        LinkDescs fs, hs;
    } __attribute__((packed)) d;
    memset(&d, 0, sizeof d);
    d.head.magic = htole32(FUNCTIONFS_DESCRIPTORS_MAGIC_V2);
    d.head.flags = htole32(FUNCTIONFS_HAS_FS_DESC | FUNCTIONFS_HAS_HS_DESC);
    d.head.length = htole32(sizeof d);
    d.fs_count = d.hs_count = htole32(2);
    LinkDescs *s[2] = {&d.fs, &d.hs};
    for (int k = 0; k < 2; k++) {
        s[k]->intf.bLength = USB_DT_INTERFACE_SIZE;
        s[k]->intf.bDescriptorType = USB_DT_INTERFACE;
        s[k]->intf.bNumEndpoints = 1;
        s[k]->intf.bInterfaceClass = USB_CLASS_VENDOR_SPEC;
        s[k]->intf.bInterfaceSubClass = LINK_SUBCLASS;
        s[k]->intf.bInterfaceProtocol = LINK_PROTOCOL;
        s[k]->intf.iInterface = 1;
        s[k]->out.bLength = USB_DT_ENDPOINT_SIZE;
        s[k]->out.bDescriptorType = USB_DT_ENDPOINT;
        s[k]->out.bEndpointAddress = 1 | USB_DIR_OUT;
        s[k]->out.bmAttributes = USB_ENDPOINT_XFER_BULK;
        s[k]->out.wMaxPacketSize = htole16(k ? 512 : 64);
    }
    if ((int)sizeof d > max)
        return 0;
    memcpy(buf, &d, sizeof d);
    return (int)sizeof d;
}

int link_strings(uint8_t *buf, int max)
{
    struct {
        struct usb_functionfs_strings_head head;
        __le16 code;
        char str[sizeof LINK_NAME];
    } __attribute__((packed)) s;
    memset(&s, 0, sizeof s);
    s.head.magic = htole32(FUNCTIONFS_STRINGS_MAGIC);
    s.head.length = htole32(sizeof s);
    s.head.str_count = htole32(1);
    s.head.lang_count = htole32(1);
    s.code = htole16(0x0409);
    memcpy(s.str, LINK_NAME, sizeof LINK_NAME);
    if ((int)sizeof s > max)
        return 0;
    memcpy(buf, &s, sizeof s);
    return (int)sizeof s;
}

/* ------------------------------------------------------------ files */

static int put(const char *path, const char *text)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    int ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
    close(fd);
    return ok;
}

static int get(const char *path, char *buf, int n)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        return 0;
    ssize_t r = read(fd, buf, (size_t)n - 1);
    close(fd);
    if (r < 0)
        r = 0;
    buf[r] = 0;
    while (r > 0 && (buf[r - 1] == '\n' || buf[r - 1] == ' '))
        buf[--r] = 0;
    return r > 0;
}

/* ------------------------------------------------- what the kernel has */

static int has_filesystem(const char *name)
{
    FILE *f = fopen("/proc/filesystems", "r");
    char line[128];
    int found = 0;
    while (f && !found && fgets(line, sizeof line, f)) {
        char *w = strrchr(line, '\t');
        w = w ? w + 1 : line;
        w[strcspn(w, "\n")] = 0;
        found = !strcmp(w, name);
    }
    if (f)
        fclose(f);
    return found;
}

/* kernel modules that might give USB device mode, under dir (two levels
 * down), named in the log for whoever looks into it: none are loaded */
static void list_modules(const char *dir, int depth, char *out, int n)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.')
            continue;
        char p[512];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        const char *x = strstr(e->d_name, ".ko");
        if (x && !x[3]) {
            if (strstr(e->d_name, "gadget") || strstr(e->d_name, "composite") || strstr(e->d_name, "f_fs") ||
                strstr(e->d_name, "udc") || strstr(e->d_name, "otg") || strstr(e->d_name, "musb") ||
                strstr(e->d_name, "dwc")) {
                size_t l = strlen(out);
                snprintf(out + l, (size_t)n - l, "%s%s", l ? " " : "", p);
            }
        }
        else if (depth > 0)
            list_modules(p, depth - 1, out, n);
    }
    if (d)
        closedir(d);
}

/* the device controller (the first in /sys/class/udc), or 0 */
static int find_udc(char *udc, int n)
{
    DIR *d = opendir("/sys/class/udc");
    struct dirent *e;
    udc[0] = 0;
    while (d && (e = readdir(d)))
        if (e->d_name[0] != '.') {
            snprintf(udc, (size_t)n, "%.63s", e->d_name);
            break;
        }
    if (d)
        closedir(d);
    return udc[0] != 0;
}

/* another gadget (the system's own) holding the controller */
static int udc_busy(void)
{
    DIR *d = opendir(GADGETS);
    struct dirent *e;
    int busy = 0;
    while (d && (e = readdir(d))) {
        if (e->d_name[0] == '.' || !strcmp(e->d_name, "cslink"))
            continue;
        char p[512], v[64];
        snprintf(p, sizeof p, GADGETS "/%s/UDC", e->d_name);
        if (get(p, v, sizeof v)) {
            fprintf(stderr, "link: the USB port is in use (gadget %s), not linking\n", e->d_name);
            busy = 1;
        }
    }
    if (d)
        closedir(d);
    return busy;
}

/* ---------------------------------------------------------- follower */

static int ep0 = -1, ep1 = -1, set_up;
static volatile int running;
static volatile uint64_t last_in;
static pthread_t th;
static const char *why = "NOT STARTED";

int link_follow_state(void)
{
    if (!set_up)
        return 0;
    uint64_t t = last_in;
    return t && now_us() - t < 2000000 ? 2 : 1;
}

const char *link_why(void) { return why; }

/* the gadget as it would be left by a run that didn't end cleanly, or ours now */
static void teardown(void)
{
    put(GADGET "/UDC", "\n");
    if (ep1 >= 0)
        close(ep1);
    if (ep0 >= 0)
        close(ep0);
    ep0 = ep1 = -1;
    umount(FFS_DIR);
    rmdir(FFS_DIR);
    unlink(GADGET "/configs/c.1/ffs." FFS_NAME);
    rmdir(GADGET "/configs/c.1/strings/0x409");
    rmdir(GADGET "/configs/c.1");
    rmdir(GADGET "/functions/ffs." FFS_NAME);
    rmdir(GADGET "/strings/0x409");
    rmdir(GADGET);
}

static void on_wake(int s) { (void)s; }

static void *loop(void *arg)
{
    (void)arg;
    uint8_t buf[512];
    while (running) {
        if (ep1 < 0 && (ep1 = open(FFS_DIR "/ep1", O_RDONLY | O_CLOEXEC)) < 0) {
            usleep(200000);
            continue;
        }
        /* blocks until a leader sends (or the port is let go, or link_follow_stop wakes it) */
        ssize_t r = read(ep1, buf, sizeof buf);
        if (r > 0) {
            uint64_t t = now_us();
            if (!last_in || t - last_in > 2000000)
                midi_notice("USB LINK: %.40s", "FOLLOWING THE LEADER BRICK");
            last_in = t;
            midi_feed(buf, (int)r, t);
        }
        else if (r < 0 && errno != EINTR) {
            close(ep1);
            ep1 = -1;
            usleep(200000);
        }
    }
    return NULL;
}

int link_follow_start(void)
{
    const char *v = getenv("CS_LINK");
    if (v && *v == '0') {
        fprintf(stderr, "link: off (CS_LINK=0)\n");
        why = "OFF";
        return 0;
    }
    /* what the kernel offers, in the log, so a missing part can be told */
    char udc[64], mods[600] = "";
    int have_udc = find_udc(udc, sizeof udc), have_cfs = has_filesystem("configfs"),
        have_ffs = has_filesystem("functionfs");
    if (have_cfs && access(GADGETS, F_OK) != 0)
        mount("none", CONFIGFS, "configfs", 0, NULL); /* not mounted yet */
    int have_gadgets = access(GADGETS, W_OK) == 0;
    list_modules("/config/modules", 2, mods, sizeof mods);
    list_modules("/lib/modules", 2, mods, sizeof mods);
    fprintf(stderr, "link: usb device controller %s, configfs %s, usb_gadget %s, functionfs %s; modules: %s\n",
            have_udc ? udc : "none", have_cfs ? "yes" : "no", have_gadgets ? "yes" : "no", have_ffs ? "yes" : "no",
            mods[0] ? mods : "none found");
    if (!have_udc || !have_gadgets || !have_ffs) {
        why = !have_udc ? "NO DEVICE MODE" : !have_gadgets ? "NO GADGET CONFIGFS" : "NO FUNCTIONFS";
        fprintf(stderr, "link: this kernel can't be a USB clock link (%s), not linking\n", why);
        return 0;
    }
    teardown(); /* what a run that didn't end cleanly left */
    if (udc_busy()) {
        why = "PORT IN USE";
        return 0;
    }
    int ok = mkdir(GADGET, 0755) == 0 && put(GADGET "/idVendor", "0x1d6b") && put(GADGET "/idProduct", "0x0104") &&
             put(GADGET "/bcdUSB", "0x0200") && mkdir(GADGET "/strings/0x409", 0755) == 0 &&
             put(GADGET "/strings/0x409/manufacturer", "Columbia Street") &&
             put(GADGET "/strings/0x409/product", LINK_NAME) && mkdir(GADGET "/configs/c.1", 0755) == 0 &&
             mkdir(GADGET "/configs/c.1/strings/0x409", 0755) == 0 &&
             put(GADGET "/configs/c.1/strings/0x409/configuration", "clock link") &&
             put(GADGET "/configs/c.1/MaxPower", "100") && mkdir(GADGET "/functions/ffs." FFS_NAME, 0755) == 0 &&
             symlink(GADGET "/functions/ffs." FFS_NAME, GADGET "/configs/c.1/ffs." FFS_NAME) == 0;
    if (ok) {
        mkdir(FFS_DIR, 0755);
        ok = mount(FFS_NAME, FFS_DIR, "functionfs", 0, NULL) == 0;
    }
    uint8_t b[256];
    int n;
    ok = ok && (ep0 = open(FFS_DIR "/ep0", O_RDWR | O_CLOEXEC)) >= 0 && (n = link_descriptors(b, sizeof b)) > 0 &&
         write(ep0, b, (size_t)n) == n && (n = link_strings(b, sizeof b)) > 0 && write(ep0, b, (size_t)n) == n;
    char nl[80];
    snprintf(nl, sizeof nl, "%s\n", udc);
    ok = ok && put(GADGET "/UDC", nl);
    if (!ok) {
        fprintf(stderr, "link: could not set up the USB port (%s)\n", strerror(errno));
        why = "SET-UP FAILED";
        teardown();
        return 0;
    }
    /* a signal (no restart) wakes the reader for link_follow_stop */
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_wake;
    sigaction(SIGUSR2, &sa, NULL);
    set_up = running = 1;
    last_in = 0;
    if (pthread_create(&th, NULL, loop, NULL) != 0) {
        set_up = running = 0;
        why = "NO THREAD";
        teardown();
        return 0;
    }
    why = "";
    fprintf(stderr, "link: the USB port is a %s on %s: a Brick with a hub in its top port can lead this one\n",
            LINK_NAME, udc);
    return 1;
}

void link_follow_stop(void)
{
    if (!set_up)
        return;
    running = 0;
    put(GADGET "/UDC", "\n"); /* the leader sees it go */
    pthread_kill(th, SIGUSR2);
    pthread_join(th, NULL);
    teardown();
    set_up = 0;
}
