/* SPDX-License-Identifier: MIT
 * The launcher's sounds as samples (design/feedback.md, Sound), rendered by
 * tools/design_sounds.py into sounds.c: 16-bit mono at sound_rate.
 */
#ifndef LAUNCHER_SOUNDS_H
#define LAUNCHER_SOUNDS_H
#include <stdint.h>

#define SOUNDS 10	/* enum ui_sound, in its order */

struct sound {
	const int16_t *pcm;
	int frames;
};

extern const int sound_rate;
extern const struct sound sounds[SOUNDS];
#endif
