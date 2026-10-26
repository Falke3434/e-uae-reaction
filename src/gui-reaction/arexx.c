/*
 * E-UAE - ReAction GUI: ARexx port "UAE" (v18)
 *
 * Taken over from gui-muirexx/ami-rexx.c (Samuel Devulder, Richard
 * Drummond), which was only built with the old MUIRexx GUI - E-UAE with the
 * ReAction GUI had no ARexx port, so RunInUAE could not control it.
 *
 * Commands (as before):
 *   QUIT | BYE                    quit E-UAE
 *   RESET                         reset the emulated Amiga
 *   INSERT <drive> "<file>"       insert a disk image (drive 0-3)
 *   EJECT <drive>                 eject the disk
 *   UAEEXE "<command>"            run a command in the emulated Amiga
 *                                 (C:uae_rcli must run there, e.g. RunInUAE)
 *   QUERY LED_POW|LED_DF0..3|NAME_DF0..3|DISPLAY|FRAMERATE|FRAMENUM|SOUND
 *   FRAMERATE <1-20>              show every n-th frame
 *   DISPLAY ON|OFF|TOGGLE         switch the picture output on/off
 *   SOUND ON|OFF|BEST|TOGGLE
 *   FEEDBACK LED_xxx|NAME_DFx|ON_EXIT PORT <port> CMD_ON <cmd> [CMD_OFF <cmd>]
 *   VERSION STRING|NUM|AUTHOR|PORT
 *   DEBUG                         enter the debugger
 *
 * Changes against ami-rexx.c: OS4 only (public port with
 * AllocSysObject()/AddPort(), no amiga.lib CreatePort()), RESET through
 * uae_reset() (was m68k_reset() in the middle of the CPU loop), DISPLAY and
 * QUERY DISPLAY only look at the display switch bit (inhibit_frame is a bit
 * mask, an RTG screen sets IHF_PICASSO), a second E-UAE runs without port.
 */

#include "sysconfig.h"
#include "sysdeps.h"

#include "uae.h"
#include "options.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "disk.h"
#include "audio.h"
#include "uaeexe.h"
#include "xwin.h"
#include "drawing.h"
#include "debug.h"
#include "version.h"

#ifndef __USE_INLINE__
#define __USE_INLINE__
#endif

#include <exec/types.h>
#include <exec/ports.h>
#include <exec/tasks.h>
#include <rexx/storage.h>
#include <rexx/errors.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/rexxsyslib.h>

/* rexx/rexxio.h vs. sys/dirent.h */
#undef DT_DIR

#include <ctype.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "arexx.h"

#define UAE_PORTNAME    "UAE"
#define RESULT_LEN      1024
#define PORT_LEN        80
#define CMD_LEN         1024

typedef struct {
    char port[PORT_LEN];
    char cmd_on[CMD_LEN];
    char cmd_off[CMD_LEN];
} gui_rexx_s;

enum {
    LED_POW, LED_DF0, LED_DF1, LED_DF2, LED_DF3,
    NAME_DF0, NAME_DF1, NAME_DF2, NAME_DF3,
    ON_EXIT,
    GUI_REXX_MAX
};

struct Library        *RexxSysBase = NULL;
struct RexxSysIFace   *IRexxSys = NULL;

static struct MsgPort *ARexxPort;
static gui_rexx_s      gui_rexx[GUI_REXX_MAX];
static char            RESULT[RESULT_LEN];
static int             led_state[5];

extern unsigned long frame_num;                          /* ami-win.c */

static int  ADDRESS    (const char *hostname, const char *cmd);
static int  matchstr   (const char **line, const char *pat);
static void extractstr (const char **line, char *result, unsigned int len);
static int  matchnum   (const char **line);

/****************************************************************************/

