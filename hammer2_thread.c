// SPDX-License-Identifier: BSD-3-Clause
/*
 * hammer2_thread.c -- Linux port of the HAMMER2 management-thread API.
 *
 * DragonFly backs hammer2_thread_t with lwkt threads and uses tsleep/wakeup
 * keyed on &thr->flags for the flag-change handshake.  This port backs them
 * with Linux kthreads and a per-thread waitqueue (thr->wait).  The flag word
 * (thr->flags) is manipulated with the same atomic_cmpset_int() helper used
 * elsewhere in the port; every state change wakes thr->wait so waiters that
 * blocked in wait_event*() re-evaluate promptly.
 *
 * The only consumer at present is the PFS synchronization thread in
 * hammer2_synchro.c.  This port does NOT auto-start that thread from the
 * mount path (the filesystem is built around synchronous, single-threaded
 * XOP execution), so this API exists for completeness and explicit/manual
 * activation rather than being driven during normal mount/unmount.
 */

#include "hammer2.h"

#include <linux/kthread.h>
#include <linux/wait.h>
#include <linux/sched.h>

/*
 * Set flags and wakeup any waiters.
 *
 * WARNING! During teardown (thr) can disappear the instant our cmpset
 *	    succeeds.
 */
void
hammer2_thr_signal(hammer2_thread_t *thr, uint32_t flags)
{
	uint32_t oflags;
	uint32_t nflags;

	for (;;) {
		oflags = thr->flags;
		cpu_ccfence();
		nflags = (oflags | flags) & ~HAMMER2_THREAD_WAITING;

		if (atomic_cmpset_int(&thr->flags, oflags, nflags)) {
			if (oflags & HAMMER2_THREAD_WAITING)
				wake_up(&thr->wait);
			break;
		}
	}
}

/*
 * Set and clear flags and wakeup any waiters.
 */
void
hammer2_thr_signal2(hammer2_thread_t *thr, uint32_t posflags, uint32_t negflags)
{
	uint32_t oflags;
	uint32_t nflags;

	for (;;) {
		oflags = thr->flags;
		cpu_ccfence();
		nflags = (oflags | posflags) &
			~(negflags | HAMMER2_THREAD_WAITING);
		if (atomic_cmpset_int(&thr->flags, oflags, nflags)) {
			if (oflags & HAMMER2_THREAD_WAITING)
				wake_up(&thr->wait);
			break;
		}
	}
}

/*
 * Wait until all the bits in flags are set.
 */
void
hammer2_thr_wait(hammer2_thread_t *thr, uint32_t flags)
{
	wait_event(thr->wait, (READ_ONCE(thr->flags) & flags) == flags);
}

/*
 * Wait until any of the bits in flags are set, with timeout (in ticks).
 */
int
hammer2_thr_wait_any(hammer2_thread_t *thr, uint32_t flags, int timo)
{
	long ret;

	if (timo == 0) {
		wait_event(thr->wait, (READ_ONCE(thr->flags) & flags) != 0);
		return 0;
	}
	ret = wait_event_timeout(thr->wait,
				 (READ_ONCE(thr->flags) & flags) != 0, timo);
	if (ret == 0)
		return HAMMER2_ERROR_ETIMEDOUT;
	return 0;
}

/*
 * Wait until the bits in flags are clear.
 */
void
hammer2_thr_wait_neg(hammer2_thread_t *thr, uint32_t flags)
{
	wait_event(thr->wait, (READ_ONCE(thr->flags) & flags) == 0);
}

/*
 * kthread trampoline.  Linux threadfns return int; the HAMMER2 thread body
 * returns void and exits by setting thr->td = NULL and signalling STOPPED.
 * We never call kthread_stop() on these (the STOP-flag handshake drives
 * teardown), so the thread simply returns and exits on its own.
 */
static int
hammer2_thr_trampoline(void *data)
{
	hammer2_thread_t *thr = data;

	thr->func(thr);
	return 0;
}

