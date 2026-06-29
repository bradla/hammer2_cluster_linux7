/*-
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

#ifndef _HAMMER2_COMPAT_H_
#define _HAMMER2_COMPAT_H_

#include <linux/kernel.h>
#include <linux/bug.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <linux/jiffies.h>
#include <linux/types.h>
#include <linux/limits.h>

/* C99 stdint.h limits -- not always present in kernel headers. */
#ifndef UINT32_MAX
#define UINT32_MAX	(0xffffffffU)
#endif
#ifndef UINT64_MAX
#define UINT64_MAX	(0xffffffffffffffffULL)
#endif
#ifndef INT32_MAX
#define INT32_MAX	(0x7fffffff)
#endif
#ifndef INT64_MAX
#define INT64_MAX	(0x7fffffffffffffffLL)
#endif
#include <asm/barrier.h>
#include <asm/processor.h>

/*
 * BSD-style UUID, on-disk compatible with DragonFlyBSD's struct uuid.
 * Total size: 16 bytes.  Linux has uuid_t but we need the exact field
 * layout for the on-disk HAMMER2 format.
 */
struct uuid {
	uint32_t	time_low;
	uint16_t	time_mid;
	uint16_t	time_hi_and_version;
	uint8_t		clock_seq_hi_and_reserved;
	uint8_t		clock_seq_low;
	uint8_t		node[6];
};

/* BSD memory zero helper, ubiquitous in DragonFly sources. */
#ifndef bzero
#define bzero(p, n)	memset((p), 0, (n))
#endif
#ifndef bcopy
#define bcopy(src, dst, n) memmove((dst), (src), (n))
#endif
#ifndef bcmp
#define bcmp(a, b, n)	memcmp((a), (b), (n))
#endif

/*
 * BSD memory allocator flag/zone shims.
 *
 * DragonFly uses kmalloc(size, M_TAG, flags) where M_TAG is a malloc type
 * descriptor and flags include M_WAITOK / M_ZERO.  Linux uses kmalloc(size,
 * gfp) with gfp flags only.  We map M_* tags to no-ops (Linux's slab does
 * not need named allocators) and map the flag bits to GFP equivalents via
 * the kmalloc/kzalloc translation in the H2_KMALLOC helper.
 */
#include <linux/slab.h>

#define M_WAITOK	GFP_KERNEL
#define M_NOWAIT	GFP_ATOMIC
#define M_ZERO		0x80000000U	/* sentinel; handled by hammer2_kmalloc */
#define M_NULL		0
#define M_HAMMER	NULL
#define M_HAMMER2	NULL
#define M_TEMP		NULL
#define M_FORCE		0
#define M_VOLHDRS	NULL
#define M_IGNINO	0
#define M_PERMANENT	0
#define M_END		0
#define M_LEVEL		0
#define M_X		0

static inline void *
hammer2_kmalloc(size_t sz, void *tag, unsigned int flags)
{
	gfp_t gfp = (flags & ~M_ZERO) ? (gfp_t)(flags & ~M_ZERO) : GFP_KERNEL;
	/*
	 * Use kvmalloc(): several HAMMER2 structures (hammer2_dev with its
	 * iohash[32768] + heur_dedup[262144] arrays ~= 7MB, hammer2_pfs with
	 * inumhash[32768]) exceed Linux's ~4MB kmalloc limit, where DragonFly's
	 * kmalloc handled any size.  kvmalloc falls back to vmalloc for large
	 * allocations and behaves like kmalloc for small ones; kvfree() copes
	 * with both.  (For GFP_ATOMIC kvmalloc only attempts kmalloc, which is
	 * the desired behaviour.)
	 */
	if (flags & M_ZERO)
		return kvzalloc(sz, gfp);
	return kvmalloc(sz, gfp);
}

/*
 * BSD's kmalloc(size, M_TAG, flags) has a 3-arg signature that conflicts
 * with Linux's kmalloc(size, gfp).  We do NOT redefine kmalloc globally;
 * .c files that need the BSD form should call hammer2_kmalloc() directly.
 * A future sweep can sed-replace the call sites.
 *
 * Similarly BSD's kfree(p, M_TAG) takes a tag; map call sites to kfree(p)
 * via a wrapper.
 */
static inline void
hammer2_kfree(void *p, void *tag)
{
	kfree(p);
}

/*
 * Per-port allocation helpers used by HAMMER2 source:
 *   hmalloc(size, M_TAG, flags)
 *   hrealloc(ptr, newsize, M_TAG, flags)
 *   hfree(ptr, M_TAG)
 * Map them onto Linux kmalloc/krealloc/kfree, dropping the unused tag.
 */
