/*
 * penfix - fixes pen taps for VEIKK pen displays on old (3.14) Android kernels.
 *
 * Problem: the pen's HID report has both "Tip Switch" (0x42) and
 * "Secondary Tip Switch" (0x43). Kernel 3.14 hid-input maps BOTH to BTN_TOUCH,
 * so every report while the tip is pressed emits BTN_TOUCH DOWN then UP.
 * Android only sees the final UP -> the pen hovers but never "touches".
 *
 * Fix: grab the pen's evdev node exclusively and re-emit its events through a
 * uinput device where BTN_TOUCH = "a BTN_TOUCH DOWN appeared in this frame".
 * The virtual device also sets INPUT_PROP_DIRECT so Android treats it as a
 * pen on the screen (direct touch) instead of a touchpad.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/uinput.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define VENDOR_VEIKK 0x2feb
#define NBITS(x) ((((x) - 1) / (8 * sizeof(long))) + 1)
#define TEST_BIT(bit, arr) (((arr)[(bit) / (8 * sizeof(long))] >> ((bit) % (8 * sizeof(long)))) & 1UL)

static int verbose = 0;

static void logmsg(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void logmsg(const char *fmt, ...) {
    va_list ap;
    FILE *f = fopen("/data/local/tmp/penfix.log", "a");
    if (!f) return;
    time_t t = time(NULL);
    char ts[32];
    strftime(ts, sizeof ts, "%m-%d %H:%M:%S", localtime(&t));
    fprintf(f, "%s ", ts);
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Find the VEIKK node that carries the pen: vendor 0x2feb with BTN_TOOL_PEN + ABS_PRESSURE. */
static int find_pen(char *path_out, size_t len) {
    DIR *d = opendir("/dev/input");
    if (!d) return -1;
    struct dirent *e;
    int found = -1;
    while ((e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5)) continue;
        char p[64];
        snprintf(p, sizeof p, "/dev/input/%s", e->d_name);
        int fd = open(p, O_RDONLY);
        if (fd < 0) continue;
        struct input_id id;
        unsigned long keys[NBITS(KEY_CNT)] = {0}, abs[NBITS(ABS_CNT)] = {0};
        char name[128] = "";
        ioctl(fd, EVIOCGNAME(sizeof name), name);
        if (ioctl(fd, EVIOCGID, &id) == 0 && id.vendor == VENDOR_VEIKK &&
            strstr(name, "penfix") == NULL &&
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof keys), keys) >= 0 &&
            ioctl(fd, EVIOCGBIT(EV_ABS, sizeof abs), abs) >= 0 &&
            TEST_BIT(BTN_TOOL_PEN, keys) && TEST_BIT(ABS_PRESSURE, abs) && TEST_BIT(ABS_X, abs)) {
            snprintf(path_out, len, "%s", p);
            found = fd;
            break;
        }
        close(fd);
    }
    closedir(d);
    return found;
}

static int make_uinput(int src) {
    int u = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (u < 0) { logmsg("open /dev/uinput: %s", strerror(errno)); return -1; }

    unsigned long keys[NBITS(KEY_CNT)] = {0}, abs[NBITS(ABS_CNT)] = {0};
    ioctl(src, EVIOCGBIT(EV_KEY, sizeof keys), keys);
    ioctl(src, EVIOCGBIT(EV_ABS, sizeof abs), abs);

    ioctl(u, UI_SET_EVBIT, EV_SYN);
    ioctl(u, UI_SET_EVBIT, EV_KEY);
    ioctl(u, UI_SET_EVBIT, EV_ABS);
    ioctl(u, UI_SET_PROPBIT, INPUT_PROP_DIRECT);

    for (int k = 0; k < KEY_CNT; k++)
        if (TEST_BIT(k, keys)) ioctl(u, UI_SET_KEYBIT, k);
    ioctl(u, UI_SET_KEYBIT, BTN_TOUCH);
    ioctl(u, UI_SET_KEYBIT, BTN_TOOL_PEN);

    struct uinput_user_dev ud;
    memset(&ud, 0, sizeof ud);
    snprintf(ud.name, UINPUT_MAX_NAME_SIZE, "VEIKK VK2200PRO penfix");
    struct input_id id;
    ioctl(src, EVIOCGID, &id);
    ud.id.bustype = BUS_USB;
    ud.id.vendor = id.vendor;
    ud.id.product = id.product;
    ud.id.version = 0x0f10;

    for (int a = 0; a < ABS_CNT; a++) {
        if (!TEST_BIT(a, abs)) continue;
        struct input_absinfo ai;
        if (ioctl(src, EVIOCGABS(a), &ai) < 0) continue;
        ioctl(u, UI_SET_ABSBIT, a);
        ud.absmin[a] = ai.minimum;
        ud.absmax[a] = ai.maximum;
        ud.absfuzz[a] = ai.fuzz;
        ud.absflat[a] = ai.flat;
    }

    if (write(u, &ud, sizeof ud) != sizeof ud || ioctl(u, UI_DEV_CREATE) < 0) {
        logmsg("uinput create failed: %s", strerror(errno));
        close(u);
        return -1;
    }
    return u;
}

