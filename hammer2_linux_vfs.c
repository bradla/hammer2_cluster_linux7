// SPDX-License-Identifier: BSD-3-Clause
/*
 * hammer2_linux_vfs.c Linux VFS glue for the HAMMER2 filesystem port.
 *
 * The HAMMER2 internals (chains, freemap, flush, XOPs, dedup, LZ4, the
 * device-buffer I/O layer in hammer2_io.c, and the logical-block strategy
 * path in hammer2_strategy.c) are already ported and functional.  This file
 * is the edge translation layer that turns them into a real, mountable Linux
 * filesystem:
 *
 *   - register_filesystem() via the modern fs_context mount API
 *   - super_operations (statfs, sync_fs, evict_inode, put_super)
 *   - the inode<->Linux-inode linkage (iget5_locked, i_private <-> ip->vp)
 *   - inode_operations for directories, regular files and symlinks
 *   - file_operations (read_iter / write_iter / iterate_shared / fsync)
 *
 * The read and write data paths are driven synchronously through the existing
 * hammer2_strategy() dispatcher: because XOPs run inline in the calling
 * thread on Linux, hammer2_strategy() fully populates (read) or drains
 * (write) the BSD-style struct buf before returning.  We wrap a 64K logical
 * block in a struct buf and let the chain layer do the rest.
 */

#include "hammer2.h"

#include <linux/module.h>
#include <linux/init.h>
#include <linux/fs.h>
#include <linux/blkdev.h>	/* sync_blockdev, blkdev_issue_flush */
#include <linux/buffer_head.h>
#include <linux/fs_context.h>
#include <linux/pagemap.h>
#include <linux/highmem.h>
#include <linux/writeback.h>
#include <linux/mm.h>
#include <linux/slab.h>
#include <linux/statfs.h>
#include <linux/uio.h>
#include <linux/namei.h>
#include <linux/version.h>
#include <linux/uaccess.h>

/*
 * Port version string.  Bump HAMMER2_PORT_VERSION on each change so the
 * loaded build is identifiable via `dmesg` (printed at module load) and
 * `modinfo hammer2.ko | grep version`.  The build date/time is appended
 * automatically so two builds of the same version number are still
 * distinguishable.
 */
#define HAMMER2_PORT_VERSION	"0.43"
#define HAMMER2_PORT_BUILD	HAMMER2_PORT_VERSION " built " __DATE__ " " __TIME__

/* BSD-shaped vfsops entry points (un-static'd in hammer2_vfsops.c). */
int hammer2_mount(struct mount *mp);
int hammer2_unmount(struct mount *mp, int mntflags);
int hammer2_root(struct mount *mp, int flags, struct inode **vpp);
int hammer2_statfs(struct mount *mp, struct h2statfs *sbp);
int hammer2_sync(struct mount *mp, int waitfor);
hammer2_tid_t hammer2_trans_newinum(hammer2_pfs_t *pmp);
struct vfsconf;
int hammer2_init(struct vfsconf *vfsp);
int hammer2_uninit(struct vfsconf *vfsp);

static const struct super_operations hammer2_super_ops;
static const struct inode_operations hammer2_dir_iops;
static const struct inode_operations hammer2_file_iops;
static const struct inode_operations hammer2_symlink_iops;
static const struct inode_operations hammer2_special_iops;
static const struct file_operations hammer2_dir_fops;
static const struct file_operations hammer2_file_fops;
static const struct address_space_operations hammer2_aops;

/* ------------------------------------------------------------------------ */
/* Type and attribute translation helpers				    */
/* ------------------------------------------------------------------------ */

static umode_t
hammer2_objtype_to_ifmt(uint8_t type)
{
	switch (type) {
	case HAMMER2_OBJTYPE_DIRECTORY:	return S_IFDIR;
	case HAMMER2_OBJTYPE_REGFILE:	return S_IFREG;
	case HAMMER2_OBJTYPE_FIFO:	return S_IFIFO;
	case HAMMER2_OBJTYPE_CDEV:	return S_IFCHR;
	case HAMMER2_OBJTYPE_BDEV:	return S_IFBLK;
	case HAMMER2_OBJTYPE_SOFTLINK:	return S_IFLNK;
	case HAMMER2_OBJTYPE_SOCKET:	return S_IFSOCK;
	default:			return 0;
	}
}

/* Linux umode_t S_IF* -> BSD DT_* (which is what vattr.va_type carries). */
static uint8_t
hammer2_ifmt_to_dtype(umode_t mode)
{
	if (S_ISDIR(mode))	return DT_DIR;
	if (S_ISREG(mode))	return DT_REG;
	if (S_ISLNK(mode))	return DT_LNK;
	if (S_ISCHR(mode))	return DT_CHR;
	if (S_ISBLK(mode))	return DT_BLK;
	if (S_ISFIFO(mode))	return DT_FIFO;
	if (S_ISSOCK(mode))	return DT_SOCK;
	return DT_UNKNOWN;
}

static void
hammer2_set_inode_ops(struct inode *inode)
{
	switch (inode->i_mode & S_IFMT) {
	case S_IFREG:
		inode->i_op = &hammer2_file_iops;
		inode->i_fop = &hammer2_file_fops;
		inode->i_mapping->a_ops = &hammer2_aops;
		/*
		 * Let the page cache build folios up to one HAMMER2 logical
		 * block (64KiB) but no larger.  Matching the fs block size
		 * lets a full-block overwrite skip the read-modify-write and
		 * collapses fill/writeback from 16x4KiB folios per block down
		 * to one, killing the write amplification that made large
		 * writes (write_100k/writev) slow.  min order 0 keeps a 4KiB
		 * fallback under memory pressure.  No-op unless THP is built
		 * in (it is on this kernel).
		 */
		mapping_set_folio_order_range(inode->i_mapping, 0,
		    HAMMER2_PBUFRADIX - PAGE_SHIFT);
		break;
	case S_IFDIR:
		inode->i_op = &hammer2_dir_iops;
		inode->i_fop = &hammer2_dir_fops;
		break;
	case S_IFLNK:
		inode->i_op = &hammer2_symlink_iops;
		inode_nohighmem(inode);
		break;
	default:
		inode->i_op = &hammer2_special_iops;
		init_special_inode(inode, inode->i_mode,
		    MKDEV(VTOI(inode)->meta.rmajor, VTOI(inode)->meta.rminor));
		break;
	}
}

/*
 * Copy HAMMER2 inode metadata into a freshly-allocated Linux inode.
 */
static void
hammer2_install_meta(struct inode *inode, hammer2_inode_t *ip)
{
	struct timespec64 ts;

	inode->i_mode = (ip->meta.mode & (S_IALLUGO)) |
	    hammer2_objtype_to_ifmt(ip->meta.type);
	i_uid_write(inode, hammer2_inode_to_uid(ip));
	i_gid_write(inode, hammer2_inode_to_gid(ip));
	set_nlink(inode, ip->meta.nlinks);
	i_size_write(inode, ip->meta.size);
	inode->i_blkbits = HAMMER2_PBUFRADIX;

	hammer2_time_to_timespec(ip->meta.mtime, &ts);
	inode_set_mtime_to_ts(inode, ts);
	hammer2_time_to_timespec(ip->meta.atime, &ts);
	inode_set_atime_to_ts(inode, ts);
	hammer2_time_to_timespec(ip->meta.ctime, &ts);
	inode_set_ctime_to_ts(inode, ts);

	hammer2_set_inode_ops(inode);
}

/* ------------------------------------------------------------------------ */
/* iget machinery							    */
/* ------------------------------------------------------------------------ */

static int
hammer2_iget_test(struct inode *inode, void *data)
{
	return inode->i_private == data;
}

static int
hammer2_iget_set(struct inode *inode, void *data)
{
	hammer2_inode_t *ip = data;

	inode->i_private = ip;
	inode->i_ino = (unsigned long)ip->meta.inum;
	ip->vp = inode;
	hammer2_inode_ref(ip);		/* the vnode reference, dropped at evict */
	return 0;
}

/*
 * Return the Linux struct inode for a (locked, referenced) hammer2_inode_t,
 * allocating and populating it on first use.  This is the body behind the
 * hammer2_igetv() shim used throughout the inode/vfsops code.
 *
 * The caller retains ownership of its inode lock; we only borrow ip to read
 * its metadata.  A brand new Linux inode takes its own ip reference (see
 * hammer2_iget_set) so ip survives until the inode is evicted.
 */
struct inode *
hammer2_iget(struct super_block *sb, hammer2_inode_t *ip)
{
	struct inode *inode;

	inode = iget5_locked(sb, (unsigned long)ip->meta.inum,
	    hammer2_iget_test, hammer2_iget_set, ip);
	if (!inode)
		return ERR_PTR(-ENOMEM);
	if (!(inode_state_read(inode) & I_NEW))
		return inode;

	hammer2_install_meta(inode, ip);
	unlock_new_inode(inode);
	return inode;
}

/* ------------------------------------------------------------------------ */
/* Synchronous logical-block I/O via the strategy path			    */
/* ------------------------------------------------------------------------ */

/*
 * Read or write a single HAMMER2_PBUFSIZE logical block at PBUF-aligned base
 * lbase.  data must point at a HAMMER2_PBUFSIZE buffer.  On read the buffer
 * is filled (sparse/zero-fill handled by the strategy completion); on write
 * the buffer's contents are committed via the chain layer.
 *
 * Because XOPs execute inline, hammer2_strategy() has fully completed the
 * transfer (and called bufdone()) by the time it returns.
 */