#define hmalloc(sz, tag, fl)		hammer2_kmalloc((sz), (tag), (fl))
/*
 * DragonFly hfree() may be called as hfree(p, tag) or hfree(p, tag, size).
 * kvfree() handles both kmalloc- and vmalloc-backed pointers from hmalloc().
 */
#define hfree(p, ...)			kvfree((p))

static inline void *
hammer2_krealloc(void *p, size_t sz, void *tag, unsigned int flags)
{
	gfp_t gfp = (flags & ~M_ZERO) ? (gfp_t)(flags & ~M_ZERO) : GFP_KERNEL;
	void *np = krealloc(p, sz, gfp);
	/* M_ZERO on realloc is not standard; ignore for now. */
	return np;
}
#define hrealloc(p, sz, tag, fl)	hammer2_krealloc((p), (sz), (tag), (fl))

/* Debug accounting hook in DragonFly; no-op on Linux. */
#define adjust_malloc_leak(delta, tag)	do { (void)(delta); } while (0)

/*
 * BSD MALLOC_DEFINE registers a malloc type with the kernel for accounting.
 * Linux's slab allocator tracks per-cache stats automatically; the macro is
 * a no-op here.  SYSCTL_NODE / SYSCTL_INT register sysctl entries; Linux
 * exposes module params via /sys/module/<name>/parameters which we'd wire
 * up separately if needed.
 */
#define MALLOC_DEFINE(tag, short_descr, long_descr) \
	struct __hammer2_malloc_define_##tag { int dummy; }
#define SYSCTL_NODE(parent, nbr, name, access, handler, descr) \
	struct __hammer2_sysctl_node_##name { int dummy; }
#define SYSCTL_INT(parent, nbr, name, access, ptr, val, descr) \
	struct __hammer2_sysctl_int_##name { int dummy; }
#define SYSCTL_LONG(parent, nbr, name, access, ptr, val, descr) \
	struct __hammer2_sysctl_long_##name { int dummy; }
#define OID_AUTO	0
#define CTLFLAG_RW	0
#define CTLFLAG_RD	0
#define UMA_ALIGN_PTR	0

/*
 * BSD VFS mount-path constants and BSD-globals stubs.  HAMMER2's mount
 * code references several FreeBSD-internal globals (nbuf, desiredvnodes)
 * and helpers (vfs_getopt, vfs_filteropt, vfs_hash_*) that don't exist on
 * Linux.  Provide weak stubs so the file compiles; a real port will swap
 * these for super_block / file_system_type / fs_context plumbing.
 */
#ifndef MNT_UPDATE
#define MNT_UPDATE	0x10000
#endif
#ifndef MNT_LOCAL
#define MNT_LOCAL	0x1000
#endif
#ifndef MNAMELEN
#define MNAMELEN	1024
#endif

static const int nbuf = 1024;
static const int desiredvnodes = 1024;
static const int maxphys = (1 << 20);	/* 1 MiB */

static inline unsigned long *hashinit(int elements, void *type,
				      unsigned long *hashmask)
{
	int i, sz = roundup_pow_of_two(elements);
	struct list_head *t = kzalloc(sz * sizeof(struct list_head), GFP_KERNEL);

	/*
	 * Each bucket is a list head and MUST be self-initialized -- a zeroed
	 * list_head (next=prev=NULL) is not a valid empty list, and iterating
	 * it (list_first_entry_or_null sees next != head) dereferences NULL.
	 */
	if (t)
		for (i = 0; i < sz; ++i)
			INIT_LIST_HEAD(&t[i]);
	if (hashmask)
		*hashmask = sz - 1;
	(void)type;
	return (unsigned long *)t;
}
static inline void hashdestroy(void *table, void *type, unsigned long hashmask)
{
	(void)hashmask;
	(void)type;
	kfree(table);
}
#define HASH_LIST	0
#define M_HAMMER2_HASH	NULL

static inline char *hstrdup(const char *s) { return kstrdup(s, GFP_KERNEL); }
static inline void hstrfree(char *s)       { kfree(s); }

/* TAILQ_EMPTY shim -- list_empty on the embedded list_head. */
#define TAILQ_EMPTY(hp)		list_empty(&(hp)->head)

/*
 * BSD vfs_getopt -- on Linux the mount glue (hammer2_linux_vfs.c) builds a
 * small option list in mp->mnt_optnew and points hammer2_mount() at it.  A
 * NULL list degrades to "no options found" (ENOENT) so call sites still
 * compile and behave for the BSD vfsops dispatch path that is never reached.
 */