static void emit(int u, int type, int code, int value) {
    struct input_event ev;
    memset(&ev, 0, sizeof ev);
    ev.type = type;
    ev.code = code;
    ev.value = value;
    if (write(u, &ev, sizeof ev) < 0 && verbose) perror("write");
}

/*
 * Two HID usages share one key code (tip 0x42/0x43 -> BTN_TOUCH, barrel
 * buttons -> BTN_STYLUS). The kernel key state ends each frame as whichever
 * usage was processed last, and repeated values are filtered out. Per frame:
 *   pressed = (a DOWN was seen in this frame) || (kernel state is DOWN)
 * covers every combination: (1,0) -> DOWN+UP each frame; (1,1)/(0,1) -> state
 * stays DOWN with no events; (0,0) -> state UP, no DOWN.
 */
static unsigned char kstate[KEY_CNT];   /* kernel's view of each key */
static unsigned char saw_down[KEY_CNT]; /* DOWN seen in current frame */
static unsigned char out[KEY_CNT];      /* what we've sent to Android */

static void run(int src, int u) {
    struct input_event buf[64];
    memset(kstate, 0, sizeof kstate);
    memset(saw_down, 0, sizeof saw_down);
    memset(out, 0, sizeof out);
    unsigned long keys[NBITS(KEY_CNT)] = {0};
    if (ioctl(src, EVIOCGKEY(sizeof keys), keys) >= 0)
        for (int k = 0; k < KEY_CNT; k++) kstate[k] = TEST_BIT(k, keys);

    for (;;) {
        ssize_t n = read(src, buf, sizeof buf);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            logmsg("pen read ended: %s", n < 0 ? strerror(errno) : "eof");
            break;
        }
        int cnt = n / sizeof(struct input_event);
        for (int i = 0; i < cnt; i++) {
            struct input_event *ev = &buf[i];
            switch (ev->type) {
            case EV_KEY:
                if (ev->code < KEY_CNT) {
                    kstate[ev->code] = ev->value ? 1 : 0;
                    if (ev->value) saw_down[ev->code] = 1;
                }
                break;
            case EV_ABS:
                emit(u, EV_ABS, ev->code, ev->value);
                break;
            case EV_SYN:
                if (ev->code == SYN_REPORT) {
                    /* tool (in-range) first, then buttons, so Android sees pen present */
                    for (int pass = 0; pass < 2; pass++) {
                        for (int k = 0; k < KEY_CNT; k++) {
                            int is_tool = (k >= BTN_TOOL_PEN && k <= BTN_TOOL_LENS);
                            if ((pass == 0) != is_tool) continue;
                            int want = saw_down[k] || kstate[k];
                            /* pen left range: release touch & buttons */
                            if (!is_tool && !(saw_down[BTN_TOOL_PEN] || kstate[BTN_TOOL_PEN]) &&
                                !(saw_down[BTN_TOOL_RUBBER] || kstate[BTN_TOOL_RUBBER]) &&
                                (k == BTN_TOUCH || k == BTN_STYLUS || k == BTN_STYLUS2))
                                want = 0;
                            if (want != out[k]) {
                                emit(u, EV_KEY, k, want);
                                out[k] = want;
                            }
                        }
                    }
                    memset(saw_down, 0, sizeof saw_down);
                    emit(u, EV_SYN, SYN_REPORT, 0);
                }
                break;
            default:
                break; /* drop MSC_SCAN etc. */
            }
        }
    }
    for (int k = 0; k < KEY_CNT; k++)
        if (out[k]) emit(u, EV_KEY, k, 0);
    emit(u, EV_SYN, SYN_REPORT, 0);
}

int main(int argc, char **argv) {
    int foreground = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-f")) foreground = 1;
        if (!strcmp(argv[i], "-v")) verbose = 1;
    }
    if (!foreground) {
        pid_t p = fork();
        if (p < 0) return 1;
        if (p > 0) return 0;
        setsid();
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
    }
    signal(SIGPIPE, SIG_IGN);
    logmsg("penfix started (pid %d)", getpid());

    for (;;) {
        char path[64];
        int src = find_pen(path, sizeof path);
        if (src < 0) { sleep(2); continue; }
        if (ioctl(src, EVIOCGRAB, 1) < 0) {
            logmsg("grab %s failed: %s", path, strerror(errno));
            close(src);
            sleep(2);
            continue;
        }
        int u = make_uinput(src);
        if (u < 0) { close(src); sleep(5); continue; }
        logmsg("attached to %s", path);
        run(src, u);
        ioctl(u, UI_DEV_DESTROY);
        close(u);
        close(src);
        sleep(1);
    }
}