static int
hammer2_strategy_block(struct inode *inode, hammer2_key_t lbase, char *data,
    int iocmd, int nowait)
{
	struct vop_strategy_args ap;
	struct buf b;

	memset(&b, 0, sizeof(b));
	b.b_iocmd = iocmd;
	b.b_data = data;
	b.b_offset = lbase;
	b.b_bcount = HAMMER2_PBUFSIZE;
	b.b_bufsize = HAMMER2_PBUFSIZE;
	b.b_resid = HAMMER2_PBUFSIZE;
	b.b_lblkno = lbase / HAMMER2_PBUFSIZE;
	b.b_blkno = b.b_lblkno;
	b.b_flags = nowait ? B_NOWAIT : 0;

	ap.a_vp = inode;
	ap.a_bp = &b;
	hammer2_strategy(&ap);

	if (b.b_ioflags & BIO_ERROR)
		return b.b_error ? -b.b_error : -EIO;
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Read path								    */
/* ------------------------------------------------------------------------ */

static ssize_t __maybe_unused
hammer2_read_iter(struct kiocb *iocb, struct iov_iter *to)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	hammer2_inode_t *ip = VTOI(inode);
	loff_t pos = iocb->ki_pos;
	loff_t isize;
	size_t want = iov_iter_count(to);
	ssize_t total = 0;
	char *blk;
	int error = 0;

	if (S_ISDIR(inode->i_mode))
		return -EISDIR;

	hammer2_mtx_sh(&ip->lock);
	isize = ip->meta.size;
	hammer2_mtx_unlock(&ip->lock);

	if (want == 0 || pos >= isize)
		return 0;

	blk = kmalloc(HAMMER2_PBUFSIZE, GFP_KERNEL);
	if (!blk)
		return -ENOMEM;

	while (want > 0 && pos < isize) {
		hammer2_key_t lbase = pos & ~(hammer2_key_t)HAMMER2_PBUFMASK;
		int loff = (int)(pos - lbase);
		size_t n = HAMMER2_PBUFSIZE - loff;

		if (n > want)
			n = want;
		if ((loff_t)(pos + n) > isize)
			n = (size_t)(isize - pos);

		error = hammer2_strategy_block(inode, lbase, blk, BIO_READ, 0);
		if (error)
			break;
		if (copy_to_iter(blk + loff, n, to) != n) {
			error = -EFAULT;
			break;
		}
		pos += n;
		total += n;
		want -= n;
	}

	kfree(blk);
	iocb->ki_pos = pos;
	/*
	 * a hand-rolled read loop rather than generic_file_read_iter(),
	 * so nothing was updating the access time atime never advanced at all
	 * (xfstests generic/003).  file_accessed() applies the mount's atime
	 * policy (relatime/noatime) for us.
	 */
	if (total > 0)
		file_accessed(iocb->ki_filp);
	return total ? total : error;
}

/*
 * Read a symlink target.  HAMMER2 stores the link as ordinary file data
 * (embedded in the inode for short links), so a single block read of base 0
 * recovers it.
 */
static const char *
hammer2_get_link(struct dentry *dentry, struct inode *inode,
    struct delayed_call *done)
{
	hammer2_inode_t *ip = VTOI(inode);
	loff_t len;
	char *blk, *target;
	int error;

	if (!dentry)
		return ERR_PTR(-ECHILD);

	hammer2_mtx_sh(&ip->lock);
	len = ip->meta.size;
	hammer2_mtx_unlock(&ip->lock);

	if (len < 0 || len >= HAMMER2_PBUFSIZE)
		return ERR_PTR(-EIO);

	blk = kmalloc(HAMMER2_PBUFSIZE, GFP_KERNEL);
	if (!blk)
		return ERR_PTR(-ENOMEM);

	error = hammer2_strategy_block(inode, 0, blk, BIO_READ, 0);
	if (error) {
		kfree(blk);
		return ERR_PTR(error);
	}
	blk[len] = '\0';

	target = blk;
	set_delayed_call(done, kfree_link, target);
	return target;
}

/*
 * Longest legal name component.  HAMMER2 stores the name in a 256-byte field
 * (HAMMER2_INODE_MAXNAME) and hammer2_dirent_create() asserts
 * name_len < HAMMER2_INODE_MAXNAME, so 255 is the maximum not 256.
 *
 * The guards below used to read "> HAMMER2_INODE_MAXNAME", which let a
 * 256-byte component through to that assert and BUG()'d the kernel: an
 * unprivileged panic, one mkdir away.  statfs() also advertised 256 as
 * NAME_MAX, so a conforming application asking pathconf(_PC_NAME_MAX) was
 * told to use exactly the length that panics.  Found by fstest chmod/03.t.
 */
#define HAMMER2_NAME_MAX	(HAMMER2_INODE_MAXNAME - 1)

/*
 * Directory lookup resolve a name to its inode via the nresolve XOP.
 */
static struct dentry *
hammer2_lookup(struct inode *dir, struct dentry *dentry, unsigned int flags)
{
	hammer2_inode_t *dip = VTOI(dir);
	hammer2_xop_nresolve_t *xop;
	hammer2_inode_t *ip;
	struct inode *inode = NULL;
	int error;

	if (dentry->d_name.len > HAMMER2_NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);

	hammer2_inode_lock(dip, HAMMER2_RESOLVE_SHARED);
	xop = hammer2_xop_alloc(dip, 0);
	hammer2_xop_setname(&xop->head, dentry->d_name.name, dentry->d_name.len);
	hammer2_xop_start(&xop->head, &hammer2_nresolve_desc);
	error = hammer2_xop_collect(&xop->head, 0);
	error = hammer2_error_to_errno(error);

	if (error == 0) {
		ip = hammer2_inode_get(dip->pmp, &xop->head, -1, -1);
		hammer2_inode_unlock(dip);
		if (ip) {
			inode = hammer2_iget(dir->i_sb, ip);
			hammer2_inode_unlock(ip);
			if (IS_ERR(inode)) {
				hammer2_xop_retire(&xop->head,
				    HAMMER2_XOPMASK_VOP);
				return ERR_CAST(inode);
			}
		}
	} else {
		hammer2_inode_unlock(dip);
		if (error != ENOENT) {
			hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
			return ERR_PTR(-error);
		}
	}
	hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

	/* NULL inode => negative dentry (ENOENT), which is fine. */
	return d_splice_alias(inode, dentry);
}

/*
 * Read directory entries.  Mirrors hammer2_readdir() (BSD) but emits through
 * the Linux dir_context instead of uiomove().  ctx->pos is used as the
 * HAMMER2 directory hash cursor, exactly as the BSD code used uio_offset.
 */
static int
hammer2_iterate(struct file *file, struct dir_context *ctx)
{
	struct inode *inode = file_inode(file);
	hammer2_inode_t *ip = VTOI(inode);
	hammer2_xop_readdir_t *xop;
	const hammer2_inode_data_t *ripdata;
	hammer2_blockref_t bref;
	hammer2_tid_t inum;
	off_t saveoff = ctx->pos;
	int dtype, error = 0;
	uint16_t namlen;
	const char *dname;

	if (!S_ISDIR(inode->i_mode))
		return -ENOTDIR;

	hammer2_inode_lock(ip, HAMMER2_RESOLVE_SHARED);

	/* Artificial '.' and '..' entries (cursor 0 and 1). */
	if (saveoff == 0) {
		inum = ip->meta.inum & HAMMER2_DIRHASH_USERMSK;
		if (!dir_emit(ctx, ".", 1, inum, DT_DIR))
			goto done;
		ctx->pos = ++saveoff;
	}
	if (saveoff == 1) {
		inum = ip->meta.inum & HAMMER2_DIRHASH_USERMSK;
		if (ip != ip->pmp->iroot)
			inum = ip->meta.iparent & HAMMER2_DIRHASH_USERMSK;
		if (!dir_emit(ctx, "..", 2, inum, DT_DIR))
			goto done;
		ctx->pos = ++saveoff;
	}

	/*
	 * Bounded FIFO: without it the backend buffers every remaining entry on
	 * each getdents() call while we consume only a bufferful, making readdir
	 * O(n^2) (measured 15.8s for 20k entries).  We resume from ctx->pos.
	 */
	xop = hammer2_xop_alloc(ip, HAMMER2_XOP_FIFO_BOUND);
	xop->lkey = saveoff | HAMMER2_DIRHASH_VISIBLE;
	hammer2_xop_start(&xop->head, &hammer2_readdir_desc);

	for (;;) {
		error = hammer2_xop_collect(&xop->head, 0);
		error = hammer2_error_to_errno(error);
		if (error)
			break;
		hammer2_cluster_bref(&xop->head.cluster, &bref);

		if (bref.type == HAMMER2_BREF_TYPE_INODE) {
			ripdata = &((const hammer2_media_data_t *)
			    hammer2_xop_gdata(&xop->head))->ipdata;
			dtype = hammer2_get_dtype(ripdata->meta.type);
			saveoff = bref.key & HAMMER2_DIRHASH_USERMSK;
			if (!dir_emit(ctx,
			    (const char *)ripdata->filename,
			    ripdata->meta.name_len,
			    ripdata->meta.inum & HAMMER2_DIRHASH_USERMSK,
			    dtype)) {
				hammer2_xop_pdata(&xop->head);
				break;
			}
			hammer2_xop_pdata(&xop->head);
		} else if (bref.type == HAMMER2_BREF_TYPE_DIRENT) {
			dtype = hammer2_get_dtype(bref.embed.dirent.type);
			saveoff = bref.key & HAMMER2_DIRHASH_USERMSK;
			namlen = bref.embed.dirent.namlen;
			if (namlen <= sizeof(bref.check.buf))
				dname = bref.check.buf;
			else
				dname = ((const hammer2_media_data_t *)
				    hammer2_xop_gdata(&xop->head))->buf;
			if (!dir_emit(ctx, dname, namlen,
			    bref.embed.dirent.inum, dtype)) {
				if (namlen > sizeof(bref.check.buf))
					hammer2_xop_pdata(&xop->head);
				break;
			}
			if (namlen > sizeof(bref.check.buf))
				hammer2_xop_pdata(&xop->head);
		} else {
			hprintf("bad blockref type %d\n", bref.type);
			continue;
		}
		/* Advance cursor past the entry just emitted. */
		ctx->pos = (saveoff + 1) & ~HAMMER2_DIRHASH_VISIBLE;
	}
	hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);

	if (error == ENOENT || error == -ENOENT)
		error = 0;
