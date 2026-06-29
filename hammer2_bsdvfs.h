/*
 * hammer2_bsdvfs.h -- BSD VFS surface shims for the Linux port.
 *
 * This header lets hammer2_vnops.c compile under Linux *as-is*, without
 * rewriting the BSD VOP_* dispatch model in place.  Functions still take
 * struct vop_<name>_args bags and reach out to BSD-named helpers; those
 * helpers are stubbed here.  The resulting .o is correct C code but is not
 * yet wired to the Linux VFS -- a separate translation layer must register
 * struct file_operations / inode_operations / address_space_operations and
 * forward into the BSD-shaped entry points.
 */

#ifndef _HAMMER2_BSDVFS_H_
#define _HAMMER2_BSDVFS_H_

#include <linux/fs.h>
#include <linux/uio.h>
#include <linux/cred.h>
#include <linux/namei.h>

/*
 * BSD vnode shim -- minimal subset of fields used in HAMMER2 vnops.
 * Aliased to (struct vnode *) but distinct from Linux's struct inode.
 */
struct hammer2_bsd_bufobj {
	void		*bo_private;
	void		*bo_ops;
};

struct vnode {
	void				*v_data;	/* hammer2_inode_t * */
	int				v_type;		/* VREG / VDIR / ... */
	void				*v_op;
	dev_t				v_rdev;
	void				*v_object;
	struct mount			*v_mount;
	struct mount			*v_mountedhere;
	struct super_block		*v_super;	/* backref */
	struct inode			*v_inode;	/* paired Linux inode */
	struct hammer2_bsd_bufobj	v_bufobj;
	void				*v_vnlock;
	int				v_holdcnt;
	int				v_iflag;
};

/*
 * BSD struct uio represents a scatter-gather user/kernel I/O buffer.
 * On Linux this is struct iov_iter; just enough fields to satisfy callers.
 */
struct uio {
	off_t			uio_offset;
	size_t			uio_resid;
	int			uio_segflg;	/* UIO_SYSSPACE / UIO_USERSPACE */
	int			uio_rw;		/* UIO_READ / UIO_WRITE */
	struct iovec		*uio_iov;
	int			uio_iovcnt;
	struct thread		*uio_td;	/* alias of task_struct */
};

#define UIO_USERSPACE	0
#define UIO_SYSSPACE	1
#define UIO_READ	0
#define UIO_WRITE	1

/* BSD struct componentname -- name lookup payload. */
struct componentname {
	uint32_t	cn_nameiop;	/* LOOKUP / CREATE / RENAME / DELETE */
	uint32_t	cn_flags;	/* FOLLOW / LOCKLEAF / ISLASTCN ... */
	uint32_t	cn_lkflags;	/* LK_EXCLUSIVE / LK_SHARED */
	struct ucred	*cn_cred;
	struct task_struct *cn_thread;
	const char	*cn_nameptr;
	size_t		cn_namelen;
	size_t		cn_consume;
};

#define LOOKUP		1
#define CREATE		2
#define DELETE		3
#define RENAME		4
#define FOLLOW		0x0040
#define LOCKLEAF	0x0004
#define LOCKPARENT	0x0008
#define LOCKSHARED	0x0100
#define ISLASTCN	0x0080
#define NOCACHE		0x0020
#define DOWHITEOUT	0x1000
#define MAKEENTRY	0x0400

/*
 * struct vop_*_args -- one struct per VOP.  Each contains a leading
 * a_gen pointer (BSD vop_generic_args) then per-op fields.  For the port
 * we just keep the per-op fields HAMMER2 accesses (collected from the
 * vnops source).
 */
struct vop_generic_args { void *a_desc; };

