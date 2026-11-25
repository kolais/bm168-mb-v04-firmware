/*
 * GD32 firmware patch: report the BMW driver profile to the ARM side
 * (docs/mcu-profile-patch.md).
 *
 * Stock firmware (flash sha256 6eb98cdd...) hands every bus-0 CAN frame to the
 * gateway with `bl 0x080051a8` at 0x08007264 (FUN_08007238, main loop). The
 * patch changes that one instruction to `bl profile_hook` (this file, linked
 * at 0x0801a800 in erased flash) and leaves everything else untouched.
 * profile_hook runs in the same main-loop context, notes CAN 0x23A byte 0
 * (the profile of the key that unlocked the car, or of a menu switch: 00,
 * 11, 22, aa), sends it to the ARM with the firmware's own send routine
 * (FUN_08003d9c, main-loop only), and then calls the stock gateway exactly
 * as before. Receive-only: nothing new is sent on either CAN bus.
 *
 * Report to the ARM: AA 55 00 06 E6 02 3A <profile> <fresh> <csum>
 *   fresh = 1 once 0x23A has been seen in the current bus session (a session
 *   starts after a gap of 5 s without bus-0 frames: the bus is quiet for at
 *   least 30 s before the MCU sleeps, and SysTick keeps counting meanwhile).
 * Sent on every change and every 2 s while bus-0 frames flow.
 */
#include <stdint.h>

struct canmsg {               /* GD32 can_receive_message_struct, 20 bytes */
	uint32_t sfid;
	uint32_t efid;
	uint8_t ff;           /* 0 standard, 4 extended */
	uint8_t ft;
	uint8_t dlen;
	uint8_t data[8];
	uint8_t fi;
};

typedef void (*gateway_t)(struct canmsg *m);
typedef void (*send_t)(uint32_t cmd, const uint8_t *data, uint32_t len);

#ifdef HOST_TEST
extern uint32_t host_tick;
extern void host_gateway(struct canmsg *m);
extern void host_send(uint32_t cmd, const uint8_t *data, uint32_t len);
extern struct state host_state;
#define TICK host_tick
#define STOCK_GATEWAY host_gateway
#define STOCK_SEND host_send
#define STATE (&host_state)
#define ENTRY_SECTION
#else
#define TICK (*(volatile uint32_t *)0x200003D4u)          /* SysTick, 1 ms, 32 bit */
#define STOCK_GATEWAY ((gateway_t)0x080051a9u)            /* FUN_080051a8 */
#define STOCK_SEND ((send_t)0x08003d9du)                  /* FUN_08003d9c */
#define STATE ((struct state *)0x2000F000u)               /* free SRAM above the stack */
#define ENTRY_SECTION __attribute__((section(".text.entry")))
#endif

#define PROFILE_ID      0x23Au
#define REPORT_CMD      0xE6u
#define PERIOD_MS       2000u
#define SESSION_GAP_MS  5000u
#define STATE_MAGIC     0x50524f46u /* "PROF" */
#define UNKNOWN         0xffu

struct state {                /* read over SWD for diagnostics */
	uint32_t magic;
	uint32_t version;
	uint32_t last_frame;      /* TICK of the last bus-0 frame */
	uint32_t session_start;   /* TICK of the first frame after a gap */
	uint32_t sessions;
	uint32_t last_report;
	uint32_t reports;
	uint32_t seen;            /* 0x23A frames with a valid profile byte */
	uint32_t latency;         /* ms from session start to its first 0x23A */
	uint32_t frames;          /* bus-0 frames seen by the hook */
	uint8_t profile;          /* UNKNOWN until 0x23A is seen */
	uint8_t fresh;            /* 0x23A seen in the current session */
	uint8_t reported_profile;
	uint8_t reported_fresh;
};

ENTRY_SECTION
void profile_hook(struct canmsg *m)
{
	struct state *s = STATE;
	uint32_t now = TICK;

	if (s->magic != STATE_MAGIC) { /* first run after power-on: RAM is garbage */
		s->version = 1;
		s->last_frame = now;
		s->session_start = now;
		s->sessions = 1;
		s->last_report = now;
		s->reports = 0;
		s->seen = 0;
		s->latency = 0;
		s->frames = 0;
		s->profile = UNKNOWN;
		s->fresh = 0;
		s->reported_profile = UNKNOWN;
		s->reported_fresh = 0;
		s->magic = STATE_MAGIC;
	}
	if ((uint32_t)(now - s->last_frame) >= SESSION_GAP_MS) {
		s->sessions++;
		s->session_start = now;
		s->fresh = 0;
	}
	s->last_frame = now;
	s->frames++;

	if (m->ff == 0 && m->sfid == PROFILE_ID && m->dlen >= 1) {
		uint8_t p = m->data[0];
		if ((p >> 4) == (p & 0x0fu)) { /* 00, 11, 22, aa: both nibbles equal */
			s->seen++;
			if (!s->fresh) {
				s->fresh = 1;
				s->latency = now - s->session_start;
			}
			s->profile = p;
		}
	}

	if (s->profile != UNKNOWN &&
	    (s->profile != s->reported_profile || s->fresh != s->reported_fresh ||
	     (uint32_t)(now - s->last_report) >= PERIOD_MS)) {
		uint8_t d[4];
		d[0] = (uint8_t)(PROFILE_ID >> 8);
		d[1] = (uint8_t)(PROFILE_ID & 0xffu);
		d[2] = s->profile;
		d[3] = s->fresh;
		STOCK_SEND(REPORT_CMD, d, 4);
		s->reported_profile = s->profile;
		s->reported_fresh = s->fresh;
		s->last_report = now;
		s->reports++;
	}

	STOCK_GATEWAY(m);
}
