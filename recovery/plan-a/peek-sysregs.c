/* SPDX-License-Identifier: MIT
 * Read-only dump of one physical register page through /dev/mem.
 *   peek-sysregs [phys_page [words]]   default 0xe4900000, 128 words
 * Maps PROT_READ only and never writes. Restricted to the system-register
 * page by default; other pages must be named explicitly.
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(int argc, char **argv)
{
	unsigned long base = argc > 1 ? strtoul(argv[1], NULL, 0) : 0xe4900000UL;
	unsigned words = argc > 2 ? (unsigned)strtoul(argv[2], NULL, 0) : 128;
	volatile const uint32_t *r;
	int fd;

	if (base & 0xfff || !words || words > 1024)
		return fprintf(stderr, "page-aligned base and 1..1024 words required\n"), 2;
	fd = open("/dev/mem", O_RDONLY | O_SYNC);
	if (fd < 0)
		return perror("/dev/mem"), 1;
	r = mmap(0, 4096, PROT_READ, MAP_SHARED, fd, (off_t)base);
	if (r == MAP_FAILED)
		return perror("mmap"), 1;
	for (unsigned i = 0; i < words; i += 4) {
		printf("%08lx:", base + i * 4);
		for (unsigned j = i; j < i + 4 && j < words; j++)
			printf(" %08x", r[j]);
		putchar('\n');
	}
	return 0;
}
