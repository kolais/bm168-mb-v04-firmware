/* SPDX-License-Identifier: MIT
 * A flash page writer that runs in the GD32's RAM (docs/mcu-osd-text.md).
 *
 * The debugger loads this and one page of data into free RAM, points the
 * halted MCU at `entry` and lets it run to the breakpoint at its end. The
 * MCU then erases and programs the page by itself in a fraction of a
 * second, with interrupts masked, and checks the result: no SWD transfer
 * happens while the page is incomplete (the lesson of 2026-11-21, when one
 * failed transfer left a code page half-written and the MCU was resumed).
 *
 *   r0 page address   r1 data in RAM (0: erase only)   r2 halfwords   r3 their checksum
 *   back at the breakpoint: r0 status (0 = the page reads back as the data), r1 FMC status
 *
 * A status other than 0 leaves interrupts masked; the debugger then retries
 * or parks the MCU in `park` (a loop in RAM) and never lets the firmware
 * run on a damaged page.
 */
#include <stdint.h>

#define PAGE 0x800u
#define APP_START 0x08002000u	/* below: the boot loader, never written */
#define APP_END 0x0801f000u	/* from here: the firmware's settings pages */

enum { OK, BAD_SUM, BAD_PAGE, LOCKED, ERASE_ERROR, NOT_ERASED, PROGRAM_ERROR, MISMATCH, TIMEOUT };

#ifdef HOST_TEST
uint32_t fmc_read(uint32_t reg);
void fmc_write(uint32_t reg, uint32_t value);
uint16_t flash_read(uint32_t addr);
void flash_write(uint32_t addr, uint16_t value);
void feed(void);
#else
#define FMC 0x40022000u
static inline uint32_t fmc_read(uint32_t reg) { return *(volatile uint32_t *)(FMC + reg); }
static inline void fmc_write(uint32_t reg, uint32_t value) { *(volatile uint32_t *)(FMC + reg) = value; }
static inline uint16_t flash_read(uint32_t addr) { return *(volatile uint16_t *)addr; }
static inline void flash_write(uint32_t addr, uint16_t value) { *(volatile uint16_t *)addr = value; }
/* The free watchdog, if the firmware started it, keeps counting while this
 * runs; reloading it is harmless when it is off. */
static inline void feed(void) { *(volatile uint32_t *)0x40003000u = 0xAAAAu; }
#endif

enum { KEY = 0x04, STAT = 0x0c, CTL = 0x10, ADDR = 0x14 };
enum { BUSY = 1, PGERR = 4, WPERR = 0x10, FLAGS = 0x34 };
enum { PG = 1, PER = 2, START = 0x40, LK = 0x80 };

uint32_t loader_sum(const uint16_t *data, uint32_t n)
{
	uint32_t a = 1, b = 0;

	for (uint32_t i = 0; i < n; i++) {
		a += data[i];
		b += a;
	}
	return (b << 16) ^ a;
}

static int wait(void)
{
	for (uint32_t i = 0; i < 20000000u; i++) {	/* seconds, where an erase takes well under one */
		if (!(fmc_read(STAT) & BUSY))
			return 0;
		if (!(i & 0xfff))
			feed();
	}
	return -1;
}

static int finish(int status, uint32_t *stat)
{
	*stat = fmc_read(STAT);
	fmc_write(CTL, 0);
	fmc_write(CTL, LK);
	return status;
}

int loader(uint32_t page, const uint16_t *data, uint32_t n, uint32_t sum, uint32_t *stat)
{
	*stat = 0;
	if ((page & (PAGE - 1)) || page < APP_START || page >= APP_END || n > PAGE / 2)
		return BAD_PAGE;
	if (data && loader_sum(data, n) != sum)
		return BAD_SUM;		/* nothing touched */
	feed();
	if (fmc_read(CTL) & LK) {
		fmc_write(KEY, 0x45670123u);
		fmc_write(KEY, 0xCDEF89ABu);
	}
	if (fmc_read(CTL) & LK)
		return LOCKED;
	if (wait())
		return finish(TIMEOUT, stat);
	fmc_write(STAT, FLAGS);
	fmc_write(CTL, PER);
	fmc_write(ADDR, page);
	fmc_write(CTL, PER | START);
	if (wait())
		return finish(TIMEOUT, stat);
	if (fmc_read(STAT) & (PGERR | WPERR))
		return finish(ERASE_ERROR, stat);
	fmc_write(CTL, 0);
	feed();
	for (uint32_t i = 0; i < PAGE / 2; i++)
		if (flash_read(page + 2 * i) != 0xffff)
			return finish(NOT_ERASED, stat);
	if (data) {
		fmc_write(STAT, FLAGS);
		fmc_write(CTL, PG);
		for (uint32_t i = 0; i < n; i++) {
			if (data[i] == 0xffff)
				continue;
			flash_write(page + 2 * i, data[i]);
			if (wait())
				return finish(TIMEOUT, stat);
			if (fmc_read(STAT) & (PGERR | WPERR))
				return finish(PROGRAM_ERROR, stat);
		}
		for (uint32_t i = 0; i < PAGE / 2; i++)
			if (flash_read(page + 2 * i) != (i < n ? data[i] : 0xffff))
				return finish(MISMATCH, stat);
	}
	return finish(OK, stat);
}

#ifndef HOST_TEST
/* Entered with the MCU halted by the debugger and its registers set as
 * above; sp is a stack of this code's own. The breakpoint is the end: the
 * debugger takes the status and puts the firmware's registers back.
 * Interrupts come back on only when the page is right; a handler that runs
 * between there and the breakpoint returns here. */
__attribute__((naked, section(".text.entry"))) void entry(void)
{
	__asm__("mrs r4, primask\n"
		"cpsid i\n"
		"sub sp, #8\n"
		"str r4, [sp, #4]\n"
		"mov r4, sp\n"		/* the fifth argument: where the FMC status goes */
		"push {r4, r5}\n"	/* two words: the stack stays 8-aligned */
		"bl loader\n"
		"add sp, #8\n"
		"ldr r1, [sp, #0]\n"
		"ldr r4, [sp, #4]\n"
		"add sp, #8\n"
		"cbnz r0, 1f\n"
		"msr primask, r4\n"
		"1: bkpt #0\n"
		"b 1b\n");
}

/* Where the debugger leaves the MCU if the page could not be made right:
 * interrupts masked, the watchdog fed, nothing of the firmware running,
 * until a later session finishes the page. */
__attribute__((naked, section(".text.park"))) void park(void)
{
	__asm__("cpsid i\n"
		"ldr r0, =0x40003000\n"
		"ldr r1, =0xAAAA\n"
		"1: str r1, [r0]\n"
		"b 1b\n"
		".ltorg\n");
}
#endif
