 /*
  * UAE - The Un*x Amiga Emulator
  *
  * bsdsocket.library emulation - AmigaOS 4 version
  *
  * Copyright 1997, 1998 Mathias Ortmann
  *
  * Library initialization code (c) Tauno Taipaleenmaki
  *
  * AmigaOS 4 port by Kjetil Hvalstrand. v17: rewritten call layer
  * (Falke_34 et al.):
  *  - every Amiga SocketBase has its own Roadshow SocketBase, shared by a
  *    small pool of host worker processes (od-amiga/bsdsock.c), so calls
  *    of several tasks of a program run at the same time; Amiga socket
  *    numbers are the Roadshow numbers of that base
  *  - the emulation never waits behind a call that waits for the network:
  *    the calling Amiga task waits on a signal of its own (as in WinUAE);
  *    break signals (SetSocketSignals / SBTC_BREAKMASK, default CTRL-C)
  *    and the signal mask of WaitSelect() work
  *  - results with pointers (hostent, servent, protoent, Inet_NtoA) are
  *    copied into Amiga memory; no host pointers reach the Amiga
  *  - CloseLibrary, reset and Stop end the proxy cleanly
  */

#include "sysconfig.h"
#include "sysdeps.h"

#include <assert.h>
#include <stddef.h>

#include "options.h"
#include "memory.h"
#include "custom.h"
#include "newcpu.h"
#include "autoconf.h"

#include "traps.h"

#include "threaddep/thread.h"
#include "native2amiga.h"
#include "bsdsocket_amigaos41.h"

#ifdef BSDSOCKET

#include <proto/exec.h>
#include <proto/dos.h>
#undef __USE_INLINE__
#include <proto/bsdsocket.h>
#include <libraries/bsdsocket.h>
#include <netdb.h>

#include "od-amiga/bsdsock_proxy.h"

struct socketbase *socketbases;

static uae_u32 SockLibBase;
static uae_u32 functable, datatable;
static uae_u32 release_str;          /* "UAE bsdsocket.library 4.1" in Amiga memory */

extern APTR sigqueue_mx;             /* od-amiga/main.c: protects socketbases */

static void close_all (int drain);

/* ---------------------------------------------------------------------- */
/* Amiga memory helpers                                                   */

STATIC_INLINE void memcpyha (uae_u32 dst, const char *src, int size)
{
    while (size--)
	put_byte (dst++, *src++);
}

uae_u32 strcpyha (uae_u32 dst, const char *src)
{
    uae_u32 res = dst;

    do {
	put_byte (dst++, *src);
    } while (*src++);
    return res;
}

uae_u32 strncpyha (uae_u32 dst, const char *src, int size)
{
    uae_u32 res = dst;

    while (size--) {
	put_byte (dst++, *src);
	if (!*src++)
	    return res;
    }
    return res;
}

uae_u32 addstr (uae_u32 *dst, const char *src)
{
    uae_u32 res = *dst;
    int len = strlen (src) + 1;

    strcpyha (*dst, src);
    (*dst) += len;
    return res;
}

uae_u32 addmem (uae_u32 *dst, const char *src, int len)
{
    uae_u32 res = *dst;

    if (!src)
	return 0;
    memcpyha (*dst, src, len);
    (*dst) += len;
    return res;
}

/* host address of an Amiga buffer for Roadshow, NULL if 0 or invalid */
static void *hptr (uae_u32 a, uae_u32 size)
{
    if (!a || !valid_address (a, size ? size : 1))
	return NULL;
    return get_real_address (a);
}

/* ---------------------------------------------------------------------- */
/* exec.library calls of the calling Amiga task (trap thread only)        */

static uae_u32 guest_FindTask (TrapContext *context)
{
    uae_u32 a1 = m68k_areg (&context->regs, 1), t;

    m68k_areg (&context->regs, 1) = 0;
    t = CallLib (context, get_long (4), -0x126);        /* FindTask */
    m68k_areg (&context->regs, 1) = a1;
    return t;
}

static uae_u32 guest_SetSignal (TrapContext *context, uae_u32 newsig, uae_u32 mask)
{
    m68k_dreg (&context->regs, 0) = newsig;
    m68k_dreg (&context->regs, 1) = mask;
    return CallLib (context, get_long (4), -0x132);     /* SetSignal */
}

static uae_u32 guest_Wait (TrapContext *context, uae_u32 mask)
{
    m68k_dreg (&context->regs, 0) = mask;
    return CallLib (context, get_long (4), -0x13e);     /* Wait */
}

static uae_u32 guest_AllocMem (TrapContext *context, uae_u32 size, uae_u32 type)
{
    m68k_dreg (&context->regs, 0) = size;
    m68k_dreg (&context->regs, 1) = type;
    return CallLib (context, get_long (4), -0xC6);      /* AllocMem */
}

static void guest_FreeMem (TrapContext *context, uae_u32 ptr, uae_u32 size)
{
    m68k_areg (&context->regs, 1) = ptr;
    m68k_dreg (&context->regs, 0) = size;
    CallLib (context, get_long (4), -0xD2);             /* FreeMem */
}

/* ---------------------------------------------------------------------- */
/* Calls                                                                  */

STATIC_INLINE struct socketbase *get_socketbase (TrapContext *context)
{
    return get_pointer (m68k_areg (&context->regs, 6) + offsetof (struct UAEBSDBase, sb));
}

#define D(n) (m->r[(n)])
#define A(n) (m->r[(n) + 8])
#define IS   (bsdproxy_iface (m))

static void msg_init (struct bsd_msg *m, TrapContext *context, bsd_fn fn)
{
    int i;

    memset (m, 0, sizeof *m);
    m->fn = fn;
    for (i = 0; i < 16; i++)
	m->r[i] = context->regs.regs[i];
}

static uae_u32 call_blocking (TrapContext *context, struct socketbase *sb,
			      struct bsd_msg *m, uae_u32 extra);

/* put signals back that Wait() took but nobody reports */
static void restore_signals (TrapContext *context, uae_u32 sigs)
{
    if (sigs)
	guest_SetSignal (context, sigs, sigs);
}

/*
 * Short call (never waits in Roadshow): if a worker is idle, the
 * emulation waits for it (microseconds). If all workers are busy (e.g.
 * waiting in recv() for other tasks of the program), it is run like a
 * blocking call - the emulation must never wait behind a blocked call.
 */
static void call_quick (TrapContext *context, struct socketbase *sb, struct bsd_msg *m)
{
    if (!sb || !sb->px) {
	m->result = -1;
	return;
    }
    if (bsdproxy_call_idle (sb->px, m))
	return;
    restore_signals (context, call_blocking (context, sb, m, 0));
}

/*
 * Call that can block: the calling Amiga task Wait()s on a signal of its
 * own (allocated for this call - a SocketBase may be used by several
 * tasks of a program), the emulation keeps running. A break signal
 * (sb->eintrsigs) or one of 'extra' (WaitSelect) aborts the Roadshow call
 * with CTRL-C to the worker running it. Returns the signals other than
 * the call signal that were received (taken by Wait(); the caller puts
 * back what it does not report).
 */
static uae_u32 call_blocking (TrapContext *context, struct socketbase *sb,
			      struct bsd_msg *m, uae_u32 extra)
{
    uae_u32 task, sigbit, waitmask, sigs, got = 0;
    int bit, own = 0;