done:
	hammer2_inode_unlock(ip);
	return -error;		/* HAMMER2 positive errno -> Linux negative */
}

/* ------------------------------------------------------------------------ */
/* Attribute operations							    */
/* ------------------------------------------------------------------------ */

static int
hammer2_getattr(struct mnt_idmap *idmap, const struct path *path,
    struct kstat *stat, u32 request_mask, unsigned int query_flags)
{
	struct inode *inode = d_inode(path->dentry);
	hammer2_inode_t *ip = VTOI(inode);

	generic_fillattr(idmap, request_mask, inode, stat);
	stat->blksize = HAMMER2_PBUFSIZE;
	if (ip && ip->meta.type != HAMMER2_OBJTYPE_DIRECTORY)
		stat->blocks = (hammer2_inode_data_count(ip) + 511) / 512;
	return 0;
}

/* Shrink or grow the on-media file, updating in-memory inode metadata. */
/*
 * Zero the part of the straddling block that a truncate left beyond EOF.
 *
 * hammer2_xop_inode_chain_sync() deletes whole DATA chains past the new EOF,
 * but the 64KiB block containing the new EOF keeps its old contents.  Extend
 * the file again and those bytes reappear as file data: fsx caught exactly
 * that (generic/075):
 *
 *   TRUNCATE DOWN 0x40000 -> 0x22030
 *   TRUNCATE UP   0x22030 -> 0x36612
 *   MAPREAD 0x1a6fe..0x29c03  -> mismatch at 0x2f3b5, inside 0x20000-0x30000
 *
 * Zero [osize, end-of-that-block) through the page cache once the size has
 * grown, so the region is inside EOF and writeback rewrites the block.
 *
 * A write(2) that extends EOF needs this too truncate down to 0x61b6, then
 * write at 0x1c236, and the hole left behind exposed the old tail of the first
 * block (generic/075 again, at op 104).  But such a write supplies the bytes
 * for [pos, end) itself and hammer2_write_end() has already copied them into
 * the page cache, so zeroing that far discards the write.  Callers cap the
 * range with nsize: pass the start of the caller-supplied data, so only the
 * hole ahead of it is zeroed.  See hammer2_resize_meta().
 */
static void
hammer2_zero_extend_tail(struct inode *inode, loff_t osize, loff_t nsize)
{
	struct address_space *mapping = inode->i_mapping;
	struct folio *folio;
	loff_t pos, zend;

	if (nsize <= osize || (osize & HAMMER2_PBUFMASK) == 0)
		return;

	zend = (osize + HAMMER2_PBUFSIZE) & ~(loff_t)HAMMER2_PBUFMASK;
	if (zend > nsize)
		zend = nsize;

	for (pos = osize; pos < zend; ) {
		size_t off, len;

		folio = __filemap_get_folio(mapping, pos >> PAGE_SHIFT,
		    FGP_LOCK | FGP_CREAT, mapping_gfp_mask(mapping));
		if (IS_ERR(folio))
			break;
		if (!folio_test_uptodate(folio))
			folio_zero_range(folio, 0, folio_size(folio));
		off = offset_in_folio(folio, pos);
		len = folio_size(folio) - off;
		if (pos + (loff_t)len > zend)
			len = zend - pos;
		folio_zero_range(folio, off, len);
		folio_mark_uptodate(folio);
		filemap_dirty_folio(mapping, folio);
		folio_unlock(folio);
		folio_put(folio);
		pos += len;
	}
}

static void
hammer2_resize_meta(struct inode *inode, hammer2_inode_t *ip, loff_t nsize,
    loff_t zlimit)
{
	hammer2_pfs_t *pmp = ip->pmp;
	struct folio *folio = NULL;
	loff_t osize;
	int crossing;

	/*
	 * Growing past the embedded-data limit reuses the inode's union for the
	 * blockref table, the bytes living there must reach a real DATA block
	 * or they are simply lost write 40 bytes, truncate to 4, extend to
	 * 4096, and after a remount the 4 surviving bytes were gone (xfstests
	 * generic/393).  Pull them into the page cache BEFORE the conversion
	 * (afterwards a read would see the zeroed union) and dirty the folio
	 * after it, so writeback allocates a normal block for them.
	 */
	osize = i_size_read(inode);
	crossing = (osize > 0 && osize <= HAMMER2_EMBEDDED_BYTES &&
	    nsize > HAMMER2_EMBEDDED_BYTES);
	if (crossing) {
		folio = read_mapping_folio(inode->i_mapping, 0, NULL);
		if (IS_ERR(folio))
			folio = NULL;
	}

	hammer2_trans_init(pmp, 0);
	hammer2_mtx_ex(&ip->lock);
	osize = ip->meta.size;
	if (nsize == osize) {
		hammer2_mtx_unlock(&ip->lock);
		hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);
		return;
	}
	hammer2_inode_modify(ip);
	ip->osize = osize;
	ip->meta.size = nsize;

	/*
	 * A truncation, or an extension that crosses the embedded-data
	 * boundary, requires the indirect block table to be (re)prepared
	 * before any further strategy I/O touches the inode.
	 */
	if (nsize < osize ||
	    (osize <= HAMMER2_EMBEDDED_BYTES && nsize > HAMMER2_EMBEDDED_BYTES)) {
		atomic_set_int(&ip->flags, HAMMER2_INODE_RESIZED);
		hammer2_inode_chain_sync(ip);
	}
	hammer2_mtx_unlock(&ip->lock);
	hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);

	i_size_write(inode, nsize);

	/*
	 * Page cache may only be discarded on a shrink.  Both call sites reach
	 * here, and on a write(2)-driven growth the folio the copy just landed
	 * in still has to reach writeback.  truncate_pagecache() also unmaps
	 * the truncated range, which plain truncate_inode_pages() does not 
	 * mmap of a file shrunk by ftruncate(2) kept its stale mappings.
	 *
	 * On a growth, zlimit bounds stale-tail zeroing to the region the caller
	 * has NOT filled itself: truncate(2) passes nsize (zero the whole stale
	 * tail), write(2) passes the start of its copy (zero only the hole in
	 * front of it, never the bytes just written).
	 */
	if (nsize < osize)
		truncate_pagecache(inode, nsize);
	else
		hammer2_zero_extend_tail(inode, osize, min(nsize, zlimit));

	if (folio) {
		folio_lock(folio);
		if (folio->mapping == inode->i_mapping)
			filemap_dirty_folio(inode->i_mapping, folio);
		folio_unlock(folio);
		folio_put(folio);
	}
}

static int
hammer2_setattr(struct mnt_idmap *idmap, struct dentry *dentry,
    struct iattr *iattr)
{
	struct inode *inode = d_inode(dentry);
	hammer2_inode_t *ip = VTOI(inode);
	hammer2_pfs_t *pmp = ip->pmp;
	uint64_t ctime;
	int error;

	if (pmp->rdonly)
		return -EROFS;

	error = setattr_prepare(idmap, dentry, iattr);
	if (error)
		return error;

	if ((iattr->ia_valid & ATTR_SIZE) && S_ISDIR(inode->i_mode))
		return -EISDIR;

	if ((iattr->ia_valid & ATTR_SIZE) && iattr->ia_size != i_size_read(inode))
		hammer2_resize_meta(inode, ip, iattr->ia_size, iattr->ia_size);

	hammer2_trans_init(pmp, 0);
	hammer2_mtx_ex(&ip->lock);
	hammer2_inode_modify(ip);
	hammer2_update_time(&ctime);

	if (iattr->ia_valid & ATTR_MODE) {
		ip->meta.mode = (ip->meta.mode & ~(S_IALLUGO)) |
		    (iattr->ia_mode & S_IALLUGO);
		ip->meta.ctime = ctime;
	}
	if (iattr->ia_valid & ATTR_UID)
		hammer2_guid_to_uuid(&ip->meta.uid,
		    from_kuid(&init_user_ns, iattr->ia_uid));
	if (iattr->ia_valid & ATTR_GID)
		hammer2_guid_to_uuid(&ip->meta.gid,
		    from_kgid(&init_user_ns, iattr->ia_gid));
	if (iattr->ia_valid & ATTR_MTIME)
		ip->meta.mtime = hammer2_timespec_to_time(&iattr->ia_mtime);
	if (iattr->ia_valid & ATTR_ATIME)
		ip->meta.atime = hammer2_timespec_to_time(&iattr->ia_atime);
	/*
	 * A size change updates BOTH mtime and ctime, and the filesystem has to
	 * do it: path-based truncate(2) reaches do_truncate() with time_attrs=0
	 * (only ftruncate(2) passes ATTR_MTIME|ATTR_CTIME), so relying on
	 * ia_valid leaves the timestamps untouched.  fstest truncate/00.t.
	 */
	if (iattr->ia_valid & ATTR_SIZE)
		ip->meta.mtime = ctime;
	if (iattr->ia_valid & (ATTR_CTIME | ATTR_MODE | ATTR_SIZE))
		ip->meta.ctime = ctime;
	hammer2_mtx_unlock(&ip->lock);
	hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);

	setattr_copy(idmap, inode, iattr);
	/* setattr_copy() only carries times that are in ia_valid (see above). */
	if (iattr->ia_valid & ATTR_SIZE)
		inode_set_mtime_to_ts(inode, inode_set_ctime_current(inode));
	mark_inode_dirty(inode);
	return 0;
}

/* ------------------------------------------------------------------------ */
/* Write path								    */
/* ------------------------------------------------------------------------ */

