/*
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2022-2023 Tomohiro Kusumi <tkusumi@netbsd.org>
 * Copyright (c) 2011-2022 The DragonFly Project.  All rights reserved.
 *
 * This code is derived from software contributed to The DragonFly Project
 * by Matthew Dillon <dillon@dragonflybsd.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name of The DragonFly Project nor the names of its
 *    contributors may be used to endorse or promote products derived
 *    from this software without specific, prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * ``AS IS'' AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE
 * COPYRIGHT HOLDERS OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT
 * OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#ifndef _HAMMER2_H_
#define _HAMMER2_H_

#include <linux/types.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>
#include <linux/rwsem.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/rbtree.h>
#include <linux/uuid.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/bio.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/mm.h>
#include <asm/atomic.h>

#include "hammer2_compat.h"
#include "hammer2_os.h"
#include "hammer2_disk.h"
#include "hammer2_ioctl.h"

/*
 * Linux replacements for BSD queue macros.
 *
 * Note: macro parameter is named `hp` (head pointer) rather than `head` to
 * avoid colliding with the literal field name `head` used inside the
 * generated struct.
 */
#define TAILQ_ENTRY(type) struct list_head
#define TAILQ_HEAD(name, type) struct name { struct list_head head; }
#define TAILQ_INIT(hp) INIT_LIST_HEAD(&(hp)->head)
#define TAILQ_INSERT_TAIL(hp, elm, field) list_add_tail(&(elm)->field, &(hp)->head)
#define TAILQ_INSERT_HEAD(hp, elm, field) list_add(&(elm)->field, &(hp)->head)
#define TAILQ_REMOVE(hp, elm, field) list_del(&(elm)->field)
/*
 * BSD TAILQ_FOREACH terminates with (var) == NULL when the list is empty or
 * exhausted, and a LOT of HAMMER2 code relies on that (e.g. iterate, then
 * `if (var == NULL)` means "not found").  Linux's list_for_each_entry leaves
 * the iterator pointing at the list head (container_of(head,...)) a bogus
 * non-NULL pointer which makes those checks fail and dereference garbage.
 * So implement BSD NULL-terminating semantics explicitly.
 */
#define TAILQ_FOREACH(var, hp, field) \
    for ((var) = list_first_entry_or_null(&(hp)->head, typeof(*(var)), field); \
         (var) != NULL; \
         (var) = list_is_last(&(var)->field, &(hp)->head) ? NULL \
                 : list_next_entry((var), field))
/*
 * BSD's TAILQ_FIRST(&head) returns an element pointer; since our shim TAILQ
 * head encodes no element type, callers must spell out the type by using
 * list_first_entry_or_null() directly, or wrap with the LIST_FIRST_AS()
 * convenience below.
 */
#define LIST_FIRST_AS(hp, type, field) \
    list_first_entry_or_null(&(hp)->head, type, field)
#define TAILQ_CONCAT(dst, src, field) \
    do { list_splice_tail_init(&(src)->head, &(dst)->head); } while (0)

#define LIST_HEAD(name, type) struct name { struct list_head head; }
#define LIST_ENTRY(type) struct list_head
#define LIST_INIT(hp) INIT_LIST_HEAD(&(hp)->head)
#define LIST_INSERT_HEAD(hp, elm, field) list_add(&(elm)->field, &(hp)->head)
#define LIST_REMOVE(elm, field) list_del(&(elm)->field)
/* NULL-terminating like BSD (see TAILQ_FOREACH note above). */
#define LIST_FOREACH(var, hp, field) \
    for ((var) = list_first_entry_or_null(&(hp)->head, typeof(*(var)), field); \
         (var) != NULL; \
         (var) = list_is_last(&(var)->field, &(hp)->head) ? NULL \
                 : list_next_entry((var), field))

/*
 * Lock primitive typedefs (hammer2_mtx_t, hammer2_lk_t, hammer2_lkc_t,
 * hammer2_spin_t) and the hammer2_mtx_* inline helpers live in
 * hammer2_os.h, which is included above.  Only the additional non-mtx
 * helpers are defined here.
 */
/*
 * BSD lockmgr() shims.  DragonFly call sites pass an optional name string
 * for diagnostics; the Linux equivalents ignore the extra argument.
 */
#define hammer2_lk_init(l, ...)		init_rwsem(l)
#define hammer2_lk_destroy(l)
#define hammer2_lk_lock(l, flags)	down_write(l)
#define hammer2_lk_unlock(l)		up_write(l)
#define hammer2_lk_sh(l)		down_read(l)
#define hammer2_lk_shunlock(l)		up_read(l)

#define hammer2_spin_init(s, ...)	spin_lock_init(s)
#define hammer2_spin_destroy(s)
#define hammer2_spin_lock(s)		spin_lock(s)
#define hammer2_spin_unlock(s)		spin_unlock(s)

/*
 * Diagnostic-only attribute.  BSD uses __diagused to mark a variable that
 * is only referenced inside #ifdef INVARIANTS blocks; on Linux we always
 * keep it live so just suppress unused-variable warnings.
 */
#ifndef __diagused
#define __diagused	__attribute__((unused))
#endif

/* Linux atomic operations */
#define atomic_add_32(v, i) atomic_add(i, (atomic_t *)v)

/* Linux RB tree wrapper */
struct hammer2_rb_node {
    struct rb_node node;
};

/*
 * Generic BSD <sys/tree.h>-style red-black tree macros, implemented over
 * Linux's <linux/rbtree.h>.  RB_GENERATE() emits type-specific functions, so
 * both the per-core chain tree (hammer2_chain_tree) and the kdmsg message
 * state trees (kdmsg_state_tree) work from the same definitions.
 */
#include "hammer2_rb.h"

/* Forward declarations */
struct hammer2_io;
struct hammer2_chain;
struct hammer2_depend;
struct hammer2_inode;
struct hammer2_dev;
struct hammer2_pfs;
struct hammer2_xop_head;
union hammer2_xop;

typedef struct hammer2_io hammer2_io_t;
typedef struct hammer2_chain hammer2_chain_t;
typedef struct hammer2_depend hammer2_depend_t;
typedef struct hammer2_inode hammer2_inode_t;
typedef struct hammer2_dev hammer2_dev_t;
typedef struct hammer2_pfs hammer2_pfs_t;
typedef union hammer2_xop hammer2_xop_t;

/* Global list of PFS */
struct hammer2_pfslist {
    struct list_head head;
};
typedef struct hammer2_pfslist hammer2_pfslist_t;

/* Per HAMMER2 list of device vnode */
struct hammer2_devvp_list {
    struct list_head head;
};
typedef struct hammer2_devvp_list hammer2_devvp_list_t;

/* Per PFS list of inode */
struct hammer2_ipdep_list {
    struct list_head head;
};
typedef struct hammer2_ipdep_list hammer2_ipdep_list_t;

/* Per chain rbtree of sub-chain (RB_HEAD form: { struct rb_root rbh_root; }) */
RB_HEAD(hammer2_chain_tree, hammer2_chain);
RB_PROTOTYPE(hammer2_chain_tree, hammer2_chain, rbnode, hammer2_chain_cmp);
typedef struct hammer2_chain_tree hammer2_chain_tree_t;

/* Per PFS list of depend */
struct hammer2_depq_head {
    struct list_head head;
};
typedef struct hammer2_depq_head hammer2_depq_head_t;

/* Per PFS / depend list of inode */
struct hammer2_inoq_head {
    struct list_head head;
};
typedef struct hammer2_inoq_head hammer2_inoq_head_t;

/*
 * HAMMER2 inode dependency record.  One per pmp->depq entry; each holds a
 * TAILQ of inodes that must be sync'd together.
 */
struct hammer2_depend {
    TAILQ_ENTRY(hammer2_depend) entry;
    hammer2_inoq_head_t  sideq;        /* TAILQ of hammer2_inode_t */
    long                 count;
    int                  pass2;
    int                  unused01;
};

/*
 * Fatal-error helper.  BSD code calls hpanic("..."); on Linux we map it to
 * the existing hammer2_panic() macro defined in hammer2_compat.h.
 */
#define hpanic(fmt, ...)	hammer2_panic(fmt "\n", ##__VA_ARGS__)

/*
 * Forward declarations for routines defined in other translation units that
 * are referenced from inline helpers in this header.
 */
void hammer2_io_putblk(hammer2_io_t **diop);
void hammer2_chain_unhold(hammer2_chain_t *chain);
void hammer2_chain_rehold(hammer2_chain_t *chain);

