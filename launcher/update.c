/* SPDX-License-Identifier: MIT
 * See update.h.
 */
#include "update.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

/* The one page: pick the file, send it, read the answer. No script from
 * elsewhere, nothing stored; it works in a phone's browser as it stands. */
static const char page[] =
	"<!doctype html><html lang=en><meta charset=utf-8>"
	"<meta name=viewport content='width=device-width,initial-scale=1'>"
	"<title>Software update</title>"
	"<style>body{margin:0;padding:24px;background:#0b0d10;color:#e6e9ee;font:18px/1.45 -apple-system,system-ui,sans-serif}"
	"h1{font-size:24px;margin:0 0 16px}p{margin:0 0 16px;color:#aab2c0}"
	"input,button{font:inherit;width:100%;box-sizing:border-box;margin:0 0 16px}"
	"button{padding:14px;border:0;border-radius:12px;background:#e6e9ee;color:#0b0d10;font-weight:600}"
	"button:disabled{opacity:.4}progress{width:100%;height:12px}#say{color:#e6e9ee;font-weight:600}</style>"
	"<h1>Software update</h1>"
	"<p>Choose the update file (autotop-app-\xE2\x80\xA6.bundle) and send it. Keep the ignition on.</p>"
	"<input type=file id=f><button id=b disabled>Send to the unit</button>"
	"<progress id=p max=1 value=0 hidden></progress><p id=say></p>"
	"<p style='margin-top:40px'><a href=/logs download=autotop-logs.tar style='color:#aab2c0'>Save this session\xE2\x80\x99s logs</a>"
	" (a file for whoever asked you for it; it names the phones this unit knows)</p>"
	"<script>var f=document.getElementById('f'),b=document.getElementById('b'),p=document.getElementById('p'),"
	"s=document.getElementById('say');f.onchange=function(){b.disabled=!f.files.length};"
	"b.onclick=function(){var x=new XMLHttpRequest();b.disabled=true;p.hidden=false;s.textContent='Sending\\u2026';"
	"x.upload.onprogress=function(e){if(e.lengthComputable)p.value=e.loaded/e.total};"
	"x.upload.onload=function(){s.textContent='Checking\\u2026'};"
	"x.onload=function(){s.textContent=x.responseText;b.disabled=false};"
	"x.onerror=function(){s.textContent='The unit did not answer. Is this phone still on its Wi-Fi?';b.disabled=false};"
	"x.open('POST','/upload');x.send(f.files[0])}</script></html>";