    if (!sb || !sb->px) {
	m->result = -1;
	return 0;
    }
    task = guest_FindTask (context);
    m68k_dreg (&context->regs, 0) = (uae_u32) -1;
    bit = (uae_s8) CallLib (context, get_long (4), -0x14A);    /* AllocSignal */
    if (bit >= 0)
	own = 1;
    else if (task == sb->ownertask && sb->signal >= 0)
	bit = sb->signal;                    /* all signals in use */
    if (bit < 0) {
	bsdproxy_call (sb->px, m);           /* last resort */
	return 0;
    }
    sigbit = 1u << bit;
    guest_SetSignal (context, 0, sigbit);
    m->guest_task = task;
    m->guest_mask = sigbit;
    bsdproxy_post (sb->px, m);

    waitmask = sigbit | sb->eintrsigs | extra;
    for (;;) {
	sigs = guest_Wait (context, waitmask);
	got |= sigs & ~sigbit;
	/* the worker sends the signal after setting done: waiting for the
	 * signal too means none can arrive after the bit is freed */
	if ((sigs & sigbit) && m->done)
	    break;
	if ((sigs & (sb->eintrsigs | extra)) && !m->abort) {
	    m->abort = 1;
	    __sync_synchronize ();
	    bsdproxy_abort (m);
	    waitmask = sigbit;
	}
    }
    if (own) {
	m68k_dreg (&context->regs, 0) = bit;
	CallLib (context, get_long (4), -0x150);                   /* FreeSignal */
    }
    return got;
}

/* blocking call without signal mask: break signals stay set (WinUAE) */
static uae_u32 blocking (TrapContext *context, bsd_fn fn)
{
    struct socketbase *sb = get_socketbase (context);
    struct bsd_msg m;
    uae_u32 got;

    msg_init (&m, context, fn);
    got = call_blocking (context, sb, &m, 0);
    restore_signals (context, got);
    return m.result;
}

static uae_u32 quick (TrapContext *context, bsd_fn fn)
{
    struct bsd_msg m;

    msg_init (&m, context, fn);
    call_quick (context, get_socketbase (context), &m);
    return m.result;
}

/* --- proxy side: these run in the proxy process of the SocketBase ----- */

/*
 * v17-2: the proxy is a plain AmigaOS process (not a newlib child of
 * E-UAE). It must not call newlib: newlib keeps per-task data and a task
 * that used it and then ended can leave a stale entry behind, which made
 * later newlib calls of other E-UAE threads crash (GrimReaper: "UAE
 * thread" in newlib.library.kmod, list walk into 0xABADCAFE). So: exec
 * memory (AllocVec/FreeVec, also freed by the trap thread) and own
 * string helpers.
 */
static size_t px_strlen (const char *s)
{
    size_t n = 0;

    if (s)
	while (s[n])
	    n++;
    return n;
}

static void *px_alloc (size_t size)
{
    return AllocVecTags (size, AVT_Type, MEMF_SHARED, TAG_END);
}

static void fn_socket (struct bsd_msg *m)
{
    m->result = IS->socket (D(0), D(1), D(2));
}

static void fn_bind (struct bsd_msg *m)
{
    m->result = IS->bind (D(0), hptr (A(0), D(1)), D(1));
}

static void fn_listen (struct bsd_msg *m)
{
    m->result = IS->listen (D(0), D(1));
}

static void fn_accept (struct bsd_msg *m)
{
    m->result = IS->accept (D(0), hptr (A(0), 16), hptr (A(1), 4));
}

static void fn_connect (struct bsd_msg *m)
{
    m->result = IS->connect (D(0), hptr (A(0), D(1)), D(1));
}

static void fn_sendto (struct bsd_msg *m)
{
    m->result = IS->sendto (D(0), hptr (A(0), D(1)), D(1), D(2), hptr (A(1), D(3)), D(3));
}

static void fn_send (struct bsd_msg *m)
{
    m->result = IS->send (D(0), hptr (A(0), D(1)), D(1), D(2));
}

static void fn_recvfrom (struct bsd_msg *m)
{
    m->result = IS->recvfrom (D(0), hptr (A(0), D(1)), D(1), D(2), hptr (A(1), 16), hptr (A(2), 4));
}

static void fn_recv (struct bsd_msg *m)
{
    m->result = IS->recv (D(0), hptr (A(0), D(1)), D(1), D(2));
}

static void fn_shutdown (struct bsd_msg *m)
{
    m->result = IS->shutdown (D(0), D(1));
}

static void fn_setsockopt (struct bsd_msg *m)
{
    m->result = IS->setsockopt (D(0), D(1), D(2), hptr (A(0), D(3)), D(3));
}

static void fn_getsockopt (struct bsd_msg *m)
{
    m->result = IS->getsockopt (D(0), D(1), D(2), hptr (A(0), 4), hptr (A(1), 4));
}

static void fn_getsockname (struct bsd_msg *m)
{
    m->result = IS->getsockname (D(0), hptr (A(0), 16), hptr (A(1), 4));
}

static void fn_getpeername (struct bsd_msg *m)
{
    m->result = IS->getpeername (D(0), hptr (A(0), 16), hptr (A(1), 4));
}

static void fn_IoctlSocket (struct bsd_msg *m)
{
    m->result = IS->IoctlSocket (D(0), D(1), hptr (A(0), 4));
}

static void fn_CloseSocket (struct bsd_msg *m)
{
    m->result = IS->CloseSocket (D(0));
}

static void fn_WaitSelect (struct bsd_msg *m)
{
    LONG nfds = D(0);
    uae_u32 setsize;

    if (nfds < 0)
	nfds = 0;
    if (nfds > 4096)
	nfds = 4096;
    setsize = ((nfds + 31) / 32) * 4;
    m->result = IS->WaitSelect (nfds, hptr (A(0), setsize), hptr (A(1), setsize),
				hptr (A(2), setsize), hptr (A(3), 8), NULL);
}

static void fn_getdtablesize (struct bsd_msg *m)
{
    m->result = IS->getdtablesize ();
}

static void fn_ObtainSocket (struct bsd_msg *m)
{
    m->result = IS->ObtainSocket (D(0), D(1), D(2), D(3));
}

static void fn_ReleaseSocket (struct bsd_msg *m)
{
    m->result = IS->ReleaseSocket (D(0), D(1));
}

static void fn_ReleaseCopyOfSocket (struct bsd_msg *m)
{
    m->result = IS->ReleaseCopyOfSocket (D(0), D(1));
}

static void fn_Errno (struct bsd_msg *m)
{
    m->result = IS->Errno ();
}

static void fn_SetErrnoPtr (struct bsd_msg *m)
{
    LONG size = D(0);
    /* Roadshow writes errno straight into Amiga memory */
    if (size == 1 || size == 2 || size == 4)
	IS->SetErrnoPtr (hptr (A(0), size), size);
    m->result = 0;
}

static void fn_Inet_NtoA (struct bsd_msg *m)
{
    STRPTR s = IS->Inet_NtoA (D(0));

    m->buf[0] = 0;
    if (s) {
	int i;
	for (i = 0; i < BSD_MSGBUF - 1 && s[i]; i++)
	    m->buf[i] = s[i];
	m->buf[i] = 0;
    }
    m->result = s != NULL;
}

static void fn_inet_addr (struct bsd_msg *m)
{
    m->result = IS->inet_addr (hptr (A(0), 1));
}

static void fn_Inet_LnaOf (struct bsd_msg *m)
{
    m->result = IS->Inet_LnaOf (D(0));
}