/*
 * Additional forward declarations for hammer2_*.c -internal helpers.  These
 * cross translation-unit boundaries; defining them as plain externs lets the
 * caller-side compile while leaving link-time resolution to the proper
 * implementation file.
 */
void hammer2_chain_ref(hammer2_chain_t *chain);
void hammer2_chain_ref_hold(hammer2_chain_t *chain);
void hammer2_chain_drop(hammer2_chain_t *chain);
void hammer2_chain_drop_unhold(hammer2_chain_t *chain);
int  hammer2_chain_lock(hammer2_chain_t *chain, int how);
void hammer2_chain_unlock(hammer2_chain_t *chain);
int hammer2_chain_cmp(const hammer2_chain_t *chain1,
		const hammer2_chain_t *chain2);
hammer2_chain_t *hammer2_chain_lookup(hammer2_chain_t **parentp,
		hammer2_key_t *key_nextp, hammer2_key_t key_beg,
		hammer2_key_t key_end, int *errorp, int flags);
hammer2_chain_t *hammer2_chain_next(hammer2_chain_t **parentp,
		hammer2_chain_t *chain, hammer2_key_t *key_nextp,
		hammer2_key_t key_end, int *errorp, int flags);
int  hammer2_chain_create(hammer2_chain_t **parentp, hammer2_chain_t **chainp,
		hammer2_dev_t *hmp, hammer2_pfs_t *pmp, int methods,
		hammer2_key_t key, int keybits, int type, size_t bytes,
		hammer2_tid_t mtid, hammer2_off_t dedup_off, int flags);
int  hammer2_chain_modify(hammer2_chain_t *chain, hammer2_tid_t mtid,
		hammer2_off_t dedup_off, int flags);
int  hammer2_chain_resize(hammer2_chain_t *chain, hammer2_tid_t mtid,
		hammer2_off_t dedup_off, int radix, int flags);
void hammer2_chain_setflush(hammer2_chain_t *chain);
int  hammer2_chain_scan(hammer2_chain_t *parent, hammer2_chain_t **chainp,
		hammer2_blockref_t *bref, int *firstp, int flags);
void hammer2_chain_setcheck(hammer2_chain_t *chain, void *bdata);
int  hammer2_chain_indirect_maintenance(hammer2_chain_t *parent,
		hammer2_chain_t *chain);
void hammer2_chain_lookup_done(hammer2_chain_t *parent);
hammer2_chain_t *hammer2_chain_bulksnap(hammer2_dev_t *hmp);
hammer2_chain_t *hammer2_chain_lookup_init(hammer2_chain_t *parent, int flags);
int  hammer2_chain_inode_find(hammer2_pfs_t *pmp, hammer2_key_t inum,
		int clindex, int flags, hammer2_chain_t **parentp,
		hammer2_chain_t **chainp);
int  hammer2_chain_dirent_test(const hammer2_chain_t *chain, const char *name,
		size_t name_len);
hammer2_chain_t *hammer2_chain_getparent(hammer2_chain_t *chain, int flags);
void hammer2_chain_bulkdrop(hammer2_chain_t *copy);
void hammer2_dedup_clear(hammer2_dev_t *hmp);
void hammer2_voldata_lock(hammer2_dev_t *hmp);
void hammer2_voldata_unlock(hammer2_dev_t *hmp);
void hammer2_voldata_modify(hammer2_dev_t *hmp);
int  hammer2_signal_check(void);
struct hammer2_iostat;
void hammer2_inc_iostat(struct hammer2_iostat *ios, int btype, size_t bytes);
char *hammer2_io_data(hammer2_io_t *dio, hammer2_off_t lbase);
int  hammer2_io_newnz(hammer2_dev_t *hmp, int btype, hammer2_off_t lbase,
		int lsize, hammer2_io_t **diop);
void hammer2_io_dedup_set(hammer2_dev_t *hmp, hammer2_blockref_t *bref);
void hammer2_io_dedup_delete(hammer2_dev_t *hmp, uint8_t btype,
		hammer2_off_t data_off, unsigned int bytes);
void hammer2_io_dedup_assert(hammer2_dev_t *hmp, hammer2_off_t data_off,
		unsigned int bytes);

/*
 * Method/check mask values for hammer2_chain_create() and friends.  In
 * DragonFly these are constructed via HAMMER2_ENC_METH(); a plain default
 * of NONE is enough for the porting layer where no compression / extra
 * checks are wired up yet.
 */
#ifndef HAMMER2_METH_DEFAULT
#define HAMMER2_METH_DEFAULT	0
#endif
#ifndef HAMMER2_FREEMAP_DORECOVER
#define HAMMER2_FREEMAP_DORECOVER	0x0001
#endif
int  hammer2_get_dtype(uint8_t type);
int  hammer2_get_vtype(uint8_t type);
uint8_t hammer2_get_obj_type(uint8_t vtype);
int  hammer2_getradix(size_t bytes);
int  hammer2_calc_physical(hammer2_inode_t *ip, hammer2_key_t lbase);
int  hammer2_calc_logical(hammer2_inode_t *ip, hammer2_off_t uoff,
		hammer2_key_t *lbasep, hammer2_key_t *leofp);
void hammer2_time_to_timespec(uint64_t xtime, struct timespec64 *ts);
uint64_t hammer2_timespec_to_time(const struct timespec64 *ts);
void hammer2_update_time(uint64_t *timep);
hammer2_key_t hammer2_dirhash(const char *aname, size_t len);
uint32_t hammer2_to_unix_xid(const struct uuid *uuid);
void hammer2_guid_to_uuid(struct uuid *uuid, uint32_t guid);
uid_t hammer2_inode_to_uid(const hammer2_inode_t *ip);
gid_t hammer2_inode_to_gid(const hammer2_inode_t *ip);
const char *hammer2_breftype_to_str(uint8_t type);

/* hammer2_io.c symbols */
int  hammer2_io_new(hammer2_dev_t *hmp, int btype, hammer2_off_t lbase,
		int lsize, hammer2_io_t **diop);
int  hammer2_io_bread(hammer2_dev_t *hmp, int btype, hammer2_off_t lbase,
		int lsize, hammer2_io_t **diop);
void hammer2_io_bqrelse(hammer2_io_t **diop);
void hammer2_io_brelse(hammer2_io_t **diop);
void hammer2_io_setdirty(hammer2_io_t *dio);
int  hammer2_io_bwrite(hammer2_io_t **diop);
void hammer2_io_bawrite(hammer2_io_t **diop);
void hammer2_io_bdwrite(hammer2_io_t **diop);
hammer2_io_t *hammer2_io_getquick(hammer2_dev_t *hmp, off_t lbase, int lsize);
struct hammer2_volume *hammer2_get_volume(hammer2_dev_t *hmp,
		hammer2_off_t off);
uint64_t hammer2_dedup_mask(hammer2_io_t *dio, hammer2_off_t lbase,
		unsigned int lsize);
int  hammer2_chain_modify_ip(hammer2_inode_t *ip, hammer2_chain_t *chain,
		hammer2_tid_t mtid, int flags);

/* hammer2_inode.c symbols */
void hammer2_inode_lock(hammer2_inode_t *ip, int how);
void hammer2_inode_unlock(hammer2_inode_t *ip);
hammer2_inode_t *hammer2_inode_create_pfs(hammer2_pfs_t *spmp,
		const char *name, size_t name_len, int *errorp);
int  hammer2_inode_chain_sync(hammer2_inode_t *ip);
int  hammer2_inode_chain_flush(hammer2_inode_t *ip, int flags);
int  hammer2_inode_chain_des(hammer2_inode_t *ip);
int  hammer2_inode_chain_ins(hammer2_inode_t *ip);
hammer2_inode_t *hammer2_inode_create_normal(hammer2_inode_t *pip,
		struct vattr *vap, struct ucred *cred,
		hammer2_key_t inum, int *errorp);
int  hammer2_inode_unlink_finisher(hammer2_inode_t *ip, struct inode **vprp);
void hammer2_inode_depend(hammer2_inode_t *ip, hammer2_inode_t *related);
int  hammer2_dirent_create(hammer2_inode_t *dip, const char *name,
		size_t name_len, hammer2_key_t inum, uint8_t type);
hammer2_key_t hammer2_inode_data_count(const hammer2_inode_t *ip);
hammer2_key_t hammer2_inode_inode_count(const hammer2_inode_t *ip);

