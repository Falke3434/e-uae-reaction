/*
 * E-UAE - The portable Amiga Emulator
 *
 * bsdsocket.library emulation - AmigaOS 4 host side (worker processes).
 * See bsdsock_proxy.h for the design. v17 rewrite.
 *
 * Copyright 2026 Falke_34 et al. (based on the OS4 port by Kjetil
 * Hvalstrand). License: GPL version 2 or later.
 *
 * The workers are plain AmigaOS processes: no newlib in this file (exec
 * and dos only), see the note in bsdsocket.c.
 */

#include "sysconfig.h"
#include "sysdeps.h"

#include "threaddep/thread.h"
#include "options.h"
#include "include/memory.h"
#include "custom.h"
#include "newcpu.h"
#include "autoconf.h"
#include "traps.h"
#include "native2amiga.h"

#include <exec/exec.h>
#include <dos/dos.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#undef __USE_INLINE__
#include <proto/bsdsocket.h>
#include <libraries/bsdsocket.h>

#include "od-amiga/bsdsock_proxy.h"

#define BSD_MAXW 8              /* worker processes per SocketBase */

/*
 * v17: several workers sharing one Roadshow base (SBTC_CAN_SHARE_LIBRARY_
 * BASES) froze E-UAE as soon as IBrowse started (tester, QEMU): most
 * likely Roadshow serialises the calls of a shared base, so a short call
 * on a second worker waited behind a WaitSelect() of the first one -
 * with the emulation waiting for that short call. Off until it can be
 * tested on real hardware: one worker per SocketBase. Short calls then
 * only run directly when that worker is idle, everything else waits in
 * the calling Amiga task, so the emulation never waits for the network.
 */
#define BSD_PARALLEL 0

/*
 * v17: signal that breaks a Roadshow call of a worker. NOT CTRL-C: the
 * worker calls uae_Signal(), whose semaphores (td-amigaos/thread.c,
 * commpipe.h) treat CTRL-C as "interrupted" and then go on without
 * holding the lock. A CTRL-C left over from an abort broke the lock of
 * the native2amiga pipe, and E-UAE hung reading it (tester: GUI dead
 * while the OS4 Workbench kept running). Roadshow breaks on CTRL-E.
 */
#define BSD_ABORTSIG SIGBREAKF_CTRL_E

struct bsd_worker {
    struct bsd_proxy *px;
    int index;
    struct Process *proc;
    struct MsgPort *port;
    volatile int state;         /* 0 starting, 1 running, -1 failed/ended */
    volatile int busy;          /* under px->mutex */
    struct Task *creator;
    int creatorbit;
    struct bsd_msg quitmsg;
};

struct bsd_proxy {
    APTR mutex;                 /* busy flags, pending list, nworkers */
    struct Library *base;       /* opened by worker 0 */
    struct SocketIFace *IS;
    int shared;                 /* Roadshow accepted base sharing */
    int nworkers;
    int creating;
    struct List pending;        /* calls waiting for a free worker */
    volatile int resetting;     /* Amiga state gone: no more uae_Signal() */
    volatile int quitting;      /* calls still waiting get EINTR */
    volatile int orphaned;      /* a worker did not end: px is leaked */
    struct bsd_worker w[BSD_MAXW];
};

struct SocketIFace *bsdproxy_iface (struct bsd_msg *m)
{
    return m->px->IS;
}

void bsdproxy_set_errno (struct bsd_msg *m, int err)
{
    struct TagItem t[2];

    t[0].ti_Tag  = SBTM_SETVAL (SBTC_ERRNO);
    t[0].ti_Data = err;
    t[1].ti_Tag  = TAG_DONE;
    t[1].ti_Data = 0;
    m->px->IS->SocketBaseTagList (t);
}

/* Mark m done and wake whoever waits for it. After this the worker must
 * not touch m: the caller may return (and free m) at once. */