/*
 * Initialize the supplied thread structure, starting the specified thread.
 *
 * NOTE: thr structure can be retained across mounts and unmounts for this
 *	 pmp, so make sure the flags are in a sane state.
 */
void
hammer2_thr_create(hammer2_thread_t *thr, hammer2_pfs_t *pmp,
		   hammer2_dev_t *hmp,
		   const char *id, int clindex, int repidx,
		   void (*func)(void *arg))
{
	struct task_struct *task;

	thr->pmp = pmp;		/* xop helpers */
	thr->hmp = hmp;		/* bulkfree */
	thr->clindex = clindex;
	thr->repidx = repidx;
	thr->func = func;
	init_waitqueue_head(&thr->wait);
	atomic_clear_int(&thr->flags, HAMMER2_THREAD_STOP |
				      HAMMER2_THREAD_STOPPED |
				      HAMMER2_THREAD_FREEZE |
				      HAMMER2_THREAD_FROZEN);
	if (thr->scratch == NULL)
		thr->scratch = hmalloc(MAXPHYS, M_HAMMER2, M_WAITOK | M_ZERO);

	if (repidx >= 0) {
		task = kthread_run(hammer2_thr_trampoline, thr, "%s-%s.%02d",
				   id, pmp->pfs_names[clindex], repidx);
	} else if (pmp) {
		task = kthread_run(hammer2_thr_trampoline, thr, "%s-%s",
				   id, pmp->pfs_names[clindex]);
	} else {
		task = kthread_run(hammer2_thr_trampoline, thr, "%s", id);
	}

	if (IS_ERR(task)) {
		thr->task = NULL;
		thr->td = NULL;
		hprintf("failed to start thread %s: %ld\n", id, PTR_ERR(task));
		return;
	}
	thr->task = task;
	thr->td = (thread_t)task;
}

/*
 * Terminate a thread.  This function will silently return if the thread
 * was never initialized or has already been deleted.
 *
 * This is accomplished by setting the STOP flag and waiting for the thread
 * to clear thr->td and signal STOPPED.
 */
void
hammer2_thr_delete(hammer2_thread_t *thr)
{
	if (thr->td == NULL)
		return;
	hammer2_thr_signal(thr, HAMMER2_THREAD_STOP);
	hammer2_thr_wait(thr, HAMMER2_THREAD_STOPPED);
	thr->pmp = NULL;
	thr->task = NULL;
	if (thr->scratch) {
		hfree(thr->scratch, M_HAMMER2, MAXPHYS);
		thr->scratch = NULL;
	}
}

/*
 * Asynchronous remaster request.  Ask the synchronization thread to start
 * over soon.  The thread always recalculates mastership when restarting.
 */
void
hammer2_thr_remaster(hammer2_thread_t *thr)
{
	if (thr->td == NULL)
		return;
	hammer2_thr_signal(thr, HAMMER2_THREAD_REMASTER);
}

void
hammer2_thr_freeze_async(hammer2_thread_t *thr)
{
	hammer2_thr_signal(thr, HAMMER2_THREAD_FREEZE);
}

void
hammer2_thr_freeze(hammer2_thread_t *thr)
{
	if (thr->td == NULL)
		return;
	hammer2_thr_signal(thr, HAMMER2_THREAD_FREEZE);
	hammer2_thr_wait(thr, HAMMER2_THREAD_FROZEN);
}

void
hammer2_thr_unfreeze(hammer2_thread_t *thr)
{
	if (thr->td == NULL)
		return;
	hammer2_thr_signal(thr, HAMMER2_THREAD_UNFREEZE);
	hammer2_thr_wait_neg(thr, HAMMER2_THREAD_FROZEN);
}

int
hammer2_thr_break(hammer2_thread_t *thr)
{
	if (thr->flags & (HAMMER2_THREAD_STOP |
			  HAMMER2_THREAD_REMASTER |
			  HAMMER2_THREAD_FREEZE)) {
		return 1;
	}
	return 0;
}