static void fn_Inet_NetOf (struct bsd_msg *m)
{
    m->result = IS->Inet_NetOf (D(0));
}

static void fn_Inet_MakeAddr (struct bsd_msg *m)
{
    m->result = IS->Inet_MakeAddr (D(0), D(1));
}

static void fn_inet_network (struct bsd_msg *m)
{
    m->result = IS->inet_network (hptr (A(0), 1));
}

/* deep copies of the Roadshow results (valid only until the next call) */
static void count_strlist (char **list, int *count, size_t *size)
{
    int n = 0;

    if (list)
	while (list[n])
	    *size += px_strlen (list[n++]) + 1;
    *count = n;
    *size += (n + 1) * sizeof (char *);
}

static char *put_str (char **p, const char *s)
{
    char *r = *p, *d = r;

    if (s)
	while (*s)
	    *d++ = *s++;
    *d++ = 0;
    *p = d;
    return r;
}

static struct hostent *dup_hostent (const struct hostent *h)
{
    struct hostent *d;
    size_t size;
    int na = 0, nd = 0, len, i;
    char **al, **dl, *p;

    if (!h)
	return NULL;
    len = h->h_length > 0 ? h->h_length : 0;
    size = sizeof (struct hostent) + px_strlen ((const char *) h->h_name) + 1 + 8;
    count_strlist ((char **) h->h_aliases, &na, &size);
    if (h->h_addr_list)
	while (h->h_addr_list[nd])
	    nd++;
    size += (nd + 1) * sizeof (char *) + nd * len;
    d = px_alloc (size);
    if (!d)
	return NULL;
    al = (char **) (d + 1);
    dl = al + na + 1;
    p = (char *) (dl + nd + 1);
    d->h_addrtype = h->h_addrtype;
    d->h_length = len;
    d->h_aliases = (void *) al;
    d->h_addr_list = (void *) dl;
    for (i = 0; i < nd; i++) {
	CopyMem (h->h_addr_list[i], p, len);
	dl[i] = p;
	p += len;
    }
    dl[nd] = NULL;
    for (i = 0; i < na; i++)
	al[i] = put_str (&p, h->h_aliases[i]);
    al[na] = NULL;
    d->h_name = put_str (&p, h->h_name);
    return d;
}

static struct servent *dup_servent (const struct servent *s)
{
    struct servent *d;
    size_t size;
    int na = 0, i;
    char **al, *p;

    if (!s)
	return NULL;
    size = sizeof (struct servent) + 2 + 8
	+ px_strlen ((const char *) s->s_name) + px_strlen ((const char *) s->s_proto);
    count_strlist ((char **) s->s_aliases, &na, &size);
    d = px_alloc (size);
    if (!d)
	return NULL;
    al = (char **) (d + 1);
    p = (char *) (al + na + 1);
    d->s_port = s->s_port;
    d->s_aliases = (void *) al;
    for (i = 0; i < na; i++)
	al[i] = put_str (&p, s->s_aliases[i]);
    al[na] = NULL;
    d->s_name = put_str (&p, s->s_name);
    d->s_proto = put_str (&p, s->s_proto);
    return d;
}

static struct protoent *dup_protoent (const struct protoent *s)
{
    struct protoent *d;
    size_t size;
    int na = 0, i;
    char **al, *p;

    if (!s)
	return NULL;
    size = sizeof (struct protoent) + 1 + 8 + px_strlen ((const char *) s->p_name);
    count_strlist ((char **) s->p_aliases, &na, &size);
    d = px_alloc (size);
    if (!d)
	return NULL;
    al = (char **) (d + 1);
    p = (char *) (al + na + 1);
    d->p_proto = s->p_proto;
    d->p_aliases = (void *) al;
    for (i = 0; i < na; i++)
	al[i] = put_str (&p, s->p_aliases[i]);
    al[na] = NULL;
    d->p_name = put_str (&p, s->p_name);
    return d;
}

static void fn_gethostbyname (struct bsd_msg *m)
{
    m->hostdata = dup_hostent (IS->gethostbyname (hptr (A(0), 1)));
    m->result = m->hostdata != NULL;
}

static void fn_gethostbyaddr (struct bsd_msg *m)
{
    m->hostdata = dup_hostent (IS->gethostbyaddr (hptr (A(0), D(0)), D(0), D(1)));
    m->result = m->hostdata != NULL;
}

static void fn_getservbyname (struct bsd_msg *m)
{
    m->hostdata = dup_servent (IS->getservbyname (hptr (A(0), 1), hptr (A(1), 1)));
    m->result = m->hostdata != NULL;
}

static void fn_getservbyport (struct bsd_msg *m)
{
    m->hostdata = dup_servent (IS->getservbyport (D(0), hptr (A(0), 1)));
    m->result = m->hostdata != NULL;
}

static void fn_getprotobyname (struct bsd_msg *m)
{
    m->hostdata = dup_protoent (IS->getprotobyname (hptr (A(0), 1)));
    m->result = m->hostdata != NULL;
}

static void fn_getprotobynumber (struct bsd_msg *m)
{
    m->hostdata = dup_protoent (IS->getprotobynumber (D(0)));
    m->result = m->hostdata != NULL;
}

static void fn_Dup2Socket (struct bsd_msg *m)
{
    m->result = IS->Dup2Socket (D(0), D(1));
}

static void fn_gethostname (struct bsd_msg *m)
{
    m->result = IS->gethostname (hptr (A(0), D(0)), D(0));
}

static void fn_gethostid (struct bsd_msg *m)
{
    m->result = IS->gethostid ();
}

static void fn_SocketBaseTagList (struct bsd_msg *m)
{
    m->result = IS->SocketBaseTagList ((struct TagItem *) m->hostdata);
}

/* --- copies into Amiga memory (trap thread) ---------------------------- */

/* struct hostent: h_name, h_aliases, h_addrtype, h_length, h_addr_list */
static uae_u32 copyHostentToGuest (TrapContext *context, const struct hostent *h, SB)
{
    int size = 28, i, numaddr = 0, numaliases = 0;
    uae_u32 aptr;

    if (h->h_name)
	size += strlen (h->h_name) + 1;
    if (h->h_aliases)
	while (h->h_aliases[numaliases])
	    size += strlen (h->h_aliases[numaliases++]) + 5;
    if (h->h_addr_list) {
	while (h->h_addr_list[numaddr])
	    numaddr++;
	size += numaddr * (h->h_length + 4);
    }
    if (sb->hostent)
	guest_FreeMem (context, sb->hostent, sb->hostentsize);
    sb->hostent = guest_AllocMem (context, size, 0);
    sb->hostentsize = sb->hostent ? size : 0;
    if (!sb->hostent)
	return 0;

    aptr = sb->hostent + 28 + numaliases * 4 + numaddr * 4;
    put_long (sb->hostent + 4, sb->hostent + 20);
    put_long (sb->hostent + 8, h->h_addrtype);
    put_long (sb->hostent + 12, h->h_length);
    put_long (sb->hostent + 16, sb->hostent + 24 + numaliases * 4);
    for (i = 0; i < numaliases; i++)
	put_long (sb->hostent + 20 + i * 4, addstr (&aptr, h->h_aliases[i]));
    put_long (sb->hostent + 20 + numaliases * 4, 0);
    for (i = 0; i < numaddr; i++)
	put_long (sb->hostent + 24 + (numaliases + i) * 4,
		  addmem (&aptr, h->h_addr_list[i], h->h_length));
    put_long (sb->hostent + 24 + numaliases * 4 + numaddr * 4, 0);
    put_long (sb->hostent, aptr);
    addstr (&aptr, h->h_name ? (const char *) h->h_name : "");
    return sb->hostent;
}