/*
 * ----------------------------------------------------------------------
 * Page-cache address_space operations
 * ----------------------------------------------------------------------
 *
 * The file data path is routed through the Linux page cache so that
 * read(2), write(2) and mmap(2) all observe a single coherent view of a
 * file.  read_folio()/writepages() bridge the 4KiB folio world to
 * HAMMER2's native 64KiB (HAMMER2_PBUFSIZE) logical block via the same
 * hammer2_strategy_block() primitive the old block path used.  Because a
 * folio is smaller than a logical block, writeback read-modify-writes the
 * containing block so bytes belonging to sibling folios are preserved.
 */

/*
 * Populate a folio from on-media data, zero-filling any region at or past
 * EOF.  Shared by read_folio() and write_begin() (partial-write RMW).
 */
static int
hammer2_fill_folio(struct inode *inode, struct folio *folio)
{
	hammer2_inode_t *ip = VTOI(inode);
	loff_t fpos = folio_pos(folio);
	size_t fsize = folio_size(folio);
	loff_t isize;
	char *blk;
	size_t done = 0;
	int error = 0;

	hammer2_mtx_sh(&ip->lock);
	isize = ip->meta.size;
	hammer2_mtx_unlock(&ip->lock);

	blk = kmalloc(HAMMER2_PBUFSIZE, GFP_KERNEL);
	if (!blk)
		return -ENOMEM;

	while (done < fsize) {
		loff_t pos = fpos + done;
		hammer2_key_t lbase = pos & ~(hammer2_key_t)HAMMER2_PBUFMASK;
		int loff = (int)(pos - lbase);
		size_t chunk = HAMMER2_PBUFSIZE - loff;
		size_t valid;

		if (chunk > fsize - done)
			chunk = fsize - done;

		if (pos >= isize) {
			folio_zero_range(folio, done, fsize - done);
			break;
		}

		error = hammer2_strategy_block(inode, lbase, blk, BIO_READ, 0);
		if (error)
			goto out;

		valid = chunk;
		if ((loff_t)(pos + chunk) > isize)
			valid = (size_t)(isize - pos);
		memcpy_to_folio(folio, done, blk + loff, valid);
		if (valid < chunk)
			folio_zero_range(folio, done + valid, chunk - valid);
		done += chunk;
	}
out:
	kfree(blk);
	return error;
}

static int
hammer2_read_folio(struct file *file, struct folio *folio)
{
	int error = hammer2_fill_folio(folio->mapping->host, folio);

	if (!error)
		folio_mark_uptodate(folio);
	folio_unlock(folio);
	return error;
}

static int
hammer2_write_begin(const struct kiocb *iocb, struct address_space *mapping,
    loff_t pos, unsigned len, struct folio **foliop, void **fsdata)
{
	struct inode *inode = mapping->host;
	struct folio *folio;
	int error;

	/*
	 * fgf_set_order(len) requests a folio sized for this write (capped at
	 * one 64KiB fs block by mapping_set_folio_order_range()).  Without it
	 * __filemap_get_folio() allocates an order-0 (4KiB) folio, so a large
	 * write would still be split into 16 folios per block the order
	 * hint is what actually collapses a 64KiB write into a single folio
	 * op instead of 16.  A folio already present is returned as-is.
	 */
	folio = __filemap_get_folio(mapping, pos >> PAGE_SHIFT,
	    FGP_WRITEBEGIN | fgf_set_order(len), mapping_gfp_mask(mapping));
	if (IS_ERR(folio))
		return PTR_ERR(folio);

	/*
	 * Bring the folio uptodate before the copy so that a sub-folio write
	 * (or a write that only partially covers the trailing block) does not
	 * expose stale page-cache contents.  A brand-new folio past EOF is
	 * zero-filled by hammer2_fill_folio().
	 *
	 * When the write covers the whole folio (aligned start, len spans the
	 * folio) there is nothing to preserve, so skip the read entirely 
	 * the copy overwrites every byte and hammer2_write_end() marks the
	 * folio uptodate once the full copy lands.  This keeps a synchronous
	 * 64KiB read-modify-write out of the write(2) path for aligned
	 * full-block overwrites, matching what block/iomap filesystems do.
	 */
	if (!folio_test_uptodate(folio) &&
	    !(offset_in_folio(folio, pos) == 0 && len >= folio_size(folio))) {
		error = hammer2_fill_folio(inode, folio);
		if (error) {
			folio_unlock(folio);
			folio_put(folio);
			return error;
		}
		folio_mark_uptodate(folio);
	}

	*foliop = folio;
	return 0;
}

static int
hammer2_write_end(const struct kiocb *iocb, struct address_space *mapping,
    loff_t pos, unsigned len, unsigned copied, struct folio *folio,
    void *fsdata)
{
	struct inode *inode = mapping->host;
	hammer2_inode_t *ip = VTOI(inode);
	loff_t end;

	/*
	 * A folio that is still !uptodate here had its read skipped by
	 * hammer2_write_begin() (full-folio overwrite).  Only a copy that
	 * filled the entire folio may mark it uptodate; a short copy would
	 * leave uninitialised page-cache bytes, so discard it and let the
	 * caller retry this mirrors block_write_end().
	 */
	if (!folio_test_uptodate(folio)) {
		if (copied < len)
			copied = 0;
		else
			folio_mark_uptodate(folio);
	}

	end = pos + copied;

	if (copied) {
		flush_dcache_folio(folio);
		folio_mark_dirty(folio);
	}
	folio_unlock(folio);
	folio_put(folio);

	/*
	 * Grow the file.  hammer2_resize_meta() updates ip->meta.size, marks
	 * the inode modified (so the new size is flushed) and migrates
	 * embedded inode data into a real block when the write crosses the
	 * HAMMER2_EMBEDDED_BYTES boundary.
	 */
	if (end > i_size_read(inode)) {
		i_size_write(inode, end);
		hammer2_resize_meta(inode, ip, end, pos);
	}
	return copied;
}

/*
 * Write one dirty folio back to media.  Each 64KiB logical block the folio
 * overlaps is read-modify-written so bytes owned by sibling folios (or the
 * embedded-inode region) survive.
 */
static int
hammer2_writeback_folio(struct inode *inode, struct folio *folio, char *blk,
    int nowait)
{
	hammer2_inode_t *ip = VTOI(inode);
	hammer2_pfs_t *pmp = ip->pmp;
	loff_t fpos = folio_pos(folio);
	size_t fsize = folio_size(folio);
	loff_t isize = i_size_read(inode);
	size_t done = 0;
	int error = 0;

	while (done < fsize) {
		loff_t pos = fpos + done;
		hammer2_key_t lbase = pos & ~(hammer2_key_t)HAMMER2_PBUFMASK;
		int loff = (int)(pos - lbase);
		size_t chunk = HAMMER2_PBUFSIZE - loff;

		if (chunk > fsize - done)
			chunk = fsize - done;
		if (pos >= isize)		/* nothing live left in the folio */
			break;

		/*
		 * Serialize the whole read-modify-write of this logical block.
		 * Two threads (writeback kworker + the inline sync from
		 * hammer2_pfs_memory_wait) otherwise interleave read/overlay/write
		 * on the same 64KiB block: the later write is assembled from a
		 * stale read and clobbers the earlier one, while the blockref check
		 * code is whichever setcheck landed last.  Media then holds a splice
		 * of two versions with a check matching neither.
		 */
		/*
		 * Transaction OUTSIDE the rmw_lock, deliberately.  Holding the lock
		 * across hammer2_trans_init() deadlocks: the flusher holds an ISFLUSH
		 * transaction and then wants rmw_lock (write_inode_now ->
		 * hammer2_writepages), while the rmw_lock holder waits for a BUFCACHE
		 * transaction that ISFLUSH blocks.  Acquiring the transaction first
		 * means nothing ever blocks on a transaction while holding rmw_lock,
		 * so the lock holder always makes progress.
		 */
		hammer2_trans_init(pmp, HAMMER2_TRANS_BUFCACHE);
		hammer2_mtx_ex(&ip->rmw_lock);
		if (loff != 0 || chunk != HAMMER2_PBUFSIZE) {
			/*
			 * Read-modify-write.  deferral must NOT fall into the
			 * zero-fill below that would write zeroes over the part
			 * of the block this folio does not cover.  Propagate EAGAIN
			 * and let the caller redirty the folio instead.
			 */
			error = hammer2_strategy_block(inode, lbase, blk,
			    BIO_READ, nowait);
			if (error == -EAGAIN) {
				hammer2_mtx_unlock(&ip->rmw_lock);
				hammer2_trans_done(pmp, HAMMER2_TRANS_BUFCACHE);
				break;
			}
			if (error != 0) {
				memset(blk, 0, HAMMER2_PBUFSIZE);
				error = 0;
			}
		} else {
			memset(blk, 0, HAMMER2_PBUFSIZE);
		}
		memcpy_from_folio(blk + loff, folio, done, chunk);

		error = hammer2_strategy_block(inode, lbase, blk, BIO_WRITE,
		    nowait);
		hammer2_mtx_unlock(&ip->rmw_lock);
		hammer2_trans_done(pmp, HAMMER2_TRANS_BUFCACHE);
		if (error)
			break;
		done += chunk;
	}
	return error;
}

static int
hammer2_writepages(struct address_space *mapping,
    struct writeback_control *wbc)
{
	struct inode *inode = mapping->host;
	struct folio *folio = NULL;
	char *blk;
	int error = 0;
	int nowait;

	blk = kmalloc(HAMMER2_PBUFSIZE, GFP_KERNEL);
	if (!blk)
		return -ENOMEM;

	/*
	 * Background writeback (WB_SYNC_NONE) may defer a folio whose inode is
	 * busy in another XOP: blocking here would hold the folio in writeback
	 * state (and a BUFCACHE transaction) while the interlock holder may be
	 * waiting on exactly that, which deadlocks.  Data-integrity writeback
	 * (WB_SYNC_ALL, i.e. fsync/sync) must not skip anything, so it blocks.
	 */
	nowait = (wbc->sync_mode == WB_SYNC_NONE);

