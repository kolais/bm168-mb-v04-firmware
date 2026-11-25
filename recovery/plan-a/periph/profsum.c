// SPDX-License-Identifier: MIT
// Kernel profile summary for the RAM spike (kernel booted with profile=N,
// CONFIG_PROFILING): every timer tick in kernel mode adds one hit to the
// bucket of the interrupted program counter in /proc/profile.
//
//   profsum reset      zero the counters (write to /proc/profile)
//   profsum [TOP]      fold the buckets into functions with /proc/kallsyms
//                      and print the TOP (default 40) by hits
//
// /proc/profile: one unsigned int sample step (bytes per bucket), then one
// unsigned int counter per bucket starting at _stext.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct sym { unsigned long addr; char name[128]; unsigned long hits; };

static int by_addr(const void *a, const void *b)
{
    const struct sym *x = a, *y = b;
    return x->addr < y->addr ? -1 : x->addr > y->addr;
}

static int by_hits(const void *a, const void *b)
{
    const struct sym *x = a, *y = b;
    return x->hits < y->hits ? 1 : x->hits > y->hits ? -1 : 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "reset")) {
        FILE *f = fopen("/proc/profile", "w");
        if (!f || fputs("0", f) < 0 || fclose(f))
            return perror("/proc/profile"), 1;
        return 0;
    }
    int top = argc > 1 ? atoi(argv[1]) : 40;

    // Kernel text symbols.
    FILE *k = fopen("/proc/kallsyms", "r");
    if (!k)
        return perror("/proc/kallsyms"), 1;
    size_t n = 0, cap = 4096;
    struct sym *s = malloc(cap * sizeof *s);
    unsigned long stext = 0;
    char line[256], type, name[128];
    unsigned long addr;
    while (fgets(line, sizeof line, k)) {
        if (sscanf(line, "%lx %c %127s", &addr, &type, name) != 3)
            continue;
        if (!strcmp(name, "_stext"))
            stext = addr;
        if (type != 't' && type != 'T')
            continue;
        if (n == cap)
            s = realloc(s, (cap *= 2) * sizeof *s);
        s[n].addr = addr;
        snprintf(s[n].name, sizeof s[n].name, "%s", name);
        s[n].hits = 0;
        n++;
    }
    fclose(k);
    if (!stext || !n)
        return fprintf(stderr, "no _stext or text symbols (kallsyms hidden?)\n"), 1;
    qsort(s, n, sizeof *s, by_addr);

    FILE *p = fopen("/proc/profile", "rb");
    if (!p)
        return perror("/proc/profile (boot with profile=2?)"), 1;
    unsigned int step, c;
    if (fread(&step, sizeof step, 1, p) != 1 || !step)
        return fprintf(stderr, "bad /proc/profile header\n"), 1;
    unsigned long total = 0, unknown = 0, i = 0;
    size_t j = 0;
    while (fread(&c, sizeof c, 1, p) == 1) {
        if (c) {
            unsigned long pc = stext + i * step;
            while (j + 1 < n && s[j + 1].addr <= pc)
                j++;
            if (s[j].addr <= pc)
                s[j].hits += c;
            else
                unknown += c;
            total += c;
        }
        i++;
    }
    fclose(p);
    qsort(s, n, sizeof *s, by_hits);
    printf("profile: %lu kernel ticks, step %u bytes, %lu outside symbols\n", total, step, unknown);
    for (int t = 0; t < top && (size_t)t < n && s[t].hits; t++)
        printf("%7lu %5.1f%%  %s\n", s[t].hits, 100.0 * s[t].hits / (total ? total : 1), s[t].name);
    return 0;
}