/* struct servent: s_name, s_aliases, s_port, s_proto */
static uae_u32 copyServentToGuest (TrapContext *context, const struct servent *s, SB)
{
    int size = 16 + 4, i, numaliases = 0;
    uae_u32 aptr;

    size += strlen (s->s_name) + 1 + strlen (s->s_proto) + 1;
    while (s->s_aliases[numaliases])
	size += strlen (s->s_aliases[numaliases++]) + 5;
    if (sb->servent)
	guest_FreeMem (context, sb->servent, sb->serventsize);
    sb->servent = guest_AllocMem (context, size, 0);
    sb->serventsize = sb->servent ? size : 0;
    if (!sb->servent)
	return 0;

    aptr = sb->servent + 16 + numaliases * 4 + 4;
    put_long (sb->servent + 4, sb->servent + 16);
    put_long (sb->servent + 8, s->s_port);
    for (i = 0; i < numaliases; i++)
	put_long (sb->servent + 16 + i * 4, addstr (&aptr, s->s_aliases[i]));
    put_long (sb->servent + 16 + numaliases * 4, 0);
    put_long (sb->servent, addstr (&aptr, s->s_name));
    put_long (sb->servent + 12, addstr (&aptr, s->s_proto));
    return sb->servent;
}

/* struct protoent: p_name, p_aliases, p_proto */
static uae_u32 copyProtoentToGuest (TrapContext *context, const struct protoent *p, SB)
{
    int size = 12 + 4, i, numaliases = 0;
    uae_u32 aptr;

    size += strlen (p->p_name) + 1;
    while (p->p_aliases[numaliases])
	size += strlen (p->p_aliases[numaliases++]) + 5;
    if (sb->protoent)
	guest_FreeMem (context, sb->protoent, sb->protoentsize);
    sb->protoent = guest_AllocMem (context, size, 0);
    sb->protoentsize = sb->protoent ? size : 0;
    if (!sb->protoent)
	return 0;

    aptr = sb->protoent + 12 + numaliases * 4 + 4;
    put_long (sb->protoent + 4, sb->protoent + 12);
    put_long (sb->protoent + 8, p->p_proto);
    for (i = 0; i < numaliases; i++)
	put_long (sb->protoent + 12 + i * 4, addstr (&aptr, p->p_aliases[i]));
    put_long (sb->protoent + 12 + numaliases * 4, 0);
    put_long (sb->protoent, addstr (&aptr, p->p_name));
    return sb->protoent;
}

/* ---------------------------------------------------------------------- */
/* SocketBase                                                             */

static void sb_unlink (struct socketbase *sb)
{
    struct socketbase **pp;

    MutexObtain (sigqueue_mx);
    for (pp = &socketbases; *pp; pp = &(*pp)->next)
	if (*pp == sb) {
	    *pp = sb->next;
	    break;
	}
    MutexRelease (sigqueue_mx);
}

static void guest_FreeSignal (TrapContext *context, int sig)
{
    m68k_dreg (&context->regs, 0) = sig;
    CallLib (context, get_long (4), -0x150);                /* FreeSignal */
}

static struct socketbase *alloc_socketbase (TrapContext *context)
{
    struct socketbase *sb = calloc (sizeof (struct socketbase), 1);

    if (!sb)
	return NULL;
    sb->ownertask = guest_FindTask (context);
    m68k_dreg (&context->regs, 0) = (uae_u32) -1;
    sb->signal = (uae_s8) CallLib (context, get_long (4), -0x14A);  /* AllocSignal */
    if (sb->signal < 0) {
	write_log ("bsdsocket: no free signal for task 0x%x\n", sb->ownertask);
	free (sb);
	return NULL;
    }
    sb->dtablesize = DEFAULT_DTABLE_SIZE;
    sb->eintrsigs = SIGBREAKF_CTRL_C;         /* default break mask */
    sb->px = bsdproxy_start ();
    if (!sb->px) {
	guest_FreeSignal (context, sb->signal);
	free (sb);
	return NULL;
    }
    MutexObtain (sigqueue_mx);
    sb->next = socketbases;
    socketbases = sb;
    MutexRelease (sigqueue_mx);
    return sb;
}

static uae_u32 REGPARAM2 bsdsocklib_Expunge (TrapContext *context)
{
    return 0;
}

static uae_u32 REGPARAM2 bsdsocklib_Open (TrapContext *context)
{
    struct socketbase *sb;
    uae_u32 result;

    if (!(sb = alloc_socketbase (context)))
	return 0;

    m68k_areg (&context->regs, 0) = functable;
    m68k_areg (&context->regs, 1) = datatable;
    m68k_areg (&context->regs, 2) = 0;
    m68k_dreg (&context->regs, 0) = sizeof (struct UAEBSDBase);
    m68k_dreg (&context->regs, 1) = 0;
    result = CallLib (context, get_long (4), -0x54);              /* MakeLibrary */
    if (!result) {
	sb_unlink (sb);
	bsdproxy_quit (sb->px, 0);
	guest_FreeSignal (context, sb->signal);
	free (sb);
	return 0;
    }
    put_pointer (result + offsetof (struct UAEBSDBase, sb), sb);
    put_word (SockLibBase + 32, get_word (SockLibBase + 32) + 1);
    return result;
}

static uae_u32 REGPARAM2 bsdsocklib_Close (TrapContext *context)
{
    uae_u32 base = m68k_areg (&context->regs, 6);
    uae_u32 negsize = get_word (base + 16);
    uae_u32 possize = get_word (base + 18);
    struct socketbase *sb = get_socketbase (context);

    if (sb) {
	sb_unlink (sb);
	/* ends the proxy; closing its Roadshow base closes all sockets */
	bsdproxy_quit (sb->px, 0);
	sb->px = NULL;
	if (sb->hostent)
	    guest_FreeMem (context, sb->hostent, sb->hostentsize);
	if (sb->protoent)
	    guest_FreeMem (context, sb->protoent, sb->protoentsize);
	if (sb->servent)
	    guest_FreeMem (context, sb->servent, sb->serventsize);
	guest_FreeSignal (context, sb->signal);
	free (sb);
    }
    if (SockLibBase && get_word (SockLibBase + 32) > 0)
	put_word (SockLibBase + 32, get_word (SockLibBase + 32) - 1);

    m68k_areg (&context->regs, 1) = base - negsize;
    m68k_dreg (&context->regs, 0) = negsize + possize;
    CallLib (context, get_long (4), -0xD2);                       /* FreeMem */
    return 0;
}

/* ---------------------------------------------------------------------- */
/* Library functions                                                      */

