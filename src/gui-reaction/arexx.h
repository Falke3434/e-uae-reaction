/*
 * E-UAE - ReAction GUI: ARexx port "UAE" (v18), see arexx.c
 */
#ifndef EUAE_REACTION_AREXX_H
#define EUAE_REACTION_AREXX_H

int  rexx_init (void);
void rexx_exit (void);
void rexx_handle_events (void);
void rexx_led (int led, int on);
void rexx_filename (int num, const char *filename);

#endif