struct vop_inactive_args {
	struct vnode	*a_vp;
	struct task_struct *a_td;
};
struct vop_reclaim_args {
	struct vnode	*a_vp;
	struct task_struct *a_td;
};
struct vop_fsync_args {
	struct vnode	*a_vp;
	int		a_waitfor;
	struct task_struct *a_td;
};
struct vop_access_args {
	struct vnode	*a_vp;
	int		a_accmode;
	struct ucred	*a_cred;
	struct task_struct *a_td;
};
struct vop_getattr_args {
	struct vnode	*a_vp;
	struct vattr	*a_vap;
	struct ucred	*a_cred;
};
struct vop_setattr_args {
	struct vnode	*a_vp;
	struct vattr	*a_vap;
	struct ucred	*a_cred;
};
struct vop_read_args {
	struct vnode	*a_vp;
	struct uio	*a_uio;
	int		a_ioflag;
	struct ucred	*a_cred;
};
struct vop_write_args {
	struct vnode	*a_vp;
	struct uio	*a_uio;
	int		a_ioflag;
	struct ucred	*a_cred;
};
struct vop_readdir_args {
	struct vnode	*a_vp;
	struct uio	*a_uio;
	struct ucred	*a_cred;
	int		*a_eofflag;
	int		*a_ncookies;
	off_t		**a_cookies;
};
struct vop_open_args {
	struct vnode	*a_vp;
	int		a_mode;
	struct ucred	*a_cred;
	struct file	*a_fp;
	struct task_struct *a_td;
};
struct vop_close_args {
	struct vnode	*a_vp;
	int		a_fflag;
	struct ucred	*a_cred;
	struct task_struct *a_td;
};
struct vop_ioctl_args {
	struct vnode	*a_vp;
	unsigned long	a_command;
	void		*a_data;
	int		a_fflag;
	struct ucred	*a_cred;
	struct task_struct *a_td;
};
struct vop_lookup_args {
	struct vnode		*a_dvp;
	struct vnode		**a_vpp;
	struct componentname	*a_cnp;
};
#define vop_cachedlookup_args vop_lookup_args
struct vop_create_args {
	struct vnode		*a_dvp;
	struct vnode		**a_vpp;
	struct componentname	*a_cnp;
	struct vattr		*a_vap;
};
struct vop_mkdir_args {
	struct vnode		*a_dvp;
	struct vnode		**a_vpp;
	struct componentname	*a_cnp;
	struct vattr		*a_vap;
};
struct vop_mknod_args {
	struct vnode		*a_dvp;
	struct vnode		**a_vpp;
	struct componentname	*a_cnp;
	struct vattr		*a_vap;
};
struct vop_symlink_args {
	struct vnode		*a_dvp;
	struct vnode		**a_vpp;
	struct componentname	*a_cnp;
	struct vattr		*a_vap;
	const char		*a_target;
};
struct vop_link_args {
	struct vnode		*a_tdvp;
	struct vnode		*a_vp;
	struct componentname	*a_cnp;
};
struct vop_remove_args {
	struct vnode		*a_dvp;
	struct vnode		*a_vp;
	struct componentname	*a_cnp;
};
struct vop_rmdir_args {
	struct vnode		*a_dvp;
	struct vnode		*a_vp;
	struct componentname	*a_cnp;
};
struct vop_rename_args {
	struct vnode		*a_fdvp;
	struct vnode		*a_fvp;
	struct componentname	*a_fcnp;
	struct vnode		*a_tdvp;
	struct vnode		*a_tvp;
	struct componentname	*a_tcnp;
};
struct vop_readlink_args {
	struct vnode	*a_vp;
	struct uio	*a_uio;
	struct ucred	*a_cred;
};
struct vop_pathconf_args {
	struct vnode	*a_vp;
	int		a_name;
	long		*a_retval;
};
struct vop_print_args {
	struct vnode	*a_vp;
};
struct vop_bmap_args {
	struct vnode	*a_vp;
	long		a_bn;
	struct hammer2_bsd_bufobj **a_bop;
	long		*a_bnp;
	int		*a_runp;
	int		*a_runb;
	int		a_cmd;
};
struct vop_getpages_args {
	struct vnode	*a_vp;
	void		**a_m;
	int		a_count;
	int		*a_rbehind;
	int		*a_rahead;
};
struct vop_vptofh_args {
	struct vnode	*a_vp;
	struct fid	*a_fhp;
};

