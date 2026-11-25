/* SPDX-License-Identifier: MIT
 * The update's way in over the unit's own Wi-Fi: a very small HTTP server
 * that gives a phone's browser one page and takes one file back.
 *
 *   GET /          the page: choose the bundle, send it, read the answer
 *   POST /upload   the bundle as the request's body
 *   GET /logs      this session's logs as one file, for whoever is asked
 *                  to send them (`logs` makes the file)
 *
 * A phone only stays on a Wi-Fi it believes leads somewhere. So, while this
 * runs, the unit also answers every name question with its own address and
 * a phone's "is there a network here?" page with the yes it looks for.
 *
 * A received file is written to `file` and `install` is run with its path
 * (recovery/plan-a/app-slot install: the signature and the version decide,
 * not this code); the command's last line is the answer the phone shows.
 * `status` is a one-line text file, rewritten whole, for the launcher's
 * screen: "waiting", "receiving N of M", "checking", "done LINE", "refused
 * LINE". The launcher runs this in a process of its own, only while the
 * Software update page is open, and ends it with the page.
 */
#ifndef UPDATE_H
#define UPDATE_H

struct update_cfg {
	int port;
	const char *file;	/* where the received bundle is put */
	const char *install;	/* the command that judges and keeps it */
	const char *status;	/* the status file */
	long max_bytes;		/* a larger body is refused unread */
	int name_port;		/* 53: answer every name question with `ip` (0: do not) */
	const char *ip;		/* the unit's address on its own network */
	const char *logs;	/* a command that writes the logs' file to the path it is given (NULL: no /logs) */
};

/* Serves until the process is ended; returns only if it cannot listen. */
int update_serve(const struct update_cfg *cfg);
#endif