struct h2_mount_opt {
	const char	*name;
	void		*value;
	int		len;
};
struct h2_mount_optlist {
	int			count;
	struct h2_mount_opt	opts[8];
};
static inline int vfs_getopt(void *optsv, const char *name,
			     void **buf, int *len)
{
	struct h2_mount_optlist *ol = optsv;
	int i;

	if (ol) {
		for (i = 0; i < ol->count; ++i) {
			if (strcmp(ol->opts[i].name, name) == 0) {
				if (buf)
					*buf = ol->opts[i].value;
				if (len)
					*len = ol->opts[i].len;
				return 0;
			}
		}
	}
	if (buf) *buf = NULL;
	if (len) *len = 0;
	return ENOENT;
}
static inline int vfs_filteropt(void *opts, const char **legal)
{
	(void)opts; (void)legal;
	return 0;
}

/*
 * MPTOPMP(mp) -- BSD macro that fetches the FS-private hammer2_pfs from a
 * struct mount.  Every caller in this port passes the BSD `struct mount`
 * shim (NOT a super_block), and hammer2_mount_helper() stores the pmp in
 * mp->mnt_data -- so read it from there.  (Casting to super_block and reading
 * s_fs_info read past the small struct mount shim and returned garbage/NULL.)
 */
#define MPTOPMP(mp)	((hammer2_pfs_t *)((struct mount *)(mp))->mnt_data)

/*
 * BSD struct mount stub.  HAMMER2's vfsops reads a handful of fields.
 * On Linux these are spread across struct super_block / file_system_type;
 * this stub lets the code compile -- a real port replaces every call site
 * with the proper Linux API.
 */
/*
 * BSD struct statfs collides with Linux's UAPI struct of the same name.
 * Use a distinct port-internal type and #define statfs to it in this TU.
 */
/*
 * BSD struct statfs has different fields from Linux's UAPI struct statfs.
 * Wrap it in an anonymous struct inside struct mount so callers using
 * `struct statfs *` (BSD style) can be patched to use `typeof(mp->mnt_stat) *`
 * or be rewritten.  Inside struct mount we just inline the fields.
 */
/* Port-internal statfs struct (BSD field layout). */
struct h2statfs {
	long			f_iosize;
	long			f_bsize;
	char			f_mntfromname[MNAMELEN];
	char			f_mntonname[MNAMELEN];
	struct { int val[2]; } f_fsid;
	uint64_t		f_blocks;
	uint64_t		f_bfree;
	uint64_t		f_bavail;
	uint64_t		f_files;
	uint64_t		f_ffree;
};

struct mount {
	int				mnt_flag;
	int				mnt_kern_flag;
	void				*mnt_data;
	void				*mnt_optnew;
	struct h2statfs			mnt_stat;
	void				*mnt_vfc;
	long				mnt_iosize_max;
};

#define MNT_ILOCK(mp)		do { (void)(mp); } while (0)
#define MNT_IUNLOCK(mp)		do { (void)(mp); } while (0)
#define MNT_KERN_LOOKUP_SHARED	0
#define MNT_KERN_EXTENDED_SHARED 0

/* BSD mount kernel-flag bits (vfsops sets these on mp->mnt_kern_flag). */
#ifndef MNTK_LOOKUP_SHARED
#define MNTK_LOOKUP_SHARED	0
#endif
#ifndef MNTK_EXTENDED_SHARED
#define MNTK_EXTENDED_SHARED	0
#endif
#ifndef MNTK_USES_BCACHE
#define MNTK_USES_BCACHE	0
#endif
#ifndef FORCECLOSE
#define FORCECLOSE	0
#endif

/* BSD qaddr_t / caddr_t aliases. */
typedef long *qaddr_t;
#ifndef caddr_t
typedef char *caddr_t;
#endif

/*
 * curthread maps to Linux's `current` task pointer.  BSD treats it as a
 * struct thread *, which is structurally equivalent to struct task_struct *
 * for our purposes; declare struct thread as an alias and let casts work.
 */
#define curthread	((struct thread *)current)
struct thread;

/* BSD thread_t is an opaque thread pointer (only ever compared/stored). */
typedef struct thread *thread_t;

/*
 * Misc BSD helpers used by the kdmsg / iocom code.
 *   ksnprintf -> snprintf
 *   hostname  -> the running kernel's UTS node name
 *   printf_uuid -> debug-only UUID dump; cheap hex of the first bytes
 */