/* hammer2_vfsops.c symbols */
void hammer2_trans_init(hammer2_pfs_t *pmp, uint32_t flags);
void hammer2_trans_done(hammer2_pfs_t *pmp, uint32_t flags);
void hammer2_trans_setflags(hammer2_pfs_t *pmp, uint32_t flags);
void hammer2_trans_assert_strategy(hammer2_pfs_t *pmp);
void hammer2_trans_manage_init(hammer2_pfs_t *pmp);
void hammer2_inum_hash_init(hammer2_pfs_t *pmp);
void hammer2_inum_hash_destroy(hammer2_pfs_t *pmp);
void hammer2_inode_delayed_sideq(hammer2_inode_t *ip);
hammer2_inode_t *hammer2_inode_get(hammer2_pfs_t *pmp,
		struct hammer2_xop_head *xop, hammer2_tid_t inum, int idx);
void hammer2_trans_clearflags(hammer2_pfs_t *pmp, uint32_t flags);
void hammer2_print_iostat(const struct hammer2_iostat *ios, const char *msg);
/* hammer2_assert_inode_meta is defined as a static inline below. */
int  hammer2_vfs_sync_pmp(hammer2_pfs_t *pmp, int waitfor);
int  hammer2_sync(struct mount *mp, int waitfor);
void hammer2_pfs_memory_wait(hammer2_pfs_t *pmp);
extern long hammer2_limit_dirty_chains;

/* Lifted from hammer2_bulkfree.c so other TUs can name the type. */
typedef struct hammer2_chain_save {
	struct list_head	entry;	/* TAILQ_ENTRY equivalent */
	hammer2_chain_t		*chain;
} hammer2_chain_save_t;
hammer2_pfs_t *hammer2_pfsalloc(hammer2_chain_t *chain,
		const hammer2_inode_data_t *ripdata,
		hammer2_dev_t *force_local);
void hammer2_pfsdealloc(hammer2_pfs_t *pmp, int clindex, int destroying);

/* hammer2_bulkfree.c symbols */
int  hammer2_bulkfree_pass(hammer2_dev_t *hmp, hammer2_chain_t *vchain,
		struct hammer2_ioc_bulkfree *bfi);

/* XOP descriptor objects (defined in hammer2_admin.c) live after the
 * hammer2_xop_desc_t typedef declare them below as extern struct. */
struct hammer2_xop_desc;
extern struct hammer2_xop_desc hammer2_unlink_desc;
extern struct hammer2_xop_desc hammer2_scanlhc_desc;
extern struct hammer2_xop_desc hammer2_inode_create_desc;
extern struct hammer2_xop_desc hammer2_inode_create_det_desc;
extern struct hammer2_xop_desc hammer2_inode_create_ins_desc;
extern struct hammer2_xop_desc hammer2_inode_destroy_desc;
extern struct hammer2_xop_desc hammer2_inode_chain_sync_desc;
extern struct hammer2_xop_desc hammer2_inode_unlinkall_desc;
extern struct hammer2_xop_desc hammer2_inode_connect_desc;
extern struct hammer2_xop_desc hammer2_inode_flush_desc;
extern struct hammer2_xop_desc hammer2_inode_mkdirent_desc;
extern struct hammer2_xop_desc hammer2_ipcluster_desc;
extern struct hammer2_xop_desc hammer2_lookup_desc;
extern struct hammer2_xop_desc hammer2_nrename_desc;
extern struct hammer2_xop_desc hammer2_nresolve_desc;
extern struct hammer2_xop_desc hammer2_readdir_desc;
extern struct hammer2_xop_desc hammer2_scanall_desc;
extern struct hammer2_xop_desc hammer2_bmap_desc;
extern struct hammer2_xop_desc hammer2_delete_desc;

/*
 * BSD attribute markers used in source we haven't fully ported.
 */
#ifndef __unused
#define __unused	__attribute__((unused))
#endif

/*
 * BSD VTOI(vp) maps a struct vnode * to its filesystem-private inode.
 * On Linux the equivalent is reading struct inode::i_private.
 */
static inline hammer2_inode_t *
hammer2_vtoi(struct inode *vp)
{
	return vp ? (hammer2_inode_t *)vp->i_private : NULL;
}
#define VTOI(vp)	hammer2_vtoi((struct inode *)(vp))

/* hammer2_freemap.c symbols */
int  hammer2_freemap_alloc(hammer2_chain_t *chain, size_t bytes);
void hammer2_freemap_adjust(hammer2_dev_t *hmp, hammer2_blockref_t *bref,
		int how);

/* hammer2_flush.c / hammer2_chain.c cross-refs */
int  hammer2_flush(hammer2_chain_t *chain, int flags);
void hammer2_chain_init(hammer2_chain_t *chain);
void hammer2_inode_modify(hammer2_inode_t *ip);
/* hammer2_chain_insert/repparent are static within chain.c no extern decl. */
int  hammer2_chain_delete(hammer2_chain_t *parent, hammer2_chain_t *chain,
		hammer2_tid_t mtid, int flags);
void hammer2_base_insert(hammer2_chain_t *parent, hammer2_blockref_t *base,
		int count, hammer2_chain_t *chain, hammer2_blockref_t *elm);
void hammer2_base_delete(hammer2_chain_t *parent, hammer2_blockref_t *base,
		int count, hammer2_chain_t *chain, hammer2_blockref_t *elm);
hammer2_inode_t *hammer2_inode_lookup(hammer2_pfs_t *pmp,
		hammer2_tid_t inum);
hammer2_chain_t *hammer2_inode_chain(hammer2_inode_t *ip, int clindex, int how);
hammer2_chain_t *hammer2_inode_chain_and_parent(hammer2_inode_t *ip,
		int clindex, hammer2_chain_t **parentp, int how);

/* RB-tree macros are provided generically by hammer2_rb.h (included above). */

/* DragonFly's pause(msg, ticks) -> Linux cond_resched + msleep. */
#define pause(msg, ticks) \
	do { (void)(msg); schedule_timeout_uninterruptible((ticks) ?: 1); } while (0)
/*
 * BSD tsleep(channel, prio, label, ticks) channel matches a future
 * wakeup() call; in the absence of an explicit wakeup it just times out.
 * callers use it as a throttle/yield, so a plain interruptible
 * sleep with the requested tick count is functionally equivalent.
 */
/* Statement-expression form so call sites can use tsleep as an rvalue. */
#define tsleep(ch, prio, label, ticks) \
	({ (void)(ch); (void)(prio); (void)(label); \
	   schedule_timeout_uninterruptible((ticks) ?: 1); 0; })

/*
 * BSD tsleep_interlock(9) arms a sleep against a future wakeup on `ch` without
 * losing wakeups raced between the check and the sleep.  The port's tsleep is
 * timeout-based and ignores the channel, so the interlock is a no-op; callers
 * re-check their condition after the (bounded) sleep.  PINTERLOCKED is the BSD
 * tsleep priority flag that pairs with it.
 */
#define tsleep_interlock(ch, flags)	do { (void)(ch); (void)(flags); } while (0)
#ifndef PINTERLOCKED
#define PINTERLOCKED	0
#endif

/* BSD kprintf -> the port's printf shim (kernel pr_* under the hood). */
#ifndef kprintf
#define kprintf	printf
#endif

/* Global debug mask (DragonFly sysctl vfs.hammer2.debug); defined in synchro. */
extern int hammer2_debug;

/* BSD's `hz` is the same as Linux's HZ. */
#ifndef hz
#define hz	HZ
#endif

/* Assert helpers used by chain.c; Linux has no read/write distinction. */
#define hammer2_mtx_assert_ex(p)	WARN_ON(!mutex_is_locked(&(p)->lock))
#define hammer2_mtx_assert_sh(p)	WARN_ON(!mutex_is_locked(&(p)->lock))
#define hammer2_mtx_assert_locked(p)	WARN_ON(!mutex_is_locked(&(p)->lock))

void hammer2_inode_ref(hammer2_inode_t *ip);
void hammer2_inode_drop(hammer2_inode_t *ip);

struct hammer2_trans;
struct hammer2_cluster;
uint64_t hammer2_trans_sub(hammer2_pfs_t *pmp);

int hammer2_get_logical(void);

int hammer2_cluster_check(struct hammer2_cluster *cluster, hammer2_key_t key, int flags);
uint8_t hammer2_cluster_type(const struct hammer2_cluster *cluster);
void hammer2_cluster_unhold(struct hammer2_cluster *cluster);
void hammer2_cluster_rehold(struct hammer2_cluster *cluster);
void hammer2_cluster_bref(const struct hammer2_cluster *cluster,
		hammer2_blockref_t *bref);

