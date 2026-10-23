 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Target-independent support for parsing hotkey sequences
  *
  * Copyright 2004 Richard Drummond
  *
  * Based on the hotkey handling previously found
  * in x11win.c and sdlgfx.c
  */

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "inputdevice.h"
#include "hotkeys.h"


static struct uae_hotkeyseq *hotkey_table;


void set_default_hotkeys (struct uae_hotkeyseq *keys)
{
    hotkey_table = keys;
    reset_hotkeys ();
}

void reset_hotkeys (void) {
    struct uae_hotkeyseq *seq = hotkey_table;
    int i;

    if (!seq)
	return;

    while (seq->event) {
	for (i=0; i < HOTKEYSEQ_MAXKEYS; i++)
	    seq->key_pressed[i] = 0;
	seq->complete = 0;
	seq++;
    }
}

/*
 * Key down: all keys of a sequence except the last one (the modifiers) may
 * be pressed in any order; the last key has to come last. Before, the keys
 * had to be pressed exactly in table order (Ctrl+Alt worked, Alt+Ctrl not).
 * If several sequences match (e.g. Ctrl+Alt+R and Ctrl+Alt+Shift+R), the
 * longest one wins.
 */
int match_hotkey_sequence (int key, int state)
{
    struct uae_hotkeyseq *seq = hotkey_table;
    struct uae_hotkeyseq *best = 0;
    int bestlen = 0;
    int event = 0;

    if (!seq)
	return 0;

    if (state) {
	/* Key down */
	while (seq->event != 0) {
	    int i, last = -1;

	    for (i = 0; i < HOTKEYSEQ_MAXKEYS && seq->keyseq[i] != (uae_u16)-1; i++)
		last = i;
	    for (i = 0; i <= last; i++)
		if (key == seq->keyseq[i])
		    seq->key_pressed[i] = 1;
	    if (last >= 0 && key == seq->keyseq[last]) {
		/* final key: are all modifiers held down? */
		for (i = 0; i < last; i++)
		    if (!seq->key_pressed[i])
			break;
		if (i == last && last + 1 > bestlen) {
		    best = seq;
		    bestlen = last + 1;
		}
	    }
	    seq++;
	}
	if (best) {
	    best->complete = 1;
	    event = best->event;
	}
    } else {
	/* key up */
	while (seq->event != 0) {
	    int i, j;
	    for (i = 0; i < HOTKEYSEQ_MAXKEYS; i++) {
		if ((key == seq->keyseq[i]) && (seq->key_pressed[i] == 1)) {
		    /* letting up a key: only this key is no longer held (the
		     * order of the modifiers doesn't matter any more) */
		    seq->key_pressed[i] = 0;
		    /* if it was previously complete generate return the event
		     * (so that it can be used as a key-up event */
		    if (seq->complete) {
			seq->complete = 0;
			event = seq->event;
		    }
		}
	    }
	    seq++;
	}
    }
    return event;
}
