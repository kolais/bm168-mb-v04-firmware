// SPDX-License-Identifier: GPL-2.0
// Bench Wi-Fi load for the RAM spike (no iperf or nc in its busybox).
//
//   wifiload udp IP PORT KBPS SECS  paced UDP stream to a client (keeps it
//                                   awake, like a projection stream); prints
//                                   datagrams sent and send errors per second
//   wifiload http PORT MB           serve a test page at http://AP:PORT/ that
//                                   uploads 3 x 16 MB from the phone and then
//                                   downloads MB megabytes; logs throughput
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static double now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int udp(const char *ip, int port, double kbps, double secs)
{
    static char buf[1200];
    struct sockaddr_in to = { .sin_family = AF_INET, .sin_port = htons(port) };
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0 || inet_pton(AF_INET, ip, &to.sin_addr) != 1)
        return perror("udp setup"), 1;
    double gap = sizeof buf * 8 / (kbps * 1000), start = now(), next = start, mark = start + 1;
    long sent = 0, errs = 0, total = 0, terrs = 0;
    while (now() - start < secs) {
        double t = now();
        if (t < next) {
            struct timespec d = { 0, (long)((next - t) * 1e9) };
            nanosleep(&d, NULL);
        }
        next += gap;
        if (sendto(fd, buf, sizeof buf, 0, (struct sockaddr *)&to, sizeof to) < 0)
            errs++;
        else
            sent++;
        if (now() >= mark) {
            printf("t=%.0f sent=%ld err=%ld kbit=%.0f\n", mark - start, sent, errs, sent * sizeof buf * 8 / 1000.0);
            fflush(stdout);
            total += sent, terrs += errs, sent = errs = 0, mark += 1;
        }
    }
    printf("udp total sent=%ld err=%ld\n", total + sent, terrs + errs);
    return 0;
}

static const char page[] =
    "<!doctype html><meta name=viewport content='width=device-width'>"
    "<title>wifiload</title><body style='font:20px system-ui;margin:16px'>"
    "<h3>BM168 Wi-Fi test</h3><div id=o>running...</div><script>"
    "const o=document.getElementById('o');const r=[];"
    "function show(s){r.push(s);o.innerHTML=r.join('<br>');}"
    "async function up(mb){const b=new Uint8Array(mb<<20);const t=performance.now();"
    "await (await fetch('/up',{method:'POST',body:b})).text();"
    "return (mb*8.388608)/((performance.now()-t)/1000);}"
    "async function down(){const t=performance.now();const b=await (await fetch('/down')).arrayBuffer();"
    "return b.byteLength*8/1e6/((performance.now()-t)/1000);}"
    "(async()=>{try{for(let i=1;i<=3;i++)show('upload '+i+': '+(await up(16)).toFixed(1)+' Mbit/s');"
    "show('download: '+(await down()).toFixed(1)+' Mbit/s');show('done');}"
    "catch(e){show('error: '+e);}})();</script>";

static void reply(int c, const char *status, const char *type, const char *body)
{
    char h[256];
    int n = snprintf(h, sizeof h, "HTTP/1.0 %s\r\nContent-Type: %s\r\nCache-Control: no-store\r\n"
                     "Content-Length: %zu\r\n\r\n", status, type, strlen(body));
    if (write(c, h, n) != n || write(c, body, strlen(body)) < 0) { /* client went away */ }
}

// Serves the test page (GET /), counts uploads (POST /up) and serves a
// download of MB megabytes (GET /down), one connection at a time, logging
// throughput per second, until killed.
static int http(int port, long mb)
{
    static char buf[16384];
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    int one = 1, s = socket(AF_INET, SOCK_STREAM, 0);
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    if (s < 0 || bind(s, (struct sockaddr *)&a, sizeof a) || listen(s, 4))
        return perror("http setup"), 1;
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0)
            return perror("accept"), 1;
        size_t got = 0;
        char *end = NULL;
        while (got < sizeof buf - 1 && !end) {
            ssize_t n = read(c, buf + got, sizeof buf - 1 - got);
            if (n <= 0)
                break;
            got += n;
            buf[got] = 0;
            end = strstr(buf, "\r\n\r\n");
        }
        if (!end) {
            close(c);
            continue;
        }
        double start = now(), mark = start + 1;
        long long done = 0, last = 0;
        if (!strncmp(buf, "GET / ", 6)) {
            reply(c, "200 OK", "text/html", page);
            printf("page served\n");
        } else if (!strncmp(buf, "POST /up ", 9)) {
            char *cl = strcasestr(buf, "Content-Length:");
            long long size = cl ? atoll(cl + 15) : 0;
            done = got - (end + 4 - buf);
            while (done < size) {
                ssize_t n = read(c, buf, sizeof buf);
                if (n <= 0)
                    break;
                done += n;
                if (now() >= mark) {
                    printf("up t=%.0f kbit/s=%.0f total=%lld\n", mark - start, (done - last) * 8 / 1000.0, done);
                    last = done, mark += 1;
                }
            }
            double el = now() - start;
            printf("up done %lld of %lld bytes in %.1f s: %.0f kbit/s\n", done, size, el, done * 8 / 1000.0 / el);
            reply(c, "200 OK", "text/plain", "ok");
        } else if (!strncmp(buf, "GET /down ", 10)) {
            long long size = mb * 1048576LL;
            int h = snprintf(buf, sizeof buf, "HTTP/1.0 200 OK\r\nContent-Type: application/octet-stream\r\n"
                             "Cache-Control: no-store\r\nContent-Length: %lld\r\n\r\n", size);
            if (write(c, buf, h) == h) {
                memset(buf, 0x5a, sizeof buf);
                while (done < size) {
                    ssize_t w = write(c, buf, size - done < (long long)sizeof buf ? size - done : (long long)sizeof buf);
                    if (w <= 0)
                        break;
                    done += w;
                    if (now() >= mark) {
                        printf("down t=%.0f kbit/s=%.0f total=%lld\n", mark - start, (done - last) * 8 / 1000.0, done);
                        last = done, mark += 1;
                    }
                }
            }
            double el = now() - start;
            printf("down done %lld of %lld bytes in %.1f s: %.0f kbit/s\n", done, size, el, done * 8 / 1000.0 / el);
        } else {
            reply(c, "404 Not Found", "text/plain", "");
        }
        fflush(stdout);
        close(c);
    }
}

int main(int argc, char **argv)
{
    if (argc == 6 && !strcmp(argv[1], "udp"))
        return udp(argv[2], atoi(argv[3]), atof(argv[4]), atof(argv[5]));
    if (argc == 4 && !strcmp(argv[1], "http"))
        return http(atoi(argv[2]), atol(argv[3]));
    fprintf(stderr, "usage: wifiload udp IP PORT KBPS SECS | wifiload http PORT MB\n");
    return 2;
}