static void complete (struct bsd_proxy *px, struct bsd_msg *m)
{
    if (m->waiter) {
	/* Forbid: the waiter must not run (and return) between done = 1
	 * and Signal(), else Signal() could hit a task that is gone */
	Forbid ();
	m->done = 1;
	Signal (m->waiter, 1UL << m->waitbit);
	Permit ();
    } else {
	uae_u32 task = m->guest_task;
	uae_u32 mask = m->guest_mask;
	int reset = px->resetting;

	m->done = 1;
	__sync_synchronize ();
	if (task && !reset)
	    uae_Signal (task, mask);
    }
}

static void run_msg (struct bsd_worker *w, struct bsd_msg *m, int quitting)
{
    struct bsd_proxy *px = w->px;

    m->px = px;
    Forbid ();
    m->runner = (struct Task *) w->proc;
    Permit ();
    /* an abort signal left over from an earlier call must not break
     * this one; an abort that came before this call starts is seen in
     * m->abort (it is set before the signal is sent) */
    SetSignal (0, BSD_ABORTSIG);
    if (m->abort || quitting || !px->IS) {
	m->result = -1;
	if (px->IS)
	    bsdproxy_set_errno (m, 4);     /* EINTR */
    } else {
	m->fn (m);
    }
    Forbid ();
    m->runner = NULL;
    Permit ();
    SetSignal (0, BSD_ABORTSIG);   /* nothing left over (see BSD_ABORTSIG) */
    complete (px, m);
}

static int32 worker_main (STRPTR args, int32 len, struct ExecBase *sysbase)
{
    struct bsd_worker *w = (struct bsd_worker *) FindTask (NULL)->tc_UserData;
    struct bsd_proxy *px = w->px;
    struct bsd_msg *m;
    int ok;

    w->port = AllocSysObjectTags (ASOT_PORT, TAG_END);
    ok = w->port != NULL;
    if (ok && w->index == 0) {
	px->base = OpenLibrary ("bsdsocket.library", 4);
	if (px->base)
	    px->IS = (struct SocketIFace *) GetInterface (px->base, "main", 1, NULL);
	ok = px->IS != NULL;
	if (ok) {
	    /* Roadshow breaks a waiting call on this signal (default CTRL-C) */
	    struct TagItem t[2];
	    t[0].ti_Tag  = SBTM_SETVAL (SBTC_BREAKMASK);
	    t[0].ti_Data = BSD_ABORTSIG;
	    t[1].ti_Tag  = TAG_DONE;
	    t[1].ti_Data = 0;
	    px->IS->SocketBaseTagList (t);
	}
	if (ok && BSD_PARALLEL) {
	    /* let the other workers use this base too */
	    struct TagItem t[2];
	    t[0].ti_Tag  = SBTM_SETVAL (SBTC_CAN_SHARE_LIBRARY_BASES);
	    t[0].ti_Data = TRUE;
	    t[1].ti_Tag  = TAG_DONE;
	    t[1].ti_Data = 0;
	    px->shared = px->IS->SocketBaseTagList (t) == 0;
	}
    }
    if (!ok) {
	if (w->index == 0) {
	    if (px->IS)
		DropInterface ((struct Interface *) px->IS);
	    if (px->base)
		CloseLibrary (px->base);
	    px->IS = NULL;
	    px->base = NULL;
	}
	if (w->port)
	    FreeSysObject (ASOT_PORT, w->port);
	w->port = NULL;
	Forbid ();
	w->state = -1;
	Signal (w->creator, 1UL << w->creatorbit);
	Permit ();
	return 0;
    }

    Forbid ();
    w->state = 1;
    Signal (w->creator, 1UL << w->creatorbit);
    Permit ();

    for (;;) {
	WaitPort (w->port);
	m = (struct bsd_msg *) GetMsg (w->port);
	if (!m)
	    continue;
	if (!m->fn)
	    break;                       /* quit (w->quitmsg) */
	/* run it, then take waiting calls until there are none */
	for (;;) {
	    run_msg (w, m, px->quitting);
	    MutexObtain (px->mutex);
	    m = (struct bsd_msg *) RemHead (&px->pending);
	    if (!m)
		w->busy = 0;
	    MutexRelease (px->mutex);
	    if (!m)
		break;
	}
    }

    /* ending: nothing new is sent to this worker any more */
    while ((m = (struct bsd_msg *) GetMsg (w->port)))
	if (m->fn)
	    run_msg (w, m, 1);
    if (w->index == 0) {
	/* last worker: calls still waiting get EINTR, then the Roadshow
	 * base is closed - that closes all its sockets */
	for (;;) {
	    MutexObtain (px->mutex);
	    m = (struct bsd_msg *) RemHead (&px->pending);
	    MutexRelease (px->mutex);
	    if (!m)
		break;
	    run_msg (w, m, 1);
	}
	DropInterface ((struct Interface *) px->IS);
	CloseLibrary (px->base);
	px->IS = NULL;
	px->base = NULL;
    }
    FreeSysObject (ASOT_PORT, w->port);
    w->port = NULL;

    Forbid ();
    w->state = -1;
    w->quitmsg.done = 1;             /* the owner may free px after this */
    Permit ();
    return 0;
}