	while ((folio = writeback_iter(mapping, wbc, folio, &error))) {
		folio_start_writeback(folio);
		folio_unlock(folio);
		error = hammer2_writeback_folio(inode, folio, blk, nowait);
		if (error == -EAGAIN) {
			/* Interlock busy: leave it dirty and come back later. */
			folio_end_writeback(folio);
			filemap_dirty_folio(mapping, folio);
			error = 0;
			continue;
		}
		folio_end_writeback(folio);
	}

	kfree(blk);
	return error;
}

/*
 * write(2) entry point.  generic_file_write_iter() drives the page cache
 * (write_begin/copy/write_end); we retain HAMMER2's dirty-chain write
 * throttle and read-only guard around it, and stamp mtime once per call so
 * heavy writes do not spin a transaction per folio.  The throttle runs
 * before generic_file_write_iter() takes the inode lock, matching the old
 * block path's ordering so the inline flush cannot self-deadlock.
 */
static ssize_t
hammer2_write_iter(struct kiocb *iocb, struct iov_iter *from)
{
	struct inode *inode = file_inode(iocb->ki_filp);
	hammer2_inode_t *ip = VTOI(inode);
	hammer2_pfs_t *pmp = ip->pmp;
	struct timespec64 ts;
	uint64_t mtime;
	ssize_t ret;

	if (pmp->rdonly || (pmp->flags & HAMMER2_PMPF_EMERG))
		return -EROFS;

	hammer2_pfs_memory_wait(pmp);

	ret = generic_file_write_iter(iocb, from);

	if (ret > 0) {
		hammer2_trans_init(pmp, 0);
		hammer2_mtx_ex(&ip->lock);
		hammer2_update_time(&mtime);
		hammer2_inode_modify(ip);
		ip->meta.mtime = mtime;
		ip->meta.ctime = mtime;
		hammer2_mtx_unlock(&ip->lock);
		hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);
		/*
		 * Mirror the times into the VFS inode, from the SAME value.  stat()
		 * reads the VFS inode (generic_fillattr), so updating only ip->meta
		 * left a stale mtime/ctime visible until the inode was re-read 
		 * which then looked like the times had spontaneously changed across a
		 * remount.  Also broke relatime: the VFS saw mtime slightly OLDER
		 * than atime, so atime was never updated on read.  (generic/003)
		 */
		hammer2_time_to_timespec(mtime, &ts);
		inode_set_mtime_to_ts(inode, ts);
		inode_set_ctime_to_ts(inode, ts);
	}
	return ret;
}

/*
 * Shared helper for create / mkdir / mknod / symlink: build the inode and its
 * directory entry, then instantiate the dentry.  Returns the new struct inode
 * (referenced) or an ERR_PTR.
 */
static struct inode *
hammer2_create_obj(struct inode *dir, struct dentry *dentry, umode_t mode,
    dev_t rdev, const char *symlink_target)
{
	hammer2_inode_t *dip = VTOI(dir);
	hammer2_pfs_t *pmp = dip->pmp;
	hammer2_inode_t *nip = NULL;
	struct inode *inode = NULL;
	struct vattr va;
	struct ucred cred;
	hammer2_tid_t inum;
	uint64_t mtime;
	int error;

	if (pmp->rdonly || (pmp->flags & HAMMER2_PMPF_EMERG))
		return ERR_PTR(-EROFS);
	if (dentry->d_name.len > HAMMER2_NAME_MAX)
		return ERR_PTR(-ENAMETOOLONG);

	/*
	 * hammer2_inode_create_normal() dereferences cred (via
	 * vop_helper_create_uid -> cred->cr_uid), so a real ucred is required.
	 */
	memset(&cred, 0, sizeof(cred));
	cred.uid = from_kuid(&init_user_ns, current_fsuid());
	cred.gid = from_kgid(&init_user_ns, current_fsgid());

	/*
	 * SGID inheritance, as inode_init_owner() would do it: a new entry in a
	 * setgid directory takes the directory's group, and a new SUBDIRECTORY
	 * also inherits the setgid bit itself.  Without this, xfstests
	 * generic/314 sees drwxr-xr-x where drwxr-sr-x is required.
	 */
	if (dir->i_mode & S_ISGID) {
		cred.gid = from_kgid(&init_user_ns, dir->i_gid);
		if (S_ISDIR(mode))
			mode |= S_ISGID;
	}

	memset(&va, 0, sizeof(va));
	va.va_type = hammer2_ifmt_to_dtype(mode);
	va.va_mode = mode;
	va.va_uid = cred.uid;
	va.va_gid = cred.gid;
	va.va_rdev = rdev;

	hammer2_trans_init(pmp, 0);
	inum = hammer2_trans_newinum(pmp);

	hammer2_inode_lock(dip, 0);
	nip = hammer2_inode_create_normal(dip, &va, &cred, inum, &error);
	if (error)
		error = hammer2_error_to_errno(error);
	else
		error = hammer2_dirent_create(dip, dentry->d_name.name,
		    dentry->d_name.len, nip->meta.inum, nip->meta.type);

	if (error) {
		if (nip) {
			hammer2_inode_unlink_finisher(nip, NULL);
			hammer2_inode_unlock(nip);
			nip = NULL;
		}
		inode = ERR_PTR(-error);
	} else {
		hammer2_inode_depend(dip, nip);
		inode = hammer2_iget(dir->i_sb, nip);
		hammer2_inode_unlock(nip);
	}

	if (!IS_ERR_OR_NULL(inode) && symlink_target) {
		/* Write the symlink body as embedded/file data. */
		size_t tlen = strlen(symlink_target);
		char *blk = kzalloc(HAMMER2_PBUFSIZE, GFP_KERNEL);

		if (!blk) {
			error = ENOMEM;
			inode = ERR_PTR(-ENOMEM);
		} else {
			hammer2_mtx_ex(&nip->lock);
			hammer2_inode_modify(nip);
			nip->osize = nip->meta.size;
			nip->meta.size = tlen;
			if (tlen > HAMMER2_EMBEDDED_BYTES) {
				atomic_set_int(&nip->flags,
				    HAMMER2_INODE_RESIZED);
				hammer2_inode_chain_sync(nip);
			}
			hammer2_mtx_unlock(&nip->lock);

			memcpy(blk, symlink_target, tlen);
			hammer2_trans_init(pmp, HAMMER2_TRANS_BUFCACHE);
			hammer2_strategy_block(inode, 0, blk, BIO_WRITE, 0);
			hammer2_trans_done(pmp, HAMMER2_TRANS_BUFCACHE);
			kfree(blk);
			i_size_write(inode, tlen);
		}
	}

	if (!IS_ERR_OR_NULL(inode)) {
		hammer2_update_time(&mtime);
		hammer2_inode_modify(dip);
		dip->meta.mtime = mtime;
		dip->meta.ctime = mtime;
		if (S_ISDIR(mode) && dip->meta.nlinks != 1) {
			++dip->meta.nlinks;
			inc_nlink(dir);
		}
		inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	}

	hammer2_inode_unlock(dip);
	hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);
	return inode;
}

static int
hammer2_create(struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode, bool excl)
{
	struct inode *inode = hammer2_create_obj(dir, dentry, mode, 0, NULL);

	if (IS_ERR(inode))
		return PTR_ERR(inode);
	d_instantiate(dentry, inode);
	return 0;
}

static int
hammer2_mknod(struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode, dev_t rdev)
{
	struct inode *inode = hammer2_create_obj(dir, dentry, mode, rdev, NULL);

	if (IS_ERR(inode))
		return PTR_ERR(inode);
	d_instantiate(dentry, inode);
	return 0;
}

static struct dentry *
hammer2_mkdir(struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, umode_t mode)
{
	struct inode *inode = hammer2_create_obj(dir, dentry, mode | S_IFDIR,
	    0, NULL);

	if (IS_ERR(inode))
		return ERR_CAST(inode);
	d_instantiate(dentry, inode);
	return NULL;
}

static int
hammer2_symlink(struct mnt_idmap *idmap, struct inode *dir,
    struct dentry *dentry, const char *symname)
{
	struct inode *inode;

	if (strlen(symname) >= HAMMER2_PBUFSIZE)
		return -ENAMETOOLONG;
	inode = hammer2_create_obj(dir, dentry, S_IFLNK | 0777, 0, symname);
	if (IS_ERR(inode))
		return PTR_ERR(inode);
	d_instantiate(dentry, inode);
	return 0;
}

static int
hammer2_link(struct dentry *old_dentry, struct inode *dir,
    struct dentry *dentry)
{
	struct inode *inode = d_inode(old_dentry);
	hammer2_inode_t *tdip = VTOI(dir);
	hammer2_inode_t *ip = VTOI(inode);
	hammer2_pfs_t *pmp = tdip->pmp;
	uint64_t cmtime;
	int error;

	if (pmp->rdonly || (pmp->flags & HAMMER2_PMPF_EMERG))
		return -EROFS;
	if (dentry->d_name.len > HAMMER2_NAME_MAX)
		return -ENAMETOOLONG;
	if (ip->meta.nlinks >= HAMMER2_LINK_MAX)
		return -EMLINK;

	hammer2_trans_init(pmp, 0);
	hammer2_inode_lock(tdip, 0);
	hammer2_inode_lock(ip, 0);
	hammer2_update_time(&cmtime);

