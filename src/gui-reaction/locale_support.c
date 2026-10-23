/*
 * E-UAE - ReAction GUI: Katalog-(Uebersetzungs-)Unterstuetzung
 *
 * Alle sichtbaren Texte stehen im Quelltext auf Englisch. ra_tr() sucht den
 * englischen Text in der eingebauten Tabelle (locale_defs.c, erzeugt aus
 * Catalogs/E-UAE.cd durch tools/mkcatalog.py) und fragt locale.library nach
 * der Uebersetzung aus PROGDIR:Catalogs/<Sprache>/E-UAE.catalog bzw.
 * LOCALE:Catalogs/<Sprache>/. Ohne Katalog (oder bei fehlendem Eintrag)
 * bleibt der englische Text.
 *
 * locale.library wird hier selbst geoeffnet (eigene Variablen, um nicht mit
 * libauto zu kollidieren).
 */

#include <exec/types.h>
#include <utility/tagitem.h>
#include <libraries/locale.h>
#include <interfaces/locale.h>
#include <proto/exec.h>
#include <string.h>
#include <stdlib.h>

#include "locale_ids.h"
#include "reaction_gui.h"

static struct Library      *loc_base;
static struct LocaleIFace  *loc_if;
static struct Catalog      *loc_cat;

struct tr_array {
    struct tr_array *next;
    const char      *items[1];
};
static struct tr_array *tr_arrays;

void ra_locale_open (void)
{
    if (loc_cat || loc_if)
	return;

    loc_base = OpenLibrary ("locale.library", 38);
    if (!loc_base)
	return;
    loc_if = (struct LocaleIFace *) GetInterface (loc_base, "main", 1, NULL);
    if (!loc_if) {
	CloseLibrary (loc_base);
	loc_base = NULL;
	return;
    }
    loc_cat = loc_if->OpenCatalog (NULL, "E-UAE.catalog",
				   OC_BuiltInLanguage, "english",
				   TAG_DONE);
}

/* Erst nach dem Zerstoeren des Fensters aufrufen: Gadgets/Labels halten
 * Zeiger in den Katalog. */
void ra_locale_close (void)
{
    while (tr_arrays) {
	struct tr_array *n = tr_arrays->next;
	free (tr_arrays);
	tr_arrays = n;
    }
    if (loc_cat && loc_if)
	loc_if->CloseCatalog (loc_cat);
    loc_cat = NULL;
    if (loc_if)
	DropInterface ((struct Interface *) loc_if);
    loc_if = NULL;
    if (loc_base)
	CloseLibrary (loc_base);
    loc_base = NULL;
}

const char *ra_tr (const char *english)
{
    int id;

    if (!english || !loc_cat || !loc_if)
	return english;
    for (id = 1; id <= MSG_MAX_ID; id++)
	if (strcmp (g_MsgDefault[id], english) == 0)
	    return (const char *) loc_if->GetCatalogStr (loc_cat, id, (CONST_STRPTR) english);
    return english;
}

/* Uebersetztes NULL-terminiertes Array (bis ra_locale_close gueltig). */
const char *const *ra_tr_array (const char *const *names)
{
    struct tr_array *a;
    int n = 0, i;

    if (!loc_cat)
	return names;
    while (names[n])
	n++;
    a = malloc (sizeof *a + n * sizeof (char *));
    if (!a)
	return names;
    for (i = 0; i < n; i++)
	a->items[i] = ra_tr (names[i]);
    a->items[n] = NULL;
    a->next = tr_arrays;
    tr_arrays = a;
    return a->items;
}
