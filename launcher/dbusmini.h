/* SPDX-License-Identifier: MIT
 * Minimal D-Bus client for the new Launcher: connect to the session bus
 * (unix:abstract= or unix:path= address), SASL EXTERNAL, Hello, and method
 * calls / signal matching with simple signatures. No library dependencies,
 * so the Launcher stays a static binary independent of the vendor stack.
 */
#ifndef DBUSMINI_H
#define DBUSMINI_H
#include <stdint.h>

struct dbm {
	int fd;
	uint32_t serial;
	char name[64];		/* unique name from Hello */
};

/* Arguments for dbm_call: signature characters 'i' (int32) and 's'
 * (string); values in order. */
struct dbm_arg {
	char type;
	int32_t i;
	const char *s;
};

int dbm_connect(struct dbm *c, const char *address);
/* Method call without waiting for a reply (flag NO_REPLY_EXPECTED). */
int dbm_call(struct dbm *c, const char *dest, const char *path, const char *iface,
	     const char *member, const struct dbm_arg *args, int nargs);
/* AddMatch rule (waits for the reply). */
int dbm_add_match(struct dbm *c, const char *rule);
/* Read one message. Fills type (1 call, 2 return, 3 error, 4 signal),
 * member (may be empty) and up to 8 leading int32 body values for an
 * 'i...' signature. Returns 0, or -1 on error/EOF. timeout_ms < 0 blocks. */
int dbm_read(struct dbm *c, int timeout_ms, int *type, char *member, int member_len,
	     int32_t *ints, int *nints);
void dbm_close(struct dbm *c);
#endif