/*
 * BSD VOP_* dispatch macros.  In the port these are just passthroughs to
 * Linux equivalents or stubs; never actually invoked at runtime until the
 * Linux VFS glue is wired up.
 */
#define VOP_ACCESS(vp, mode, cred, td)		0
#define VOP_UNLOCK(vp)				do { (void)(vp); } while (0)
#define VOP_ISLOCKED(vp)			0
#define VOP_BMAP(vp, bn, bop, bnp, runp, runb)	0
#define VOP_LOOKUP(vp, vpp, cnp)		0
/* VOP_PANIC slot stub -- declared in hammer2_init.c. */
extern int hammer2_vop_panic(void *ap);
#define VOP_PANIC	((void *)hammer2_vop_panic)

/* BSD vnode access mode bits. */
#define VREAD	(1 << 2)
#define VWRITE	(1 << 1)
#define VEXEC	(1 << 0)

/* BSD inode immutable/append flags (file_attributes). */
#define SF_IMMUTABLE	0x00020000
#define SF_APPEND	0x00040000
#define SF_NOUNLINK	0x00100000
#define SF_SETTABLE	0xFFFF0000
#define UF_IMMUTABLE	0x00000002
#define UF_APPEND	0x00000004
#define UF_NOUNLINK	0x00000010
#define UF_NODUMP	0x00000001
#define IMMUTABLE	(SF_IMMUTABLE | UF_IMMUTABLE)
#define APPEND		(SF_APPEND | UF_APPEND)
#define VA_UTIMES_NULL	0x01

/* BSD I/O flag fields */
#define IO_SEQSHIFT	16
#define IO_APPEND	0x0002
#define IO_INVAL	0x0100
#define IO_NORMAL	0x0040
#define UIO_NOCOPY	2
#define B_CACHE		0x00000020
#define B_NOCACHE	0x00008000
#define MAXBSIZE	(1 << 16)
#define MNT_NOCLUSTERR	0x40000000
#define MNT_NOCLUSTERW	0x80000000
#define NOCRED		((struct ucred *)NULL)

/* time_t (long-form epoch seconds). */
#ifndef _LINUX_TIME_T
typedef long time_t;
#define _LINUX_TIME_T
#endif

/* Additional uio fields used in vnops. */
struct hammer2_uio_extras { struct task_struct *uio_td; };

/* BSD per-thread fields used in vnops -- stubs. */
static inline int vn_rlimit_fsize(void *vp, struct uio *uio, void *td)
{ (void)vp; (void)uio; (void)td; return 0; }

/* BSD buf-cache helpers used by vnops -- stub to errors. */
static inline int hammer2_cluster_read_stub(struct vnode *vp, off_t filesize,
		long lblkno, int size, struct ucred *cred,
		int totread, int seqcount, int gbflag, struct buf **bpp)
{ (void)vp; (void)filesize; (void)lblkno; (void)size; (void)cred;
  (void)totread; (void)seqcount; (void)gbflag; if (bpp) *bpp = NULL; return EIO; }
#define cluster_read(vp, fs, lblk, sz, cred, tot, seq, gb, bpp) \
	hammer2_cluster_read_stub((vp), (fs), (lblk), (sz), (cred), \
	    (tot), (seq), (gb), (bpp))

static inline int hammer2_bread_stub(void *vp, long lblkno, int size,
		struct ucred *cred, struct buf **bpp)
{ (void)vp; (void)lblkno; (void)size; (void)cred; if (bpp) *bpp = NULL;
  return EIO; }
#define bread(vp, lblk, sz, cred, bpp) \
	hammer2_bread_stub((vp), (lblk), (sz), (cred), (bpp))

static inline struct buf *hammer2_getblk_stub(struct vnode *vp, long lblkno,
		int size, int slpflag, int slptimeo, int flags)
{ (void)vp; (void)lblkno; (void)size; (void)slpflag; (void)slptimeo;
  (void)flags; return NULL; }
#define getblk(vp, lblk, sz, slp, slpt, fl) \
	hammer2_getblk_stub((vp), (lblk), (sz), (slp), (slpt), (fl))

