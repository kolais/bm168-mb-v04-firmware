/* SPDX-License-Identifier: MIT */
#include "phones.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int get(const char *buf, const char *key, char *out, size_t len)
{
	size_t k = strlen(key);

	for (const char *p = buf; p && *p; p = strchr(p, '\n') ? strchr(p, '\n') + 1 : NULL) {
		if (!strncmp(p, key, k) && p[k] == '=') {
			size_t n = strcspn(p + k + 1, "\r\n");
			if (n >= len)
				n = len - 1;
			memcpy(out, p + k + 1, n);
			out[n] = 0;
			return 0;
		}
	}
	return -1;
}

int phones_colon(const char *in, char out[18])
{
	char hex[12];
	int n = 0;

	for (const char *s = in; *s && n < 12; s++)
		if (isxdigit((unsigned char)*s))
			hex[n++] = toupper((unsigned char)*s);
		else if (*s != ':')
			return -1;
	if (n != 12)
		return -1;
	for (int i = 0; i < 6; i++) {
		out[i * 3] = hex[i * 2];
		out[i * 3 + 1] = hex[i * 2 + 1];
		out[i * 3 + 2] = i < 5 ? ':' : 0;
	}
	return 0;
}

/* gocsdk keeps its phones in numbered slots (addrN, nameN) and, apart from
 * them, the order it counts them in: pairlistindex, slot numbers with the
 * most recently connected first ("1,0,"). Only the slots named there are
 * paired: a slot that was dropped keeps its old lines in the file. Its own
 * listing (MX<n>) and its delete command (AT#CV<n>) count in that order,
 * from 1, and the order changes with every connection: the most recently
 * connected phone is its first. Without the key (older files) the first
 * pairlisttotal slots are taken, in slot order. The phones are returned by
 * slot, so a tile keeps its place while others connect. */
int phones_load(const char *bt_conf, struct phone *p)
{
	char buf[4096], val[80], key[16], last[18] = "";
	FILE *f = fopen(bt_conf, "r");
	size_t n;
	int total, count = 0, order[MAX_PHONES], listed = 0;

	if (!f)
		return 0;
	n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = 0;
	if (get(buf, "pairlisttotal", val, sizeof(val)))
		return 0;
	total = atoi(val);
	if (total > MAX_PHONES)
		total = MAX_PHONES;
	if (!get(buf, "pairlistindex", val, sizeof(val))) {
		char *c = val, *end;

		while (listed < total && isdigit((unsigned char)*c)) {
			order[listed++] = (int)strtol(c, &end, 10);
			c = *end == ',' ? end + 1 : end;
		}
	}
	if (!listed)	/* no order given: the first slots, counted as they stand */
		for (; listed < total; listed++)
			order[listed] = listed;
	if (!get(buf, "lastaddr", val, sizeof(val)))
		phones_colon(val, last);
	for (int slot = 0; slot < 2 * MAX_PHONES && count < MAX_PHONES; slot++) {
		int place = 0;

		for (int k = 0; k < listed; k++)
			if (order[k] == slot)
				place = k + 1;
		if (!place)
			continue;	/* not paired: whatever its lines still say */
		snprintf(key, sizeof(key), "addr%d", slot);
		if (get(buf, key, val, sizeof(val)) || phones_colon(val, p[count].addr))
			continue;
		snprintf(key, sizeof(key), "name%d", slot);
		if (get(buf, key, p[count].name, sizeof(p[count].name)) || !p[count].name[0])
			snprintf(p[count].name, sizeof(p[count].name), "iPhone %d", slot + 1);
		p[count].index = place;
		p[count].last = !strcmp(p[count].addr, last);
		count++;
	}
	return count;
}