static uae_u32 REGPARAM2 bsdsocklib_socket (TrapContext *context)      { return quick (context, fn_socket); }
static uae_u32 REGPARAM2 bsdsocklib_bind (TrapContext *context)        { return quick (context, fn_bind); }
static uae_u32 REGPARAM2 bsdsocklib_listen (TrapContext *context)      { return quick (context, fn_listen); }
static uae_u32 REGPARAM2 bsdsocklib_accept (TrapContext *context)      { return blocking (context, fn_accept); }
static uae_u32 REGPARAM2 bsdsocklib_connect (TrapContext *context)     { return blocking (context, fn_connect); }
static uae_u32 REGPARAM2 bsdsocklib_sendto (TrapContext *context)      { return blocking (context, fn_sendto); }
static uae_u32 REGPARAM2 bsdsocklib_send (TrapContext *context)        { return blocking (context, fn_send); }
static uae_u32 REGPARAM2 bsdsocklib_recvfrom (TrapContext *context)    { return blocking (context, fn_recvfrom); }
static uae_u32 REGPARAM2 bsdsocklib_recv (TrapContext *context)        { return blocking (context, fn_recv); }
static uae_u32 REGPARAM2 bsdsocklib_shutdown (TrapContext *context)    { return quick (context, fn_shutdown); }
static uae_u32 REGPARAM2 bsdsocklib_setsockopt (TrapContext *context)  { return quick (context, fn_setsockopt); }
static uae_u32 REGPARAM2 bsdsocklib_getsockopt (TrapContext *context)  { return quick (context, fn_getsockopt); }
static uae_u32 REGPARAM2 bsdsocklib_getsockname (TrapContext *context) { return quick (context, fn_getsockname); }
static uae_u32 REGPARAM2 bsdsocklib_getpeername (TrapContext *context) { return quick (context, fn_getpeername); }
static uae_u32 REGPARAM2 bsdsocklib_IoctlSocket (TrapContext *context) { return quick (context, fn_IoctlSocket); }
/* CloseSocket can wait (SO_LINGER) */
static uae_u32 REGPARAM2 bsdsocklib_CloseSocket (TrapContext *context) { return blocking (context, fn_CloseSocket); }

/* WaitSelect(nfds, readfds, writefds, exceptfds, timeout, maskp)(d0/a0/a1/a2/a3/d1) */
static uae_u32 REGPARAM2 bsdsocklib_WaitSelect (TrapContext *context)
{
    struct socketbase *sb = get_socketbase (context);
    struct bsd_msg m;
    uae_u32 maskp = m68k_dreg (&context->regs, 1);
    uae_u32 extra = 0, got, rep, i;
    LONG nfds = m68k_dreg (&context->regs, 0);

    if (maskp && valid_address (maskp, 4))
	extra = get_long (maskp);
    else
	maskp = 0;
    msg_init (&m, context, fn_WaitSelect);
    got = call_blocking (context, sb, &m, extra);

    rep = got & extra;
    if (sb && m.abort && m.result < 0 && rep && !(got & sb->eintrsigs & ~extra)) {
	/* woken by a signal of the mask: no socket ready, no error */
	uae_u32 sets[3];
	uae_u32 setsize = nfds > 0 ? ((nfds + 31) / 32) * 4 : 0;
	int s;

	sets[0] = m.r[8];
	sets[1] = m.r[9];
	sets[2] = m.r[10];
	for (s = 0; s < 3; s++)
	    if (sets[s] && setsize && valid_address (sets[s], setsize))
		for (i = 0; i < setsize; i += 4)
		    put_long (sets[s] + i, 0);
	m.result = 0;
    }
    if (maskp)
	put_long (maskp, rep);
    restore_signals (context, got & ~rep);
    return m.result;
}

/* SetSocketSignals(SIGINTR, SIGIO, SIGURG)(d0/d1/d2) - Amiga signals,
 * kept here (Roadshow would signal the proxy) */
static uae_u32 REGPARAM2 bsdsocklib_SetSocketSignals (TrapContext *context)
{
    struct socketbase *sb = get_socketbase (context);

    if (sb) {
	sb->eintrsigs = m68k_dreg (&context->regs, 0);
	sb->sigiomask = m68k_dreg (&context->regs, 1);
	sb->sigurgmask = m68k_dreg (&context->regs, 2);
    }
    return 0;
}

static uae_u32 REGPARAM2 bsdsocklib_getdtablesize (TrapContext *context)  { return quick (context, fn_getdtablesize); }
static uae_u32 REGPARAM2 bsdsocklib_ObtainSocket (TrapContext *context)   { return quick (context, fn_ObtainSocket); }
static uae_u32 REGPARAM2 bsdsocklib_ReleaseSocket (TrapContext *context)  { return quick (context, fn_ReleaseSocket); }
static uae_u32 REGPARAM2 bsdsocklib_ReleaseCopyOfSocket (TrapContext *context) { return quick (context, fn_ReleaseCopyOfSocket); }
static uae_u32 REGPARAM2 bsdsocklib_Errno (TrapContext *context)          { return quick (context, fn_Errno); }

/* SetErrnoPtr(errno_p, size)(a0/d0) */
static uae_u32 REGPARAM2 bsdsocklib_SetErrnoPtr (TrapContext *context)
{
    struct socketbase *sb = get_socketbase (context);

    if (sb) {
	sb->errnoptr = m68k_areg (&context->regs, 0);
	sb->errnosize = m68k_dreg (&context->regs, 0);
    }
    return quick (context, fn_SetErrnoPtr);
}

/* Inet_NtoA(in)(d0): string in the scratch buffer of the library base */
static uae_u32 REGPARAM2 bsdsocklib_Inet_NtoA (TrapContext *context)
{
    uae_u32 buf = m68k_areg (&context->regs, 6) + offsetof (struct UAEBSDBase, scratchbuf);
    struct bsd_msg m;

    msg_init (&m, context, fn_Inet_NtoA);
    call_quick (context, get_socketbase (context), &m);
    if (m.result <= 0)
	return 0;
    strncpyha (buf, m.buf, SCRATCHBUFSIZE);
    return buf;
}

static uae_u32 REGPARAM2 bsdsocklib_inet_addr (TrapContext *context)      { return quick (context, fn_inet_addr); }
static uae_u32 REGPARAM2 bsdsocklib_Inet_LnaOf (TrapContext *context)     { return quick (context, fn_Inet_LnaOf); }
static uae_u32 REGPARAM2 bsdsocklib_Inet_NetOf (TrapContext *context)     { return quick (context, fn_Inet_NetOf); }
static uae_u32 REGPARAM2 bsdsocklib_Inet_MakeAddr (TrapContext *context)  { return quick (context, fn_Inet_MakeAddr); }
static uae_u32 REGPARAM2 bsdsocklib_inet_network (TrapContext *context)   { return quick (context, fn_inet_network); }

/* name/number lookups; host lookups may ask a DNS server, so they block */
enum { ENT_HOST, ENT_SERV, ENT_PROTO };

static uae_u32 lookup (TrapContext *context, bsd_fn fn, int kind, int block)
{
    struct socketbase *sb = get_socketbase (context);
    struct bsd_msg m;
    uae_u32 res = 0;

    msg_init (&m, context, fn);
    if (block)
	restore_signals (context, call_blocking (context, sb, &m, 0));
    else
	call_quick (context, sb, &m);
    if (m.hostdata && sb) {
	if (kind == ENT_HOST)
	    res = copyHostentToGuest (context, m.hostdata, sb);
	else if (kind == ENT_SERV)
	    res = copyServentToGuest (context, m.hostdata, sb);
	else
	    res = copyProtoentToGuest (context, m.hostdata, sb);
    }
    FreeVec (m.hostdata);                 /* exec memory from the proxy */
    return res;
}