int rexx_init (void)
{
    struct MsgPort *port;

    if (ARexxPort)
	return 1;

    RexxSysBase = OpenLibrary ("rexxsyslib.library", 0L);
    if (!RexxSysBase) {
	write_log ("ARexx: rexxsyslib.library not found - no ARexx port\n");
	return 0;
    }
    IRexxSys = (struct RexxSysIFace *) GetInterface (RexxSysBase, "main", 1, NULL);
    if (!IRexxSys) {
	CloseLibrary (RexxSysBase);
	RexxSysBase = NULL;
	return 0;
    }

    /* v18: the first version created the port with ASOPORT_Name and
     * then looked for the name - OS4 already makes a named port public,
     * so E-UAE found its own port ("already exists") and ran without.
     * Now: look first, then create an unnamed port and add it with
     * AddPort(). */
    Forbid ();
    port = FindPort (UAE_PORTNAME);
    if (port) {
	struct Task *owner = port->mp_SigTask;
	char who[64];
	snprintf (who, sizeof who, "%s",
		  owner && owner->tc_Node.ln_Name ? owner->tc_Node.ln_Name : "?");
	Permit ();
	write_log ("ARexx: port \"%s\" already exists (task \"%s\") - running without ARexx port\n",
		   UAE_PORTNAME, who);
	rexx_exit ();
	return 0;
    }
    Permit ();

    port = AllocSysObjectTags (ASOT_PORT, TAG_END);
    if (!port) {
	write_log ("ARexx: could not create the port \"%s\"\n", UAE_PORTNAME);
	rexx_exit ();
	return 0;
    }
    port->mp_Node.ln_Name = (STRPTR) UAE_PORTNAME;
    port->mp_Node.ln_Pri = 0;

    Forbid ();
    if (FindPort (UAE_PORTNAME)) {
	Permit ();
	port->mp_Node.ln_Name = NULL;
	FreeSysObject (ASOT_PORT, port);
	write_log ("ARexx: port \"%s\" appeared meanwhile - running without ARexx port\n", UAE_PORTNAME);
	rexx_exit ();
	return 0;
    }
    AddPort (port);
    Permit ();

    ARexxPort = port;
    write_log ("ARexx: port \"%s\" installed\n", UAE_PORTNAME);
    return 1;
}

/****************************************************************************/

void rexx_exit (void)
{
    if (ARexxPort) {
	struct RexxMsg *msg;
	gui_rexx_s *gui = &gui_rexx[ON_EXIT];

	if (gui->port[0] && gui->cmd_on[0]) {
	    if (ADDRESS (gui->port, gui->cmd_on) != RC_OK)
		write_log ("%s:%s:%s\n", gui->port, gui->cmd_on, RESULT);
	    gui->port[0] = '\0';
	}
	Forbid ();
	RemPort (ARexxPort);
	while ((msg = (struct RexxMsg *) GetMsg (ARexxPort))) {
	    msg->rm_Result1 = RC_ERROR;
	    msg->rm_Result2 = 0;
	    ReplyMsg ((struct Message *) msg);
	}
	Permit ();
	ARexxPort->mp_Node.ln_Name = NULL;	/* not allocated by AllocSysObject() */
	FreeSysObject (ASOT_PORT, ARexxPort);
	ARexxPort = NULL;
    }
    if (IRexxSys) {
	DropInterface ((struct Interface *) IRexxSys);
	IRexxSys = NULL;
    }
    if (RexxSysBase) {
	CloseLibrary (RexxSysBase);
	RexxSysBase = NULL;
    }
}

/****************************************************************************/

static int EJECT (const char *line)
{
    int drive = matchnum (&line);

    if (drive < 0 || drive > 3) {
	sprintf (RESULT, "Invalid drive: %d", drive);
	return RC_WARN;
    }
    if (currprefs.dfxtype[drive] < 0) {
	sprintf (RESULT, "Drive %d is disabled.", drive);
	return RC_WARN;
    }
    disk_eject (drive);
    sprintf (RESULT, "Drive %d ejected.", drive);
    return RC_OK;
}

static int INSERT (const char *line)
{
    int drive = matchnum (&line);
    char buff[256];

    if (drive < 0 || drive > 3) {
	sprintf (RESULT, "Invalid drive: %d", drive);
	return RC_WARN;
    }
    if (currprefs.dfxtype[drive] < 0) {
	sprintf (RESULT, "Drive %d is disabled.", drive);
	return RC_WARN;
    }
    extractstr (&line, buff, sizeof buff);
    snprintf (changed_prefs.df[drive], sizeof changed_prefs.df[drive], "%s", buff);
    return RC_OK;
}

/* display switch (hotkey / Scroll Lock) only, see set_title() in ami-win.c */
static int display_on (void)
{
    return (inhibit_frame & (1 << IHF_SCROLLLOCK)) ? 0 : 1;
}