/*
 * XOP storage_func entry points.  Each has the signature
 *     void f(hammer2_xop_t *arg, void *scratch, int clindex)
 * matching hammer2_xop_func_t (defined further down).  They are referenced
 * by H2XOPDESCRIPTOR() initializers, which require visible declarations
 * before the macro expansions.  Definitions live in hammer2_xops.c (and
 * a few in hammer2_strategy.c).
 */
union hammer2_xop;
typedef void (*hammer2_xop_func_fwd_t)(union hammer2_xop *, void *, int);

void hammer2_xop_ipcluster(union hammer2_xop *, void *, int);
void hammer2_xop_readdir(union hammer2_xop *, void *, int);
void hammer2_xop_nresolve(union hammer2_xop *, void *, int);
void hammer2_xop_unlink(union hammer2_xop *, void *, int);
void hammer2_xop_nrename(union hammer2_xop *, void *, int);
void hammer2_xop_scanlhc(union hammer2_xop *, void *, int);
void hammer2_xop_scanall(union hammer2_xop *, void *, int);
void hammer2_xop_lookup(union hammer2_xop *, void *, int);
void hammer2_xop_delete(union hammer2_xop *, void *, int);
void hammer2_xop_inode_mkdirent(union hammer2_xop *, void *, int);
void hammer2_xop_inode_create(union hammer2_xop *, void *, int);
void hammer2_xop_inode_create_det(union hammer2_xop *, void *, int);
void hammer2_xop_inode_create_ins(union hammer2_xop *, void *, int);
void hammer2_xop_inode_destroy(union hammer2_xop *, void *, int);
void hammer2_xop_inode_chain_sync(union hammer2_xop *, void *, int);
void hammer2_xop_inode_unlinkall(union hammer2_xop *, void *, int);
void hammer2_xop_inode_connect(union hammer2_xop *, void *, int);
void hammer2_xop_inode_flush(union hammer2_xop *, void *, int);
void hammer2_xop_strategy_read(union hammer2_xop *, void *, int);
void hammer2_xop_strategy_write(union hammer2_xop *, void *, int);
void hammer2_xop_bmap(union hammer2_xop *, void *, int);

/* The xop zone allocator handle, defined in hammer2_vfsops.c. */
extern uma_zone_t hammer2_zone_xops;
extern uma_zone_t hammer2_zone_rbuf;
extern uma_zone_t hammer2_zone_wbuf;
extern uma_zone_t hammer2_zone_inode;
extern uma_zone_t hammer2_zone_xops;
extern struct hammer2_xop_desc hammer2_strategy_read_desc;
extern struct hammer2_xop_desc hammer2_strategy_write_desc;

/*
 * Cap the dynamic calculation for the maximum number of dirty
 * chains and dirty inodes allowed.
 */
#define HAMMER2_LIMIT_DIRTY_CHAINS    (1024*1024)
#define HAMMER2_LIMIT_DIRTY_INODES    (65536)

#define HAMMER2_IOHASH_SIZE        32768
#define HAMMER2_IOHASH_MASK        (HAMMER2_IOHASH_SIZE - 1)

#define HAMMER2_INUMHASH_SIZE        32768
#define HAMMER2_INUMHASH_MASK        (HAMMER2_INUMHASH_SIZE - 1)

/*
 * HAMMER2 dio - Management structure wrapping system buffer cache.
 */
struct hammer2_io {
    struct hammer2_io    *next;
    hammer2_mtx_t        lock;
    hammer2_dev_t        *hmp;
    struct block_device  *bdev;
    struct buffer_head   *bh;       /* unused on Linux; kept for ABI parity */
    char                 *data;     /* private 64K buffer (PAGE_SIZE-chunked I/O) */
    uint32_t             refs;
    u64                  dbase;      /* offset of devvp within volumes */
    u64                  pbase;
    int                  psize;
    int                  act;        /* activity */
    int                  btype;
    int                  ticks;
    int                  error;
    u64                  dedup_valid;    /* valid for dedup operation */
    u64                  dedup_alloc;    /* allocated / de-dupable */
};

struct hammer2_io_hash {
    hammer2_spin_t       spin;
    struct hammer2_io    *base;
};

typedef struct hammer2_io_hash    hammer2_io_hash_t;

#define HAMMER2_DIO_GOOD    0x40000000U    /* dio->bh is stable */
#define HAMMER2_DIO_DIRTY   0x10000000U    /* flush last drop */
#define HAMMER2_DIO_FLUSH   0x08000000U    /* immediate flush */
#define HAMMER2_DIO_MASK    0x00FFFFFFU

struct hammer2_inum_hash {
    hammer2_spin_t       spin;
    struct hammer2_inode *base;
};

typedef struct hammer2_inum_hash hammer2_inum_hash_t;

/*
 * The chain structure tracks a portion of the media topology.
 */
struct hammer2_reptrack {
    struct hammer2_reptrack *next;
    hammer2_chain_t        *chain;
    hammer2_spin_t         spin;
};

typedef struct hammer2_reptrack hammer2_reptrack_t;

/*
 * Core topology for chain (embedded in chain).
 */
struct hammer2_chain_core {
    hammer2_reptrack_t     *reptrack;
    hammer2_chain_tree_t   rbtree;      /* sub-chains */
    hammer2_spin_t         spin;
    int                    live_zero;   /* blockref array opt */
    unsigned int           live_count;  /* live chains in tree */
    unsigned int           chain_count; /* live + deleted chains under core */
    int                    generation;  /* generation number */
};

typedef struct hammer2_chain_core hammer2_chain_core_t;

/*
 * Primary chain structure.
 */
struct hammer2_chain {
    struct rb_node         rbnode;      /* live chain(s) */
    hammer2_mtx_t          lock;
    hammer2_mtx_t          diolk;       /* xop focus interlock */
    hammer2_lk_t           inp_lock;
    hammer2_lkc_t          inp_cv;
    hammer2_chain_core_t   core;
    hammer2_blockref_t     bref;
    hammer2_dev_t          *hmp;
    hammer2_pfs_t          *pmp;        /* A PFS or super-root (spmp) */
    hammer2_chain_t        *parent;
    hammer2_io_t           *dio;        /* physical data buffer */
    hammer2_media_data_t   *data;       /* data pointer shortcut */
    unsigned int           refs;
    unsigned int           lockcnt;
    unsigned int           flags;       /* for HAMMER2_CHAIN_xxx */
    unsigned int           bytes;       /* physical data size */
    int                    error;       /* on-lock data error state */
    int                    cache_index; /* heur speeds up lookup */
};

/*
 * Special notes on flags.
 */
#define HAMMER2_CHAIN_MODIFIED      0x00000001
#define HAMMER2_CHAIN_ALLOCATED     0x00000002
#define HAMMER2_CHAIN_DESTROY       0x00000004
#define HAMMER2_CHAIN_DEDUPABLE     0x00000008
#define HAMMER2_CHAIN_DELETED       0x00000010
#define HAMMER2_CHAIN_INITIAL       0x00000020
#define HAMMER2_CHAIN_UPDATE        0x00000040
#define HAMMER2_CHAIN_NOTTESTED     0x00000080
#define HAMMER2_CHAIN_TESTEDGOOD    0x00000100
#define HAMMER2_CHAIN_ONFLUSH       0x00000200
#define HAMMER2_CHAIN_VOLUMESYNC    0x00000800
#define HAMMER2_CHAIN_COUNTEDBREFS  0x00002000
#define HAMMER2_CHAIN_ONRBTREE      0x00004000
#define HAMMER2_CHAIN_RELEASE       0x00020000
#define HAMMER2_CHAIN_BLKMAPPED     0x00040000
#define HAMMER2_CHAIN_BLKMAPUPD     0x00080000
#define HAMMER2_CHAIN_IOINPROG      0x00100000
#define HAMMER2_CHAIN_IOSIGNAL      0x00200000
#define HAMMER2_CHAIN_PFSBOUNDARY   0x00400000
#define HAMMER2_CHAIN_HINT_LEAF_COUNT   0x00800000

#define HAMMER2_CHAIN_FLUSH_MASK    (HAMMER2_CHAIN_MODIFIED | \
                     HAMMER2_CHAIN_UPDATE | \
                     HAMMER2_CHAIN_ONFLUSH | \
                     HAMMER2_CHAIN_DESTROY)

/*
 * HAMMER2 error codes.
 */
/*
 * Consecutive hammer2_vfs_sync_pmp() passes that each failed to write out at
 * least one inode before the sync gives up instead of restarting forever.
 * Reached in milliseconds when the device is gone; far more retrying than any
 * transient failure needs.
 */
#define HAMMER2_SYNC_IOFAIL_MAX	64

