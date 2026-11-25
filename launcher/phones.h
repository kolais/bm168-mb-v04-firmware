/* SPDX-License-Identifier: MIT
 * Paired phones from gocsdk's /etc/goc/bt_conf.ini (stock rootfs format).
 * Names are shown on the unit's screen only; never log them.
 */
#ifndef PHONES_H
#define PHONES_H

#define MAX_PHONES 8

struct phone {
	int index;		/* its place in gocsdk's own count (pairlistindex order, from 1): what AT#CV takes */
	char addr[18];		/* "AA:BB:CC:DD:EE:FF", as carplay parses it */
	char name[64];
	int last;		/* lastaddr: the most recently connected phone */
};

/* Fills up to MAX_PHONES entries; returns the count (0 if none or no file). */
int phones_load(const char *bt_conf, struct phone *p);
/* "aabbccddeeff" or "AA:BB:CC:DD:EE:FF" -> "AA:BB:CC:DD:EE:FF"; 0 on success. */
int phones_colon(const char *in, char out[18]);
#endif