	error = hammer2_dirent_create(tdip, dentry->d_name.name,
	    dentry->d_name.len, ip->meta.inum, ip->meta.type);
	if (error == 0) {
		hammer2_inode_modify(ip);
		++ip->meta.nlinks;
		ip->meta.ctime = cmtime;
		hammer2_inode_modify(tdip);
		tdip->meta.mtime = cmtime;
		tdip->meta.ctime = cmtime;
	}
	hammer2_inode_unlock(ip);
	hammer2_inode_unlock(tdip);
	hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);

	if (error == 0) {
		inc_nlink(inode);
		inode_set_ctime_current(inode);
		/* The directory gained an entry: refresh its VFS times too. */
		inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
		ihold(inode);
		d_instantiate(dentry, inode);
	}
	return -error;
}

/* Shared unlink/rmdir backend. */
static int
hammer2_unlink_obj(struct inode *dir, struct dentry *dentry, int isdir)
{
	hammer2_inode_t *dip = VTOI(dir);
	struct inode *inode = d_inode(dentry);
	hammer2_inode_t *ip = VTOI(inode);
	hammer2_pfs_t *pmp = dip->pmp;
	hammer2_xop_unlink_t *xop;
	uint64_t mtime;
	int error;

	if (pmp->rdonly)
		return -EROFS;

	hammer2_trans_init(pmp, 0);
	hammer2_inode_lock(dip, 0);

	xop = hammer2_xop_alloc(dip, HAMMER2_XOP_MODIFYING);
	hammer2_xop_setname(&xop->head, dentry->d_name.name, dentry->d_name.len);
	xop->isdir = isdir;
	xop->dopermanent = 0;
	hammer2_xop_start(&xop->head, &hammer2_unlink_desc);
	error = hammer2_xop_collect(&xop->head, 0);
	error = hammer2_error_to_errno(error);

	if (error == 0) {
		ip = hammer2_inode_get(dip->pmp, &xop->head, -1, -1);
		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
		if (ip) {
			hammer2_inode_unlink_finisher(ip, NULL);
			hammer2_inode_depend(dip, ip);
			hammer2_inode_unlock(ip);
		}
	} else {
		hammer2_xop_retire(&xop->head, HAMMER2_XOPMASK_VOP);
	}

	if (error == 0) {
		hammer2_update_time(&mtime);
		hammer2_inode_modify(dip);
		dip->meta.mtime = mtime;
		dip->meta.ctime = mtime;
		if (isdir && dip->meta.nlinks != 1)
			--dip->meta.nlinks;
	}
	hammer2_inode_unlock(dip);
	hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);

	if (error == 0) {
		if (isdir) {
			clear_nlink(inode);
			if (dir->i_nlink > 2)
				drop_nlink(dir);
		} else {
			if (inode->i_nlink)
				drop_nlink(inode);
		}
		inode_set_ctime_current(inode);
		inode_set_mtime_to_ts(dir, inode_set_ctime_current(dir));
	}
	return -error;
}

static int
hammer2_unlink(struct inode *dir, struct dentry *dentry)
{
	return hammer2_unlink_obj(dir, dentry, 0);
}

static int
hammer2_rmdir(struct inode *dir, struct dentry *dentry)
{
	return hammer2_unlink_obj(dir, dentry, 1);
}

static int
hammer2_rename(struct mnt_idmap *idmap, struct inode *fdir,
    struct dentry *fdentry, struct inode *tdir, struct dentry *tdentry,
    unsigned int flags)
{
	hammer2_inode_t *fdip = VTOI(fdir);
	hammer2_inode_t *tdip = VTOI(tdir);
	hammer2_inode_t *fip = VTOI(d_inode(fdentry));
	hammer2_inode_t *tip = d_inode(tdentry) ? VTOI(d_inode(tdentry)) : NULL;
	hammer2_pfs_t *pmp = fdip->pmp;
	hammer2_xop_nrename_t *xop4;
	hammer2_xop_scanlhc_t *sxop;
	hammer2_key_t tlhc, lhcbase;
	uint64_t mtime;
	int error;

	if (flags & ~RENAME_NOREPLACE)
		return -EINVAL;
	if (pmp->rdonly || (pmp->flags & HAMMER2_PMPF_EMERG))
		return -EROFS;
	/* The target name is new; the source already exists so it is in range. */
	if (tdentry->d_name.len > HAMMER2_NAME_MAX ||
	    fdentry->d_name.len > HAMMER2_NAME_MAX)
		return -ENAMETOOLONG;

	hammer2_trans_init(pmp, 0);
	hammer2_inode_ref(fip);

	/* Lock the involved inodes (order by pointer to avoid deadlock). */
	hammer2_inode_lock(fdip, 0);
	if (tdip != fdip)
		hammer2_inode_lock(tdip, 0);
	hammer2_inode_lock(fip, 0);
	if (tip)
		hammer2_inode_lock(tip, 0);

	/* Resolve the target name's collision space. */
	tlhc = hammer2_dirhash(tdentry->d_name.name, tdentry->d_name.len);
	lhcbase = tlhc;
	sxop = hammer2_xop_alloc(tdip, HAMMER2_XOP_MODIFYING);
	sxop->lhc = tlhc;
	hammer2_xop_start(&sxop->head, &hammer2_scanlhc_desc);
	while ((error = hammer2_xop_collect(&sxop->head, 0)) == 0) {
		if (tlhc != sxop->head.cluster.focus->bref.key)
			break;
		++tlhc;
	}
	error = hammer2_error_to_errno(error);
	hammer2_xop_retire(&sxop->head, HAMMER2_XOPMASK_VOP);
	if (error == ENOENT || error == -ENOENT) {
		++tlhc;
		error = 0;
	}
	if (error == 0 && ((lhcbase ^ tlhc) & ~HAMMER2_DIRHASH_LOMASK))
		error = ENOSPC;		/* positive; negated at return */

	if (error == 0) {
		xop4 = hammer2_xop_alloc(fdip, HAMMER2_XOP_MODIFYING);
		xop4->lhc = tlhc;
		xop4->ip_key = fip->meta.name_key;
		hammer2_xop_setip2(&xop4->head, fip);
		hammer2_xop_setip3(&xop4->head, tdip);
		if (tip && tip->meta.type == HAMMER2_OBJTYPE_DIRECTORY)
			hammer2_xop_setip4(&xop4->head, tip);
		hammer2_xop_setname(&xop4->head, fdentry->d_name.name,
		    fdentry->d_name.len);
		hammer2_xop_setname2(&xop4->head, tdentry->d_name.name,
		    tdentry->d_name.len);
		hammer2_xop_start(&xop4->head, &hammer2_nrename_desc);
		error = hammer2_xop_collect(&xop4->head, 0);
		error = hammer2_error_to_errno(error);
		hammer2_xop_retire(&xop4->head, HAMMER2_XOPMASK_VOP);
		if (error == ENOENT || error == -ENOENT)
			error = 0;

		if (error == 0 &&
		    (fip->meta.name_key & HAMMER2_DIRHASH_VISIBLE)) {
			hammer2_inode_modify(fip);
			fip->meta.name_len = tdentry->d_name.len;
			fip->meta.name_key = tlhc;
		}
		if (error == 0) {
			hammer2_inode_modify(fip);
			fip->meta.iparent = tdip->meta.inum;
		}
	}

	if (error == 0 && tip)
		hammer2_inode_unlink_finisher(tip, NULL);

	if (error == 0) {
		hammer2_update_time(&mtime);
		/*
		 * POSIX: rename() updates the ctime of the renamed file and the
		 * mtime AND ctime of both parent directories.  Only the two mtimes
		 * were being set, and nothing refreshed the VFS inodes, so stat()
		 * kept reporting the old values.  fstest rename/23.t.
		 */
		hammer2_inode_modify(fip);
		fip->meta.ctime = mtime;
		/*
		 * If the rename replaced an existing file, that inode just lost a
		 * link, so its ctime changes too observable through any other
		 * link to it.  fstest rename/23.t.
		 */
		if (tip) {
			hammer2_inode_modify(tip);
			tip->meta.ctime = mtime;
			if (d_inode(tdentry))
				inode_set_ctime_current(d_inode(tdentry));
		}
		hammer2_inode_modify(fdip);
		fdip->meta.mtime = mtime;
		fdip->meta.ctime = mtime;
		if (fip->meta.type == HAMMER2_OBJTYPE_DIRECTORY &&
		    fdip->meta.nlinks != 1)
			--fdip->meta.nlinks;
		hammer2_inode_modify(tdip);
		tdip->meta.mtime = mtime;
		tdip->meta.ctime = mtime;
		inode_set_ctime_current(d_inode(fdentry));
		inode_set_mtime_to_ts(fdir, inode_set_ctime_current(fdir));
		if (fdir != tdir)
			inode_set_mtime_to_ts(tdir, inode_set_ctime_current(tdir));
		if (fip->meta.type == HAMMER2_OBJTYPE_DIRECTORY &&
		    tdip->meta.nlinks != 1)
			++tdip->meta.nlinks;
	}

	if (tip)
		hammer2_inode_unlock(tip);
	hammer2_inode_unlock(fip);
	if (tdip != fdip)
		hammer2_inode_unlock(tdip);
	hammer2_inode_unlock(fdip);
	hammer2_inode_drop(fip);
	hammer2_trans_done(pmp, HAMMER2_TRANS_SIDEQ);

	if (error == 0 && tip && d_inode(tdentry)) {
		struct inode *tinode = d_inode(tdentry);

		if (S_ISDIR(tinode->i_mode))
			clear_nlink(tinode);
		else if (tinode->i_nlink)
			drop_nlink(tinode);
	}
	if (error == 0 && fip->meta.type == HAMMER2_OBJTYPE_DIRECTORY &&
	    fdir != tdir) {
		if (fdir->i_nlink > 2)
			drop_nlink(fdir);
		inc_nlink(tdir);
	}
	return -error;
}

/* ------------------------------------------------------------------------ */
/* fsync								    */
/* ------------------------------------------------------------------------ */