static uae_u32 REGPARAM2 bsdsocklib_gethostbyname (TrapContext *context)  { return lookup (context, fn_gethostbyname, ENT_HOST, 1); }
static uae_u32 REGPARAM2 bsdsocklib_gethostbyaddr (TrapContext *context)  { return lookup (context, fn_gethostbyaddr, ENT_HOST, 1); }
static uae_u32 REGPARAM2 bsdsocklib_getservbyname (TrapContext *context)  { return lookup (context, fn_getservbyname, ENT_SERV, 0); }
static uae_u32 REGPARAM2 bsdsocklib_getservbyport (TrapContext *context)  { return lookup (context, fn_getservbyport, ENT_SERV, 0); }
static uae_u32 REGPARAM2 bsdsocklib_getprotobyname (TrapContext *context) { return lookup (context, fn_getprotobyname, ENT_PROTO, 0); }
static uae_u32 REGPARAM2 bsdsocklib_getprotobynumber (TrapContext *context) { return lookup (context, fn_getprotobynumber, ENT_PROTO, 0); }

static uae_u32 REGPARAM2 bsdsocklib_getnetbyname (TrapContext *context)
{
    write_log ("bsdsocket: UNSUPPORTED: getnetbyname()\n");
    return 0;
}

static uae_u32 REGPARAM2 bsdsocklib_getnetbyaddr (TrapContext *context)
{
    write_log ("bsdsocket: UNSUPPORTED: getnetbyaddr()\n");
    return 0;
}

static uae_u32 REGPARAM2 bsdsocklib_vsyslog (TrapContext *context)
{
    return 0;
}

static uae_u32 REGPARAM2 bsdsocklib_Dup2Socket (TrapContext *context)     { return quick (context, fn_Dup2Socket); }

/* struct msghdr contains Amiga pointers (iovec) - not converted */
static uae_u32 REGPARAM2 bsdsocklib_sendmsg (TrapContext *context)
{
    write_log ("bsdsocket: UNSUPPORTED: sendmsg()\n");
    return (uae_u32) -1;
}

static uae_u32 REGPARAM2 bsdsocklib_recvmsg (TrapContext *context)
{
    write_log ("bsdsocket: UNSUPPORTED: recvmsg()\n");
    return (uae_u32) -1;
}

static uae_u32 REGPARAM2 bsdsocklib_gethostname (TrapContext *context)    { return quick (context, fn_gethostname); }
static uae_u32 REGPARAM2 bsdsocklib_gethostid (TrapContext *context)      { return quick (context, fn_gethostid); }

/* ---------------------------------------------------------------------- */
/* SocketBaseTagList                                                      */

static const char * const errortexts[] =
{"No error", "Operation not permitted", "No such file or directory",
 "No such process", "Interrupted system call", "Input/output error", "Device not configured",
 "Argument list too long", "Exec format error", "Bad file descriptor", "No child processes",
 "Resource deadlock avoided", "Cannot allocate memory", "Permission denied", "Bad address",
 "Block device required", "Device busy", "Object exists", "Cross-device link",
 "Operation not supported by device", "Not a directory", "Is a directory", "Invalid argument",
 "Too many open files in system", "Too many open files", "Inappropriate ioctl for device",
 "Text file busy", "File too large", "No space left on device", "Illegal seek",
 "Read-only file system", "Too many links", "Broken pipe", "Numerical argument out of domain",
 "Result too large", "Resource temporarily unavailable", "Operation now in progress",
 "Operation already in progress", "Socket operation on non-socket", "Destination address required",
 "Message too long", "Protocol wrong type for socket", "Protocol not available",
 "Protocol not supported", "Socket type not supported", "Operation not supported",
 "Protocol family not supported", "Address family not supported by protocol family",
 "Address already in use", "Can't assign requested address", "Network is down",
 "Network is unreachable", "Network dropped connection on reset", "Software caused connection abort",
 "Connection reset by peer", "No buffer space available", "Socket is already connected",
 "Socket is not connected", "Can't send after socket shutdown", "Too many references: can't splice",
 "Connection timed out", "Connection refused", "Too many levels of symbolic links",
 "File name too long", "Host is down", "No route to host", "Directory not empty",
 "Too many processes", "Too many users", "Disc quota exceeded", "Stale NFS file handle",
 "Too many levels of remote in path", "RPC struct is bad", "RPC version wrong",
 "RPC prog. not avail", "Program version wrong", "Bad procedure for program", "No locks available",
 "Function not implemented", "Inappropriate file type or format", "PError 0"};

#define NUM_SYS_ERR ((int) (sizeof (errortexts) / sizeof (*errortexts)))
static uae_u32 errnotextptrs[NUM_SYS_ERR];

static const char * const herrortexts[] =
{"No error", "Unknown host", "Host name lookup failure", "Unknown server error",
 "No address associated with name"};

#define NUM_HOST_ERR ((int) (sizeof (herrortexts) / sizeof (*herrortexts)))
static uae_u32 herrnotextptrs[NUM_HOST_ERR];

static const char * const strErr = "Errlist lookup error";
static uae_u32 strErrptr;

#define MAX_TAGS 64

/* GET: write v either into the tag (VAL) or where it points (REF) */
static void tag_get (uae_u32 tag, uae_u32 t, uae_u32 d, uae_u32 v)
{
    if (t & SBTF_REF) {
	if (valid_address (d, 4))
	    put_long (d, v);
    } else {
	put_long (tag + 4, v);
    }
}

static uae_u32 tag_value (uae_u32 t, uae_u32 d)
{
    if (t & SBTF_REF)
	return valid_address (d, 4) ? get_long (d) : 0;
    return d;
}

static uae_u32 errstr (uae_u32 n, const uae_u32 *table, int count)
{
    return (n < (uae_u32) count && table[n]) ? table[n] : strErrptr;
}

/*
 * The tags are read here (trap thread). What concerns the Amiga side
 * (break/event signals, string pointers, the errno pointer, the log tag)
 * is answered here; the rest goes to Roadshow with REF pointers turned
 * into host addresses, and GET values are copied back into the Amiga tag.
 */