#ifndef ksnprintf
#define ksnprintf	snprintf
#endif
#include <linux/utsname.h>
#ifndef hostname
#define hostname	(init_utsname()->nodename)
#endif
static inline void
printf_uuid(const void *uuid)
{
	/* Accepts either struct uuid or Linux uuid_t (both 16-byte). */
	if (uuid)
		printk(KERN_CONT "%pUb", uuid);
}

/*
 * DragonFly lock-depth accounting macros used by the CCMS subsystem; they are
 * pure debug instrumentation on BSD.  No-op on Linux.
 */
#ifndef LOCKENTER
#define LOCKENTER	do { } while (0)
#endif
#ifndef LOCKEXIT
#define LOCKEXIT	do { } while (0)
#endif

/* BSD struct fid is the NFS-style file handle.  Linux uses struct fid in
 * <linux/exportfs.h> with a different shape.  Stub for HAMMER2's fhtovp
 * callback which we don't actually wire up. */
struct fid {
	unsigned short	fid_len;
	unsigned short	fid_reserved;
	char		fid_data[28];
};

/* ino_t is already typedef'd by <linux/types.h>; no shim needed. */

/* BSD LK_* lock flags used in cleanup paths -- map to no-ops on Linux. */
#ifndef LK_EXCLUSIVE
#define LK_EXCLUSIVE	0
#define LK_SHARED	0
#define LK_NOWAIT	0
#define LK_RETRY	0
#define LK_INTERLOCK	0
#define LK_NOWITNESS	0
#endif
#ifndef LK_RELEASE
#define LK_RELEASE	0x100
#endif

/*
 * BSD lockmgr() lock used by the kdmsg DMSG layer (iocom->msglk).  DragonFly's
 * struct lock is a recursive read/write lockmgr lock; the kdmsg code uses it as
 * a coarse exclusive lock, so map it to a Linux rw_semaphore taken for write.
 * lockmgr(lk, LK_RELEASE) releases; any other flag acquires exclusively.
 */
struct lock {
	struct rw_semaphore	lk_rw;
};
#define lockinit(lk, name, timo, flags)	init_rwsem(&(lk)->lk_rw)
#define lockuninit(lk)			do { } while (0)
#define lockmgr(lk, flags)						\
	do {								\
		if ((flags) & LK_RELEASE)				\
			up_write(&(lk)->lk_rw);				\
		else							\
			down_write(&(lk)->lk_rw);			\
	} while (0)

/* BSD sleep priority flags -- unused on Linux. */
#ifndef PCATCH
#define PCATCH	0
#define PRIBIO	0
#endif

/* BSD vflush() forcibly invalidates all vnodes of a mount; on Linux this
 * is the kernel's job during ->kill_sb.  Stub to success. */
static inline int vflush(struct mount *mp, int rootrefs, int flags, void *td)
{
	(void)mp; (void)rootrefs; (void)flags; (void)td;
	return 0;
}
static inline void vfs_mountedfrom(struct mount *mp, const char *name)
{
	(void)mp; (void)name;
}
static inline int vfs_flagopt(void *opts, const char *name, int *flagp, int flag)
{
	(void)opts; (void)name; (void)flagp; (void)flag;
	return 0;
}

/*
 * BSD struct vfsops is the per-fstype dispatch table.  Linux uses
 * struct file_system_type registered via register_filesystem().  Provide
 * a stub struct so the static initializer compiles; the actual VFS
 * registration is done elsewhere via a Linux file_system_type.
 */
struct vfsconf;
struct vfsops {
	int  (*vfs_init)(struct vfsconf *);
	int  (*vfs_uninit)(struct vfsconf *);
	int  (*vfs_mount)(struct mount *);
	int  (*vfs_unmount)(struct mount *, int);
	int  (*vfs_sync)(struct mount *, int);
	int  (*vfs_vget)(struct mount *, ino_t, int, struct inode **);
	int  (*vfs_root)(struct mount *, int, struct inode **);
	int  (*vfs_statfs)(struct mount *, struct h2statfs *);
	int  (*vfs_fhtovp)(struct mount *, struct fid *, int, struct inode **);
};
#define VFS_SET(ops, name, flag) \
	static int __maybe_unused __h2_vfs_set_##name = 0
#define MODULE_VERSION(name, ver) \
	static int __maybe_unused __h2_modver_##name = (ver)

/*
 * BSD vnode type constants.  Linux has no equivalent enum (file type lives
 * in inode->i_mode via S_IF*).  HAMMER2 uses these only as opaque tags in
 * mapping functions, so we map them to the corresponding DT_* constants
 * (which Linux defines in <linux/fs.h>).
 */
