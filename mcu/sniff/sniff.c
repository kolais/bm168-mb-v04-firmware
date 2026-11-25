/*
 * RAM-only, receive-only CAN recorder for the unit's GD32F305 MCU (stock
 * firmware, flash image sha256 6eb98cdd...). Purpose: find which CAN message
 * carries the BMW driver profile (docs/mcu-can-sniffer.md).
 *
 * Nothing here is written to flash. The installer (sniff.py) copies the
 * application's vector table into RAM with the two CAN receive entries
 * pointing at the hooks below, and points VTOR at it. Each hook records the
 * frame waiting in the controller's receive FIFO 0 (registers only read;
 * nothing transmitted, nothing released) and then calls the stock handler,
 * which reads and releases the same frame as before. An MCU reset, or the
 * firmware's own wake-up path (it rewrites VTOR), restores stock exactly.
 *
 * The table is read back over SWD. Per CAN ID and bus it keeps the last
 * data, an OR-mask of every bit that changed since the last clear, the
 * number of frames and the number of data changes.
 */
#include <stdint.h>

typedef void (*handler_t)(void);

#ifdef HOST_TEST
volatile uint32_t *host_reg(uint32_t addr);
void host_stock(uint32_t handler);
#define REG(a) (*host_reg(a))
#define CALL_STOCK(h) host_stock(h)
#define SNIFF_SECTION(n)
#else
#define REG(a) (*(volatile uint32_t *)(a))
#define CALL_STOCK(h) ((handler_t)(h))()
#define SNIFF_SECTION(n) __attribute__((section(n)))
#endif
#define CAN0_BASE 0x40006400u
#define CAN1_BASE 0x40006800u
/* bxCAN-compatible register layout (GD32F30x CAN) */
#define CAN_RFIFO0(b)   REG((b) + 0x00Cu)
#define CAN_RFIFOMI0(b) REG((b) + 0x1B0u)
#define CAN_RFIFOMP0(b) REG((b) + 0x1B4u)
#define CAN_RDATA00(b)  REG((b) + 0x1B8u)
#define CAN_RDATA10(b)  REG((b) + 0x1BCu)

/* Stock handlers from the application vector table (Thumb bit set). */
#define STOCK_CAN0_RX0 0x08005199u
#define STOCK_CAN1_RX0 0x080051a1u

#define SNIFF_MAGIC   0x534e4631u /* "SNF1" */
#define NSLOT         1024u
#define PROBE_LIMIT   32u

struct slot {
	uint16_t key;      /* bit 15 extended, bit 14 bus, low bits ID (ext: low 14 bits) */
	uint8_t dlc;
	uint8_t used;
	uint8_t data[8];
	uint8_t mask[8];   /* bits that changed since the last clear */
	uint16_t frames;   /* saturating */
	uint16_t changes;  /* saturating: frames whose data differed from the previous one */
};

struct header {
	uint32_t magic;      /* SNIFF_MAGIC once initialised */
	uint32_t version;
	uint32_t frames;     /* all frames seen */
	uint32_t full;       /* frames dropped: probe limit reached */
	uint32_t clear_req;  /* set to 1 over SWD: next frame clears masks and counters */
	uint32_t clears;
	uint32_t nslot;
	uint32_t used;
	uint32_t ext_id[2];  /* last extended ID per bus (diagnostic) */
	uint32_t bus_frames[2];
	uint32_t reserved[4];
};

/* Placed by link.ld: header then table, both outside the stock firmware's RAM. */
SNIFF_SECTION(".sniff_hdr") struct header hdr;
SNIFF_SECTION(".sniff_table") struct slot table[NSLOT];

static void wipe(int all)
{
	uint32_t i, j;
	for (i = 0; i < NSLOT; i++) {
		struct slot *s = &table[i];
		if (all) {
			s->key = 0; s->dlc = 0; s->used = 0;
			for (j = 0; j < 8; j++) s->data[j] = 0;
		}
		for (j = 0; j < 8; j++) s->mask[j] = 0;
		s->frames = 0; s->changes = 0;
	}
}

static void record(uint32_t base, uint32_t bus)
{
	uint32_t mi, mp, d0, d1, key, h, n, j;
	uint8_t data[8];

	if (hdr.magic != SNIFF_MAGIC) {
		wipe(1);
		hdr.version = 1; hdr.frames = 0; hdr.full = 0; hdr.clear_req = 0;
		hdr.clears = 0; hdr.nslot = NSLOT; hdr.used = 0;
		hdr.bus_frames[0] = hdr.bus_frames[1] = 0;
		hdr.magic = SNIFF_MAGIC;
	}
	if (hdr.clear_req) {
		hdr.clear_req = 0;
		wipe(0);
		hdr.clears++;
	}
	if ((CAN_RFIFO0(base) & 3u) == 0)
		return; /* nothing pending: leave it to the stock handler */
	mi = CAN_RFIFOMI0(base);
	mp = CAN_RFIFOMP0(base);
	d0 = CAN_RDATA00(base);
	d1 = CAN_RDATA10(base);
	for (j = 0; j < 4; j++) {
		data[j] = (uint8_t)(d0 >> (8 * j));
		data[j + 4] = (uint8_t)(d1 >> (8 * j));
	}
	if (mi & 4u) { /* extended frame */
		key = 0x8000u | (bus << 14) | ((mi >> 3) & 0x3fffu);
		hdr.ext_id[bus] = mi >> 3;
	} else {
		key = (bus << 14) | (mi >> 21);
	}
	hdr.frames++;
	hdr.bus_frames[bus]++;
	h = (key * 2654435761u) >> 22; /* 10 bits: NSLOT 1024 */
	for (n = 0; n < PROBE_LIMIT; n++, h = (h + 1u) & (NSLOT - 1u)) {
		struct slot *s = &table[h];
		if (s->used && s->key == key) {
			uint32_t changed = 0;
			for (j = 0; j < 8; j++) {
				uint8_t x = (uint8_t)(s->data[j] ^ data[j]);
				s->mask[j] |= x;
				changed |= x;
				s->data[j] = data[j];
			}
			if ((mp & 0xfu) != s->dlc) changed = 1;
			s->dlc = (uint8_t)(mp & 0xfu);
			if (s->frames != 0xffffu) s->frames++;
			if (changed && s->changes != 0xffffu) s->changes++;
			return;
		}
		if (!s->used) {
			s->key = (uint16_t)key;
			s->dlc = (uint8_t)(mp & 0xfu);
			for (j = 0; j < 8; j++) { s->data[j] = data[j]; s->mask[j] = 0; }
			s->frames = 1; s->changes = 0;
			s->used = 1;
			hdr.used++;
			return;
		}
	}
	hdr.full++;
}

void sniff_can0_rx0(void)
{
	record(CAN0_BASE, 0);
	CALL_STOCK(STOCK_CAN0_RX0);
}

void sniff_can1_rx0(void)
{
	record(CAN1_BASE, 1);
	CALL_STOCK(STOCK_CAN1_RX0);
}
