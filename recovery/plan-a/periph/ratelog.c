/* SPDX-License-Identifier: MIT
 * Timeline of CarPlay data flow, for placing video freezes. Read-only.
 *
 *   ratelog SECONDS [PERIOD_MS] > file
 *
 * Every PERIOD_MS (default 20) prints one line:
 *   <monotonic ms> <wlan0 rx packets> <wlan0 rx bytes> <decoder irqs> <sdio irqs>
 * from /proc/net/dev and /proc/interrupts (decoder: the line naming "vdec" on
 * Plan A or "hx170dec" on stock; SDIO: "dw-mci" or "mmc"). A frame decode is
 * one decoder interrupt. Analyse on the host: a freeze with packets still
 * arriving points at the decoder/display, one without at Wi-Fi or the phone.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static long long now_ms(void)
{
	struct timespec t;

	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec * 1000LL + t.tv_nsec / 1000000;
}

static unsigned long long irq_count(const char *line)
{
	const char *p = strchr(line, ':');

	return p ? strtoull(p + 1, NULL, 10) : 0;
}

int main(int argc, char **argv)
{
	int secs = argc > 1 ? atoi(argv[1]) : 30, period = argc > 2 ? atoi(argv[2]) : 20;
	long long start = now_ms(), next = start;
	char line[512];

	if (secs < 1 || secs > 3600 || period < 5 || period > 1000)
		return fprintf(stderr, "usage: ratelog SECONDS [PERIOD_MS 5-1000]\n"), 2;
	setvbuf(stdout, NULL, _IOFBF, 1 << 16);
	while (now_ms() - start < secs * 1000LL) {
		unsigned long long pk = 0, by = 0, dec = 0, sdio = 0;
		FILE *f = fopen("/proc/net/dev", "r");

		while (f && fgets(line, sizeof(line), f)) {
			char *p = strstr(line, "wlan0:");

			if (p) {
				p += 6;
				by = strtoull(p, &p, 10);
				pk = strtoull(p, &p, 10);
			}
		}
		if (f)
			fclose(f);
		f = fopen("/proc/interrupts", "r");
		while (f && fgets(line, sizeof(line), f)) {
			if (strstr(line, "vdec") || strstr(line, "hx170dec"))
				dec += irq_count(line);
			else if (strstr(line, "dw-mci") || strstr(line, "mmc"))
				sdio += irq_count(line);
		}
		if (f)
			fclose(f);
		printf("%lld %llu %llu %llu %llu\n", now_ms() - start, pk, by, dec, sdio);
		next += period;
		long long wait = next - now_ms();
		if (wait > 0) {
			struct timespec t = { wait / 1000, (wait % 1000) * 1000000L };

			nanosleep(&t, NULL);
		}
	}
	return 0;
}