static uae_u32 REGPARAM2 bsdsocklib_SocketBaseTagList (TrapContext *context)
{
    struct socketbase *sb = get_socketbase (context);
    struct TagItem host[MAX_TAGS + 1];
    uae_u32 gtag[MAX_TAGS];
    uae_u32 tag = m68k_areg (&context->regs, 0);
    struct bsd_msg m;
    int n = 0, i, guard = 0;

    if (!sb)
	return 0;

    while (tag && n < MAX_TAGS && guard++ < 1024) {
	uae_u32 t, d, code, v;

	if (!valid_address (tag, 8))
	    break;
	t = get_long (tag);
	d = get_long (tag + 4);
	if (t == TAG_DONE)
	    break;
	if (t == TAG_IGNORE) {
	    tag += 8;
	    continue;
	}
	if (t == TAG_MORE) {
	    tag = d;
	    continue;
	}
	if (t == TAG_SKIP) {
	    tag += 8 * (d + 1);
	    continue;
	}
	if (!(t & TAG_USER)) {
	    tag += 8;
	    continue;
	}
	code = SBTM_CODE (t);
	switch (code) {
	case SBTC_BREAKMASK:
	    if (t & SBTF_SET)
		sb->eintrsigs = tag_value (t, d);
	    else
		tag_get (tag, t, d, sb->eintrsigs);
	    break;
	case SBTC_SIGIOMASK:
	    if (t & SBTF_SET)
		sb->sigiomask = tag_value (t, d);
	    else
		tag_get (tag, t, d, sb->sigiomask);
	    break;
	case SBTC_SIGURGMASK:
	    if (t & SBTF_SET)
		sb->sigurgmask = tag_value (t, d);
	    else
		tag_get (tag, t, d, sb->sigurgmask);
	    break;
	case SBTC_SIGEVENTMASK:
	    if (t & SBTF_SET)
		sb->eventsigs = tag_value (t, d);
	    else
		tag_get (tag, t, d, sb->eventsigs);
	    break;
	case SBTC_ERRNOSTRPTR:
	case SBTC_HERRNOSTRPTR:
	case SBTC_IOERRNOSTRPTR:
	case SBTC_S2ERRNOSTRPTR:
	case SBTC_S2WERRNOSTRPTR:
	    /* in: error number, out: Amiga string pointer */
	    v = tag_value (t, d);
	    if (code == SBTC_ERRNOSTRPTR)
		v = errstr (v, errnotextptrs, NUM_SYS_ERR);
	    else if (code == SBTC_HERRNOSTRPTR)
		v = errstr (v, herrnotextptrs, NUM_HOST_ERR);
	    else
		v = strErrptr;
	    tag_get (tag, t, d, v);
	    break;
	case SBTC_RELEASESTRPTR:
	    if (!(t & SBTF_SET))
		tag_get (tag, t, d, release_str);
	    break;
	case SBTC_LOGTAGPTR:
	    if (t & SBTF_SET)
		sb->logtagptr = tag_value (t, d);
	    else
		tag_get (tag, t, d, sb->logtagptr);
	    break;
	case SBTC_ERRNOBYTEPTR:
	case SBTC_ERRNOWORDPTR:
	case SBTC_ERRNOLONGPTR:
	case SBTC_HERRNOLONGPTR:
	    if (t & SBTF_SET) {
		uae_u32 p = tag_value (t, d);
		int size = code == SBTC_ERRNOBYTEPTR ? 1 : code == SBTC_ERRNOWORDPTR ? 2 : 4;
		if (code != SBTC_HERRNOLONGPTR) {
		    sb->errnoptr = p;
		    sb->errnosize = size;
		} else {
		    sb->herrnoptr = p;
		    sb->herrnosize = 4;
		}
		/* Roadshow writes (h)errno straight into Amiga memory */
		host[n].ti_Tag = SBTM_SETVAL (code);
		host[n].ti_Data = (ULONG) hptr (p, size);
		gtag[n] = 0;
		if (host[n].ti_Data || !p)
		    n++;
	    } else {
		tag_get (tag, t, d, code == SBTC_HERRNOLONGPTR ? sb->herrnoptr : sb->errnoptr);
	    }
	    break;
	case SBTC_FDCALLBACK:
	case SBTC_LOG_HOOK:
	case SBTC_SIG_ADDRESS_CHANGE_MASK:
	    /* Amiga code / Amiga signals - not supported */
	    if (!(t & SBTF_SET))
		tag_get (tag, t, d, 0);
	    break;
	case SBTC_CAN_SHARE_LIBRARY_BASES:
	    /* v17: every call waits on a signal of the calling task and the
	     * workers share the Roadshow base, so yes */
	    if (!(t & SBTF_SET))
		tag_get (tag, t, d, 1);
	    break;
	case SBTC_LOG_FILE_NAME:
	    if (t & SBTF_SET) {
		host[n].ti_Tag = SBTM_SETVAL (code);
		host[n].ti_Data = (ULONG) hptr (tag_value (t, d), 1);
		gtag[n] = 0;
		if (host[n].ti_Data)
		    n++;
	    } else {
		tag_get (tag, t, d, 0);
	    }
	    break;
	default:
	    /* plain values are forwarded; REF: host address of the value */
	    host[n].ti_Tag = t;
	    if (t & SBTF_REF) {
		host[n].ti_Data = (ULONG) hptr (d, 4);
		if (!host[n].ti_Data)
		    break;
		gtag[n] = 0;
	    } else {
		host[n].ti_Data = d;
		gtag[n] = (t & SBTF_SET) ? 0 : tag;   /* GETVAL: copy back */
	    }
	    n++;
	    break;
	}
	tag += 8;
    }
    host[n].ti_Tag = TAG_DONE;
    host[n].ti_Data = 0;
    if (n == 0)
	return 0;

    msg_init (&m, context, fn_SocketBaseTagList);
    m.hostdata = host;
    call_quick (context, sb, &m);
    for (i = 0; i < n; i++)
	if (gtag[i])
	    put_long (gtag[i] + 4, host[i].ti_Data);
    return m.result;
}

/* GetSocketEvents(eventmaskp)(a0): Roadshow would signal the proxy, not
 * the Amiga task - not supported */
static uae_u32 REGPARAM2 bsdsocklib_GetSocketEvents (TrapContext *context)
{
    return (uae_u32) -1;
}

/* ---------------------------------------------------------------------- */
/* Library set-up                                                         */

static uae_u32 REGPARAM2 bsdsocklib_init (TrapContext *context)
{
    uae_u32 tmp1;
    int i;

    write_log ("Creating UAE bsdsocket.library 4.1\n");
    if (SockLibBase)
	close_all (0);

    m68k_areg (&context->regs, 0) = functable;
    m68k_areg (&context->regs, 1) = datatable;
    m68k_areg (&context->regs, 2) = 0;
    m68k_dreg (&context->regs, 0) = LIBRARY_SIZEOF;
    m68k_dreg (&context->regs, 1) = 0;
    tmp1 = CallLib (context, m68k_areg (&context->regs, 6), -0x54);   /* MakeLibrary */
    if (!tmp1) {
	write_log ("bsdsocket: FATAL: Cannot create bsdsocket.library!\n");
	return 0;
    }
    m68k_areg (&context->regs, 1) = tmp1;
    CallLib (context, m68k_areg (&context->regs, 6), -0x18c);         /* AddLibrary */
    SockLibBase = tmp1;

    /* error strings in Amiga memory */
    tmp1 = strlen (strErr) + 1;
    for (i = 0; i < NUM_SYS_ERR; i++)
	tmp1 += strlen (errortexts[i]) + 1;
    for (i = 0; i < NUM_HOST_ERR; i++)
	tmp1 += strlen (herrortexts[i]) + 1;
    tmp1 = guest_AllocMem (context, tmp1, 0);
    if (!tmp1) {
	write_log ("bsdsocket: FATAL: Ran out of memory while creating bsdsocket.library!\n");
	return 0;
    }
    for (i = 0; i < NUM_SYS_ERR; i++)
	errnotextptrs[i] = addstr (&tmp1, errortexts[i]);
    for (i = 0; i < NUM_HOST_ERR; i++)
	herrnotextptrs[i] = addstr (&tmp1, herrortexts[i]);
    strErrptr = addstr (&tmp1, strErr);

    /* v17: the old code put a jmp into the level 6 vector here, writing
     * into the rtarea at every boot - not needed: Amiga tasks are woken
     * through uae_Signal() (EXTER interrupt of the filesys board) */
    m68k_dreg (&context->regs, 0) = 1;
    return 0;
}

/*
 * Reset, Stop and re-init: end all proxies (their Roadshow bases and
 * sockets) and forget the Amiga-side state. No Amiga code may run here.
 */
static void close_all (int drain)
{
    struct socketbase *sb, *nsb;

    if (!sigqueue_mx)
	return;
    MutexObtain (sigqueue_mx);
    sb = socketbases;
    socketbases = NULL;
    MutexRelease (sigqueue_mx);

    if (sb)
	write_log ("bsdsocket: reset, closing all SocketBases\n");
    for (; sb; sb = nsb) {
	nsb = sb->next;
	bsdproxy_quit (sb->px, 1);
	free (sb);
    }
    SockLibBase = 0;
    /* signals queued for tasks of the old session must not arrive
     * (reset_all_systems() calls us before the filesys is set up again) */
    if (drain)
	native2amiga_reset ();
}

