/* SPDX-License-Identifier: MIT
 * The seam between launcherd's logic (launcherd.c) and the unit it runs on.
 *
 * launcherd.c decides; it never opens a device, a bus or a process. What it
 * needs done it asks for through io_*(); what happens outside reaches it
 * through core_*(). On the unit the io side is unit.c; in the scenario
 * tests it is a recording double with a clock the test moves, so the whole
 * launcher can be exercised without the unit.
 */
#ifndef LAUNCHER_IO_H
#define LAUNCHER_IO_H
#include "dbusmini.h"

/* ---- launcherd.c asks the unit ---- */

long io_now_ms(void);			/* monotonic clock */
void io_log(const char *line);		/* one log line (codes only: never names, addresses or PINs) */
const char *io_root(void);		/* the stock root: "/mnt/stock" on the unit */

int io_mcu_open(void);			/* the MCU link is there */
int io_mcu_write(const unsigned char *frame, int n);	/* 0 = written */

int io_bt_open(void);			/* gocsdk's serial link is open */
int io_bt_connect(void);		/* resolve and open it; 0 = open now */
int io_bt_write(const char *line, int n);	/* 0 = written */

/* A D-Bus method call without a reply (carplay, the audio service). */
void io_call(const char *dest, const char *path, const char *iface, const char *member,
	     const struct dbm_arg *a, int n);

long io_vdec_frames(void);		/* video frames decoded so far, -1 unknown */
int io_carplay_running(void);		/* a carplay process is alive */
void io_carplay_end(void);		/* end the carplay process; D-Bus starts a fresh one at the next call */
void io_rotate_ap(void);		/* new access point passphrase, every client off */
void io_ap_name(const char *name);	/* the access point's name, in its configuration: in force from the next io_rotate_ap() */
void io_put(const char *path, const char *text);	/* a small file under the stock root, written whole: its one line */
int io_state_kept(void);		/* the unit has its state partition: what is saved comes back */
void io_state_save(void);		/* keep the unit's changed state across power-off; does not wait */
void io_sound(int which);		/* one of enum ui_sound, mixed over whatever plays; never waits */
/* App updates (recovery/plan-a/app-slot, launcher/update.h). */
void io_app_confirm(void);		/* the app that runs is good: a trial of it is over; does not wait */
int io_dev_state(void);			/* developer access (recovery/plan-a/dev-access): 1 the image has a key for it, +2 it is on */
void io_dev_access(int on);		/* switch it, now and for the starts to come; does not wait. While it is on,
					 * io_rotate_ap() gives the same passphrase every time: one made when it
					 * was switched on and kept with the unit's state */
int io_update_start(void);		/* take an update over the unit's Wi-Fi from now on; 0 = listening */
void io_update_stop(void);		/* no longer */
int io_update_status(char *line, int n);	/* the receiver's one line ("waiting", "receiving N of M", "checking",
					 * "done ...", "refused ..."); 0 = there is none */
int io_ap_clients(void);		/* phones on the access point now (waits a moment for hostapd); -1 unknown */
void io_ap_info(char *ssid, int ssid_n, char *psk, int psk_n);	/* the access point as a phone joins it: for the
					 * screen only, never for the log */
void io_fb_clear(void);			/* no UI: make the GUI layer transparent */
void io_reboot(void);			/* sync and restart the unit */

/* ---- the unit tells launcherd.c ---- */

void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

void core_begin(void);			/* first of all: the clock's origin, the configuration */
void core_start(int ui_ok);		/* devices are open: the start frames, Bluetooth, the first seek */
void core_flush(void);			/* each loop pass, before reading: the next queued Bluetooth command */
void core_tick(void);			/* each loop pass, after reading: everything that goes by time */
int core_poll_ms(void);			/* how long the loop may sleep */

void core_bt_bytes(const char *b, int n);		/* from gocsdk */
void core_mcu_bytes(const unsigned char *b, int n);	/* from the MCU */
void core_link_status(int type, int status);		/* carplay's onLinkStatusChange */
void core_touch(int type, int x, int y);		/* first finger: 1 down, 2 move, 3 up */
void core_key(char c);		/* the bench's control file: l, r, p, b, m, as the iDrive would send */
#endif