static int QUERY (const char *line)
{
    const char *res = NULL;

    if      (matchstr (&line, "LED_POW"))      res = led_state[0] ? "1" : "0";
    else if (matchstr (&line, "LED_DF0"))      res = led_state[1] ? "1" : "0";
    else if (matchstr (&line, "LED_DF1"))      res = led_state[2] ? "1" : "0";
    else if (matchstr (&line, "LED_DF2"))      res = led_state[3] ? "1" : "0";
    else if (matchstr (&line, "LED_DF3"))      res = led_state[4] ? "1" : "0";
    else if (matchstr (&line, "NAME_DF0"))     res = currprefs.df[0];
    else if (matchstr (&line, "NAME_DF1"))     res = currprefs.df[1];
    else if (matchstr (&line, "NAME_DF2"))     res = currprefs.df[2];
    else if (matchstr (&line, "NAME_DF3"))     res = currprefs.df[3];
    else if (matchstr (&line, "FAKEJOYSTICK")) res = "0";
    else if (matchstr (&line, "DISPLAY"))      res = display_on () ? "1" : "0";
    else if (matchstr (&line, "FRAMERATE")) {
	sprintf (RESULT, "%d", currprefs.gfx_framerate);
	return RC_OK;
    } else if (matchstr (&line, "FRAMENUM")) {
	sprintf (RESULT, "%lu", frame_num);
	return RC_OK;
    } else if (matchstr (&line, "SOUND")) {
	sprintf (RESULT, "%d", sound_available ? currprefs.produce_sound : -1);
	return RC_OK;
    } else
	return RC_ERROR;

    strncpy (RESULT, res, RESULT_LEN - 1);
    RESULT[RESULT_LEN - 1] = '\0';
    return RC_OK;
}

static int FEEDBACK (const char *line)
{
    gui_rexx_s *gui = NULL;

    if      (matchstr (&line, "LED_POW"))  gui = &gui_rexx[LED_POW];
    else if (matchstr (&line, "LED_DF0"))  gui = &gui_rexx[LED_DF0];
    else if (matchstr (&line, "LED_DF1"))  gui = &gui_rexx[LED_DF1];
    else if (matchstr (&line, "LED_DF2"))  gui = &gui_rexx[LED_DF2];
    else if (matchstr (&line, "LED_DF3"))  gui = &gui_rexx[LED_DF3];
    else if (matchstr (&line, "NAME_DF0")) gui = &gui_rexx[NAME_DF0];
    else if (matchstr (&line, "NAME_DF1")) gui = &gui_rexx[NAME_DF1];
    else if (matchstr (&line, "NAME_DF2")) gui = &gui_rexx[NAME_DF2];
    else if (matchstr (&line, "NAME_DF3")) gui = &gui_rexx[NAME_DF3];
    else if (matchstr (&line, "ON_EXIT"))  gui = &gui_rexx[ON_EXIT];
    else return RC_ERROR;

    for (;;) {
	if (matchstr (&line, "ADDRESS") || matchstr (&line, "PORT"))
	    extractstr (&line, gui->port, PORT_LEN);
	else if (matchstr (&line, "COMMAND") || matchstr (&line, "CMD") || matchstr (&line, "CMD_ON"))
	    extractstr (&line, gui->cmd_on, CMD_LEN);
	else if (matchstr (&line, "CMD_OFF"))
	    extractstr (&line, gui->cmd_off, CMD_LEN);
	else
	    break;
    }
    return RC_OK;
}

static int GET_VERSION (const char *line)
{
    if (matchstr (&line, "STRING"))
	snprintf (RESULT, RESULT_LEN, PACKAGE_NAME "-" PACKAGE_VERSION
		  " by Richard Drummond & contributors, ReAction GUI by Falke_34");
    else if (matchstr (&line, "NUM"))
	snprintf (RESULT, RESULT_LEN, "%d", UAEMAJOR * 10000 + UAEMINOR * 100 + UAESUBREV);
    else if (matchstr (&line, "AUTHOR"))
	snprintf (RESULT, RESULT_LEN, "Richard Drummond & contributors");
    else if (matchstr (&line, "PORT"))
	snprintf (RESULT, RESULT_LEN, "AmigaOS 4 port, ReAction GUI");
    else
	return RC_ERROR;
    return RC_OK;
}

static int FRAMERATE (const char *line)
{
    int num = matchnum (&line);

    if (num < 1 || num > 20) {
	sprintf (RESULT, "Invalid frame rate: %d", num);
	return RC_WARN;
    }
    changed_prefs.gfx_framerate = num;
    return RC_OK;
}

