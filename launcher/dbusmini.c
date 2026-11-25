/* SPDX-License-Identifier: MIT
 * Minimal D-Bus client; see dbusmini.h. Little-endian marshalling only
 * (the ARM target and the bus are little-endian).
 */
#include "dbusmini.h"
#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAXMSG 8192

struct buf {
	unsigned char d[MAXMSG];
	size_t n;
};

static int pad(struct buf *b, size_t align)
{
	while (b->n % align) {
		if (b->n >= MAXMSG)
			return -1;
		b->d[b->n++] = 0;
	}
	return 0;
}

static int put(struct buf *b, const void *p, size_t len)
{
	if (b->n + len > MAXMSG)
		return -1;
	memcpy(b->d + b->n, p, len);
	b->n += len;
	return 0;
}

static int put_u32(struct buf *b, uint32_t v)
{
	return pad(b, 4) || put(b, &v, 4);
}

static int put_str(struct buf *b, const char *s)
{
	uint32_t len = strlen(s);

	return put_u32(b, len) || put(b, s, len + 1);
}

static int put_sig(struct buf *b, const char *s)
{
	unsigned char len = strlen(s);

	return put(b, &len, 1) || put(b, s, len + 1);
}

/* Header field: struct (byte code, variant). */
static int field(struct buf *b, unsigned char code, const char *sig, const char *val)
{
	if (pad(b, 8) || put(b, &code, 1) || put_sig(b, sig))
		return -1;
	return sig[0] == 'g' ? put_sig(b, val) : put_str(b, val);
}

static int write_all(int fd, const void *p, size_t len)
{
	const unsigned char *c = p;

	while (len) {
		ssize_t w = write(fd, c, len);

		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		c += w;
		len -= w;
	}
	return 0;
}

static int read_all(int fd, void *p, size_t len)
{
	unsigned char *c = p;

	while (len) {
		ssize_t r = read(fd, c, len);

		if (r < 0 && errno == EINTR)
			continue;
		if (r <= 0)
			return -1;
		c += r;
		len -= r;
	}
	return 0;
}

static int send_msg(struct dbm *c, unsigned char type, unsigned char flags, const char *dest,
		    const char *path, const char *iface, const char *member,
		    const struct dbm_arg *args, int nargs)
{
	struct buf h = { .n = 0 }, body = { .n = 0 };
	char sig[16] = "";
	uint32_t fields_len;

	if (nargs > 15)
		return -1;
	for (int i = 0; i < nargs; i++) {
		sig[i] = args[i].type;
		if (args[i].type == 'i') {
			if (put_u32(&body, (uint32_t)args[i].i))
				return -1;
		} else if (args[i].type == 's') {
			if (put_str(&body, args[i].s))
				return -1;
		} else {
			return -1;
		}
	}
	sig[nargs] = 0;
	unsigned char fixed[4] = { 'l', type, flags, 1 };
	put(&h, fixed, 4);
	put_u32(&h, body.n);
	put_u32(&h, ++c->serial);
	put_u32(&h, 0);			/* array length, patched below */
	size_t start = h.n;
	if ((path && field(&h, 1, "o", path)) || (iface && field(&h, 2, "s", iface)) ||
	    (member && field(&h, 3, "s", member)) || (dest && field(&h, 6, "s", dest)) ||
	    (nargs && field(&h, 8, "g", sig)))
		return -1;
	fields_len = h.n - start;
	memcpy(h.d + 12, &fields_len, 4);
	if (pad(&h, 8) || put(&h, body.d, body.n))
		return -1;
	return write_all(c->fd, h.d, h.n);
}

