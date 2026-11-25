#!/bin/sh
# The access point's passphrase (ap_words.h) on the host: its form, its
# length, that it uses what randomness it is given, and that too little is
# refused instead of padded.
cd "$(dirname "$0")" || exit 1
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cat > "$T/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include "ap_words.h"
static int bad;
#define CHECK(c) do { if (!(c)) { printf("FAILED line %d: %s\n", __LINE__, #c); bad++; } } while (0)
int main(void)
{
	unsigned char r[AP_RANDOM];
	char p[AP_PASSPHRASE_MAX], first[AP_PASSPHRASE_MAX] = "";
	unsigned seen[AP_WORDS] = { 0 }, digits[10] = { 0 };
	int made = 0, differ = 0;

	for (int i = 0; i < AP_WORDS; i++) {	/* the list itself */
		size_t n = strlen(ap_words[i]);
		CHECK(n >= 3 && n <= 5);
		for (size_t k = 0; k < n; k++)
			CHECK(islower((unsigned char)ap_words[i][k]));
		CHECK(i == 0 || strcmp(ap_words[i - 1], ap_words[i]) < 0);	/* sorted: no word twice */
	}
	srandom(1);
	for (int t = 0; t < 200000; t++) {
		for (int k = 0; k < AP_RANDOM; k++)
			r[k] = (unsigned char)(random() >> 8);
		if (!ap_passphrase(r, AP_RANDOM, p, sizeof(p)))
			continue;
		made++;
		size_t n = strlen(p);
		int words = 0, at = 0;
		CHECK(n >= 15 && n <= 23);
		for (int part = 0; part < 7; part++) {
			if (part & 1) {
				CHECK(p[at] >= '2' && p[at] <= '9');
				digits[p[at] - '0']++;
				at++;
				continue;
			}
			int len = 0, hit = -1;
			while (islower((unsigned char)p[at + len]))
				len++;
			for (int i = 0; i < AP_WORDS && hit < 0; i++)
				if ((int)strlen(ap_words[i]) == len && !strncmp(ap_words[i], p + at, (size_t)len))
					hit = i;
			CHECK(hit >= 0);
			if (hit >= 0)
				seen[hit]++, words++;
			at += len;
		}
		CHECK(words == 4 && p[at] == 0);
		if (!first[0])
			strcpy(first, p);
		else if (strcmp(first, p))
			differ = 1;
	}
	CHECK(made > 190000 && differ);	/* nearly every draw suffices */
	unsigned lo = ~0u, hi = 0;
	for (int i = 0; i < AP_WORDS; i++) {
		lo = seen[i] < lo ? seen[i] : lo;
		hi = seen[i] > hi ? seen[i] : hi;
	}
	CHECK(lo > 450 && hi < 800);	/* every word comes up, none far more than another (mean about 620) */
	for (int d = 2; d <= 9; d++)
		CHECK(digits[d] > 72000 && digits[d] < 78000);
	memset(r, 0xff, sizeof(r));	/* every draw outside the whole rounds: refused */
	CHECK(!ap_passphrase(r, AP_RANDOM, p, sizeof(p)));
	CHECK(!ap_passphrase(r, 3, p, sizeof(p)) && !ap_passphrase(r, AP_RANDOM, p, 10));
	printf(bad ? "passphrase: %d FAILED\n" : "passphrase: %d made and checked, the list of %d words, refusals: passed\n", bad ? bad : made, AP_WORDS);
	return bad != 0;
}
C
cc -std=gnu11 -Wall -Wextra -Werror -I. -o "$T/t" "$T/t.c" && "$T/t"