void bsdlib_reset (void)
{
    close_all (1);
}

static const TrapHandler sockfuncs[] = {
    bsdsocklib_init, bsdsocklib_Open, bsdsocklib_Close, bsdsocklib_Expunge,
    bsdsocklib_socket, bsdsocklib_bind, bsdsocklib_listen, bsdsocklib_accept,
    bsdsocklib_connect, bsdsocklib_sendto, bsdsocklib_send, bsdsocklib_recvfrom, bsdsocklib_recv,
    bsdsocklib_shutdown, bsdsocklib_setsockopt, bsdsocklib_getsockopt, bsdsocklib_getsockname,
    bsdsocklib_getpeername, bsdsocklib_IoctlSocket, bsdsocklib_CloseSocket, bsdsocklib_WaitSelect,
    bsdsocklib_SetSocketSignals, bsdsocklib_getdtablesize, bsdsocklib_ObtainSocket, bsdsocklib_ReleaseSocket,
    bsdsocklib_ReleaseCopyOfSocket, bsdsocklib_Errno, bsdsocklib_SetErrnoPtr, bsdsocklib_Inet_NtoA,
    bsdsocklib_inet_addr, bsdsocklib_Inet_LnaOf, bsdsocklib_Inet_NetOf, bsdsocklib_Inet_MakeAddr,
    bsdsocklib_inet_network, bsdsocklib_gethostbyname, bsdsocklib_gethostbyaddr, bsdsocklib_getnetbyname,
    bsdsocklib_getnetbyaddr, bsdsocklib_getservbyname, bsdsocklib_getservbyport, bsdsocklib_getprotobyname,
    bsdsocklib_getprotobynumber, bsdsocklib_vsyslog, bsdsocklib_Dup2Socket, bsdsocklib_sendmsg,
    bsdsocklib_recvmsg, bsdsocklib_gethostname, bsdsocklib_gethostid, bsdsocklib_SocketBaseTagList,
    bsdsocklib_GetSocketEvents
};

static const char * const funcnames[] = {
    "bsdsocklib_init", "bsdsocklib_Open", "bsdsocklib_Close", "bsdsocklib_Expunge",
    "bsdsocklib_socket", "bsdsocklib_bind", "bsdsocklib_listen", "bsdsocklib_accept",
    "bsdsocklib_connect", "bsdsocklib_sendto", "bsdsocklib_send", "bsdsocklib_recvfrom", "bsdsocklib_recv",
    "bsdsocklib_shutdown", "bsdsocklib_setsockopt", "bsdsocklib_getsockopt", "bsdsocklib_getsockname",
    "bsdsocklib_getpeername", "bsdsocklib_IoctlSocket", "bsdsocklib_CloseSocket", "bsdsocklib_WaitSelect",
    "bsdsocklib_SetSocketSignals", "bsdsocklib_getdtablesize", "bsdsocklib_ObtainSocket", "bsdsocklib_ReleaseSocket",
    "bsdsocklib_ReleaseCopyOfSocket", "bsdsocklib_Errno", "bsdsocklib_SetErrnoPtr", "bsdsocklib_Inet_NtoA",
    "bsdsocklib_inet_addr", "bsdsocklib_Inet_LnaOf", "bsdsocklib_Inet_NetOf", "bsdsocklib_Inet_MakeAddr",
    "bsdsocklib_inet_network", "bsdsocklib_gethostbyname", "bsdsocklib_gethostbyaddr", "bsdsocklib_getnetbyname",
    "bsdsocklib_getnetbyaddr", "bsdsocklib_getservbyname", "bsdsocklib_getservbyport", "bsdsocklib_getprotobyname",
    "bsdsocklib_getprotobynumber", "bsdsocklib_vsyslog", "bsdsocklib_Dup2Socket", "bsdsocklib_sendmsg",
    "bsdsocklib_recvmsg", "bsdsocklib_gethostname", "bsdsocklib_gethostid", "bsdsocklib_SocketBaseTagList",
    "bsdsocklib_GetSocketEvents"
};

static uae_u32 sockfuncvecs[sizeof (sockfuncs) / sizeof (*sockfuncs)];

void bsdlib_install (void)
{
    uae_u32 resname, resid;
    uae_u32 begin, end;
    uae_u32 func_place, data_place, init_place;
    int i;

    if (currprefs.socket_emu == 0)
	return;

    resname = ds ("bsdsocket.library");
    resid = ds ("UAE bsdsocket.library 4.1");
    release_str = resid;

    begin = here ();
    dw (0x4AFC);                /* RT_MATCHWORD */
    dl (begin);                 /* RT_MATCHTAG */
    dl (0);                     /* RT_ENDSKIP */
    dw (0x8004);                /* RTF_AUTOINIT, RT_VERSION */
    dw (0x0970);                /* NT_LIBRARY, RT_PRI */
    dl (resname);               /* RT_NAME */
    dl (resid);                 /* RT_IDSTRING */
    dl (here () + 4);           /* RT_INIT */
    dl (512);
    func_place = here ();
    dl (0);
    data_place = here ();
    dl (0);
    init_place = here ();
    dl (0);

    for (i = 0; i < (int) (sizeof (sockfuncs) / sizeof (sockfuncs[0])); i++) {
	sockfuncvecs[i] = here ();
	calltrap (deftrap2 (sockfuncs[i], TRAPFLAG_EXTRA_STACK, funcnames[i]));
	dw (RTS);
    }

    /* FuncTable */
    functable = here ();
    for (i = 1; i < 4; i++)
	dl (sockfuncvecs[i]);   /* Open / Close / Expunge */
    dl (EXPANSION_nullfunc);    /* Null */
    for (i = 4; i < (int) (sizeof (sockfuncs) / sizeof (sockfuncs[0])); i++)
	dl (sockfuncvecs[i]);
    dl (0xFFFFFFFF);            /* end of table */

    /* DataTable */
    datatable = here ();
    dw (0xE000);                /* INITBYTE */
    dw (0x0008);                /* LN_TYPE */
    dw (0x0900);                /* NT_LIBRARY */
    dw (0xC000);                /* INITLONG */
    dw (0x000A);                /* LN_NAME */
    dl (resname);
    dw (0xE000);                /* INITBYTE */
    dw (0x000E);                /* LIB_FLAGS */
    dw (0x0600);                /* LIBF_SUMUSED | LIBF_CHANGED */
    dw (0xD000);                /* INITWORD */
    dw (0x0014);                /* LIB_VERSION */
    dw (0x0004);
    dw (0xD000);
    dw (0x0016);                /* LIB_REVISION */
    dw (0x0001);
    dw (0xC000);
    dw (0x0018);                /* LIB_IDSTRING */
    dl (resid);
    dl (0x00000000);            /* end of table */

    end = here ();

    org (begin + 6);            /* Load END value */
    dl (end);
    org (data_place);
    dl (datatable);
    org (func_place);
    dl (functable);
    org (init_place);
    dl (*sockfuncvecs);

    write_log ("bsdsocket: resident at 0x%x-0x%x\n", begin, end);
    org (end);
}

#else /* ! BSDSOCKET */

void bsdlib_install (void)
{
}

void bsdlib_reset (void)
{
}

#endif
