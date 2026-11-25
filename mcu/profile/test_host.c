/* Host test of profile.c: fake tick, gateway and send routine. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
struct state;
uint32_t host_tick;
struct canmsg;
void host_gateway(struct canmsg *m);
void host_send(uint32_t cmd, const uint8_t *data, uint32_t len);
#include "profile.c"
struct state host_state;
static unsigned gw, sent;
static uint8_t last[4];
void host_gateway(struct canmsg *m) { (void)m; gw++; }
void host_send(uint32_t cmd, const uint8_t *d, uint32_t len) { if (cmd == 0xE6 && len == 4) { memcpy(last, d, 4); sent++; } }
static void frame(uint32_t id, int ff, uint8_t b0, uint8_t dlen)
{
	struct canmsg m; memset(&m, 0, sizeof m);
	m.sfid = id; m.ff = (uint8_t)ff; m.dlen = dlen; m.data[0] = b0;
	profile_hook(&m);
}
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
int main(void)
{
	CHECK(sizeof(struct canmsg) == 20);
	memset(&host_state, 0xa5, sizeof host_state);       /* garbage RAM */
	host_tick = 1000;
	frame(0x100, 0, 0x55, 8);                           /* other traffic: no report yet */
	CHECK(host_state.magic == STATE_MAGIC && sent == 0 && gw == 1 && host_state.profile == UNKNOWN);
	host_tick += 10; frame(0x23A, 0, 0x11, 8);          /* profile 1: immediate report, fresh */
	CHECK(sent == 1 && last[0] == 0x02 && last[1] == 0x3a && last[2] == 0x11 && last[3] == 1 && host_state.latency == 10);
	host_tick += 500; frame(0x100, 0, 0, 8);            /* within 2 s: no repeat */
	CHECK(sent == 1);
	host_tick += 1600; frame(0x100, 0, 0, 8);           /* 2.1 s later: periodic repeat */
	CHECK(sent == 2 && last[2] == 0x11);
	host_tick += 10; frame(0x23A, 0, 0x12, 8);          /* nibbles differ: ignored */
	host_tick += 10; frame(0x23A, 4, 0x00, 8);          /* extended frame: ignored */
	host_tick += 10; frame(0x23A, 0, 0x00, 0);          /* no data: ignored */
	CHECK(sent == 2 && host_state.profile == 0x11 && host_state.seen == 1);
	host_tick += 10; frame(0x23A, 0, 0xaa, 8);          /* Guest: change reported at once */
	CHECK(sent == 3 && last[2] == 0xaa);
	host_tick += 40000; frame(0x100, 0, 0, 8);          /* new session after a quiet bus: stale */
	CHECK(host_state.sessions == 2 && host_state.fresh == 0 && sent == 4 && last[2] == 0xaa && last[3] == 0);
	host_tick += 1200; frame(0x23A, 0, 0x00, 8);        /* other key's profile, 1.2 s into the session */
	CHECK(sent == 5 && last[2] == 0x00 && last[3] == 1 && host_state.latency == 1200);
	host_tick = 0xfffffff0u; host_state.last_frame = host_tick - 1; host_state.last_report = host_tick - 1;
	frame(0x100, 0, 0, 8); host_tick += 0x20; frame(0x100, 0, 0, 8); /* tick wrap: no false session */
	CHECK(host_state.sessions == 2);
	CHECK(gw == 12);                                    /* every frame reached the gateway */
	printf("PASS: reports %u, sessions %u, seen %u, gateway calls %u\n", sent, host_state.sessions, host_state.seen, gw);
	return 0;
}