#include <linux/fs.h>

#define VBAD	0
#define VNON	0
#define VDIR	DT_DIR
#define VREG	DT_REG
#define VFIFO	DT_FIFO
#define VCHR	DT_CHR
#define VBLK	DT_BLK
#define VLNK	DT_LNK
#define VSOCK	DT_SOCK

/* DragonFly debug print helpers; map to Linux printk variants. */
#define hprintf(fmt, ...)		pr_info("hammer2: " fmt, ##__VA_ARGS__)
#define debug_hprintf(fmt, ...)		pr_debug("hammer2: " fmt, ##__VA_ARGS__)
/*
 * Userspace printf doesn't exist in kernel code; redirect to pr_info() so
 * BSD debug call sites compile and produce dmesg output.
 */
#define printf(fmt, ...)		pr_info(fmt, ##__VA_ARGS__)

/* BSD howmany() ceiling-divide helper. */
#ifndef howmany
#define howmany(x, y)	(((x) + ((y) - 1)) / (y))
#endif

/* BSD power-of-two rounddown.  Same semantics as Linux ALIGN_DOWN. */
#ifndef rounddown2
#define rounddown2(x, y)	((x) & ~((y) - 1))
#endif

/* BSD's default block-size unit.  Linux kernel uses 512 for daddr_t too. */
#ifndef DEV_BSIZE
#define DEV_BSIZE	512
#endif

/* daddr_t is a BSD type for disk block numbers; Linux kernel headers
 * already typedef it via <linux/types.h>, so no shim is needed here. */

/*
 * BSD mount flag / sync mode constants.  HAMMER2 only uses them as opaque
 * tokens (MNT_WAIT == "synchronous flush"); the actual sync semantics on
 * Linux are encoded by the caller of sync_filesystem() / sync_blockdev().
 */
#ifndef MNT_RDONLY
#define MNT_RDONLY	0x00000001
#endif
#ifndef MNT_WAIT
#define MNT_WAIT	1
#endif
#ifndef MNT_NOWAIT
#define MNT_NOWAIT	2
#endif

/*
 * BSD user-copy primitives.  Linux uses copy_to_user / copy_from_user.
 * BSD copyin returns 0 on success or EFAULT; Linux copy_*_user returns the
 * number of bytes that could not be copied (0 on success).  Wrap so call
 * sites get the BSD convention.
 */
#include <linux/uaccess.h>
static inline int hammer2_copyout(const void *kern, void __user *user, size_t n)
{
	return copy_to_user(user, kern, n) ? -EFAULT : 0;
}
static inline int hammer2_copyin(const void __user *user, void *kern, size_t n)
{
	return copy_from_user(kern, user, n) ? -EFAULT : 0;
}
#define copyout(kern, user, n)	hammer2_copyout((kern), (user), (n))
#define copyin(user, kern, n)	hammer2_copyin((user), (kern), (n))

/*
 * BSD struct vattr is the attribute bag passed to VOP_CREATE / VOP_SETATTR.
 * Just enough fields for HAMMER2 inode-create sites to compile; actual VFS
 * dispatch on Linux uses struct iattr / struct mnt_idmap differently.
 */
struct vattr {
	mode_t			va_mode;
	uid_t			va_uid;
	gid_t			va_gid;
	dev_t			va_rdev;
	uint8_t			va_type;	/* VBAD / VDIR / VREG / ... */
	uint64_t		va_fsid;
	uint64_t		va_fileid;
	uint64_t		va_nlink;
	uint64_t		va_size;
	uint32_t		va_flags;
	struct timespec64	va_ctime;
	struct timespec64	va_mtime;
	struct timespec64	va_atime;
	struct timespec64	va_birthtime;
	uint64_t		va_gen;
	uint32_t		va_blocksize;
	uint64_t		va_bytes;
	uint64_t		va_filerev;
	int			va_vaflags;
};

/*
 * BSD struct ucred: HAMMER2 peeks cr_uid / cr_gid.  Linux already defines
 * struct ucred in <linux/socket.h> with pid_t/uid_t/gid_t fields.  We pull
 * it in (transitively via hammer2.h) and provide BSD-style accessors via
 * macros so call sites compile.  Note: Linux ucred.uid/gid are kuid_t/kgid_t.
 */
#include <linux/socket.h>
#define cr_uid		uid	/* mapped to struct ucred.uid */
#define cr_gid		gid

