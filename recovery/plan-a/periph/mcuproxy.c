// SPDX-License-Identifier: MIT
// MCU link proxy for running stock Launcher on the RAM spike.
//
//   mcuproxy LINK LOG [ALLOW...]
//
// Opens the real MCU UART (/dev/ttyS2, Launcher's termios) and a pseudo
// terminal in raw mode; LINK becomes a symlink to the pty slave. The caller
// bind-mounts the slave onto the chroot's /dev/ttyS2 (a symlink into
// /dev/pts breaks when a program in the chroot mounts its own devpts:
// SPIKE18, gocsdk's pty then answered as ttyS2).
//   MCU -> Launcher: every byte is forwarded unchanged.
//   Launcher -> MCU: bytes are assembled into frames (AA 55 hi lo cmd data
//   cksum, or AA 80 b2 b3 data cksum, see docs/launcher-analysis.md). A frame
//   is forwarded only if its hex form, without the checksum, starts with one
//   of the ALLOW prefixes (e.g. "aa55000301" for the status request);
//   everything else is dropped. Every frame in both directions is logged
//   to LOG with a timestamp and PASS/DROP.
// Runs until killed. The MCU controls power and the vehicle side, so the
// default allow list is empty: nothing reaches the MCU unless approved.
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static FILE *logf;
static double t0;
static int nallow;
static char **allow;

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void hex(char *out, const unsigned char *b, int n)
{
    for (int i = 0; i < n; i++)
        sprintf(out + 2 * i, "%02x", b[i]);
    out[2 * n] = 0;
}

static void logline(const char *dir, const char *verdict, const unsigned char *b, int n)
{
    char h[2 * 320 + 1];
    hex(h, b, n > 320 ? 320 : n);
    fprintf(logf, "%9.3f %s %s %s\n", now() - t0, dir, verdict, h);
    fflush(logf);
}

static int allowed(const unsigned char *b, int n)
{
    char h[2 * 320 + 1];
    hex(h, b, n - 1 > 320 ? 320 : n - 1);   // without the checksum
    for (int i = 0; i < nallow; i++)
        if (!strncmp(h, allow[i], strlen(allow[i])))
            return 1;
    return 0;
}

// Frame assembler: returns the frame length once complete, else 0. Bytes
// outside a frame are returned one at a time as length -1 (logged, dropped).
struct asm_ {
    unsigned char b[320];
    int n, need;
};

static int push(struct asm_ *a, unsigned char c)
{
    if (a->n == 0 && c != 0xaa) {
        a->b[0] = c;
        return -1;
    }
    if (a->n == 1 && c != 0x55 && c != 0x80) {
        a->n = 0;
        a->b[0] = c;
        return -1;
    }
    a->b[a->n++] = c;
    if (a->n == 4)
        // AA 55: lo counts cmd+data+cksum. AA 80: length unknown in the
        // analysis; b3 is taken as the data length (Setting::reboot
        // "AA 80 B0 01 0B" has b3 = 1). Dropped either way unless allowed.
        a->need = a->b[1] == 0x55 ? 4 + a->b[3] : 4 + a->b[3] + 1;
    if (a->n >= 4 && a->n >= a->need) {
        int len = a->n;
        a->n = 0;
        return len;
    }
    if (a->n >= (int)sizeof a->b) {
        int len = a->n;
        a->n = 0;
        return len;
    }
    return 0;
}

static int raw(int fd, int baud)
{
    struct termios t;
    if (tcgetattr(fd, &t))
        return -1;
    cfmakeraw(&t);
    t.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    t.c_cflag |= CS8 | CLOCAL | CREAD;
    t.c_cc[VMIN] = 1;
    t.c_cc[VTIME] = 0;
    if (baud) {
        cfsetispeed(&t, B115200);
        cfsetospeed(&t, B115200);
    }
    return tcsetattr(fd, TCSANOW, &t);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr, "usage: mcuproxy LINK LOG [ALLOW-HEX-PREFIX...]\n");
        return 2;
    }
    nallow = argc - 3;
    allow = argv + 3;
    logf = fopen(argv[2], "a");
    if (!logf)
        return perror(argv[2]), 1;
    int mcu = open("/dev/ttyS2", O_RDWR | O_NOCTTY);
    if (mcu < 0 || raw(mcu, 1))
        return perror("/dev/ttyS2"), 1;
    int m = posix_openpt(O_RDWR | O_NOCTTY);
    if (m < 0 || grantpt(m) || unlockpt(m) || raw(m, 0))
        return perror("pty"), 1;
    const char *slave = ptsname(m);
    unlink(argv[1]);
    if (symlink(slave, argv[1]))
        return perror(argv[1]), 1;
    // Keep the slave open (no hangup between opens) and raw: no echo, no
    // line discipline processing, so Launcher's binary frames pass as is.
    int keep = open(slave, O_RDWR | O_NOCTTY);
    if (keep < 0 || raw(keep, 0))
        return perror(slave), 1;
    // Never block on Launcher's side: if nobody reads, drop and log.
    fcntl(m, F_SETFL, fcntl(m, F_GETFL) | O_NONBLOCK);
    t0 = now();
    fprintf(logf, "mcuproxy: %s -> %s, %d allow prefixes:", argv[1], slave, nallow);
    for (int i = 0; i < nallow; i++)
        fprintf(logf, " %s", allow[i]);
    fprintf(logf, "\n");
    fflush(logf);

    struct asm_ up = { .n = 0 }, down = { .n = 0 };
    struct pollfd p[2] = { { .fd = mcu, .events = POLLIN }, { .fd = m, .events = POLLIN } };
    for (;;) {
        if (poll(p, 2, -1) < 0) {
            if (errno == EINTR)
                continue;
            return perror("poll"), 1;
        }
        unsigned char b[256];
        if (p[0].revents & POLLIN) {          // MCU -> Launcher: forward all
            ssize_t n = read(mcu, b, sizeof b);
            if (n > 0) {
                if (write(m, b, n) != n)
                    logline("MCU>ARM", "DROP-FULL", b, n);
                for (ssize_t i = 0; i < n; i++) {
                    int len = push(&down, b[i]);
                    if (len > 0)
                        logline("MCU>ARM", "PASS", down.b, len);
                }
            }
        }
        if (p[1].revents & POLLIN) {          // Launcher -> MCU: filter
            ssize_t n = read(m, b, sizeof b);
            for (ssize_t i = 0; i < n; i++) {
                int len = push(&up, b[i]);
                if (len < 0) {
                    logline("ARM>MCU", "DROP-STRAY", up.b, 1);
                } else if (len > 0) {
                    int ok = allowed(up.b, len);
                    if (ok && write(mcu, up.b, len) != len)
                        logline("ARM>MCU", "WRITE-ERR", up.b, len);
                    else
                        logline("ARM>MCU", ok ? "PASS" : "DROP", up.b, len);
                }
            }
        }
    }
}