/*
 * Force this filesystem's device buffers all the way to media.
 *
 * hammer2_dev_bwrite() finishes with mark_buffer_dirty(): the data is only in
 * the block device's page cache, not on the platter.  fsync() therefore
 * returned success while a power failure still lost the data which is what
 * the xfstests crash/log-replay tests (generic/034, 056, 065, 073, 090, 101,
 * 104, 106, 107, 321, 322, 325, 335, 336 ...) were all detecting.
 */
static int
hammer2_flush_devices(hammer2_pfs_t *pmp)
{
	hammer2_dev_t *hmp;
	struct block_device *bdev;
	int i, j, error = 0, e;

	for (i = 0; i < HAMMER2_MAXCLUSTER; ++i) {
		hmp = pmp->pfs_hmps[i];
		if (hmp == NULL)
			continue;
		for (j = 0; j < hmp->nvolumes; ++j) {
			if (hmp->volumes[j].dev == NULL)
				continue;
			bdev = hmp->volumes[j].dev->bdev;
			if (bdev == NULL)
				continue;
			e = sync_blockdev(bdev);
			if (e && error == 0)
				error = e;
			e = blkdev_issue_flush(bdev);
			if (e && error == 0)
				error = e;
		}
	}
	return error;
}

static int
hammer2_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct inode *inode = file_inode(file);
	hammer2_inode_t *ip = VTOI(inode);
	int error, error2;

	error = file_write_and_wait_range(file, start, end);
	if (error)
		return error;

	hammer2_trans_init(ip->pmp, 0);
	hammer2_inode_lock(ip, 0);
	error = 0;
	if (ip->flags & (HAMMER2_INODE_RESIZED | HAMMER2_INODE_MODIFIED))
		error = hammer2_inode_chain_sync(ip);
	error2 = hammer2_inode_chain_flush(ip, HAMMER2_XOP_INODE_STOP);
	if (error2)
		error = error2;
	hammer2_inode_unlock(ip);
	hammer2_trans_done(ip->pmp, 0);

	/*
	 * inode's chains are now consistent, but the topology only becomes
	 * reachable once the volume header is rewritten, and none of it is on
	 * media until the device buffers are pushed out.  Do both with the
	 * inode lock DROPPED hammer2_vfs_sync_pmp() takes inode locks and
	 * deadlocks if called while holding one.
	 */
	if (error == 0 && hammer2_fsync_durable) {
		hammer2_vfs_sync_pmp(ip->pmp, MNT_WAIT);
		return -hammer2_flush_devices(ip->pmp);
	}

	return -hammer2_error_to_errno(error);
}

/* ------------------------------------------------------------------------ */
/* ioctl								    */
/* ------------------------------------------------------------------------ */

/*
 * HAMMER2 ioctls (used by the userland `hammer2` tool: pfs-list, snapshot,
 * etc.).  The BSD handler (hammer2_ioctl_impl, reached via
 * hammer2_ioctl_linux) expects the payload already copied into a kernel
 * buffer, so we marshal it in/out here based on the _IOC_SIZE/_IOC_DIR
 * encoded in the command (Linux _IOC encoding, which matches the userland
 * tool built against glibc's <sys/ioctl.h>).
 */
static long
hammer2_unlocked_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct inode *inode = file_inode(file);
	void __user *uarg = (void __user *)arg;
	unsigned int size = _IOC_SIZE(cmd);
	void *kdata = NULL;
	int fflag, error;

	if (size > PAGE_SIZE)		/* sanity: all HAMMER2 ioctl structs are small */
		return -EINVAL;
	if (size) {
		kdata = kzalloc(size, GFP_KERNEL);
		if (!kdata)
			return -ENOMEM;
		if ((_IOC_DIR(cmd) & _IOC_WRITE) &&
		    copy_from_user(kdata, uarg, size)) {
			kfree(kdata);
			return -EFAULT;
		}
	}

	/* BSD fflag bits: FREAD = 1, FWRITE = 2. */
	fflag = ((file->f_mode & FMODE_READ) ? 1 : 0) |
		((file->f_mode & FMODE_WRITE) ? 2 : 0);

	error = hammer2_ioctl_linux(inode, cmd, kdata, fflag);
	if (error > 0)
		error = -error;		/* BSD positive errno -> Linux negative */

	if (error == 0 && size && (_IOC_DIR(cmd) & _IOC_READ) &&
	    copy_to_user(uarg, kdata, size))
		error = -EFAULT;

	kfree(kdata);
	return error;
}

/* ------------------------------------------------------------------------ */
/* Superblock operations						    */
/* ------------------------------------------------------------------------ */

static int
hammer2_linux_statfs(struct dentry *dentry, struct kstatfs *buf)
{
	struct super_block *sb = dentry->d_sb;
	hammer2_pfs_t *pmp = sb->s_fs_info;
	struct h2statfs *h2;		/* ~2KB keep off the kernel stack */
	int error;

	h2 = kzalloc(sizeof(*h2), GFP_KERNEL);
	if (!h2)
		return -ENOMEM;
	error = hammer2_statfs(pmp->mp, h2);
	if (error) {
		kfree(h2);
		return -error;
	}

	buf->f_type = HAMMER2_VOLUME_ID_HBO;
	buf->f_bsize = HAMMER2_PBUFSIZE;
	buf->f_blocks = h2->f_blocks;
	buf->f_bfree = h2->f_bfree;
	buf->f_bavail = h2->f_bavail;
	buf->f_files = h2->f_files;

	/*
	 * DragonFly withholds the ~5% reserve from non-root callers (keyed on
	 * cred->cr_uid).  On Linux f_bavail is precisely "space available to
	 * unprivileged users", so hold the reserve back from callers without
	 * CAP_SYS_RESOURCE; privileged callers see full free space and may
	 * write into the reserve (the write-path enospace check enforces the
	 * same boundary).  f_bfree/f_blocks stay the raw totals.
	 */
	if (!capable(CAP_SYS_RESOURCE)) {
		hammer2_dev_t *hmp = pmp->pfs_hmps[0];
		u64 reserve = hmp ? (hmp->free_reserved / HAMMER2_PBUFSIZE) : 0;

		if (buf->f_bavail > reserve)
			buf->f_bavail -= reserve;
		else
			buf->f_bavail = 0;
	}
	buf->f_ffree = h2->f_ffree;
	buf->f_namelen = HAMMER2_NAME_MAX;
	kfree(h2);
	return 0;
}

static int
hammer2_sync_fs(struct super_block *sb, int wait)
{
	hammer2_pfs_t *pmp = sb->s_fs_info;
	int error;

	error = hammer2_sync(pmp->mp, wait ? MNT_WAIT : MNT_NOWAIT);
	if (error == 0 && wait && hammer2_fsync_durable)
		error = hammer2_flush_devices(pmp);
	return -error;
}

static void
hammer2_evict_inode(struct inode *inode)
{
	hammer2_inode_t *ip = VTOI(inode);

	truncate_inode_pages_final(&inode->i_data);
	clear_inode(inode);

	if (ip) {
		/*
		 * If the inode was unlinked while open, schedule the on-media
		 * deletion now that the last reference is going away.
		 */
		hammer2_inode_lock(ip, 0);
		if ((ip->flags & HAMMER2_INODE_ISUNLINKED) &&
		    !(ip->flags & HAMMER2_INODE_DELETING)) {
			atomic_set_int(&ip->flags, HAMMER2_INODE_DELETING);
			hammer2_inode_delayed_sideq(ip);
		}
		if (ip->vp == inode)
			ip->vp = NULL;
		inode->i_private = NULL;
		hammer2_inode_unlock(ip);
		hammer2_inode_drop(ip);		/* the vnode reference */
	}
}

static void
hammer2_put_super(struct super_block *sb)
{
	hammer2_pfs_t *pmp = sb->s_fs_info;
	struct mount *mp;

	if (!pmp)
		return;
	mp = pmp->mp;
	hammer2_unmount(mp, 0);
	sb->s_fs_info = NULL;
	if (mp) {
		kfree(mp->mnt_optnew);
		kfree(mp);
	}
}

static const struct super_operations hammer2_super_ops = {
	.statfs		= hammer2_linux_statfs,
	.sync_fs	= hammer2_sync_fs,
	.evict_inode	= hammer2_evict_inode,
	.put_super	= hammer2_put_super,
};

/* ------------------------------------------------------------------------ */
/* Operation vectors							    */
/* ------------------------------------------------------------------------ */

/*
 * ->update_time: capture VFS-side timestamp updates into ip->meta.
 *
 * VFS updates inode->i_atime itself (touch_atime -> generic_update_time)
 * and, with no ->update_time here, nothing ever wrote that back to ip->meta.
 * stat() therefore reported the new atime until the inode was re-read, at
 * which point it reverted to the on-media value xfstests generic/003 sees
 * that as "access time has changed after remount".  The same gap let atime
 * appear to change on a READ-ONLY mount, where nothing should change at all.
 */
static int
hammer2_update_time_op(struct inode *inode, enum fs_update_time type,
    unsigned int flags)
{
	hammer2_inode_t *ip = VTOI(inode);
	hammer2_pfs_t *pmp = ip->pmp;
	struct timespec64 ts;

	if (pmp == NULL || pmp->rdonly || (pmp->flags & HAMMER2_PMPF_EMERG))
		return 0;

	generic_update_time(inode, type, flags);

	/*
	 * Deliberately NO transaction here.  This runs on essentially every
	 * read (atime), and wrapping each one in hammer2_trans_init/done cost
	 * enough to turn a 1-minute xfstest into a 30-minute one.
	 * hammer2_inode_modify() only sets INODE_MODIFIED and queues the inode
	 * on SIDEQ, which needs no transaction; the next sync persists it.
	 */
	/*
	 * Best-effort: TRY the inode lock.  This runs on nearly every read, and
	 * taking ip->lock exclusively there serialized concurrent readers badly
	 * enough to stretch one xfstest past 30 minutes.  If another thread holds
	 * the inode we simply skip persisting this atime the VFS inode already
	 * has it, and atime is advisory.
	 */
	if (hammer2_mtx_ex_try(&ip->lock) != 0)
		return 0;
	hammer2_inode_modify(ip);
	/* 7.1 collapsed the S_* mask into enum fs_update_time. */
	if (type == FS_UPD_ATIME) {
		ts = inode_get_atime(inode);
		ip->meta.atime = hammer2_timespec_to_time(&ts);
	} else {
		ts = inode_get_mtime(inode);
		ip->meta.mtime = hammer2_timespec_to_time(&ts);
		ts = inode_get_ctime(inode);
		ip->meta.ctime = hammer2_timespec_to_time(&ts);
	}
	hammer2_mtx_unlock(&ip->lock);

	return 0;
}

