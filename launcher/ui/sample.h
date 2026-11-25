/* SPDX-License-Identifier: MIT
 * A stand-in for launcherd: sample phones and settings, and a callback that
 * does to the model what the application would (forget, disconnect, set a
 * profile) and records every event. For the snapshot tool and the tests.
 * Sample names only.
 */
#ifndef UI_SAMPLE_H
#define UI_SAMPLE_H
#include "nav.h"

enum sample_set { SAMPLE_FIVE, SAMPLE_TWO, SAMPLE_NONE, SAMPLE_NAMES, SAMPLE_NINE, SAMPLE_LONG };

extern struct ui_model sample;
void sample_reset(enum sample_set which);
void sample_event(enum ui_event ev, int arg, int value);

struct sample_record {
	enum ui_event ev;
	int arg, value;
};
extern struct sample_record sample_log[256];
extern int sample_logged;
const char *sample_event_name(enum ui_event ev);
#endif
