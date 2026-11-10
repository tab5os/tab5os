/* SPDX-License-Identifier: Apache-2.0
 * Keeping a misbehaving app from taking the OS down with it. Pure
 * decisions, host-tested; the device side is in lifecycle.c and anr.c.
 *
 *   Crashes  A CPU fault in an installed native app's code, or in a task the
 *            app created, ends the app instead of rebooting the device
 *            (t5_core/fault.h). t5_iso_blame() decides whose fault it is.
 *   Hangs    "Close app" on the not-responding dialog moves the UI task out
 *            of the app the same way, instead of restarting the device.
 *   Memory   Each app has its own heap, in PSRAM chunks that nothing else
 *            shares (apps_hw.c), so an overrun stays in the app and exit
 *            drops the chunks whole. Its tasks and kernel objects are in
 *            PSRAM too: an app can't use up internal RAM directly. Growth is
 *            refused when the OS's reserve would be touched
 *            (t5_iso_heap_room). If PSRAM or internal RAM still runs
 *            critically low, the foreground app is closed (t5_iso_mem_check,
 *            t5_iso_l2_check).
 *   CPU      An app task that keeps a core busy is moved to the lowest
 *            priority, so OS services below the app ceiling still run
 *            (t5_iso_hog).
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- crashes and hangs --------------------------------------------------- */
typedef enum {
    T5_ISO_NONE,        /* not the app's: panic as before */
    T5_ISO_UNWIND,      /* unwind the task to its recovery point */
    T5_ISO_END_TASK,    /* end the task (it is the app's own) */
    T5_ISO_UNWIND_GUESS, /* unwind, though the app isn't provably on the call chain */
} t5_iso_verdict_t;

typedef struct {
    uintptr_t text_lo, text_hi;     /* the app's code; empty: not a native app */
    bool apps_task;                 /* the task is one the app created */
    bool has_point;                 /* the task has a recovery point... */
    bool point_ok;                  /* ...that protects this app (or any app) */
    bool unwind_safe;               /* the task's state allows abandoning the call (UI: not rendering) */
    uint32_t pc, ra;
    const uint32_t *stack;          /* stack words between the fault and the recovery point */
    size_t words;
    /* When the chain doesn't show the app (a tail call from the app leaves
     * no return address behind): the task was working for the app, the
     * fault was a bad load or store, and no guess was made recently. */
    bool working_for_app;
    bool data_fault;
    bool guess_ok;
} t5_iso_fault_t;

#define T5_ISO_GUESS_GAP_MS 30000u  /* a second unprovable fault this soon panics */

t5_iso_verdict_t t5_iso_blame(const t5_iso_fault_t *f);
static inline bool t5_iso_in_text(const t5_iso_fault_t *f, uintptr_t a)
{
    return a >= f->text_lo && a < f->text_hi;
}

/* ---- memory -------------------------------------------------------------- */
#define T5_ISO_PSRAM_RESERVE (2u * 1024u * 1024u) /* PSRAM an app's heap can never take */
#define T5_ISO_PSRAM_CRITICAL (1024u * 1024u)   /* below this, the foreground app is closed... */
#define T5_ISO_LOW_FOR_MS 1500u                 /* ...once it has stayed there this long */

#define T5_ISO_ARENA_FIRST (128u * 1024u)       /* an app's first heap chunk... */
#define T5_ISO_ARENA_MAX (2u * 1024u * 1024u)   /* ...doubling up to this */

/* May an app's heap grow by n bytes, given free PSRAM? */
bool t5_iso_heap_room(size_t n, size_t psram_free);
/* Size of the next heap chunk after one of last bytes (0: the first). */
size_t t5_iso_arena_next(size_t last);

/* Internal RAM. A block is set aside at boot and handed back when free L2
 * RAM falls under T5_ISO_L2_CRITICAL, so the OS can still act; if it stays
 * short the foreground app is closed. */
#define T5_ISO_L2_RESERVE (16u * 1024u)
#define T5_ISO_L2_CRITICAL (16u * 1024u)
#define T5_ISO_L2_PLENTY (48u * 1024u)          /* take the reserve back above this */

typedef enum {
    T5_ISO_L2_NONE,
    T5_ISO_L2_RELEASE,
    T5_ISO_L2_RETAKE,
    T5_ISO_L2_CLOSE_APP,
} t5_iso_l2_action_t;

typedef struct {
    bool released;
    bool low;
    uint32_t since_ms;
} t5_iso_l2_t;

t5_iso_l2_action_t t5_iso_l2_check(t5_iso_l2_t *s, size_t l2_free, uint32_t now_ms);

typedef struct {
    bool low;
    uint32_t since_ms;
} t5_iso_mem_t;

/* Called periodically: true when free PSRAM has been critical for long
 * enough that the foreground app should be closed. Resets after. */
bool t5_iso_mem_check(t5_iso_mem_t *s, size_t psram_free, uint32_t now_ms);

/* ---- CPU ----------------------------------------------------------------- */
#define T5_ISO_HOG_PCT 90               /* of a core... */
#define T5_ISO_HOG_STRIKES 3            /* ...in this many checks in a row */
#define T5_ISO_HOG_PRIO 1               /* where a hog is moved */

/* One check of one task: ran_us out of wall_us since the last check. True
 * once the task has hogged a core for T5_ISO_HOG_STRIKES checks running. */
bool t5_iso_hog(uint8_t *strikes, uint64_t ran_us, uint64_t wall_us);

/* ---- network calls --------------------------------------------------------- */
/* An app task must not be deleted inside lwIP. A select() waiter is linked
 * into lwIP's list from the task's stack and points at the task's per-thread
 * semaphore, which the task's deletion frees; netconn calls and DNS wait on
 * that same semaphore. The tcpip thread signals it later and the device
 * panics. So every lwIP socket call made on an app task is counted (the
 * linker wraps them, netguard.c), whether the app made it or OS code made it
 * for the app (t5_net, esp-tls, VFS read/write):
 *   - a task inside one is not parked or deleted, as if it held a lock;
 *   - once its app is exiting, calls that wait on the network are refused
 *     (ECANCELED), so t5_net's waits end within a slice and unwind;
 *   - a task still inside one when the exit has to go on is orphaned: it
 *     leaves its app and ends itself when the call returns.
 * The word is per task: calls in progress, and the orphan flag. */
typedef uint32_t t5_iso_net_t;
#define T5_ISO_NET_ORPHAN 0x80000000u

/* Starting a call. waits: the call can wait on the network (not close,
 * shutdown or an option); exiting: the task is its exiting app's own. False:
 * refused, the word unchanged. */
bool t5_iso_net_enter(t5_iso_net_t *w, bool waits, bool exiting);
/* Leaving a call. True: the task was orphaned and is out of lwIP now; it
 * must end itself without going back to the caller. */
bool t5_iso_net_leave(t5_iso_net_t *w);
/* The exit deleting the task. True: delete it. False: it is inside a call
 * and is orphaned instead. */
bool t5_iso_net_delete(t5_iso_net_t *w);
static inline bool t5_iso_net_inside(t5_iso_net_t w)
{
    return (w & ~T5_ISO_NET_ORPHAN) != 0;
}

#ifdef __cplusplus
}
#endif