#define HAMMER2_ERROR_EIO       0x00000001
#define HAMMER2_ERROR_CHECK     0x00000002
#define HAMMER2_ERROR_BADBREF   0x00000010
#define HAMMER2_ERROR_ENOSPC    0x00000020
#define HAMMER2_ERROR_ENOENT    0x00000040
#define HAMMER2_ERROR_ENOTEMPTY 0x00000080
#define HAMMER2_ERROR_EAGAIN    0x00000100
#define HAMMER2_ERROR_ENOTDIR   0x00000200
#define HAMMER2_ERROR_EISDIR    0x00000400
#define HAMMER2_ERROR_ABORTED   0x00001000
#define HAMMER2_ERROR_EOF       0x00002000
#define HAMMER2_ERROR_EINVAL    0x00004000
#define HAMMER2_ERROR_EEXIST    0x00008000
#define HAMMER2_ERROR_EINPROGRESS   0x00000800
#define HAMMER2_ERROR_ESRCH         0x00020000
#define HAMMER2_ERROR_ETIMEDOUT     0x00040000
#define HAMMER2_ERROR_EOPNOTSUPP    0x10000000

/*
 * Flags passed to hammer2_chain_lookup() and hammer2_chain_next().
 */
#define HAMMER2_LOOKUP_NODATA       0x00000002
#define HAMMER2_LOOKUP_NODIRECT     0x00000004
#define HAMMER2_LOOKUP_SHARED       0x00000100
#define HAMMER2_LOOKUP_MATCHIND     0x00000200
#define HAMMER2_LOOKUP_ALWAYS       0x00000800

/*
 * Flags passed to hammer2_chain_modify().
 */
#define HAMMER2_MODIFY_OPTDATA      0x00000002

/*
 * Flags passed to hammer2_chain_lock().
 */
#define HAMMER2_RESOLVE_NEVER       1
#define HAMMER2_RESOLVE_MAYBE       2
#define HAMMER2_RESOLVE_ALWAYS      3
#define HAMMER2_RESOLVE_MASK        0x0F

#define HAMMER2_RESOLVE_SHARED      0x10
#define HAMMER2_RESOLVE_LOCKAGAIN   0x20
#define HAMMER2_RESOLVE_NONBLOCK    0x80

/*
 * Flags passed to hammer2_chain_delete().
 */
#define HAMMER2_DELETE_PERMANENT    0x0001

/*
 * Flags passed to hammer2_chain_insert() or hammer2_chain_rename()
 * or hammer2_chain_create().
 */
#define HAMMER2_INSERT_PFSROOT      0x0004
#define HAMMER2_INSERT_SAMEPARENT   0x0008

/*
 * HAMMER2 cluster.
 */
#define HAMMER2_XOPFIFO        16
/*
 * Cap for HAMMER2_XOP_FIFO_BOUND XOPs: grow the collect FIFO up to this many
 * entries, then stop feeding and let the caller resume from its cursor.
 * Bounding at HAMMER2_XOPFIFO alone is correct but costs an XOP start (and a
 * btree seek) every 16 entries; 1024 keeps readdir near-linear without
 * buffering the whole directory.
 */
#define HAMMER2_XOPFIFO_BOUND  1024

#define HAMMER2_MAXCLUSTER     8
#define HAMMER2_XOPMASK_VOP    ((uint32_t)0x80000000U)

#define HAMMER2_XOPMASK_ALLDONE    (HAMMER2_XOPMASK_VOP)

struct hammer2_cluster_item {
    hammer2_chain_t      *chain;
    uint32_t             flags;
    int                  error;
};

typedef struct hammer2_cluster_item hammer2_cluster_item_t;

#define HAMMER2_CITEM_INVALID 0x00000001
#define HAMMER2_CITEM_FEMOD   0x00000002
#define HAMMER2_CITEM_NULL    0x00000004

struct hammer2_cluster {
    hammer2_cluster_item_t array[HAMMER2_MAXCLUSTER];
    hammer2_pfs_t          *pmp;
    hammer2_chain_t        *focus;
    int                    nchains;
    int                    error;
};

typedef struct hammer2_cluster    hammer2_cluster_t;

/*
 * HAMMER2 inode.
 */
struct hammer2_inode {
    struct hammer2_inode     *next;
    struct list_head         qentry;
    struct list_head         ientry;
    hammer2_depend_t         *depend;
    hammer2_depend_t         depend_static;
    hammer2_mtx_t            lock;
    hammer2_mtx_t            truncate_lock;
    /*
     * Serializes the read-modify-write of a 64KiB logical block in
     * hammer2_writeback_folio().  Writing one folio of a larger block means
     * read the whole block, overlay the folio, write the whole block back 
     * and two threads doing that concurrently (writeback kworker plus the
     * inline sync) interleave: the later write is built on a stale read and
     * clobbers the earlier one, while the blockref check code comes from
     * whichever setcheck ran last.  Media then holds a SPLICE of two versions
     * with a check code matching neither observed as pages 0-5 of one file
     * followed by pages 6-15 of the block's previous occupant.
     */
    hammer2_mtx_t            rmw_lock;
    hammer2_spin_t           cluster_spin;
    hammer2_cluster_t        cluster;
    hammer2_cluster_item_t   ccache[HAMMER2_MAXCLUSTER];
    int                      ccache_nchains;
    hammer2_inode_meta_t     meta;
    hammer2_pfs_t            *pmp;
    u64                      osize;
    struct inode             *vp;
    unsigned int             refs;
    unsigned int             flags;
    uint8_t                  comp_heuristic;
    int                      ipdep_idx;
    int                      in_seek;
    char                     clusterw[64];  /* BSD per-inode cluster ctx (stub) */
};

/*
 * Inode flags.
 */
#define HAMMER2_INODE_MODIFIED      0x0001
#define HAMMER2_INODE_ONHASH        0x0008
#define HAMMER2_INODE_RESIZED       0x0010
#define HAMMER2_INODE_ISUNLINKED    0x0040
#define HAMMER2_INODE_SIDEQ         0x0100
#define HAMMER2_INODE_NOSIDEQ       0x0200
#define HAMMER2_INODE_DIRTYDATA     0x0400
#define HAMMER2_INODE_SYNCQ         0x0800
#define HAMMER2_INODE_DELETING      0x1000
#define HAMMER2_INODE_CREATING      0x2000
#define HAMMER2_INODE_SYNCQ_WAKEUP  0x4000
#define HAMMER2_INODE_SYNCQ_PASS2   0x8000

/*
 * Transaction management.
 */
struct hammer2_trans {
    uint32_t             flags;
};

typedef struct hammer2_trans hammer2_trans_t;

#define HAMMER2_TRANS_ISFLUSH       0x80000000
#define HAMMER2_TRANS_BUFCACHE      0x40000000
#define HAMMER2_TRANS_SIDEQ         0x20000000
#define HAMMER2_TRANS_WAITING       0x08000000
#define HAMMER2_TRANS_RESCAN        0x04000000
#define HAMMER2_TRANS_MASK          0x00FFFFFF

#define HAMMER2_FREEMAP_HEUR_NRADIX 4
#define HAMMER2_FREEMAP_HEUR_TYPES  8
#define HAMMER2_FREEMAP_HEUR_SIZE   (HAMMER2_FREEMAP_HEUR_NRADIX * \
                     HAMMER2_FREEMAP_HEUR_TYPES)

#define HAMMER2_DEDUP_HEUR_SIZE     (65536 * 4)
#define HAMMER2_DEDUP_HEUR_MASK     (HAMMER2_DEDUP_HEUR_SIZE - 1)

#define HAMMER2_FLUSH_TOP           0x0001
#define HAMMER2_FLUSH_ALL           0x0002
#define HAMMER2_FLUSH_INODE_STOP    0x0004
#define HAMMER2_FLUSH_FSSYNC        0x0008

/*
 * Support structure for dedup heuristic.
 */
struct hammer2_dedup {
    u64                  data_off;
    u64                  data_crc;
    uint32_t             ticks;
    uint32_t             saved_error;
};

typedef struct hammer2_dedup hammer2_dedup_t;

/*
 * HAMMER2 XOP - container for VOP/XOP operation.
 */
typedef void (*hammer2_xop_func_t)(union hammer2_xop *, void *, int);

struct hammer2_xop_desc {
    hammer2_xop_func_t   storage_func;
    const char           *id;
};

typedef struct hammer2_xop_desc hammer2_xop_desc_t;

struct hammer2_xop_fifo {
    hammer2_chain_t      **array;
    int                  *errors;
    int                  ri;
    int                  wi;
    int                  flags;
};