static inline void bqrelse(struct buf *bp)  { (void)bp; }
static inline void vfs_bio_clrbuf(struct buf *bp) { (void)bp; }
static inline void vfs_bio_brelse(struct buf *bp, int ioflag)
{ (void)bp; (void)ioflag; }
static inline void vfs_bio_set_flags(struct buf *bp, int flags)
{ (void)bp; (void)flags; }
static inline int bwrite(struct buf *bp)  { (void)bp; return 0; }
static inline void bawrite(struct buf *bp) { (void)bp; }
static inline void bdwrite(struct buf *bp) { (void)bp; }
static inline int vm_page_count_severe(void) { return 0; }
static inline int buf_dirty_count_severe(void) { return 0; }
static inline void cluster_write_vn(struct vnode *vp, void *cluster,
		struct buf *bp, off_t filesize, int seqcount, int gbflags)
{ (void)vp; (void)cluster; (void)bp; (void)filesize; (void)seqcount;
  (void)gbflags; }
static inline void cluster_init_vn(void *cluster) { (void)cluster; }
#define IO_DIRECT	0x0040

/* Componentname helper constants. */
#define ISDOTDOT	0x0002
#define LK_TYPE_MASK	0x000000FF
#define LK_UPGRADE	0x00000010
#define LK_DOWNGRADE	0x00000020
#define LK_CANRECURSE	0x00000040
#define EJUSTRETURN	(-2)
#define NOUNLINK	(SF_NOUNLINK | UF_NOUNLINK)

/* UINT32_MAX should already be in <linux/limits.h>, but ensure it's set. */
#ifndef UINT32_MAX
#define UINT32_MAX	(0xffffffffU)
#endif

/* BSD VFS function stubs used by lookup/create path. */
static inline int vn_vget_ino(struct vnode *dvp, ino_t ino, int lkflags,
			      struct vnode **vpp)
{ (void)dvp; (void)ino; (void)lkflags; if (vpp) *vpp = NULL; return EIO; }
static inline int VN_IS_DOOMED(struct vnode *vp) { (void)vp; return 0; }
static inline void vput(struct vnode *vp) { (void)vp; }
static inline void VREF(struct vnode *vp) { (void)vp; }
static inline void ASSERT_VOP_ELOCKED(struct vnode *vp, const char *str)
{ (void)vp; (void)str; }
static inline void vrele(struct vnode *vp) { (void)vp; }
static inline void cache_vop_rmdir(struct vnode *dvp, struct vnode *vp)
{ (void)dvp; (void)vp; }
static inline int VFS_VGET(struct mount *mp, ino_t ino, int flags,
			   struct vnode **vpp)
{ (void)mp; (void)ino; (void)flags; if (vpp) *vpp = NULL; return EIO; }

static inline int hammer2_checkpath(hammer2_inode_t *src, hammer2_inode_t *tgt)
{ (void)src; (void)tgt; return 0; }
static inline void vn_seqc_write_begin(struct vnode *vp) { (void)vp; }
static inline void vn_seqc_write_end(struct vnode *vp) { (void)vp; }
static inline void hammer2_inode_lock4(hammer2_inode_t *a, hammer2_inode_t *b,
				       hammer2_inode_t *c, hammer2_inode_t *d)
{ (void)a; (void)b; (void)c; (void)d; }
static inline void cache_vop_rename(struct vnode *fdvp, struct vnode *fvp,
		struct vnode *tdvp, struct vnode *tvp,
		struct componentname *fcnp, struct componentname *tcnp)
{ (void)fdvp; (void)fvp; (void)tdvp; (void)tvp; (void)fcnp; (void)tcnp; }
static inline void vnode_create_vobject(void *vp, off_t size, void *td)
{ (void)vp; (void)size; (void)td; }
static inline int hammer2_ioctl_impl(void *ip, unsigned long com,
				     void *data, int fflag, struct ucred *cred)
{ (void)ip; (void)com; (void)data; (void)fflag; (void)cred; return ENOTTY; }
static inline void vn_printf(void *vp, const char *fmt, ...)
{ (void)vp; (void)fmt; }

