// SPDX-License-Identifier: GPL-2.0
// Bench audio check for stock and the RAM spike (no sox or python there).
//
//   wavstat stat FILE RATE CH [FREQ...]  per-channel level of a raw S16_LE
//                                        recording (a WAV header is skipped):
//                                        DC, RMS and peak in dBFS, clipped and
//                                        stuck samples, high-band share, and
//                                        the level of each FREQ (Goertzel)
//   wavstat tone FILE RATE CH FREQ SECS DBFS
//                                        write a raw S16_LE sine for aplay
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAXCH 8

static double db(double x)
{
    return x > 0 ? 20 * log10(x / 32768.0) : -999;
}

static int tone(const char *path, int rate, int ch, double freq, double secs, double dbfs)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return perror(path), 1;
    double amp = 32767 * pow(10, dbfs / 20);
    long n = (long)(rate * secs);
    for (long i = 0; i < n; i++) {
        short s = (short)lrint(amp * sin(2 * M_PI * freq * i / rate));
        for (int c = 0; c < ch; c++)
            fwrite(&s, sizeof s, 1, f);
    }
    return fclose(f) ? (perror(path), 1) : 0;
}

static int levels(const char *path, int rate, int ch, int nf, char **fv)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return perror(path), 1;
    char hdr[44];
    long skip = 0;
    if (fread(hdr, 1, 44, f) == 44 && !memcmp(hdr, "RIFF", 4) && !memcmp(hdr + 8, "WAVE", 4))
        skip = 44;
    fseek(f, skip, SEEK_SET);

    // First pass: sums for DC; second pass: everything relative to DC.
    double sum[MAXCH] = { 0 };
    long n = 0;
    short fr[MAXCH];
    while (fread(fr, sizeof(short), ch, f) == (size_t)ch) {
        for (int c = 0; c < ch; c++)
            sum[c] += fr[c];
        n++;
    }
    if (!n)
        return fprintf(stderr, "%s: no samples\n", path), 1;

    double dc[MAXCH], sq[MAXCH] = { 0 }, dsq[MAXCH] = { 0 }, peak[MAXCH] = { 0 };
    long clip[MAXCH] = { 0 }, stuck[MAXCH] = { 0 };
    short prev[MAXCH] = { 0 };
    double s1[MAXCH][8] = { { 0 } }, s2[MAXCH][8] = { { 0 } }, coef[8];
    nf = nf > 8 ? 8 : nf;
    for (int k = 0; k < nf; k++)
        coef[k] = 2 * cos(2 * M_PI * atof(fv[k]) / rate);
    for (int c = 0; c < ch; c++)
        dc[c] = sum[c] / n;

    fseek(f, skip, SEEK_SET);
    for (long i = 0; i < n && fread(fr, sizeof(short), ch, f) == (size_t)ch; i++) {
        for (int c = 0; c < ch; c++) {
            double x = fr[c] - dc[c];
            sq[c] += x * x;
            if (fabs(x) > peak[c])
                peak[c] = fabs(x);
            if (fr[c] >= 32767 || fr[c] <= -32768)
                clip[c]++;
            if (i) {
                double d = fr[c] - prev[c];
                dsq[c] += d * d;
                if (fr[c] == prev[c])
                    stuck[c]++;
            }
            prev[c] = fr[c];
            for (int k = 0; k < nf; k++) {
                double s0 = x + coef[k] * s1[c][k] - s2[c][k];
                s2[c][k] = s1[c][k];
                s1[c][k] = s0;
            }
        }
    }
    fclose(f);

    printf("file=%s frames=%ld secs=%.2f\n", path, n, (double)n / rate);
    for (int c = 0; c < ch; c++) {
        double rms = sqrt(sq[c] / n);
        // First difference is a high-pass (+6 dB/octave): its share of the
        // total shows whether the signal is broadband hiss or low-frequency
        // (room, hum) content. Ratio ~1.41 for white noise.
        double hf = rms > 0 ? sqrt(dsq[c] / (n - 1)) / rms : 0;
        printf("ch%d dc=%.1f rms=%.1fdBFS peak=%.1fdBFS clip=%ld stuck=%.1f%% hf=%.2f",
               c, dc[c], db(rms), db(peak[c]), clip[c], 100.0 * stuck[c] / n, hf);
        for (int k = 0; k < nf; k++) {
            double p = s1[c][k] * s1[c][k] + s2[c][k] * s2[c][k] - coef[k] * s1[c][k] * s2[c][k];
            // Amplitude of a sine at this frequency over the whole window.
            printf(" f%s=%.1fdBFS", fv[k], db(2 * sqrt(p > 0 ? p : 0) / n));
        }
        printf("\n");
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 5 && !strcmp(argv[1], "stat") && atoi(argv[4]) >= 1 && atoi(argv[4]) <= MAXCH)
        return levels(argv[2], atoi(argv[3]), atoi(argv[4]), argc - 5, argv + 5);
    if (argc == 8 && !strcmp(argv[1], "tone"))
        return tone(argv[2], atoi(argv[3]), atoi(argv[4]), atof(argv[5]), atof(argv[6]), atof(argv[7]));
    fprintf(stderr, "usage: wavstat stat FILE RATE CH [FREQ...] | wavstat tone FILE RATE CH FREQ SECS DBFS\n");
    return 2;
}