typedef struct hammer2_xop_fifo hammer2_xop_fifo_t;

struct hammer2_xop_head {
    u64                  mtid;
    hammer2_xop_fifo_t   collect[HAMMER2_MAXCLUSTER];
    hammer2_cluster_t    cluster;
    hammer2_xop_desc_t   *desc;
    hammer2_inode_t      *ip1;
    hammer2_inode_t      *ip2;
    hammer2_inode_t      *ip3;
    hammer2_inode_t      *ip4;
    hammer2_io_t         *focus_dio;
    u64                  collect_key;
    uint32_t             run_mask;
    uint32_t             chk_mask;
    int                  flags;
    int                  fifo_size;
    int                  error;
    char                 *name1;
    size_t               name1_len;
    char                 *name2;
    size_t               name2_len;
    void                 *scratch;
};

typedef struct hammer2_xop_head hammer2_xop_head_t;

#define fifo_mask(xop_head)    ((xop_head)->fifo_size - 1)

struct hammer2_xop_ipcluster {
    hammer2_xop_head_t   head;
};

struct hammer2_xop_readdir {
    hammer2_xop_head_t   head;
    u64                  lkey;
};

struct hammer2_xop_nresolve {
    hammer2_xop_head_t   head;
};

struct hammer2_xop_unlink {
    hammer2_xop_head_t   head;
    int                  isdir;
    int                  dopermanent;
};

#define H2DOPERM_PERMANENT    0x01
#define H2DOPERM_FORCE        0x02
#define H2DOPERM_IGNINO       0x04

struct hammer2_xop_nrename {
    hammer2_xop_head_t   head;
    u64                  lhc;
    int                  ip_key;
};

struct hammer2_xop_scanlhc {
    hammer2_xop_head_t   head;
    u64                  lhc;
};

struct hammer2_xop_scanall {
    hammer2_xop_head_t   head;
    u64                  key_beg;
    u64                  key_end;
    int                  resolve_flags;
    int                  lookup_flags;
};

struct hammer2_xop_lookup {
    hammer2_xop_head_t   head;
    u64                  lhc;
};

struct hammer2_xop_mkdirent {
    hammer2_xop_head_t   head;
    hammer2_dirent_head_t dirent;
    u64                  lhc;
};

struct hammer2_xop_create {
    hammer2_xop_head_t   head;
    hammer2_inode_meta_t meta;
    u64                  lhc;
    int                  flags;
};

struct hammer2_xop_destroy {
    hammer2_xop_head_t   head;
};

struct hammer2_xop_fsync {
    hammer2_xop_head_t   head;
    hammer2_inode_meta_t meta;
    u64                  osize;
    unsigned int         ipflags;
    int                  clear_directdata;
};

struct hammer2_xop_unlinkall {
    hammer2_xop_head_t   head;
    u64                  key_beg;
    u64                  key_end;
};

struct hammer2_xop_connect {
    hammer2_xop_head_t   head;
    u64                  lhc;
};

struct hammer2_xop_flush {
    hammer2_xop_head_t   head;
};

struct hammer2_xop_strategy {
    hammer2_xop_head_t   head;
    u64                  lbase;
    struct buffer_head   *bh;       /* future Linux-native I/O target */
    struct buf           *bp;       /* legacy BSD-style buf, used by strategy.c */
};

struct hammer2_xop_bmap {
    hammer2_xop_head_t   head;
    sector_t             lbn;
    int                  runp;
    int                  runb;
    u64                  offset;
};

typedef struct hammer2_xop_ipcluster hammer2_xop_ipcluster_t;
typedef struct hammer2_xop_readdir hammer2_xop_readdir_t;
typedef struct hammer2_xop_nresolve hammer2_xop_nresolve_t;
typedef struct hammer2_xop_unlink hammer2_xop_unlink_t;
typedef struct hammer2_xop_nrename hammer2_xop_nrename_t;
typedef struct hammer2_xop_scanlhc hammer2_xop_scanlhc_t;
typedef struct hammer2_xop_scanall hammer2_xop_scanall_t;
typedef struct hammer2_xop_lookup hammer2_xop_lookup_t;
typedef struct hammer2_xop_mkdirent hammer2_xop_mkdirent_t;
typedef struct hammer2_xop_create hammer2_xop_create_t;
typedef struct hammer2_xop_destroy hammer2_xop_destroy_t;
typedef struct hammer2_xop_fsync hammer2_xop_fsync_t;
typedef struct hammer2_xop_unlinkall hammer2_xop_unlinkall_t;
typedef struct hammer2_xop_connect hammer2_xop_connect_t;
typedef struct hammer2_xop_flush hammer2_xop_flush_t;
typedef struct hammer2_xop_strategy hammer2_xop_strategy_t;
typedef struct hammer2_xop_bmap hammer2_xop_bmap_t;

union hammer2_xop {
    hammer2_xop_head_t       head;
    hammer2_xop_ipcluster_t  xop_ipcluster;
    hammer2_xop_readdir_t    xop_readdir;
    hammer2_xop_nresolve_t   xop_nresolve;
    hammer2_xop_unlink_t     xop_unlink;
    hammer2_xop_nrename_t    xop_nrename;
    hammer2_xop_scanlhc_t    xop_scanlhc;
    hammer2_xop_scanall_t    xop_scanall;
    hammer2_xop_lookup_t     xop_lookup;
    hammer2_xop_mkdirent_t   xop_mkdirent;
    hammer2_xop_create_t     xop_create;
    hammer2_xop_destroy_t    xop_destroy;
    hammer2_xop_fsync_t      xop_fsync;
    hammer2_xop_unlinkall_t  xop_unlinkall;
    hammer2_xop_connect_t    xop_connect;
    hammer2_xop_flush_t      xop_flush;
    hammer2_xop_strategy_t   xop_strategy;
    hammer2_xop_bmap_t       xop_bmap;
};

/*
 * Flags to hammer2_xop_collect().
 */
#define HAMMER2_XOP_COLLECT_NOWAIT    0x00000001
#define HAMMER2_XOP_COLLECT_WAITALL   0x00000002

/*
 * Flags to hammer2_xop_alloc().
 */
#define HAMMER2_XOP_MODIFYING     0x00000001
#define HAMMER2_XOP_STRATEGY      0x00000002
#define HAMMER2_XOP_INODE_STOP    0x00000004
#define HAMMER2_XOP_VOLHDR        0x00000008
#define HAMMER2_XOP_FSSYNC        0x00000010
/*
 * Do not block acquiring the per-inode XOP interlock; abort the XOP instead.
 * Used by background writeback, which must not sit in hammer2_xop_testset_ipdep
 * while it holds folios marked under-writeback (see hammer2_writepages).
 */
#define HAMMER2_XOP_NONBLOCK      0x00000020
/* Set by xop_start when NONBLOCK could not acquire the interlock. */
#define HAMMER2_XOP_ABORTED       0x00000040
/*
 * Do not grow the collect FIFO without bound; stop feeding when it is full.
 * Only safe for XOPs whose caller can resume from a cursor (readdir), and
 * required there: XOPs run inline in this port, so hammer2_xop_feed() cannot
 * sleep for flow control and instead doubles the FIFO which makes every
 * getdents() buffer the whole remaining directory and readdir O(n^2).
 */
#define HAMMER2_XOP_FIFO_BOUND    0x00000080

/*
 * Device vnode management structure (Linux uses block_device).
 */
struct hammer2_devvp {
    struct list_head         entry;
    struct block_device      *bdev;       /* derived from bdev_file when open */
    struct file              *bdev_file;  /* bdev_file_open_by_path() result */
    char                     *path;
    int                      open;
};

typedef struct hammer2_devvp hammer2_devvp_t;

/*
 * Volume management structure.
 */
struct hammer2_volume {
    hammer2_devvp_t          *dev;
    u64                      offset;
    u64                      size;
    int                      id;
};

typedef struct hammer2_volume hammer2_volume_t;

/*
 * I/O stat structure.
 */
struct hammer2_iostat_unit {
    unsigned long            count;
    unsigned long            bytes;
};

typedef struct hammer2_iostat_unit hammer2_iostat_unit_t;

struct hammer2_iostat {
    hammer2_iostat_unit_t    inode;
    hammer2_iostat_unit_t    indirect;
    hammer2_iostat_unit_t    data;
    hammer2_iostat_unit_t    dirent;
    hammer2_iostat_unit_t    freemap_node;
    hammer2_iostat_unit_t    freemap_leaf;
    hammer2_iostat_unit_t    freemap;
    hammer2_iostat_unit_t    volume;
};