extern struct vop_vector hammer2_fifoops;
static inline void fifo_printinfo(struct vnode *vp) { (void)vp; }

/* FIFO/special vnode ops vector -- declared as vop_vector so &-of works. */
extern const struct vop_vector fifo_specops;

#define FWRITE	2
#define FREAD	1

/* sysconf-style pathconf names. */
#define _PC_LINK_MAX			1
#define _PC_NAME_MAX			2
#define _PC_PIPE_BUF			3
#define _PC_CHOWN_RESTRICTED		4
#define _PC_NO_TRUNC			5
#define _PC_MIN_HOLE_SIZE		6
#define _PC_PRIO_IO			7
#define _PC_SYNC_IO			8
#define _PC_ALLOC_SIZE_MIN		9
#define _PC_FILESIZEBITS		10
#define _PC_REC_INCR_XFER_SIZE		11
#define _PC_REC_MAX_XFER_SIZE		12
#define _PC_REC_MIN_XFER_SIZE		13
#define _PC_REC_XFER_ALIGN		14
#define _PC_SYMLINK_MAX			15
#define _PC_ACL_EXTENDED		16
#define _PC_ACL_PATH_MAX		17
#define _PC_CAP_PRESENT			18
#define _PC_INF_PRESENT			19
#define _PC_MAC_PRESENT			20
#define _PC_ACL_NFS4			21
#define _PC_DEALLOC_PRESENT		22

/* BSD path limits. */
#ifndef MAXPATHLEN
#define MAXPATHLEN	PATH_MAX
#endif
#define MAXFIDSZ	28

/* BSD VM offset type. */
typedef off_t	vm_ooffset_t;

/* BSD struct vop_vector -- typed slots so initializers from real VOPs match. */
struct vop_vector {
	const struct vop_vector *vop_default;
	int (*vop_inactive)(struct vop_inactive_args *);
	int (*vop_reclaim)(struct vop_reclaim_args *);
	int (*vop_fsync)(struct vop_fsync_args *);
	int (*vop_fdatasync)(struct vop_fsync_args *);
	int (*vop_access)(struct vop_access_args *);
	int (*vop_getattr)(struct vop_getattr_args *);
	int (*vop_setattr)(struct vop_setattr_args *);
	int (*vop_readdir)(struct vop_readdir_args *);
	int (*vop_readlink)(struct vop_readlink_args *);
	int (*vop_read)(struct vop_read_args *);
	int (*vop_write)(struct vop_write_args *);
	int (*vop_bmap)(struct vop_bmap_args *);
	int (*vop_cachedlookup)(struct vop_lookup_args *);
	int (*vop_lookup)(struct vop_lookup_args *);
	int (*vop_mknod)(struct vop_mknod_args *);
	int (*vop_mkdir)(struct vop_mkdir_args *);
	int (*vop_create)(struct vop_create_args *);
	int (*vop_rmdir)(struct vop_rmdir_args *);
	int (*vop_remove)(struct vop_remove_args *);
	int (*vop_rename)(struct vop_rename_args *);
	int (*vop_link)(struct vop_link_args *);
	int (*vop_symlink)(struct vop_symlink_args *);
	int (*vop_open)(struct vop_open_args *);
	int (*vop_close)(struct vop_close_args *);
	int (*vop_ioctl)(struct vop_ioctl_args *);
	int (*vop_print)(struct vop_print_args *);
	int (*vop_pathconf)(struct vop_pathconf_args *);
	int (*vop_vptofh)(struct vop_vptofh_args *);
	int (*vop_getpages)(struct vop_getpages_args *);
	int (*vop_strategy)(struct vop_strategy_args *);
};

extern const struct vop_vector default_vnodeops;
extern int vop_stdfdatasync_buf(struct vop_fsync_args *ap);
static inline int vfs_bio_getpages(struct vnode *vp, void *m, int count,
		int *rbehind, int *rahead, void *getblkno, void *getblksz)
{ (void)vp; (void)m; (void)count; (void)rbehind; (void)rahead;
  (void)getblkno; (void)getblksz; return EOPNOTSUPP; }
