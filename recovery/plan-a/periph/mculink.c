// SPDX-License-Identifier: MIT
// GD32 MCU link probe for the RAM spike (ttyS2, stock Launcher framing).
//
//   mculink listen SECS [TTY]          receive only: print every byte and
//                                      every AA 55 frame (checksum checked)
//   mculink query status|version SECS [TTY]
//                                      send ONE of the two read requests stock
//                                      Launcher sends at every boot, then
//                                      listen: status = AA 55 00 03 01 00 FC,
//                                      version = AA 55 00 03 02 01 FA
//
// Nothing else can be sent: the MCU controls power and the vehicle side.
// Termios as Launcher (docs/launcher-analysis.md): 115200 8N1 raw, no flow
// control, VMIN 0, VTIME 1.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static const unsigned char status_req[] = { 0xaa, 0x55, 0x00, 0x03, 0x01, 0x00, 0xfc };
static const unsigned char version_req[] = { 0xaa, 0x55, 0x00, 0x03, 0x02, 0x01, 0xfa };

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int open_tty(const char *path)
{
    int fd = open(path, O_RDWR | O_NOCTTY);
    struct termios t;
    if (fd < 0 || tcgetattr(fd, &t))
        return perror(path), -1;
    cfmakeraw(&t);
    t.c_cflag &= ~(CSIZE | PARENB | CSTOPB | CRTSCTS);
    t.c_cflag |= CS8 | CLOCAL | CREAD;
    t.c_iflag &= ~(IXON | IXOFF | IXANY);
    t.c_cc[VMIN] = 0;
    t.c_cc[VTIME] = 1;
    cfsetispeed(&t, B115200);
    cfsetospeed(&t, B115200);
    if (tcsetattr(fd, TCSANOW, &t))
        return perror("tcsetattr"), -1;
    tcflush(fd, TCIFLUSH);
    return fd;
}

// Byte-wise parser like Launcher's: AA 55 hi lo cmd data... cksum, where lo
// counts cmd + data + cksum and cksum = -(sum of bytes from hi) & 0xff.
struct parser {
    unsigned char buf[300];
    int n, need;
    long frames, bad;
};

static void feed(struct parser *p, unsigned char b, double t)
{
    if (p->n == 0 && b != 0xaa)
        return;
    if (p->n == 1 && b != 0x55) {
        p->n = b == 0xaa;
        return;
    }
    p->buf[p->n++] = b;
    if (p->n == 4)
        p->need = 4 + p->buf[3];
    if (p->n < 4 || p->n < p->need)
        return;
    unsigned sum = 0;
    for (int i = 2; i < p->need - 1; i++)
        sum += p->buf[i];
    int ok = ((-sum) & 0xff) == p->buf[p->need - 1] && p->buf[2] == 0 && p->buf[3] >= 2;
    printf("%9.3f FRAME %s cmd=%02x len=%d data=", t, ok ? "ok " : "BAD", p->buf[4], p->buf[3] - 2);
    for (int i = 5; i < p->need - 1; i++)
        printf("%02x", p->buf[i]);
    printf("\n");
    ok ? p->frames++ : p->bad++;
    p->n = 0;
}

int main(int argc, char **argv)
{
    const unsigned char *req = NULL;
    int a = 2;
    if (argc >= 3 && !strcmp(argv[1], "listen")) {
        a = 2;
    } else if (argc >= 4 && !strcmp(argv[1], "query") && !strcmp(argv[2], "status")) {
        req = status_req, a = 3;
    } else if (argc >= 4 && !strcmp(argv[1], "query") && !strcmp(argv[2], "version")) {
        req = version_req, a = 3;
    } else {
        fprintf(stderr, "usage: mculink listen SECS [TTY] | mculink query status|version SECS [TTY]\n");
        return 2;
    }
    double secs = atof(argv[a]);
    const char *tty = argc > a + 1 ? argv[a + 1] : "/dev/ttyS2";
    int fd = open_tty(tty);
    if (fd < 0)
        return 1;
    struct parser p = { .n = 0 };
    double start = now();
    long bytes = 0;
    if (req) {
        ssize_t w = write(fd, req, 7);
        tcdrain(fd);
        printf("%9.3f SENT %s (%zd bytes):", now() - start, argv[2], w);
        for (int i = 0; i < 7; i++)
            printf(" %02x", req[i]);
        printf("\n");
    }
    while (now() - start < secs) {
        unsigned char b[256];
        ssize_t n = read(fd, b, sizeof b);
        if (n < 0 && errno != EINTR && errno != EAGAIN)
            return perror("read"), 1;
        if (n <= 0)
            continue;
        double t = now() - start;
        printf("%9.3f RX %zd:", t, n);
        for (ssize_t i = 0; i < n; i++)
            printf(" %02x", b[i]);
        printf("\n");
        for (ssize_t i = 0; i < n; i++)
            feed(&p, b[i], t);
        bytes += n;
        fflush(stdout);
    }
    printf("SUMMARY bytes=%ld frames=%ld bad=%ld partial=%d\n", bytes, p.frames, p.bad, p.n);
    return 0;
}
