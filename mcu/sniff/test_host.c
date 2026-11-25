/* Host test of sniff.c's recording logic: fake bxCAN FIFO registers. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "sniff.c"

static uint32_t regs[2][0x200 / 4];
static uint32_t stock_calls[2];
volatile uint32_t *host_reg(uint32_t a)
{
	int bus = a >= CAN1_BASE;
	return &regs[bus][(a - (bus ? CAN1_BASE : CAN0_BASE)) / 4];
}
void host_stock(uint32_t h) { stock_calls[h == STOCK_CAN1_RX0]++; }

static void frame(int bus, int ext, uint32_t id, int dlc, const uint8_t *d)
{
	uint32_t *r = regs[bus];
	r[0x0c / 4] = 1;
	r[0x1b0 / 4] = ext ? (id << 3) | 4u : id << 21;
	r[0x1b4 / 4] = (uint32_t)dlc;
	r[0x1b8 / 4] = d[0] | d[1] << 8 | d[2] << 16 | (uint32_t)d[3] << 24;
	r[0x1bc / 4] = d[4] | d[5] << 8 | d[6] << 16 | (uint32_t)d[7] << 24;
	if (bus) sniff_can1_rx0(); else sniff_can0_rx0();
}
static struct slot *find(int bus, uint32_t key)
{
	for (unsigned i = 0; i < NSLOT; i++)
		if (table[i].used && table[i].key == (uint16_t)(key | (unsigned)bus << 14)) return &table[i];
	return NULL;
}
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); return 1; } } while (0)
static void dump(const char *path)
{
	FILE *f = fopen(path, "wb");
	unsigned char pad[0x100 - sizeof hdr];
	memset(pad, 0, sizeof pad);
	fwrite(&hdr, sizeof hdr, 1, f); fwrite(pad, sizeof pad, 1, f); fwrite(table, sizeof table, 1, f);
	fclose(f);
}

int main(int argc, char **argv)
{
	uint8_t a[8] = {1, 2, 3, 4, 5, 6, 7, 8}, b[8] = {1, 2, 3, 4, 5, 6, 7, 8};
	CHECK(sizeof(struct slot) == 24 && sizeof(struct header) == 64);
	memset(&table, 0xa5, sizeof table); /* garbage RAM before init */
	frame(0, 0, 0x3a0, 8, a);
	CHECK(hdr.magic == SNIFF_MAGIC && hdr.frames == 1 && hdr.used == 1);
	struct slot *s = find(0, 0x3a0);
	CHECK(s && s->frames == 1 && s->changes == 0 && s->dlc == 8 && !memcmp(s->data, a, 8));
	b[2] = 0x13; frame(0, 0, 0x3a0, 8, b);   /* one byte changes */
	CHECK(s->frames == 2 && s->changes == 1 && s->mask[2] == (0x03 ^ 0x13) && s->data[2] == 0x13);
	frame(0, 0, 0x3a0, 8, b);                /* same data: no change */
	CHECK(s->frames == 3 && s->changes == 1);
	frame(1, 0, 0x3a0, 8, a);                /* same ID, other bus: own slot */
	CHECK(hdr.used == 2 && find(1, 0x3a0) && find(1, 0x3a0) != s);
	frame(1, 1, 0x18daf160, 3, a);           /* extended */
	CHECK(hdr.used == 3 && find(1, 0x8000 | (0x18daf160 & 0x3fff)) && hdr.ext_id[1] == 0x18daf160);
	CHECK(stock_calls[0] == 3 && stock_calls[1] == 2);
	hdr.clear_req = 1; frame(0, 0, 0x100, 2, a);
	CHECK(hdr.clears == 1 && s->mask[2] == 0 && s->frames == 0 && s->data[2] == 0x13 && hdr.used == 4);
	regs[0][0x0c / 4] = 0; sniff_can0_rx0(); /* FIFO empty: nothing recorded, stock still called */
	CHECK(hdr.frames == 6 && stock_calls[0] == 5);
	if (argc > 2) { /* snapshot pair for the decoder: profile byte b3 of 0x3a0 changes 1 -> 2 */
		dump(argv[1]);
		uint8_t p[8] = {1, 2, 0x13, 2, 5, 6, 7, 8};
		frame(0, 0, 0x3a0, 8, p);
		frame(1, 0, 0x2f0, 4, a);
		dump(argv[2]);
	}
	for (uint32_t id = 0; id < 0x800; id++) frame(0, 0, id, 8, a); /* fill: 2048 IDs into 1024 slots */
	CHECK(hdr.used == NSLOT && hdr.full > 0);
	printf("PASS: frames %u used %u full %u clears %u\n", hdr.frames, hdr.used, hdr.full, hdr.clears);
	return 0;
}