typedef struct hammer2_iostat hammer2_iostat_t;

/*
 * DMSG cluster messaging protocol + kdmsg iocom types (hammer2_dmsg.h is the
 * port-adapted copy of DragonFly's <sys/dmsg.h>).  Included here so that
 * struct hammer2_dev can embed a kdmsg_iocom_t for the network cluster path.
 */
#include "hammer2_dmsg.h"

/*
 * Global (per partition) management structure.
 */
struct hammer2_dev {
    struct list_head         mntentry;
    hammer2_devvp_list_t     devvp_list;
    hammer2_io_hash_t        iohash[HAMMER2_IOHASH_SIZE];
    hammer2_mtx_t            iohash_lock;
    hammer2_pfs_t            *spmp;
    struct malloc_type       *mmsg;	/* dmsg malloc tag (NULL on Linux) */
    kdmsg_iocom_t            iocom;	/* volume-level dmsg interface */
    struct block_device      *bdev;
    struct block_device      *devvp;       /* root volume bdev (set during mount) */
    hammer2_chain_t          vchain;
    hammer2_chain_t          fchain;
    hammer2_volume_data_t    voldata;
    hammer2_volume_data_t    volsync;
    hammer2_volume_t         volumes[HAMMER2_MAX_VOLUMES];
    u64                      total_size;
    uint32_t                 hflags;
    int                      rdonly;
    int                      mount_count;
    int                      nvolumes;
    int                      volhdrno;
    int                      iofree_count;
    int                      io_iterator;
    hammer2_lk_t             vollk;
    hammer2_lk_t             bulklk;
    hammer2_lk_t             bflk;
    int                      freemap_relaxed;
    /*
     * Bumped on every failed device I/O.  hammer2_vfs_sync_pmp() samples it
     * across a sync pass to tell a transient failure (retry) from a device
     * that is simply gone (stop retrying) HAMMER2_SYNC_IOFAIL_MAX.
     */
    int                      iofail_count;
    u64                      free_reserved;
    u64                      heur_freemap[HAMMER2_FREEMAP_HEUR_SIZE];
    hammer2_dedup_t          heur_dedup[HAMMER2_DEDUP_HEUR_SIZE];
    hammer2_iostat_t         iostat_read;
    hammer2_iostat_t         iostat_write;
};

/*
 * HAMMER2 management thread (Linux port).
 *
 * DragonFly backs these with lwkt threads; the Linux port backs them with
 * kthreads plus a per-thread waitqueue for the flag-change handshake.  The
 * xopq field from the upstream struct is omitted because the port runs XOPs
 * synchronously rather than dispatching them to worker threads.  Presently
 * only the PFS synchronization thread (hammer2_synchro.c) uses this API, and
 * it is not auto-started by the mount path on this port.
 */
struct hammer2_thread {
	struct hammer2_pfs	*pmp;
	struct hammer2_dev	*hmp;
	thread_t		td;		/* non-NULL while running */
	struct task_struct	*task;		/* backing Linux kthread */
	wait_queue_head_t	wait;		/* flag-change waitqueue */
	uint32_t		flags;
	int			clindex;	/* cluster element index */
	int			repidx;
	char			*scratch;	/* MAXPHYS */
	void			(*func)(void *arg);
};

typedef struct hammer2_thread hammer2_thread_t;

#define HAMMER2_THREAD_UNMOUNTING	0x0001	/* unmount request */
#define HAMMER2_THREAD_DEV		0x0002	/* related to dev, not pfs */
#define HAMMER2_THREAD_WAITING		0x0004	/* thread in idle tsleep */
#define HAMMER2_THREAD_REMASTER		0x0008	/* remaster request */
#define HAMMER2_THREAD_STOP		0x0010	/* exit request */
#define HAMMER2_THREAD_FREEZE		0x0020	/* force idle */
#define HAMMER2_THREAD_FROZEN		0x0040	/* thread is frozen */
#define HAMMER2_THREAD_XOPQ		0x0080	/* work pending */
#define HAMMER2_THREAD_STOPPED		0x0100	/* thread has stopped */
#define HAMMER2_THREAD_UNFREEZE		0x0200

#define HAMMER2_THREAD_WAKEUP_MASK	(HAMMER2_THREAD_UNMOUNTING |	\
					 HAMMER2_THREAD_REMASTER |	\
					 HAMMER2_THREAD_STOP |		\
					 HAMMER2_THREAD_FREEZE |	\
					 HAMMER2_THREAD_XOPQ)

void hammer2_thr_signal(hammer2_thread_t *thr, uint32_t flags);
void hammer2_thr_signal2(hammer2_thread_t *thr,
			uint32_t posflags, uint32_t negflags);
void hammer2_thr_wait(hammer2_thread_t *thr, uint32_t flags);
void hammer2_thr_wait_neg(hammer2_thread_t *thr, uint32_t flags);
int hammer2_thr_wait_any(hammer2_thread_t *thr, uint32_t flags, int timo);
void hammer2_thr_create(hammer2_thread_t *thr, hammer2_pfs_t *pmp,
			hammer2_dev_t *hmp, const char *id,
			int clindex, int repidx, void (*func)(void *arg));
void hammer2_thr_delete(hammer2_thread_t *thr);
void hammer2_thr_remaster(hammer2_thread_t *thr);
void hammer2_thr_freeze_async(hammer2_thread_t *thr);
void hammer2_thr_freeze(hammer2_thread_t *thr);
void hammer2_thr_unfreeze(hammer2_thread_t *thr);
int hammer2_thr_break(hammer2_thread_t *thr);
void hammer2_primary_sync_thread(void *arg);

/*
 * Per-cluster management structure.
 */
#define HAMMER2_IHASH_SIZE    32

struct hammer2_pfs {
    struct list_head         mntentry;
    hammer2_ipdep_list_t     *ipdep_lists;
    hammer2_spin_t           blockset_spin;
    hammer2_spin_t           list_spin;
    hammer2_lk_t             xop_lock[HAMMER2_IHASH_SIZE];
    hammer2_lkc_t            xop_cv[HAMMER2_IHASH_SIZE];
    hammer2_lk_t             trans_lock;
    hammer2_lkc_t            trans_cv;
    struct super_block       *sb;
    struct mount             *mp;       /* BSD struct-mount shim (sibling of sb) */
    struct uuid              pfs_clid;
    hammer2_trans_t          trans;
    hammer2_inode_t          *iroot;
    hammer2_thread_t         sync_thrs[HAMMER2_MAXCLUSTER];
    hammer2_dev_t            *spmp_hmp;
    hammer2_dev_t            *force_local;
    hammer2_dev_t            *pfs_hmps[HAMMER2_MAXCLUSTER];
    char                     *pfs_names[HAMMER2_MAXCLUSTER];
    uint8_t                  pfs_types[HAMMER2_MAXCLUSTER];
    hammer2_blockset_t       pfs_iroot_blocksets[HAMMER2_MAXCLUSTER];
    int                      flags;
    int                      rdonly;
    int                      free_ticks;
    unsigned long            ipdep_mask;
    u64                      free_reserved;
    u64                      free_nominal;
    u64                      modify_tid;
    u64                      inode_tid;
    hammer2_inoq_head_t      syncq;
    hammer2_depq_head_t      depq;
    long                     sideq_count;
    hammer2_inum_hash_t      inumhash[HAMMER2_INUMHASH_SIZE];
};

#define HAMMER2_PMPF_SPMP     0x00000001
#define HAMMER2_PMPF_EMERG    0x00000002
#define HAMMER2_PMPF_WAITING  0x10000000

#define HAMMER2_CHECK_NULL    0x00000001

#define SBTOPMT(sb)    ((hammer2_pfs_t *)(sb)->s_fs_info)

extern struct hammer2_pfslist hammer2_pfslist;

extern hammer2_lk_t hammer2_mntlk;

extern int hammer2_cluster_meta_read;
extern int hammer2_cluster_data_read;
extern int hammer2_cluster_write;
extern int hammer2_fsync_durable;
extern int hammer2_dedup_enable;
extern int hammer2_count_inode_allocated;
extern int hammer2_count_chain_allocated;
extern int hammer2_count_chain_modified;
extern int hammer2_count_dio_allocated;
extern int hammer2_dio_limit;
extern int hammer2_bulkfree_tps;
extern int hammer2_limit_scan_depth;
extern int hammer2_limit_saved_chains;
extern int hammer2_always_compress;