static int start_worker (struct bsd_proxy *px, int i)
{
    struct bsd_worker *w = &px->w[i];
    BYTE bit = AllocSignal (-1);

    if (bit < 0)
	return 0;
    w->px = px;
    w->index = i;
    w->state = 0;
    w->busy = 0;
    w->creator = FindTask (NULL);
    w->creatorbit = bit;
    w->proc = CreateNewProcTags (NP_Entry, worker_main,
				 NP_Name, i ? "E-UAE bsdsocket worker" : "E-UAE bsdsocket",
				 NP_StackSize, 65536,
				 NP_UserData, w,
				 NP_Child, FALSE,
				 TAG_END);
    if (w->proc)
	while (w->state == 0)
	    Wait (1UL << bit);
    FreeSignal (bit);
    if (!w->proc || w->state != 1) {
	w->proc = NULL;
	w->state = -1;
	return 0;
    }
    return 1;
}

struct bsd_proxy *bsdproxy_start (void)
{
    struct bsd_proxy *px;

    px = AllocVecTags (sizeof *px, AVT_Type, MEMF_SHARED,
		       AVT_ClearWithValue, 0, TAG_END);
    if (!px)
	return NULL;
    px->mutex = AllocSysObjectTags (ASOT_MUTEX, TAG_END);
    NewList (&px->pending);
    px->nworkers = 1;
    if (!px->mutex || !start_worker (px, 0)) {
	write_log ("bsdsocket: could not open a SocketBase (no TCP/IP stack running?)\n");
	if (px->mutex)
	    FreeSysObject (ASOT_MUTEX, px->mutex);
	FreeVec (px);
	return NULL;
    }
    if (BSD_PARALLEL && !px->shared)
	write_log ("bsdsocket: TCP/IP stack does not share SocketBases - one worker per base\n");
    return px;
}

/* index of an idle worker, now marked busy, or -1. May start a new one. */
static int claim_worker (struct bsd_proxy *px)
{
    int i, n, ok;

    MutexObtain (px->mutex);
    for (i = 0; i < px->nworkers; i++)
	if (px->w[i].state == 1 && !px->w[i].busy) {
	    px->w[i].busy = 1;
	    MutexRelease (px->mutex);
	    return i;
	}
    n = px->nworkers;
    if (!px->shared || n >= BSD_MAXW || px->creating || px->quitting) {
	MutexRelease (px->mutex);
	return -1;
    }
    px->creating = 1;
    px->nworkers = n + 1;            /* state 0: not used until it runs */
    MutexRelease (px->mutex);

    ok = start_worker (px, n);

    MutexObtain (px->mutex);
    px->creating = 0;
    if (ok)
	px->w[n].busy = 1;
    else
	px->nworkers = n;
    MutexRelease (px->mutex);
    return ok ? n : -1;
}

static void msg_prepare (struct bsd_proxy *px, struct bsd_msg *m)
{
    m->px = px;
    m->done = 0;
    m->runner = NULL;
    m->msg.mn_ReplyPort = NULL;
    m->msg.mn_Length = sizeof *m;
    __sync_synchronize ();
}