static const struct inode_operations hammer2_dir_iops = {
	.update_time	= hammer2_update_time_op,
	.lookup		= hammer2_lookup,
	.create		= hammer2_create,
	.link		= hammer2_link,
	.unlink		= hammer2_unlink,
	.symlink	= hammer2_symlink,
	.mkdir		= hammer2_mkdir,
	.rmdir		= hammer2_rmdir,
	.mknod		= hammer2_mknod,
	.rename		= hammer2_rename,
	.getattr	= hammer2_getattr,
	.setattr	= hammer2_setattr,
};

static const struct inode_operations hammer2_file_iops = {
	.update_time	= hammer2_update_time_op,
	.getattr	= hammer2_getattr,
	.setattr	= hammer2_setattr,
};

static const struct inode_operations hammer2_symlink_iops = {
	.update_time	= hammer2_update_time_op,
	.get_link	= hammer2_get_link,
	.getattr	= hammer2_getattr,
	.setattr	= hammer2_setattr,
};

static const struct inode_operations hammer2_special_iops = {
	.update_time	= hammer2_update_time_op,
	.getattr	= hammer2_getattr,
	.setattr	= hammer2_setattr,
};

static const struct file_operations hammer2_dir_fops = {
	.read		= generic_read_dir,
	.iterate_shared	= hammer2_iterate,
	.llseek		= generic_file_llseek,
	.fsync		= hammer2_fsync,
	.unlocked_ioctl	= hammer2_unlocked_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static const struct file_operations hammer2_file_fops = {
	.llseek		= generic_file_llseek,
	.read_iter	= generic_file_read_iter,
	.write_iter	= hammer2_write_iter,
	.mmap		= generic_file_mmap,
	.fsync		= hammer2_fsync,
	/*
	 * Without these, sendfile(2)/splice(2) fail outright (xfstests
	 * generic/249).  The generic helpers are correct for a page-cache
	 * filesystem using generic_file_read_iter/write_iter.
	 */
	.splice_read	= filemap_splice_read,
	.splice_write	= iter_file_splice_write,
	.unlocked_ioctl	= hammer2_unlocked_ioctl,
	.compat_ioctl	= compat_ptr_ioctl,
};

static const struct address_space_operations hammer2_aops = {
	.read_folio	= hammer2_read_folio,
	.writepages	= hammer2_writepages,
	.write_begin	= hammer2_write_begin,
	.write_end	= hammer2_write_end,
	.dirty_folio	= filemap_dirty_folio,
	/*
	 * Without this the kernel warns "hammer2_aops does not implement
	 * migrate_folio" and page migration fails, so memory compaction cannot
	 * move these pages.  filemap_migrate_folio() is the correct generic
	 * implementation for a filesystem whose folios carry no private data.
	 */
	.migrate_folio	= filemap_migrate_folio,
};

/* ------------------------------------------------------------------------ */
/* Mount / fill_super / registration					    */
/* ------------------------------------------------------------------------ */

static int
hammer2_fill_super(struct super_block *sb, struct fs_context *fc)
{
	struct h2_mount_optlist *ol;
	struct mount *mp;
	hammer2_pfs_t *pmp;
	struct inode *root_inode;
	char *devstr;
	int *hflags;
	int error;

	if (!fc->source)
		return -EINVAL;

	mp = kzalloc(sizeof(*mp), GFP_KERNEL);
	ol = kzalloc(sizeof(*ol), GFP_KERNEL);
	devstr = kstrdup(fc->source, GFP_KERNEL);
	hflags = kzalloc(sizeof(int), GFP_KERNEL);
	if (!mp || !ol || !devstr || !hflags) {
		error = -ENOMEM;
		goto fail_free;
	}

	/* Build the option list hammer2_mount() consumes via vfs_getopt(). */
	ol->count = 3;
	ol->opts[0].name = "from";
	ol->opts[0].value = devstr;
	ol->opts[0].len = strlen(devstr) + 1;
	ol->opts[1].name = "fspath";
	ol->opts[1].value = devstr;
	ol->opts[1].len = strlen(devstr) + 1;
	ol->opts[2].name = "hflags";
	ol->opts[2].value = hflags;
	ol->opts[2].len = sizeof(int);

	mp->mnt_optnew = ol;
	mp->mnt_flag = (fc->sb_flags & SB_RDONLY) ? MNT_RDONLY : 0;
	mp->mnt_iosize_max = MAXPHYS;

	error = hammer2_mount(mp);
	if (error) {
		/*
		 * hammer2_mount() returns positive BSD errnos from its own
		 * logic but can also propagate negative Linux errnos from
		 * helpers like hammer2_open_devvp().  Normalize to negative.
		 */
		if (error > 0)
			error = -error;
		goto fail_free;
	}

	pmp = (hammer2_pfs_t *)mp->mnt_data;
	if (!pmp) {
		error = -EINVAL;
		goto fail_unmount;
	}

	sb->s_fs_info = pmp;
	pmp->sb = sb;
	pmp->mp = mp;

	sb->s_magic = HAMMER2_VOLUME_ID_HBO;
	sb->s_blocksize = HAMMER2_PBUFSIZE;
	sb->s_blocksize_bits = HAMMER2_PBUFRADIX;
	sb->s_maxbytes = MAX_LFS_FILESIZE;
	sb->s_op = &hammer2_super_ops;
	sb->s_time_gran = 1000;		/* HAMMER2 stores microseconds */

	/*
	 * Register a writeback-capable backing_dev_info.  get_tree_nodev()
	 * otherwise leaves sb->s_bdi == noop_backing_dev_info, under which the
	 * VFS writeback machinery never issues ->writepages.  Since v0.39 all
	 * file data is written through the page cache, so without a real bdi the
	 * dirty folios would live only in memory and be silently dropped at
	 * unmount (inode size still commits via the chain path, but the data
	 * blocks read back as zero after remount).  With a real bdi, sync(2) and
	 * unmount flush dirty folios through hammer2_writepages() -> the data
	 * chains before hammer2_sync_fs() commits them to the volume header.
	 */
	error = super_setup_bdi(sb);
	if (error)
		goto fail_unmount;

	if (pmp->rdonly)
		sb->s_flags |= SB_RDONLY;

	/* Root inode (also initializes pmp->inode_tid and root meta). */
	error = hammer2_root(mp, 0, &root_inode);
	if (error) {
		if (error > 0)
			error = -error;
		goto fail_unmount;
	}

	sb->s_root = d_make_root(root_inode);
	if (!sb->s_root) {
		error = -ENOMEM;
		goto fail_unmount;
	}

	/* devstr was copied into pmp/hmp state by hammer2_mount(). */
	kfree(devstr);
	return 0;

fail_unmount:
	hammer2_unmount(mp, 0);
	sb->s_fs_info = NULL;
fail_free:
	kfree(mp);
	kfree(ol);
	kfree(devstr);
	kfree(hflags);
	return error;
}

static int
hammer2_get_tree(struct fs_context *fc)
{
	return get_tree_nodev(fc, hammer2_fill_super);
}

static int
hammer2_reconfigure(struct fs_context *fc)
{
	/* Remount: HAMMER2 currently treats this as a no-op. */
	return 0;
}

static const struct fs_context_operations hammer2_context_ops = {
	.get_tree	= hammer2_get_tree,
	.reconfigure	= hammer2_reconfigure,
};

static int
hammer2_init_fs_context(struct fs_context *fc)
{
	fc->ops = &hammer2_context_ops;
	return 0;
}

static struct file_system_type hammer2_fs_type = {
	.owner			= THIS_MODULE,
	.name			= "hammer2",
	.init_fs_context	= hammer2_init_fs_context,
	.kill_sb		= kill_anon_super,
	.fs_flags		= 0,
};
MODULE_ALIAS_FS("hammer2");

static int __init
hammer2_module_init(void)
{
	int error;

	/*
	 * Run the global initializer that the BSD vfsops table invoked via
	 * .vfs_init: it creates the UMA zones and initializes the global
	 * mount/pfs lists and locks.  hammer2_mount() walks hammer2_mntlist,
	 * so this MUST happen before register_filesystem().
	 */
	error = hammer2_init(NULL);
	if (error)
		return error;

	error = register_filesystem(&hammer2_fs_type);
	if (error) {
		pr_err("hammer2: register_filesystem failed: %d\n", error);
		hammer2_uninit(NULL);
		return error;
	}
	pr_info("hammer2: filesystem registered, version %s\n",
	    HAMMER2_PORT_BUILD);
	return 0;
}

static void __exit
hammer2_module_exit(void)
{
	unregister_filesystem(&hammer2_fs_type);
	rcu_barrier();
	hammer2_uninit(NULL);
	pr_info("hammer2: filesystem unregistered\n");
}

module_init(hammer2_module_init);
module_exit(hammer2_module_exit);

/*
 * Note: the kernel's 1-arg MODULE_VERSION() is shadowed by a 2-arg BSD shim
 * in hammer2_compat.h, so emit the modinfo "version" field directly.
 */
MODULE_INFO(version, HAMMER2_PORT_BUILD);