static void status(const char *path, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void status(const char *path, const char *fmt, ...)
{
	char tmp[256], line[200];
	va_list ap;
	int fd;

	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	snprintf(tmp, sizeof(tmp), "%s.new", path);
	fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		return;
	if (write(fd, line, strlen(line)) < 0 || write(fd, "\n", 1) < 0) {
		close(fd);
		return;
	}
	close(fd);
	rename(tmp, path);	/* whole or not at all: the launcher reads it at any moment */
}

static void answer(int c, const char *code, const char *type, const char *body, size_t n)
{
	char head[200];
	int h = snprintf(head, sizeof(head),
			 "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
			 "Connection: close\r\n\r\n", code, type, n);

	if (write(c, head, (size_t)h) < 0 || (n && write(c, body, n) < 0))
		return;
}

static void say(int c, const char *code, const char *text)
{
	answer(c, code, "text/plain; charset=utf-8", text, strlen(text));
}

/* The logs: `logs FILE` makes the file, which is sent as it is and removed. */
static void send_logs(int c, const struct update_cfg *cfg)
{
	char cmd[512], path[128], buf[8192], head[256];
	struct stat st;
	ssize_t r;
	int fd, h;

	snprintf(path, sizeof(path), "%s.logs", cfg->file);
	snprintf(cmd, sizeof(cmd), "%s %s > /dev/null 2>&1", cfg->logs, path);
	if (system(cmd) || (fd = open(path, O_RDONLY)) < 0) {
		say(c, "500 Internal Server Error", "The logs could not be collected.\n");
		return;
	}
	fstat(fd, &st);
	h = snprintf(head, sizeof(head),
		     "HTTP/1.1 200 OK\r\nContent-Type: application/x-tar\r\nContent-Length: %lld\r\n"
		     "Content-Disposition: attachment; filename=\"autotop-logs.tar\"\r\nCache-Control: no-store\r\n"
		     "Connection: close\r\n\r\n", (long long)st.st_size);
	if (write(c, head, (size_t)h) == h)
		while ((r = read(fd, buf, sizeof(buf))) > 0) {
			ssize_t at = 0, w = 0;

			while (at < r && (w = write(c, buf + at, (size_t)(r - at))) > 0)
				at += w;
			if (w <= 0)
				break;
		}
	close(fd);
	unlink(path);
}

/* Run `install FILE`; its last line of output into `line`. 0 = it said yes. */
static int run_install(const char *install, const char *file, char *line, size_t n)
{
	char cmd[512], got[200];
	FILE *f;
	int rc;

	snprintf(cmd, sizeof(cmd), "%s %s 2>&1", install, file);
	snprintf(line, n, "The update could not be started");
	f = popen(cmd, "r");
	if (!f)
		return -1;
	while (fgets(got, sizeof(got), f)) {
		got[strcspn(got, "\r\n")] = 0;
		if (got[0])
			snprintf(line, n, "%s", got);
	}
	rc = pclose(f);
	return WIFEXITED(rc) && WEXITSTATUS(rc) == 0 ? 0 : -1;
}

/* One request. A bundle that was taken leaves `done` set: nothing more is
 * taken after it (the unit is about to restart). */
static void serve(int c, const struct update_cfg *cfg, int *done)
{
	char head[4096], line[200], *body, *p;
	size_t have = 0;
	long want = -1, got = 0, shown = 0;
	int fd;

	for (;;) {	/* the request's head */
		ssize_t r = read(c, head + have, sizeof(head) - 1 - have);

		if (r <= 0)
			return;
		have += (size_t)r;
		head[have] = 0;
		if ((body = strstr(head, "\r\n\r\n")))
			break;
		if (have >= sizeof(head) - 1) {
			say(c, "431 Request Header Fields Too Large", "Too much in the request.\n");
			return;
		}
	}
	*body = 0;
	body += 4;
	if (!strncmp(head, "GET ", 4)) {
		/* A phone that has just joined asks whether this network leads
		 * anywhere: it fetches a page from one of Apple's names and wants
		 * "Success" in it. Told no, it goes back to a Wi-Fi it knows
		 * (owner's phone, 2026-11-24, no answer at all). Given anything
		 * else, it takes the network for a hotel's and opens its log-in
		 * sheet on what it got, a browser in which no file can be chosen
		 * (the same phone, given the update page). So: a request for a
		 * name, any name, is such a check and gets the yes; the update
		 * page is at the unit's address only. */
		static const char reach[] = "<HTML><HEAD><TITLE>Success</TITLE></HEAD><BODY>Success</BODY></HTML>";
		const char *host = NULL;
		int named = 0;

		for (p = head; (p = strchr(p, '\n')); )
			if (!strncasecmp(++p, "Host:", 5))
				host = p + 5;
		for (; host && *host && *host != '\r' && *host != '\n'; host++)
			if ((*host >= 'a' && *host <= 'z') || (*host >= 'A' && *host <= 'Z'))
				named = 1;
		if (named)
			answer(c, "200 OK", "text/html", reach, sizeof(reach) - 1);
		else if (!strncmp(head + 4, "/ ", 2) || !strncmp(head + 4, "/index.html ", 12))
			answer(c, "200 OK", "text/html; charset=utf-8", page, sizeof(page) - 1);
		else if (cfg->logs && !strncmp(head + 4, "/logs ", 6))
			send_logs(c, cfg);
		else
			say(c, "404 Not Found", "Nothing here.\n");
		return;
	}
	if (strncmp(head, "POST /upload ", 13)) {
		say(c, "405 Method Not Allowed", "Not that.\n");
		return;
	}
	if (*done) {
		say(c, "409 Conflict", "An update is already in. The unit is restarting.\n");
		return;
	}
	for (p = head; (p = strchr(p, '\n')); )	/* Content-Length, in any case of letters */
		if (!strncasecmp(++p, "Content-Length:", 15))
			want = atol(p + 15);
	if (want <= 0) {
		say(c, "411 Length Required", "No file was sent.\n");
		return;
	}
	if (want > cfg->max_bytes) {
		say(c, "413 Payload Too Large", "That file is too large to be an update for this unit.\n");
		return;
	}
	fd = open(cfg->file, O_WRONLY | O_CREAT | O_TRUNC, 0600);
	if (fd < 0) {
		say(c, "500 Internal Server Error", "The unit has no room to take the file.\n");
		return;
	}
	have -= (size_t)(body - head);	/* what of the file came with the head */
	status(cfg->status, "receiving 0 of %ld", want);
	for (;;) {
		if (have) {
			if (got + (long)have > want)
				have = (size_t)(want - got);
			if (write(fd, body, have) != (ssize_t)have) {
				close(fd);
				unlink(cfg->file);
				status(cfg->status, "waiting");
				say(c, "500 Internal Server Error", "The unit has no room to take the file.\n");
				return;
			}
			got += (long)have;
			if (got - shown >= 65536) {
				shown = got;
				status(cfg->status, "receiving %ld of %ld", got, want);
			}
		}
		if (got >= want)
			break;
		ssize_t r = read(c, head, sizeof(head));

		if (r <= 0) {	/* the phone went away */
			close(fd);
			unlink(cfg->file);
			status(cfg->status, "waiting");
			return;
		}
		body = head;
		have = (size_t)r;
	}
	close(fd);
	status(cfg->status, "checking");
	if (run_install(cfg->install, cfg->file, line, sizeof(line)) == 0) {
		*done = 1;
		status(cfg->status, "done %s", line);
		snprintf(head, sizeof(head), "%s\nThe unit restarts now.\n", line);
		say(c, "200 OK", head);
	} else {
		status(cfg->status, "refused %s", line);
		snprintf(head, sizeof(head), "%s\nNothing was changed.\n", line);
		say(c, "200 OK", head);
	}
	unlink(cfg->file);
}

/* Every name is this unit: the unit's DHCP names it as the name server and
 * nothing answered there, so a phone found no address for anything, the
 * check above included. One question, one answer; an address (A) is the
 * unit's, any other kind of question gets an empty answer. */
static void name_answer(int d, const struct update_cfg *cfg)
{
	unsigned char q[512 + 16];
	struct sockaddr_in from;
	socklen_t from_len = sizeof(from);
	ssize_t n = recvfrom(d, q, 512, 0, (struct sockaddr *)&from, &from_len);
	struct in_addr unit;
	ssize_t at = 12;

	if (n < 17 || (q[2] & 0x80) || q[4] != 0 || q[5] != 1 || !inet_aton(cfg->ip, &unit))
		return;	/* not one question */
	while (at < n && q[at])	/* the name: labels up to the empty one */
		at += q[at] + 1;
	at += 5;	/* the empty label, the type, the class */
	if (at > n)
		return;
	n = at;	/* anything after the question is dropped */
	q[2] = (unsigned char)(0x84 | (q[2] & 0x01));	/* an answer, with authority; "recursion desired" as asked */
	q[3] = 0x80;	/* recursion available, no error */
	q[6] = q[8] = q[9] = q[10] = q[11] = 0;
	q[7] = 0;
	if (q[at - 4] == 0 && q[at - 3] == 1 && q[at - 2] == 0 && q[at - 1] == 1) {	/* A, IN */
		static const unsigned char rr[] = { 0xC0, 0x0C, 0, 1, 0, 1, 0, 0, 0, 10, 0, 4 };

		q[7] = 1;
		memcpy(q + n, rr, sizeof(rr));
		memcpy(q + n + sizeof(rr), &unit.s_addr, 4);
		n += (ssize_t)sizeof(rr) + 4;
	}
	sendto(d, q, (size_t)n, 0, (struct sockaddr *)&from, from_len);
}

int update_serve(const struct update_cfg *cfg)
{
	struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons((uint16_t)cfg->port),
				 .sin_addr = { htonl(INADDR_ANY) } };
	struct timeval patience = { 20, 0 };
	int one = 1, done = 0, s = socket(AF_INET, SOCK_STREAM, 0), d = -1;

	signal(SIGPIPE, SIG_IGN);
	if (s < 0 || setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
	    bind(s, (struct sockaddr *)&a, sizeof(a)) || listen(s, 4)) {
		status(cfg->status, "refused The unit cannot listen for the update (%s)", strerror(errno));
		if (s >= 0)
			close(s);
		return -1;
	}
	if (cfg->name_port) {	/* without it the page still works, for a phone that stays */
		a.sin_port = htons((uint16_t)cfg->name_port);
		d = socket(AF_INET, SOCK_DGRAM, 0);
		if (d >= 0 && (setsockopt(d, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) ||
			       bind(d, (struct sockaddr *)&a, sizeof(a)))) {
			close(d);
			d = -1;
		}
	}
	status(cfg->status, "waiting");
	for (;;) {
		struct pollfd fds[2] = { { s, POLLIN, 0 }, { d, POLLIN, 0 } };
		int c;

		if (poll(fds, d >= 0 ? 2 : 1, -1) < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		if (d >= 0 && (fds[1].revents & POLLIN))
			name_answer(d, cfg);
		if (!(fds[0].revents & POLLIN))
			continue;
		c = accept(s, NULL, NULL);
		if (c < 0) {
			if (errno == EINTR)
				continue;
			return -1;
		}
		setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &patience, sizeof(patience));
		setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &patience, sizeof(patience));
		serve(c, cfg, &done);
		shutdown(c, SHUT_WR);
		close(c);
	}
}

#ifdef UPDATE_MAIN
/* For the host test (launcher/test_update.sh): update PORT FILE INSTALL STATUS MAX NAME_PORT IP LOGS */
int main(int argc, char **argv)
{
	if (argc != 9)
		return 2;
	struct update_cfg cfg = { atoi(argv[1]), argv[2], argv[3], argv[4], atol(argv[5]), atoi(argv[6]), argv[7], argv[8] };

	return update_serve(&cfg) ? 1 : 0;
}
#endif