static inline int groupmember(gid_t gid, const struct ucred *cred)
{
	(void)gid;
	(void)cred;
	return 0;	/* port stub: assume non-member */
}

static inline int priv_check_cred(const struct ucred *cred, int priv)
{
	(void)cred;
	(void)priv;
	return EPERM;	/* port stub: deny privileged ops */
}
#define PRIV_VFS_RETAINSUGID	0
#define PRIV_VFS_CHOWN		0
#define PRIV_VFS_SETGID		0
#define PRIV_VFS_STICKYFILE	0
#define PRIV_VFS_SYSFLAGS	0

/* BSD file mode helpers. */
#ifndef ALLPERMS
#define ALLPERMS	(S_ISUID | S_ISGID | S_ISVTX | 0777)
#endif
#ifndef S_ISTXT
#define S_ISTXT		S_ISVTX
#endif
#ifndef EFTYPE
#define EFTYPE		EINVAL
#endif
#ifndef NODEV
#define NODEV		((dev_t)(-1))
#endif

/* BSD major(dev_t) / minor(dev_t) -- Linux has MAJOR/MINOR via <linux/kdev_t.h> */
#include <linux/kdev_t.h>
#ifndef major
#define major(d)	MAJOR((dev_t)(d))
#endif
#ifndef minor
#define minor(d)	MINOR((dev_t)(d))
#endif

/* BSD VNOVAL sentinel for "leave attribute unchanged". */
#ifndef VNOVAL
#define VNOVAL	(-1)
#endif

/* vop_helper_create_uid is defined as a static helper inside
 * hammer2_inode.c -- no global shim needed. */

/* BSD sx-lock asserts.  Linux mutex doesn't distinguish recursion. */
#ifndef SA_XLOCKED
#define SA_XLOCKED	0x01
#endif
#ifndef SA_NOTRECURSED
#define SA_NOTRECURSED	0x02
#endif
#define hammer2_mtx_assert(p, flags) \
	WARN_ON(!mutex_is_locked(&(p)->lock))

/*
 * BSD struct buf / vop_strategy_args shims.  Just enough of the surface
 * area for hammer2_strategy.c to compile.  Actual I/O lifecycle hooks
 * (bp completion, ordering, etc.) need a proper Linux bio path before
 * any strategy code is exercised at runtime.
 */
/*
 * Note: `struct buf` is BSD's main buffer type.  We provide a placeholder
 * struct with the same name (rather than a #define alias) so that the
 * unrelated `buf` field in hammer2_media_data_t doesn't get macro-eaten.
 */
struct buf {
	int		b_iocmd;	/* BIO_READ / BIO_WRITE */
	int		b_error;
	int		b_ioflags;	/* BIO_ERROR etc. */
	int		b_flags;	/* B_CLUSTEROK / B_INVAL / B_RELBUF */
	long		b_resid;
	long		b_bufsize;
	long		b_bcount;
	off_t		b_offset;
	char		*b_data;
	sector_t	b_lblkno;
	sector_t	b_blkno;
};

struct vop_strategy_args {
	struct inode		*a_vp;
	struct buf		*a_bp;
};

#define BIO_READ	0
#define BIO_WRITE	1
#define BIO_ERROR	0x0001
#define B_CLUSTEROK	0x0100
#define B_INVAL		0x0200
#define B_RELBUF	0x0400

static inline void bufdone(struct buf *bp) { (void)bp; }
#define __DECONST(t, v)		((t)(uintptr_t)(v))

/*
 * zlib API shims.  Linux kernel uses zlib_inflateInit/Inflate/InflateEnd
 * and exposes Z_OK / Z_STREAM_END / Z_FINISH via <linux/zlib.h>.  Provide
 * the lowercase BSD/userspace spellings.  Z_NULL is not exported by the
 * kernel headers, so we provide our own NULL alias.
 */
#ifndef Z_NULL
#define Z_NULL		NULL
#endif
#define inflateInit	zlib_inflateInit
#define inflate		zlib_inflate
#define inflateEnd	zlib_inflateEnd
#define deflateInit	zlib_deflateInit
#define deflate		zlib_deflate
#define deflateEnd	zlib_deflateEnd

/*
 * BSD I/O flag constants for VOP_WRITE / strategy.  IO_SYNC requests
 * synchronous completion; IO_ASYNC is fire-and-forget.  Linux uses the
 * WB_SYNC_* / REQ_SYNC bits, but HAMMER2 only branches on the BSD
 * constants -- so defining them as opaque tokens is enough.
 */
#ifndef IO_SYNC
#define IO_SYNC		0x0080
#endif
#ifndef IO_ASYNC
#define IO_ASYNC	0x0200
#endif

