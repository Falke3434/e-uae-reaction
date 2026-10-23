/*
 * E-UAE - The portable Amiga Emulator
 *
 * bsdsocket.library emulation, AmigaOS 4 host side.
 *
 * Every emulated SocketBase gets one Roadshow SocketBase, opened by its
 * first worker process with SBTC_CAN_SHARE_LIBRARY_BASES, and a small
 * pool of worker processes that share it (v17). Calls of one Amiga
 * SocketBase can so run at the same time: a recv() that waits for data
 * no longer holds up the send() of another task of the same program.
 *
 *  - a call is a struct bsd_msg owned by the caller; a worker never
 *    touches it after marking it done
 *  - bsdproxy_call_idle(): only if a worker is idle - the trap waits on a
 *    host signal (calls that never wait in Roadshow)
 *  - bsdproxy_post(): the calling Amiga task waits (Exec Wait() on a
 *    signal of its own), the emulation keeps running; the worker wakes
 *    it with uae_Signal()
 *  - bsdproxy_abort(): CTRL-C to the worker running the call (EINTR)
 *  - workers never call Amiga code and never use newlib
 */

#ifndef EUAE_BSDSOCK_PROXY_H
#define EUAE_BSDSOCK_PROXY_H

#include <exec/ports.h>

struct SocketIFace;
struct bsd_proxy;
struct bsd_msg;

typedef void (*bsd_fn) (struct bsd_msg *m);

#define BSD_MSGBUF 64

struct bsd_msg {
    struct Message msg;
    bsd_fn fn;                  /* NULL: quit */
    struct bsd_proxy *px;
    uae_u32 r[16];              /* D0-D7/A0-A7 of the call */
    uae_s32 result;
    void *hostdata;             /* AllocVec()ed result (hostent, ...) */
    char buf[BSD_MSGBUF];
    volatile int done;
    volatile int abort;
    struct Task * volatile runner;   /* worker running it (for abort) */
    struct Task *waiter;        /* host wait: task to wake */
    int waitbit;
    uae_u32 guest_task;         /* Amiga wait: task to signal */
    uae_u32 guest_mask;
};

/* start the proxy of one SocketBase (first worker opens Roadshow),
 * NULL on failure */
extern struct bsd_proxy *bsdproxy_start (void);
/* run m at once on an idle worker and wait for it (host wait).
 * Returns 0 (m not run) if no worker is free - the caller then uses
 * bsdproxy_post() so the emulation never waits behind a blocked call. */
extern int bsdproxy_call_idle (struct bsd_proxy *px, struct bsd_msg *m);
/* run m in any case and wait for it on the host (last resort) */
extern void bsdproxy_call (struct bsd_proxy *px, struct bsd_msg *m);
/* queue m; when done, m->done is set and m->guest_task signalled */
extern void bsdproxy_post (struct bsd_proxy *px, struct bsd_msg *m);
/* break the Roadshow call of m (EINTR); m->abort must be set first */
extern void bsdproxy_abort (struct bsd_msg *m);
/* end all workers; the last one closes the Roadshow base (and with it
 * all sockets). resetting: no more Amiga signals. px is gone after. */
extern void bsdproxy_quit (struct bsd_proxy *px, int resetting);

/* for the call functions, running in a worker */
extern struct SocketIFace *bsdproxy_iface (struct bsd_msg *m);
extern void bsdproxy_set_errno (struct bsd_msg *m, int err);

#endif