static inline int vnode_pager_generic_getpages(struct vnode *vp, void *m,
		int count, int *rbehind, int *rahead, void *getblkno,
		void *getblksz)
{ (void)vp; (void)m; (void)count; (void)rbehind; (void)rahead;
  (void)getblkno; (void)getblksz; return EOPNOTSUPP; }
#define VFS_VOP_VECTOR_REGISTER(v) \
	static int __maybe_unused __h2_vop_register_##v = 0

/* v_vflag and VV_ROOT used in fifoops branch. */
#define v_vflag		v_iflag	/* alias to existing field */
#define VV_ROOT		0x0001

/*
 * hammer2_trans_newinum allocates a new inode number; defined in vfsops
 * but used here.  Forward-declare.
 */
hammer2_tid_t hammer2_trans_newinum(hammer2_pfs_t *pmp);

/* BSD uintegral aliases. */
typedef uint64_t	u_quad_t;
typedef int64_t		quad_t;

/*
 * BSD struct dirent.  Use a unique name to avoid macro-eating the
 * `dirent` field that exists in hammer2_media_data union.
 */
struct h2dirent {
	uint64_t	d_fileno;
	uint64_t	d_off;
	uint16_t	d_reclen;
	uint8_t		d_type;
	uint8_t		d_namlen;
	uint32_t	d_pad0;
	char		d_name[256];
};
#define _GENERIC_DIRLEN(namlen)	(offsetof(struct h2dirent, d_name) + (namlen) + 1)
static inline void dirent_terminate(struct h2dirent *de)
{
	de->d_name[de->d_namlen] = '\0';
}
static inline int uiomove(void *buf, int n, struct uio *uio)
{
	(void)buf; (void)n;
	if (uio) uio->uio_resid -= n;
	return 0;
}

static inline int securelevel_gt(struct ucred *cred, int level)
{ (void)cred; (void)level; return 0; }

static inline int hammer2_vfs_enospace(hammer2_inode_t *ip, off_t bytes,
				       struct ucred *cred)
{ (void)ip; (void)bytes; (void)cred; return 0; }

/* Stub helpers -- referenced but not used at runtime. */
static inline int vrecycle(struct vnode *vp) { (void)vp; return 0; }
static inline void vtruncbuf(void *vp, off_t size, int blksize)
{ (void)vp; (void)size; (void)blksize; }
static inline void vfs_hash_remove(struct vnode *vp) { (void)vp; }
static inline int vop_stdfsync(void *ap) { (void)ap; return 0; }
static inline int vop_stdpathconf(void *ap) { (void)ap; return EOPNOTSUPP; }
static inline int vop_stdgetpages(void *ap) { (void)ap; return EOPNOTSUPP; }
static inline int vaccess(int type, mode_t file_mode, uid_t uid, gid_t gid,
			  int acc_mode, struct ucred *cred)
{ (void)type; (void)file_mode; (void)uid; (void)gid; (void)acc_mode; (void)cred; return 0; }
static inline void vnode_pager_setsize(void *vp, off_t size)
{ (void)vp; (void)size; }
static inline int vfs_cache_lookup(struct vop_lookup_args *ap)
{ (void)ap; return ENOENT; }
static inline void cache_enter(struct vnode *dvp, struct vnode *vp,
			       struct componentname *cnp)
{ (void)dvp; (void)vp; (void)cnp; }
static inline void cache_purge(struct vnode *vp) { (void)vp; }
static inline int vn_lock(struct vnode *vp, int flags)
{ (void)vp; (void)flags; return 0; }

/*
 * VTOI() in this TU resolves the BSD-style v_data field; the Linux-style
 * VTOI() in hammer2.h reads i_private from struct inode.  Override here.
 */
#ifdef VTOI
#undef VTOI
#endif
#define VTOI(vp)	((hammer2_inode_t *)((vp) ? (vp)->v_data : NULL))

#endif /* !_HAMMER2_BSDVFS_H_ */