/* BSD's MAXPHYS is the largest contiguous I/O size.  Linux uses BLK_MAX_SEGMENT_SIZE
 * but HAMMER2 only uses MAXPHYS as a clamp constant; pick a reasonable value. */
#ifndef MAXPHYS
#define MAXPHYS		(1 << 20)	/* 1 MiB */
#endif

/* BSD FIOSEEKDATA / FIOSEEKHOLE are encoded with the file's own seek codes. */
#ifndef FIOSEEKDATA
#define FIOSEEKDATA	SEEK_DATA
#endif
#ifndef FIOSEEKHOLE
#define FIOSEEKHOLE	SEEK_HOLE
#endif

/* VTOI() is defined in hammer2.h after hammer2_inode_t is declared. */

/*
 * BSD kern_uuidgen(uuid, count) fills `count` randomly-generated UUIDs.
 * Linux: generate_random_uuid() fills 16 bytes into a uuid_t.  We zero
 * the BSD-style struct uuid (which has the same 16-byte layout) and
 * splatter random bytes over it.
 */
#include <linux/random.h>
#include <linux/uuid.h>
static inline void
kern_uuidgen(struct uuid *u, int count)
{
	int i;
	for (i = 0; i < count; ++i)
		get_random_bytes(&u[i], sizeof(u[i]));
}

/*
 * SHA256 compatibility shim.  BSD calls SHA256_Init / Update / Final on a
 * SHA256_CTX; Linux's crypto API exposes sha256_init() / sha256_update() /
 * sha256_final() acting on struct sha256_state.
 */
#include <crypto/sha2.h>

#define SHA256_DIGEST_LENGTH	SHA256_DIGEST_SIZE
typedef struct sha256_ctx SHA256_CTX;
static inline void SHA256_Init(SHA256_CTX *c)            { sha256_init(c); }
static inline void SHA256_Update(SHA256_CTX *c, const void *d, size_t n)
                                                         { sha256_update(c, d, n); }
static inline void SHA256_Final(uint8_t *out, SHA256_CTX *c)
                                                         { sha256_final(c, out); }

/*
 * DragonFly UMA zone allocator.  Linux equivalent is kmem_cache, but for
 * the porting effort we just map them onto kmalloc/kfree with an opaque
 * "zone" handle that tracks the element size.
 */
struct hammer2_zone {
	size_t		elem_size;
	const char	*name;
};
typedef struct hammer2_zone *uma_zone_t;

static inline uma_zone_t
uma_zcreate(const char *name, size_t sz,
	    void *ctor, void *dtor, void *init, void *fini,
	    int align, unsigned int flags)
{
	struct hammer2_zone *z = kzalloc(sizeof(*z), GFP_KERNEL);
	if (z) {
		z->elem_size = sz;
		z->name = name;
	}
	return z;
}

static inline void
uma_zdestroy(uma_zone_t z)
{
	kfree(z);
}

static inline void *
uma_zalloc(uma_zone_t z, unsigned int flags)
{
	gfp_t gfp = (flags & ~M_ZERO) ? (gfp_t)(flags & ~M_ZERO) : GFP_KERNEL;
	if (!z)
		return NULL;
	if (flags & M_ZERO)
		return kzalloc(z->elem_size, gfp);
	return kmalloc(z->elem_size, gfp);
}

static inline void
uma_zfree(uma_zone_t z, void *p)
{
	kfree(p);
}

/*
 * BSD atomic_*_int / _32 / _64 helpers.  Linux exposes generic atomic_t and
 * atomic64_t types; we adapt by reinterpreting the pointer.  These are
 * relaxed-memory-order helpers; HAMMER2 doesn't rely on stricter ordering
 * for these specific call sites.
 */
#define atomic_add_int(p, v) \
	atomic_add((int)(v), (atomic_t *)(p))
#define atomic_clear_int(p, v) \
	atomic_andnot((int)(v), (atomic_t *)(p))
#define atomic_set_int(p, v) \
	atomic_or((int)(v), (atomic_t *)(p))
#define atomic_cmpset_int(p, o, n) \
	(atomic_cmpxchg((atomic_t *)(p), (int)(o), (int)(n)) == (int)(o))
#define atomic_fetchadd_int(p, v) \
	atomic_fetch_add((int)(v), (atomic_t *)(p))

#define atomic_set_32(p, v) \
	atomic_or((int)(v), (atomic_t *)(p))
#define atomic_clear_32(p, v) \
	atomic_andnot((int)(v), (atomic_t *)(p))
