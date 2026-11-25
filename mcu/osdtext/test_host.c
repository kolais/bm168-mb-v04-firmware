/* SPDX-License-Identifier: MIT
 * Host test of the RAM page writer against a model of the flash controller:
 * locked until both keys, erase only with PER and START, a halfword
 * programs only with PG and only over an erased cell (PGERR otherwise). */
#define HOST_TEST
#include <stdio.h>
#include <string.h>
#include "loader.c"

static uint16_t flash[0x20000 / 2];
static uint32_t stat_reg, ctl = LK, addr_reg, keys, feeds, busy_reads, fail_at = ~0u, programmed, erases;
static int stuck;

static uint16_t *cell(uint32_t a) { return &flash[(a - 0x08000000u) / 2]; }
uint32_t fmc_read(uint32_t reg)
{
	if (reg == STAT) {
		if (stuck)
			return stat_reg | BUSY;
		return busy_reads ? busy_reads--, stat_reg | BUSY : stat_reg;
	}
	return reg == CTL ? ctl : 0;
}
void fmc_write(uint32_t reg, uint32_t v)
{
	if (reg == KEY) {
		keys = keys == 0 && v == 0x45670123u ? 1 : keys == 1 && v == 0xCDEF89ABu ? 2 : 0;
		if (keys == 2)
			ctl &= ~LK;
	} else if (reg == STAT) {
		stat_reg &= ~(v & FLAGS);
	} else if (reg == ADDR) {
		addr_reg = v;
	} else if (reg == CTL) {
		if (ctl & LK)
			return;
		if (v & LK) {
			ctl = LK;
			keys = 0;
			return;
		}
		ctl = v & ~START;
		if ((v & (PER | START)) == (PER | START)) {
			memset(cell(addr_reg & ~(PAGE - 1)), 0xff, PAGE);
			erases++;
			busy_reads = 3;
			stat_reg |= 0x20;
		}
	}
}
uint16_t flash_read(uint32_t a) { return *cell(a); }
void flash_write(uint32_t a, uint16_t v)
{
	if (!(ctl & PG) || (ctl & LK) || *cell(a) != 0xffff || programmed == fail_at) {
		stat_reg |= PGERR;
		return;
	}
	*cell(a) = v;
	programmed++;
	busy_reads = 2;
}
void feed(void) { feeds++; }

static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAILED line %d: %s\n", __LINE__, #c); bad++; } } while (0)

int main(void)
{
	uint16_t data[PAGE / 2], other[PAGE / 2];
	uint32_t st, page = 0x08010800u;

	for (unsigned i = 0; i < sizeof(flash) / 2; i++)
		flash[i] = (uint16_t)(i * 7 + 1);
	memcpy(other, cell(page + PAGE), PAGE);
	for (unsigned i = 0; i < PAGE / 2; i++)
		data[i] = i % 5 ? (uint16_t)(i * 3) : 0xffff;
	uint32_t sum = loader_sum(data, PAGE / 2);

	/* refusals touch nothing */
	CHECK(loader(page, data, PAGE / 2, sum ^ 1, &st) == BAD_SUM && erases == 0 && (ctl & LK));
	CHECK(loader(page + 2, data, PAGE / 2, sum, &st) == BAD_PAGE);
	CHECK(loader(0x08001800u, data, PAGE / 2, sum, &st) == BAD_PAGE);	/* boot loader */
	CHECK(loader(0x0801f000u, data, PAGE / 2, sum, &st) == BAD_PAGE);	/* settings */
	CHECK(loader(page, data, PAGE / 2 + 1, sum, &st) == BAD_PAGE && erases == 0);
	/* the page, whole */
	CHECK(loader(page, data, PAGE / 2, sum, &st) == OK && erases == 1 && (ctl & LK) && feeds);
	CHECK(!memcmp(cell(page), data, PAGE) && !memcmp(cell(page + PAGE), other, PAGE));
	CHECK(*cell(page - 2) == (uint16_t)(((page - 2 - 0x08000000u) / 2) * 7 + 1));
	/* a short one leaves the rest erased */
	CHECK(loader(page, data, 16, loader_sum(data, 16), &st) == OK && *cell(page + 32) == 0xffff && *cell(page + 2) == data[1]);
	/* erase only */
	CHECK(loader(page, 0, 0, 0, &st) == OK && *cell(page) == 0xffff && *cell(page + PAGE - 2) == 0xffff && (ctl & LK));
	/* a program error stops, locks and says so; a second run makes the page right */
	programmed = 0;
	fail_at = 100;
	CHECK(loader(page, data, PAGE / 2, sum, &st) == PROGRAM_ERROR && (st & PGERR) && (ctl & LK));
	fail_at = ~0u;
	stat_reg = 0;
	CHECK(loader(page, data, PAGE / 2, sum, &st) == OK && !memcmp(cell(page), data, PAGE));
	/* a controller that stays busy ends in a timeout, not a hang */
	stuck = 1;
	CHECK(loader(page, data, PAGE / 2, sum, &st) == TIMEOUT && (ctl & LK));
	stuck = 0;
	printf(bad ? "page writer: %d FAILED\n" : "page writer: refusals, whole page, short page, erase only, program error and retry, timeout: passed\n", bad);
	return bad != 0;
}
