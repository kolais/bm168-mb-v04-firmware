/* SPDX-License-Identifier: MIT
 * Set one ARK1668 clock register for the decoder-clock experiment.
 *   set-vdec-clk REG VALUE     REG must be 0x64 (mfcclk) or 0x68
 * Writes only the allow-listed offset in the system-register page
 * (0xE4900000), prints the old and new value, and nothing else. RAM-only
 * effect; a power cycle restores the U-Boot value.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	unsigned long reg, val;
	volatile uint32_t *r;
	int fd;

	if (argc != 3)
		return fprintf(stderr, "usage: set-vdec-clk 0x64|0x68 VALUE\n"), 2;
	reg = strtoul(argv[1], NULL, 0);
	val = strtoul(argv[2], NULL, 0);
	if (reg != 0x64 && reg != 0x68)
		return fprintf(stderr, "only 0x64 and 0x68 are allowed\n"), 2;
	fd = open("/dev/mem", O_RDWR | O_SYNC);
	if (fd < 0)
		return perror("/dev/mem"), 1;
	r = mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0xe4900000);
	if (r == MAP_FAILED)
		return perror("mmap"), 1;
	printf("0x%08lx: old 0x%08x\n", 0xe4900000UL + reg, r[reg / 4]);
	r[reg / 4] = (uint32_t)val;
	printf("0x%08lx: new 0x%08x\n", 0xe4900000UL + reg, r[reg / 4]);
	return 0;
}
