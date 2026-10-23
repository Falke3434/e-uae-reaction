 /*
  * UAE - The Un*x Amiga Emulator
  *
  * Gayle (A600/A1200) and A4000 IDE controller emulation
  *
  * Ported from WinUAE gayle.cpp / ide.cpp (c) 2006-2015 Toni Wilen
  */

#ifndef UAE_GAYLE_H
#define UAE_GAYLE_H

/* Values of currprefs.cs_ide */
#define IDE_NONE        0
#define IDE_A600A1200   1
#define IDE_A4000       2

extern addrbank gayle_bank;
extern addrbank gayle2_bank;

/* Open/close the host files, reset the controller and the drives.
 * Called from memory_reset(). */
extern void gayle_reset (void);
/* Close the host files */
extern void gayle_cleanup (void);
/* Map the Gayle register banks into the Amiga address space.
 * Called from memory_reset() after the custom/clock banks are mapped. */
extern void gayle_map_banks (void);
/* Delayed IDE interrupts; called once per horizontal sync */
extern void gayle_hsync_handler (void);

#endif /* UAE_GAYLE_H */