static int DISPLAY (const char *line)
{
    if      (matchstr (&line, "ON"))     clear_inhibit_frame (IHF_SCROLLLOCK);
    else if (matchstr (&line, "OFF"))    set_inhibit_frame (IHF_SCROLLLOCK);
    else if (matchstr (&line, "TOGGLE")) toggle_inhibit_frame (IHF_SCROLLLOCK);
    else return RC_ERROR;
    return RC_OK;
}

static int SOUND (const char *line)
{
    if (!sound_available) {
	sprintf (RESULT, "Sound not available!");
	return RC_WARN;
    }
    if      (matchstr (&line, "ON"))     currprefs.produce_sound = 2;
    else if (matchstr (&line, "OFF"))    currprefs.produce_sound = 1;
    else if (matchstr (&line, "BEST"))   currprefs.produce_sound = 3;
    else if (matchstr (&line, "TOGGLE")) currprefs.produce_sound = currprefs.produce_sound <= 1 ? 2 : 1;
    else return RC_ERROR;
    return RC_OK;
}

static int DEBUG (void)
{
#ifdef DEBUGGER
    activate_debugger ();
    return RC_OK;
#else
    return RC_ERROR;
#endif
}

static int UAEEXE (const char *line)
{
    if (uaeexe (line)) {
	sprintf (RESULT, "Remote CLI failed!");
	return RC_WARN;
    }
    return RC_OK;
}

static int process_cmd (const char *line)
{
    RESULT[0] = '\0';
    if      (matchstr (&line, "EJECT"))        return EJECT (line);
    else if (matchstr (&line, "INSERT"))       return INSERT (line);
    else if (matchstr (&line, "QUERY"))        return QUERY (line);
    else if (matchstr (&line, "FEEDBACK"))     return FEEDBACK (line);
    else if (matchstr (&line, "VERSION"))      return GET_VERSION (line);
    else if (matchstr (&line, "BYE"))          uae_quit ();
    else if (matchstr (&line, "QUIT"))         uae_quit ();
    else if (matchstr (&line, "DEBUG"))        return DEBUG ();
    else if (matchstr (&line, "RESET"))        uae_reset (0);
    else if (matchstr (&line, "DISPLAY"))      return DISPLAY (line);
    else if (matchstr (&line, "FRAMERATE"))    return FRAMERATE (line);
    else if (matchstr (&line, "FAKEJOYSTICK")) return RC_OK;
    else if (matchstr (&line, "SOUND"))        return SOUND (line);
    else if (matchstr (&line, "UAEEXE"))       return UAEEXE (line);
    else return RC_ERROR;
    return RC_OK;
}

/****************************************************************************/

void rexx_handle_events (void)
{
    struct RexxMsg *msg;

    if (!ARexxPort)
	return;
    while ((msg = (struct RexxMsg *) GetMsg (ARexxPort))) {
	if ((msg->rm_Action & RXCODEMASK) != RXCOMM) {
	    write_log ("ARexx: unknown action %08lx\n", (unsigned long) msg->rm_Action);
	    msg->rm_Result1 = RC_ERROR;
	    msg->rm_Result2 = 0;
	    ReplyMsg ((struct Message *) msg);
	    continue;
	}
	msg->rm_Result1 = process_cmd ((const char *) msg->rm_Args[0]);
	msg->rm_Result2 = 0;
	if ((msg->rm_Action & RXFF_RESULT) && msg->rm_Result1 == RC_OK)
	    msg->rm_Result2 = (int32) CreateArgstring (RESULT, strlen (RESULT));
	ReplyMsg ((struct Message *) msg);
    }
}

void rexx_led (int led, int on)
{
    gui_rexx_s *gui;

    if (led < 0 || led > 4)
	return;
    led_state[led] = on;
    gui = &gui_rexx[LED_POW + led];
    if (ARexxPort && gui->port[0] && gui->cmd_on[0] && gui->cmd_off[0]) {
	if (ADDRESS (gui->port, on ? gui->cmd_on : gui->cmd_off) != RC_OK)
	    write_log ("%s:%s:%s\n", gui->port, on ? gui->cmd_on : gui->cmd_off, RESULT);
    }
}

void rexx_filename (int num, const char *filename)
{
    gui_rexx_s *gui;

    if (num < 0 || num > 3)
	return;
    gui = &gui_rexx[NAME_DF0 + num];
    if (ARexxPort && gui->port[0] && gui->cmd_on[0]) {
	char buf[CMD_LEN];
	/* cmd_on is a format with %s for the file name */
	snprintf (buf, sizeof buf, gui->cmd_on, filename ? filename : "");
	if (ADDRESS (gui->port, buf) != RC_OK)
	    write_log ("%s:%s:%s\n", gui->port, buf, RESULT);
    }
}