/* to an idle worker, else into the pending list */
static void dispatch (struct bsd_proxy *px, struct bsd_msg *m)
{
    int i = claim_worker (px);

    if (i < 0) {
	MutexObtain (px->mutex);
	for (i = 0; i < px->nworkers; i++)
	    if (px->w[i].state == 1 && !px->w[i].busy)
		break;
	if (i < px->nworkers) {
	    px->w[i].busy = 1;
	} else {
	    AddTail (&px->pending, (struct Node *) &m->msg);
	    MutexRelease (px->mutex);
	    return;
	}
	MutexRelease (px->mutex);
    }
    PutMsg (px->w[i].port, &m->msg);
}

static void host_wait (struct bsd_proxy *px, struct bsd_msg *m, int i)
{
    BYTE bit = AllocSignal (-1);

    msg_prepare (px, m);
    m->guest_task = 0;
    if (bit < 0) {
	/* no free signal (should not happen): poll */
	m->waiter = NULL;
	if (i >= 0)
	    PutMsg (px->w[i].port, &m->msg);
	else
	    dispatch (px, m);
	while (!m->done)
	    Delay (1);
	return;
    }
    m->waiter = FindTask (NULL);
    m->waitbit = bit;
    SetSignal (0, 1UL << bit);
    if (i >= 0)
	PutMsg (px->w[i].port, &m->msg);
    else
	dispatch (px, m);
    while (!m->done)
	Wait (1UL << bit);
    FreeSignal (bit);
}

int bsdproxy_call_idle (struct bsd_proxy *px, struct bsd_msg *m)
{
    int i = claim_worker (px);

    if (i < 0)
	return 0;
    host_wait (px, m, i);
    return 1;
}

void bsdproxy_call (struct bsd_proxy *px, struct bsd_msg *m)
{
    host_wait (px, m, -1);
}

void bsdproxy_post (struct bsd_proxy *px, struct bsd_msg *m)
{
    msg_prepare (px, m);
    m->waiter = NULL;
    dispatch (px, m);
}

void bsdproxy_abort (struct bsd_msg *m)
{
    Forbid ();
    if (m->runner && !m->done)
	Signal (m->runner, BSD_ABORTSIG);
    Permit ();
}

void bsdproxy_quit (struct bsd_proxy *px, int resetting)
{
    int i, t, ok = 1;

    if (!px)
	return;
    if (resetting)
	px->resetting = 1;
    px->quitting = 1;
    __sync_synchronize ();

    /* the extra workers first, worker 0 (owner of the base) last */
    for (i = px->nworkers - 1; i >= 0; i--) {
	struct bsd_worker *w = &px->w[i];
	struct bsd_msg *q = &w->quitmsg;

	if (w->state != 1)
	    continue;
	if (i == 0 && !ok)
	    break;          /* a worker is still in Roadshow: keep the base */
	q->fn = NULL;
	q->done = 0;
	q->msg.mn_ReplyPort = NULL;
	q->msg.mn_Length = sizeof *q;
	__sync_synchronize ();
	PutMsg (w->port, &q->msg);
	/* a call may still block in Roadshow (other task of the program,
	 * or a reset): break it */
	Forbid ();
	if (w->state == 1)
	    Signal ((struct Task *) w->proc, BSD_ABORTSIG);
	Permit ();
	/* up to 5 s; Roadshow calls that ignore CTRL-C (DNS) end later */
	for (t = 0; t < 250 && !q->done; t++) {
	    Delay (1);
	    if ((t % 25) == 24) {
		Forbid ();
		if (!q->done)
		    Signal ((struct Task *) w->proc, BSD_ABORTSIG);
		Permit ();
	    }
	}
	if (!q->done)
	    ok = 0;
    }
    if (ok) {
	FreeSysObject (ASOT_MUTEX, px->mutex);
	FreeVec (px);
    } else {
	/* left running; px stays allocated for it (small leak) */
	px->orphaned = 1;
	write_log ("bsdsocket: a worker did not end in time, left to finish on its own\n");
    }
}
