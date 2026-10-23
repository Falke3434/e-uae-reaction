/*
 * E-UAE - ReAction GUI: Seitentabelle (Step 5 - komplett)
 *
 *   Medien, Hardware, Festplatten, Display, Sound, Eingabe, Konfiguration
 */

#include "reaction_gui.h"

extern const struct ra_page ra_page_media;
extern const struct ra_page ra_page_hardware;
extern const struct ra_page ra_page_harddisk;
extern const struct ra_page ra_page_display;
extern const struct ra_page ra_page_sound;
extern const struct ra_page ra_page_input;
extern const struct ra_page ra_page_shortcuts;
extern const struct ra_page ra_page_config;

const struct ra_page *const ra_pages[] = {
    &ra_page_media,
    &ra_page_hardware,
    &ra_page_harddisk,
    &ra_page_display,
    &ra_page_sound,
    &ra_page_input,
    &ra_page_shortcuts,
    &ra_page_config,
};

const int ra_num_pages = sizeof ra_pages / sizeof ra_pages[0];