/* CRC32 functions - use Linux kernel's CRC32 */
#include <linux/crc32.h>
/*
 * HAMMER2 uses the iSCSI CRC (CRC32C / Castagnoli), NOT the Ethernet CRC32.
 * DragonFly:
 *   iscsi_crc32(buf, size)        = ~calculate_crc32c(-1, buf, size)
 *   iscsi_crc32_ext(buf,size,ocrc)= ~calculate_crc32c(~ocrc, buf, size)
 * Linux crc32c() is the raw Castagnoli update (no pre/post inversion), so the
 * inversions are applied here.  Using crc32() (Ethernet poly) here makes every
 * on-disk CRC mismatch.
 */
#include <linux/crc32.h>
#define hammer2_icrc32(buf, size)	(~crc32c(~0U, (buf), (size)))
#define hammer2_icrc32c(buf, size, crc)	(~crc32c(~(uint32_t)(crc), (buf), (size)))

/* DMSG (kern_dmsg.c) names the same iSCSI CRC32C this way. */
#define iscsi_crc32(buf, size)		hammer2_icrc32((buf), (size))
#define iscsi_crc32_ext(buf, size, ocrc) hammer2_icrc32c((buf), (size), (ocrc))

/* Function declarations */
void *hammer2_xop_alloc(hammer2_inode_t *, int);
void hammer2_xop_setname(hammer2_xop_head_t *, const char *, size_t);
void hammer2_xop_setname2(hammer2_xop_head_t *, const char *, size_t);
size_t hammer2_xop_setname_inum(hammer2_xop_head_t *, u64);
void hammer2_xop_setip2(hammer2_xop_head_t *, hammer2_inode_t *);
void hammer2_xop_setip3(hammer2_xop_head_t *, hammer2_inode_t *);
void hammer2_xop_setip4(hammer2_xop_head_t *, hammer2_inode_t *);
void hammer2_xop_start(hammer2_xop_head_t *, hammer2_xop_desc_t *);
void hammer2_xop_start_except(hammer2_xop_head_t *, hammer2_xop_desc_t *,
			int notidx);

/* DMSG cluster interface (hammer2_iocom.c). */
void hammer2_iocom_init(hammer2_dev_t *hmp);
void hammer2_iocom_uninit(hammer2_dev_t *hmp);
void hammer2_cluster_reconnect(hammer2_dev_t *hmp, struct file *fp);
void hammer2_volconf_update(hammer2_dev_t *hmp, int index);
void hammer2_xop_retire(hammer2_xop_head_t *, uint32_t);
int hammer2_xop_feed(hammer2_xop_head_t *, hammer2_chain_t *, int, int);
int hammer2_xop_collect(hammer2_xop_head_t *, int);

/* Inline helper functions */
static inline int
hammer2_error_to_errno(int error)
{
    if (error == 0)
        return 0;
    if (error & HAMMER2_ERROR_EIO)
        return EIO;
    if (error & HAMMER2_ERROR_CHECK)
        return EDOM;
    if (error & HAMMER2_ERROR_BADBREF)
        return EIO;
    if (error & HAMMER2_ERROR_ENOSPC)
        return ENOSPC;
    if (error & HAMMER2_ERROR_ENOENT)
        return ENOENT;
    if (error & HAMMER2_ERROR_ENOTEMPTY)
        return ENOTEMPTY;
    if (error & HAMMER2_ERROR_EAGAIN)
        return EAGAIN;
    if (error & HAMMER2_ERROR_ENOTDIR)
        return ENOTDIR;
    if (error & HAMMER2_ERROR_EISDIR)
        return EISDIR;
    if (error & HAMMER2_ERROR_ABORTED)
        return EINTR;
    if (error & HAMMER2_ERROR_EINVAL)
        return EINVAL;
    if (error & HAMMER2_ERROR_EEXIST)
        return EEXIST;
    if (error & HAMMER2_ERROR_EOPNOTSUPP)
        return EOPNOTSUPP;
    return EDOM;
}

static inline int
hammer2_errno_to_error(int error)
{
    switch (error) {
    case 0: return 0;
    case EIO: return HAMMER2_ERROR_EIO;
    case EDOM: return HAMMER2_ERROR_CHECK;
    case ENOSPC: return HAMMER2_ERROR_ENOSPC;
    case ENOENT: return HAMMER2_ERROR_ENOENT;
    case ENOTEMPTY: return HAMMER2_ERROR_ENOTEMPTY;
    case EAGAIN: return HAMMER2_ERROR_EAGAIN;
    case ENOTDIR: return HAMMER2_ERROR_ENOTDIR;
    case EISDIR: return HAMMER2_ERROR_EISDIR;
    case EINTR: return HAMMER2_ERROR_ABORTED;
    case EINVAL: return HAMMER2_ERROR_EINVAL;
    case EEXIST: return HAMMER2_ERROR_EEXIST;
    case EOPNOTSUPP: return HAMMER2_ERROR_EOPNOTSUPP;
    default: return HAMMER2_ERROR_EINVAL;
    }
}

static inline const void *
hammer2_xop_gdata(hammer2_xop_head_t *xop)
{
    hammer2_chain_t *focus = xop->cluster.focus;
    const void *data = focus->data;

    if (focus->dio) {
        hammer2_mtx_lock(&focus->diolk);
        if ((xop->focus_dio = focus->dio) != NULL)
            atomic_add(1, (atomic_t *)&xop->focus_dio->refs);
        data = focus->data;
        hammer2_mtx_unlock(&focus->diolk);
    }

    return data;
}

static inline void
hammer2_xop_pdata(hammer2_xop_head_t *xop)
{
    if (xop->focus_dio)
        hammer2_io_putblk(&xop->focus_dio);
}

static inline void
hammer2_assert_cluster(const hammer2_cluster_t *cluster)
{
    /* Currently a valid cluster can only have 1 nchains */
    WARN_ON(cluster->nchains != 1);
}

static inline void
hammer2_assert_inode_meta(const hammer2_inode_t *ip)
{
    WARN_ON(!ip);
    WARN_ON(!ip->meta.type);
}

/* hammer2_ondisk.c symbols */
int  hammer2_open_devvp(void *mp, const hammer2_devvp_list_t *devvpl);
int  hammer2_close_devvp(const hammer2_devvp_list_t *devvpl);
int  hammer2_init_devvp(const void *mp, const char *blkdevs,
		hammer2_devvp_list_t *devvpl);
void hammer2_cleanup_devvp(hammer2_devvp_list_t *devvpl);
int  hammer2_init_volumes(const hammer2_devvp_list_t *devvpl,
		hammer2_volume_t *volumes, hammer2_volume_data_t *rootvoldata,
		int *rootvolzone, struct block_device **rootvoldevvp);
int  hammer2_access_devvp(struct block_device *bdev, int rdonly);
int  hammer2_getw_devvp(struct block_device *bdev);
int  hammer2_putw_devvp(struct block_device *bdev);
int  hammer2_getnewfsid(void *mp);

/* hammer2_io.c symbols */
void hammer2_io_hash_init(hammer2_dev_t *hmp);
void hammer2_io_hash_destroy(hammer2_dev_t *hmp);
void hammer2_io_hash_cleanup_all(hammer2_dev_t *hmp);
hammer2_io_t *hammer2_io_getblk(hammer2_dev_t *hmp, int btype,
		hammer2_off_t lbase, int lsize, int op);

/* hammer2_cluster.c */
void hammer2_dummy_xop_from_chain(struct hammer2_xop_head *xop,
		hammer2_chain_t *chain);

/* hammer2_bulkfree.c */
void hammer2_bulkfree_init(hammer2_dev_t *hmp);
void hammer2_bulkfree_uninit(hammer2_dev_t *hmp);

/* hammer2_vfsops.c-internal helpers used cross-file */
void hammer2_bioq_sync(hammer2_pfs_t *pmp);
struct vop_strategy_args;
int hammer2_strategy(struct vop_strategy_args *ap);
/* PAGE_SIZE-chunked device read/write of a 64K-aligned region (hammer2_io.c). */
int hammer2_dev_bread(struct block_device *bdev, loff_t byteoff, void *buf,
    int bytes);
int hammer2_dev_bwrite(struct block_device *bdev, loff_t byteoff,
    const void *buf, int bytes, int sync);
int  hammer2_igetv(hammer2_inode_t *ip, int flags, void *vpp);
struct inode *hammer2_iget(struct super_block *sb, hammer2_inode_t *ip);
int hammer2_ioctl_linux(struct inode *inode, unsigned long com, void *data,
    int fflag);
#define wakeup(c)	hammer2_mtx_wakeup((void *)(c))

#endif /* !_HAMMER2_H_ */