#define atomic_fetchadd_32(p, v) \
	atomic_fetch_add((int)(v), (atomic_t *)(p))
#define atomic_cmpset_32(p, o, n) \
	(atomic_cmpxchg((atomic_t *)(p), (int)(o), (int)(n)) == (int)(o))

#define atomic_set_64(p, v) \
	atomic64_or((s64)(v), (atomic64_t *)(p))
#define atomic_clear_64(p, v) \
	atomic64_andnot((s64)(v), (atomic64_t *)(p))
#define atomic_fetchadd_64(p, v) \
	atomic64_fetch_add((s64)(v), (atomic64_t *)(p))

/* Linux kernel assertion macros */
#ifndef KASSERT
#define KASSERT(exp, msg) \
    do { \
        if (unlikely(!(exp))) { \
            printk(KERN_ERR "HAMMER2: assertion failed: " msg "\n"); \
            BUG(); \
        } \
    } while (0)
#endif

/* KASSERT variant with message (similar to NetBSD's KASSERTMSG) */
#define KASSERTMSG(exp, msg, ...) \
    do { \
        if (unlikely(!(exp))) { \
            printk(KERN_ERR "HAMMER2: assertion failed: " msg "\n", ##__VA_ARGS__); \
            BUG(); \
        } \
    } while (0)

/* DragonFly KKASSERT - always with file/line info */
#define KKASSERT(exp) \
    KASSERTMSG(exp, \
        "assertion \"%s\" failed in %s at %s:%d", \
        #exp, __func__, __FILE__, __LINE__)

/*
 * CPU-specific operations
 * 
 * cpu_pause(): Hints the CPU that this is a busy-wait loop
 * On x86: PAUSE instruction, on ARM: YIELD instruction
 * On Linux, we use cpu_relax() which does the appropriate thing per architecture
 */
#define cpu_pause()     cpu_relax()

/*
 * CPU compiler fence for memory ordering
 * Ensures the compiler doesn't reorder memory accesses
 * Linux uses barrier() for compiler memory barrier
 */
#define cpu_ccfence()   barrier()

/*
 * Get system ticks/jiffies
 * DragonFly's ticks = Linux's jiffies
 * Returns the number of ticks since system boot
 */
#define getticks()      (jiffies)

/*
 * Additional compatibility macros that may be needed
 */

/*
 * NOTE: Do NOT redefine atomic_add_int / atomic_set_int / atomic_cmpset_int
 * here.  The correct BSD-semantics versions are defined above:
 *   atomic_set_int(p,v)   == atomic_or  (set the bits in v)
 *   atomic_clear_int(p,v) == atomic_andnot
 *   atomic_cmpset_int     returns a success boolean
 * The earlier (buggy) redefinitions used atomic_set() -- which OVERWRITES the
 * whole word -- so e.g. atomic_set_int(&chain->flags, BIT) clobbered every
 * other flag bit, corrupting the CHAIN_IOINPROG interlock and much more.
 */

/*
 * Time conversion helpers (if needed)
 */
#define TICKS_PER_SECOND    HZ
#define ticks_to_seconds(t) ((t) / HZ)
#define seconds_to_ticks(s) ((s) * HZ)

/*
 * Debug macros
 */
#ifdef HAMMER2_DEBUG
#define DPRINTK(fmt, ...) \
    printk(KERN_DEBUG "HAMMER2: " fmt, ##__VA_ARGS__)
#else
#define DPRINTK(fmt, ...) \
    do { } while (0)
#endif

#define DPRINTK_INFO(fmt, ...) \
    printk(KERN_INFO "HAMMER2: " fmt, ##__VA_ARGS__)

#define DPRINTK_WARN(fmt, ...) \
    printk(KERN_WARNING "HAMMER2: " fmt, ##__VA_ARGS__)

#define DPRINTK_ERR(fmt, ...) \
    printk(KERN_ERR "HAMMER2: " fmt, ##__VA_ARGS__)

/*
 * Panic helper (similar to BSD's panic())
 */
#define hammer2_panic(fmt, ...) \
    do { \
        printk(KERN_EMERG "HAMMER2: panic: " fmt, ##__VA_ARGS__); \
        BUG(); \
    } while (0)

/*
 * Unreachable code annotation
 */
#define hammer2_unreachable() \
    do { \
        printk(KERN_ERR "HAMMER2: unreachable code at %s:%d\n", \
               __FILE__, __LINE__); \
        unreachable(); \
    } while (0)

#endif /* !_HAMMER2_COMPAT_H_ */
