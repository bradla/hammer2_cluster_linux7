/*
 * hammer2_os.h - Linux OS compatibility shims for HAMMER2 port
 *
 * Provides BSD-style locking primitives (with refcount semantics) on top of
 * Linux mutexes / rwsems / wait queues.
 */

#ifndef _HAMMER2_OS_H_
#define _HAMMER2_OS_H_

#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/atomic.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/delay.h>
#include <linux/errno.h>

/*
 * Lock primitive typedefs.  These are referenced by hammer2.h and must be
 * defined before any of the inline helpers below.
 */
/*
 * RECURSIVE mutex shim.  HAMMER2's chain/inode locks are taken recursively by
 * the same thread (e.g. an embedded-data inode whose chain_lookup re-locks the
 * inode chain).  A plain Linux mutex is non-recursive and self-deadlocks on
 * that, so we track the owning task and a recursion depth: the underlying
 * mutex is physically acquired only on the 0->1 transition and released on the
 * 1->0 transition.  Shared and exclusive are both modeled as exclusive holds.
 *
 * `depth` is only ever modified by the owning thread while holding the mutex
 * (or transitioning ownership), so it needs no atomics; `owner` is published
 * with WRITE_ONCE/READ_ONCE for cross-thread visibility of the != current test.
 */
struct hammer2_mtx_wrapper {
	struct mutex		lock;
	struct task_struct	*owner;
	int			depth;
};
typedef struct hammer2_mtx_wrapper	hammer2_mtx_t;

typedef struct rw_semaphore		hammer2_lk_t;
typedef wait_queue_head_t		hammer2_lkc_t;
typedef spinlock_t			hammer2_spin_t;

static inline void
hammer2_mtx_init(hammer2_mtx_t *p, const char *s)
{
	mutex_init(&p->lock);
	p->owner = NULL;
	p->depth = 0;
}

static inline void
hammer2_mtx_init_recurse(hammer2_mtx_t *p, const char *s)
{
	hammer2_mtx_init(p, s);
}

static inline void
hammer2_mtx_ex(hammer2_mtx_t *p)
{
	if (READ_ONCE(p->owner) == current) {
		++p->depth;
		return;
	}
	mutex_lock(&p->lock);
	WRITE_ONCE(p->owner, current);
	p->depth = 1;
}

static inline void
hammer2_mtx_sh(hammer2_mtx_t *p)
{
	hammer2_mtx_ex(p);
}

static inline void
hammer2_mtx_unlock(hammer2_mtx_t *p)
{
	if (--p->depth == 0) {
		WRITE_ONCE(p->owner, NULL);
		mutex_unlock(&p->lock);
	}
}

static inline int
hammer2_mtx_refs(hammer2_mtx_t *p)
{
	return p->depth;
}

static inline void
hammer2_mtx_destroy(hammer2_mtx_t *p)
{
	mutex_destroy(&p->lock);
}

/*
 * BSD hammer2_mtx_sleep(channel, mtx, msg, timo) treats `channel` as an
 * opaque cookie used by hammer2_mtx_wakeup(channel) to wake matching
 * sleepers.  On Linux we lack a per-address wait queue, so accept a void*
 * channel and just sleep for the requested timeout; spurious wakeups are
 * tolerated by callers, which always re-check the wait condition.
 */
static inline int
hammer2_mtx_sleep(void *c, hammer2_mtx_t *p, const char *s, int timo)
{
	int depth = p->depth;

	(void)c;
	(void)s;

	/* Fully release (regardless of recursion depth), sleep, reacquire. */
	p->depth = 0;
	WRITE_ONCE(p->owner, NULL);
	mutex_unlock(&p->lock);

	schedule_timeout_uninterruptible(timo ? timo : 1);

	mutex_lock(&p->lock);
	WRITE_ONCE(p->owner, current);
	p->depth = depth;
	return 0;
}

static inline void
hammer2_mtx_wakeup(void *c)
{
	(void)c;
	/* No-op: paired with the timeout-based hammer2_mtx_sleep above. */
}

/*
 * BSD ssleep(ident, spin, flags, wmesg, timo): atomically release the spinlock,
 * sleep for `timo` ticks, then reacquire before returning.  Linux already has
 * a different ssleep() in <linux/delay.h>, so this helper is named distinctly;
 * the CCMS code maps the BSD name to it locally.  Like hammer2_mtx_sleep we
 * have no per-address wait queue and simply time out; callers loop+re-check.
 */