/****************************************************************************/

/* send a command to an ARexx port ("COMMAND" = run it as a shell command) */
static int ADDRESS (const char *hostname, const char *cmd)
{
    struct MsgPort *RexxPort, *ReplyPort;
    struct RexxMsg *HostMsg, *answer;
    int result = RC_WARN;

    if (!strcasecmp (hostname, "COMMAND"))
	return SystemTagList (cmd, NULL);

    Forbid ();
    RexxPort = FindPort (hostname);
    Permit ();
    if (!RexxPort) {
	snprintf (RESULT, RESULT_LEN, "Port \"%s\" not found!", hostname);
	return result;
    }
    if (!(ReplyPort = AllocSysObjectTags (ASOT_PORT, TAG_END))) {
	strcpy (RESULT, "Can't alloc reply port!");
	return result;
    }
    if ((HostMsg = CreateRexxMsg (ReplyPort, NULL, hostname))) {
	if ((HostMsg->rm_Args[0] = (STRPTR) CreateArgstring (cmd, strlen (cmd)))) {
	    HostMsg->rm_Action = RXCOMM | RXFF_RESULT;
	    Forbid ();
	    RexxPort = FindPort (hostname);
	    if (RexxPort)
		PutMsg (RexxPort, (struct Message *) HostMsg);
	    Permit ();
	    if (RexxPort) {
		WaitPort (ReplyPort);
		answer = (struct RexxMsg *) GetMsg (ReplyPort);
		result = answer->rm_Result1;
		if (result == RC_OK) {
		    if (answer->rm_Result2) {
			strncpy (RESULT, (const char *) answer->rm_Result2, RESULT_LEN - 1);
			RESULT[RESULT_LEN - 1] = '\0';
			DeleteArgstring ((STRPTR) answer->rm_Result2);
		    } else
			RESULT[0] = '\0';
		}
	    } else
		snprintf (RESULT, RESULT_LEN, "Port \"%s\" not found!", hostname);
	    DeleteArgstring (HostMsg->rm_Args[0]);
	} else
	    strcpy (RESULT, "Can't create argstring!");
	DeleteRexxMsg (HostMsg);
    } else
	strcpy (RESULT, "Can't create rexx message!");
    FreeSysObject (ASOT_PORT, ReplyPort);
    return result;
}

/****************************************************************************/
/* argument parsing */

static int matchstr (const char **line, const char *pat)
{
    const char *s = *line;
    char match = 0;

    while (isspace ((unsigned char) *s))
	++s;
    if (*s == '\"' || *s == '\'')
	match = *s++;
    while (*s && (tolower ((unsigned char) *s) == tolower ((unsigned char) *pat)) && (!match || *s != match)) {
	++s;
	++pat;
    }
    if (match && *s == match && s[1])
	++s;
    if (!*pat && (!*s || isspace ((unsigned char) *s))) {
	while (isspace ((unsigned char) *s))
	    ++s;
	*line = s;
	return 1;
    }
    return 0;
}

static void extractstr (const char **line, char *result, unsigned int len)
{
    const char *s = *line;
    char match = 0;

    while (isspace ((unsigned char) *s))
	++s;
    if (*s == '\"' || *s == '\'')
	match = *s++;
    while (*s && *s != match) {
	if (*s == '\\' && (s[1] == '\'' || s[1] == '\"'))
	    ++s;
	if (len > 1) {
	    *result++ = *s;
	    --len;
	}
	++s;
	if (!match && isspace ((unsigned char) *s))
	    break;
    }
    if (match && *s == match)
	++s;
    while (isspace ((unsigned char) *s))
	++s;
    *result = '\0';
    *line = s;
}

static int matchnum (const char **line)
{
    const char *s = *line;
    char match = 0;
    int sign = 1, num = 0;

    while (isspace ((unsigned char) *s))
	++s;
    if (*s == '\"' || *s == '\'')
	match = *s++;
    if (*s == '-') {
	sign = -1;
	++s;
    }
    if (*s == '+')
	++s;
    while (isspace ((unsigned char) *s))
	++s;
    while (*s >= '0' && *s <= '9')
	num = num * 10 + (*s++ - '0');
    if (match && *s == match && s[1])
	++s;
    while (isspace ((unsigned char) *s))
	++s;
    *line = s;
    return sign > 0 ? num : -num;
}