int dbm_read(struct dbm *c, int timeout_ms, int *type, char *member, int member_len,
	     int32_t *ints, int *nints)
{
	unsigned char fixed[16], rest[MAXMSG];
	uint32_t body_len, fields_len, total;
	struct pollfd p = { .fd = c->fd, .events = POLLIN };

	if (timeout_ms >= 0 && poll(&p, 1, timeout_ms) <= 0)
		return -1;
	if (read_all(c->fd, fixed, 16) || fixed[0] != 'l')
		return -1;
	memcpy(&body_len, fixed + 4, 4);
	memcpy(&fields_len, fixed + 12, 4);
	total = ((16 + fields_len + 7) & ~7u) - 16 + body_len;
	if (total > sizeof(rest) || read_all(c->fd, rest, total))
		return -1;
	*type = fixed[1];
	if (member)
		member[0] = 0;
	char sig[32] = "";
	/* Header fields: rest[0 .. fields_len), absolute offset = 16 + i. */
	size_t i = 0;
	while (i < fields_len) {
		while ((16 + i) % 8)
			i++;
		if (i >= fields_len)
			break;
		unsigned char code = rest[i++];
		unsigned char slen = rest[i++];
		char vsig = rest[i];
		i += slen + 1;
		if (vsig == 'g') {
			unsigned char l = rest[i++];
			if (code == 8 && l < sizeof(sig)) {
				memcpy(sig, rest + i, l);
				sig[l] = 0;
			}
			i += l + 1;
		} else if (vsig == 's' || vsig == 'o') {
			while ((16 + i) % 4)
				i++;
			uint32_t l;
			memcpy(&l, rest + i, 4);
			i += 4;
			if (code == 3 && member && (int)l < member_len) {
				memcpy(member, rest + i, l);
				member[l] = 0;
			}
			i += l + 1;
		} else if (vsig == 'u') {
			while ((16 + i) % 4)
				i++;
			i += 4;
		} else {
			break;		/* unexpected; skip remaining fields */
		}
	}
	if (nints) {
		size_t b = ((16 + fields_len + 7) & ~7u) - 16;
		int n = 0;

		for (const char *s = sig; *s == 'i' && n < 8 && b + 4 <= total; s++, n++, b += 4)
			memcpy(&ints[n], rest + b, 4);
		*nints = n;
	}
	return 0;
}

int dbm_connect(struct dbm *c, const char *address)
{
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	socklen_t len;
	const char *a;
	char line[256];
	int abstract = 0;

	memset(c, 0, sizeof(*c));
	if ((a = strstr(address, "unix:abstract=")))
		a += 14, abstract = 1;
	else if ((a = strstr(address, "unix:path=")))
		a += 10;
	else
		return -1;
	size_t n = strcspn(a, ",");
	if (n + 2 > sizeof(sa.sun_path))
		return -1;
	memcpy(sa.sun_path + abstract, a, n);
	len = offsetof(struct sockaddr_un, sun_path) + abstract + n + (abstract ? 0 : 1);
	c->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
	if (c->fd < 0 || connect(c->fd, (struct sockaddr *)&sa, len) < 0)
		return -1;
	/* SASL EXTERNAL with our uid as hex-encoded decimal. */
	char uid[16], hex[40] = "";
	snprintf(uid, sizeof(uid), "%u", (unsigned)getuid());
	for (char *u = uid; *u; u++)
		sprintf(hex + strlen(hex), "%02x", (unsigned char)*u);
	snprintf(line, sizeof(line), "AUTH EXTERNAL %s\r\n", hex);
	if (write_all(c->fd, "", 1) || write_all(c->fd, line, strlen(line)))
		return -1;
	n = 0;
	while (n < sizeof(line) - 1) {
		if (read(c->fd, line + n, 1) != 1)
			return -1;
		if (line[n++] == '\n')
			break;
	}
	line[n] = 0;
	if (strncmp(line, "OK ", 3) || write_all(c->fd, "BEGIN\r\n", 7))
		return -1;
	if (send_msg(c, 1, 0, "org.freedesktop.DBus", "/org/freedesktop/DBus",
		     "org.freedesktop.DBus", "Hello", NULL, 0))
		return -1;
	for (int tries = 0; tries < 8; tries++) {	/* the reply, then NameAcquired */
		int type, ni;
		int32_t ints[8];
		if (dbm_read(c, 2000, &type, NULL, 0, ints, &ni))
			return -1;
		if (type == 2)
			return 0;
	}
	return -1;
}

int dbm_call(struct dbm *c, const char *dest, const char *path, const char *iface,
	     const char *member, const struct dbm_arg *args, int nargs)
{
	return send_msg(c, 1, 1, dest, path, iface, member, args, nargs);
}

int dbm_add_match(struct dbm *c, const char *rule)
{
	struct dbm_arg a = { .type = 's', .s = rule };

	return send_msg(c, 1, 1, "org.freedesktop.DBus", "/org/freedesktop/DBus",
			"org.freedesktop.DBus", "AddMatch", &a, 1);
}

void dbm_close(struct dbm *c)
{
	if (c->fd >= 0)
		close(c->fd);
	c->fd = -1;
}