static inline int
hammer2_spin_ssleep(const volatile void *ident, hammer2_spin_t *spin,
    int flags, const char *wmesg, int timo)
{
	(void)ident;
	(void)flags;
	(void)wmesg;

	spin_unlock(spin);
	schedule_timeout_uninterruptible(timo ? timo : 1);
	spin_lock(spin);
	return 0;
}

static inline int
hammer2_mtx_owned(hammer2_mtx_t *p)
{
	return READ_ONCE(p->owner) == current;
}

static inline int
hammer2_mtx_ex_try(hammer2_mtx_t *p)
{
	if (READ_ONCE(p->owner) == current) {
		++p->depth;
		return 0;
	}
	if (mutex_trylock(&p->lock)) {
		WRITE_ONCE(p->owner, current);
		p->depth = 1;
		return 0;
	}
	return 1;
}

static inline int
hammer2_mtx_sh_try(hammer2_mtx_t *p)
{
	return hammer2_mtx_ex_try(p);
}

static inline int
hammer2_mtx_upgrade_try(hammer2_mtx_t *p)
{
	/*
	 * In this shim hammer2_mtx_sh() and hammer2_mtx_ex() both take the
	 * underlying mutex exclusively, so whenever the lock is held it is
	 * already "exclusive".  An upgrade therefore always succeeds (0).
	 *
	 * Returning failure (1) here caused hammer2_chain_unlock()'s
	 * lockcnt 1->0 path to live-loop forever ("h2race2"), since that path
	 * only completes when the upgrade succeeds.
	 */
	(void)p;
	return 0;
}

static inline int
hammer2_mtx_temp_release(hammer2_mtx_t *p)
{
	int x = p->depth;

	p->depth = 0;
	WRITE_ONCE(p->owner, NULL);
	mutex_unlock(&p->lock);
	return x;
}

static inline void
hammer2_mtx_temp_restore(hammer2_mtx_t *p, int x)
{
	mutex_lock(&p->lock);
	WRITE_ONCE(p->owner, current);
	p->depth = x;
}

/*
 * Convenience aliases.  BSD code uses _lock/_shunlock; mtx_ex is the
 * exclusive-lock primitive, and the wrapper's mtx_unlock releases both
 * shared and exclusive holds.
 */
#define hammer2_mtx_lock(p)	hammer2_mtx_ex(p)
#define hammer2_mtx_shunlock(p)	hammer2_mtx_unlock(p)

/*
 * Spinlock primitives.  BSD's spin_ex / spin_sh map to Linux spin_lock();
 * Linux doesn't distinguish read/write on plain spinlocks.
 */
#define hammer2_spin_ex(s)	spin_lock(s)
#define hammer2_spin_unex(s)	spin_unlock(s)
#define hammer2_spin_sh(s)	spin_lock(s)
#define hammer2_spin_unsh(s)	spin_unlock(s)

/*
 * rw_semaphore-based "lk" lock primitives.  hammer2_lk_t is a rw_semaphore.
 */
#define hammer2_lk_ex(l)	down_write(l)
#define hammer2_lk_unlock(l)	up_write(l)
#define hammer2_lk_assert_ex(l)	WARN_ON(!rwsem_is_locked(l))

/*
 * Diagnostic helper: BSD provides hammer2_mtx_assert_unlocked() to assert
 * the lock isn't held by us.  Linux mutex_is_locked() doesn't distinguish
 * "held by current task" from "held by someone else", so the strict BSD
 * semantics aren't reproducible.  We approximate by simply warning if the
 * mutex is currently locked at all.
 */
#define hammer2_mtx_assert_unlocked(p)	WARN_ON(READ_ONCE((p)->owner) == current)

/*
 * Condition-variable style sleep/wakeup.  hammer2_lkc_t is a
 * wait_queue_head_t.  We release the associated lk while waiting and
 * reacquire on wake.
 */
static inline int
hammer2_lkc_sleep(hammer2_lkc_t *c, hammer2_lk_t *lk, const char *s, int timo)
{
	int ret = 0;

	up_write(lk);
	if (timo == 0) {
		wait_event(*c, 0);
	} else {
		ret = wait_event_timeout(*c, 0, msecs_to_jiffies(timo));
		ret = (ret == 0) ? -ETIMEDOUT : 0;
	}
	down_write(lk);
	return ret;
}

static inline void
hammer2_lkc_wakeup(hammer2_lkc_t *c)
{
	wake_up(c);
}

static inline void
hammer2_lkc_init(hammer2_lkc_t *c, const char *s)
{
	init_waitqueue_head(c);
}

static inline void
hammer2_lkc_destroy(hammer2_lkc_t *c)
{
	(void)c;
}

#endif /* !_HAMMER2_OS_H_ */
