/*-
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Copyright (c) 2022-2023 Tomohiro Kusumi <tkusumi@netbsd.org>
 * Copyright (c) 2011-2022 The DragonFly Project.  All rights reserved.
 * All rights reserved.
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

#include "hammer2.h"
#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include <linux/fs.h>
#include <linux/string.h>

/*
 * Linux port: BSD opens a block device via namei()+g_vfs_open().  The
 * modern Linux equivalent is bdev_file_open_by_path(), which returns a
 * struct file * whose underlying inode is the block device.  We stash both
 * the file handle (for fput on close) and the derived block_device * in
 * hammer2_devvp_t.
 */

int
hammer2_open_devvp(void *mp, const hammer2_devvp_list_t *devvpl)
{
	hammer2_devvp_t *e;
	struct file *bdev_file;
	int error = 0;
	int rdonly = 0;	/* TODO: derive from super_block flags in mount path */

	(void)mp;

	TAILQ_FOREACH(e, devvpl, entry) {
		KKASSERT(e->path);
		bdev_file = bdev_file_open_by_path(e->path,
		    rdonly ? BLK_OPEN_READ : (BLK_OPEN_READ | BLK_OPEN_WRITE),
		    NULL, NULL);
		if (IS_ERR(bdev_file)) {
			error = PTR_ERR(bdev_file);
			hprintf("bdev_file_open_by_path(%s) failed: %d\n",
			    e->path, error);
			return error;
		}
		e->bdev_file = bdev_file;
		e->bdev = file_bdev(bdev_file);
		e->open = 1;

		/*
		 * No set_blocksize() needed: HAMMER2's 64K device I/O is done
		 * as PAGE_SIZE chunks via hammer2_dev_bread()/bwrite(), and
		 * __bread()/__getblk() with an explicit PAGE_SIZE return the
		 * correct data regardless of the device's default block size.
		 * (set_blocksize() to 64K -- or even 4K -- is rejected with
		 * -EINVAL on some devices, so we don't rely on it.)
		 */
	}

	return error;
}

int
hammer2_close_devvp(const hammer2_devvp_list_t *devvpl)
{
	hammer2_devvp_t *e;

	TAILQ_FOREACH(e, devvpl, entry) {
		if (e->open) {
			if (e->bdev_file)
				fput(e->bdev_file);
			e->bdev_file = NULL;
			e->bdev = NULL;
			e->open = 0;
		}
	}

	return 0;
}

int
hammer2_init_devvp(const void *mp, const char *blkdevs,
    hammer2_devvp_list_t *devvpl)
{
	hammer2_devvp_t *e;
	const char *p;
	char *path;
	size_t i, prefix_len;
	int error = 0;

	(void)mp;
	KKASSERT(blkdevs); /* may be empty string */
	p = blkdevs;

	path = hmalloc(PATH_MAX, M_TEMP, M_WAITOK | M_ZERO);
	if (!path)
		return -ENOMEM;

	while (1) {
		path[0] = '\0';
		if (*p != '/') {
			strscpy(path, "/dev/", PATH_MAX);
			prefix_len = strlen(path);
		} else {
			prefix_len = 0;
		}

		for (i = prefix_len; i < PATH_MAX - 1; ++i) {
			if (*p == '\0') {
				break;
			} else if (*p == ':') {
				p++;
				break;
			} else {
				path[i] = *p;
				p++;
			}
		}
		path[i] = '\0';

		if (strlen(path) <= strlen("/dev/")) {
			if (strlen(p)) {
				hprintf("ignore incomplete path %s\n", path);
				continue;
			}
			break;	/* end of input */
		}

		e = hmalloc(sizeof(*e), M_HAMMER2, M_WAITOK | M_ZERO);
		if (!e) {
			error = -ENOMEM;
			break;
		}
		e->bdev_file = NULL;
		e->bdev = NULL;
		e->path = kstrdup(path, GFP_KERNEL);
		e->open = 0;
		TAILQ_INSERT_TAIL(devvpl, e, entry);
	}

	hfree(path, M_TEMP, PATH_MAX);
	return error;
}

void
hammer2_cleanup_devvp(hammer2_devvp_list_t *devvpl)
{
	hammer2_devvp_t *e;

	while (!list_empty(&devvpl->head)) {
		e = list_first_entry(&devvpl->head, hammer2_devvp_t, entry);
		list_del(&e->entry);

		if (e->bdev_file) {
			fput(e->bdev_file);
			e->bdev_file = NULL;
			e->bdev = NULL;
		}
		if (e->path) {
			kfree(e->path);
			e->path = NULL;
		}
		hfree(e, M_HAMMER2, sizeof(*e));
	}
}

static int
hammer2_verify_volumes_common(const hammer2_volume_t *volumes,
			      const hammer2_volume_data_t *rootvoldata)
{
	const hammer2_volume_t *vol;
	const char *path;
	loff_t mediasize;
	int i;

	if (rootvoldata->volu_id != HAMMER2_ROOT_VOLUME) {
		hprintf("volume id %d must be %d\n", rootvoldata->volu_id,
		    HAMMER2_ROOT_VOLUME);
		return EINVAL;
	}
	/*
	 * Port note: the BSD code stringified the on-disk fstype UUID and
	 * compared against HAMMER2_UUID_STRING.  Until we wire up a Linux
	 * UUID-to-string helper, skip the textual check; the magic-number
	 * verification in read_volume_header() already catches non-HAMMER2
	 * volumes.
	 */
	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		vol = &volumes[i];
		if (vol->id == -1)
			continue;
		path = vol->dev->path;

		if (!vol->dev->bdev) {
			hprintf("%s has NULL bdev\n", path);
			return EINVAL;
		}
		if (vol->offset == (hammer2_off_t)-1) {
			hprintf("%s has bad offset %016llx\n",
			    path, (long long)vol->offset);
			return EINVAL;
		}
		if (vol->size == (hammer2_off_t)-1) {
			hprintf("%s has bad size %016llx\n",
			    path, (long long)vol->size);
			return EINVAL;
		}

		/* Linux: media size is bdev_nr_bytes() of the block device. */
		mediasize = bdev_nr_bytes(vol->dev->bdev);
		if ((hammer2_off_t)vol->size > (hammer2_off_t)mediasize) {
			hprintf("%s's size %016llx exceeds device size %016llx\n",
			    path, (long long)vol->size,
			    (long long)mediasize);
			return EINVAL;
		}
		if (vol->size == 0) {
			hprintf("%s has size of 0\n", path);
			return EINVAL;
		}
	}

	return 0;
}

static int
hammer2_verify_volumes_1(const hammer2_volume_t *volumes,
    const hammer2_volume_data_t *rootvoldata)
{
	const hammer2_volume_t *vol;
	hammer2_off_t off;
	const char *path;
	int i, nvolumes = 0;

	/* Check initialized volume count. */
	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		vol = &volumes[i];
		if (vol->id != -1)
			nvolumes++;
	}
	if (nvolumes != 1) {
		hprintf("only 1 volume supported\n");
		return (EINVAL);
	}

	/* Check volume header. */
	if (rootvoldata->nvolumes) {
		hprintf("volume count %d must be 0\n", rootvoldata->nvolumes);
		return (EINVAL);
	}
	if (rootvoldata->total_size) {
		hprintf("total size %016llx must be 0\n",
		    (long long)rootvoldata->total_size);
		return (EINVAL);
	}
	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		off = rootvoldata->volu_loff[i];
		if (off) {
			hprintf("volume offset[%d] %016llx must be 0\n",
			    i, (long long)off);
			return (EINVAL);
		}
	}

	/* Check volume. */
	vol = &volumes[HAMMER2_ROOT_VOLUME];
	path = vol->dev->path;
	if (vol->id) {
		hprintf("%s has non zero id %d\n", path, vol->id);
		return (EINVAL);
	}
	if (vol->offset) {
		hprintf("%s has non zero offset %016llx\n",
		    path, (long long)vol->offset);
		return (EINVAL);
	}
	if (vol->size & HAMMER2_VOLUME_ALIGNMASK64) {
		hprintf("%s's size is not %016llx aligned\n",
		    path, (long long)HAMMER2_VOLUME_ALIGN);
		return (EINVAL);
	}

	return (0);
}

static int
hammer2_verify_volumes_2(const hammer2_volume_t *volumes,
    const hammer2_volume_data_t *rootvoldata)
{
	const hammer2_volume_t *vol;
	hammer2_off_t off, total_size = 0;
	const char *path;
	int i, nvolumes = 0;

	/* Check initialized volume count. */
	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		vol = &volumes[i];
		if (vol->id != -1) {
			nvolumes++;
			total_size += vol->size;
		}
	}

	/* Check volume header. */
	if (rootvoldata->nvolumes != nvolumes) {
		hprintf("volume header requires %d devices, %d specified\n",
		    rootvoldata->nvolumes, nvolumes);
		return (EINVAL);
	}
	if (rootvoldata->total_size != total_size) {
		hprintf("total size %016llx does not equal sum of volumes "
		    "%016llx\n",
		    (long long)rootvoldata->total_size, (long long)total_size);
		return (EINVAL);
	}
	for (i = 0; i < nvolumes; ++i) {
		off = rootvoldata->volu_loff[i];
		if (off == (hammer2_off_t)-1) {
			hprintf("volume offset[%d] %016llx must not be -1\n",
			    i, (long long)off);
			return (EINVAL);
		}
	}
	for (i = nvolumes; i < HAMMER2_MAX_VOLUMES; ++i) {
		off = rootvoldata->volu_loff[i];
		if (off != (hammer2_off_t)-1) {
			hprintf("volume offset[%d] %016llx must be -1\n",
			    i, (long long)off);
			return (EINVAL);
		}
	}

	/* Check volumes. */
	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		vol = &volumes[i];
		if (vol->id == -1)
			continue;
		path = vol->dev->path;
		/* Check offset. */
		if (vol->offset & HAMMER2_FREEMAP_LEVEL1_MASK) {
			hprintf("%s's offset %016llx not %016llx aligned\n",
			    path, (long long)vol->offset,
			    (long long)HAMMER2_FREEMAP_LEVEL1_SIZE);
			return (EINVAL);
		}
		/* Check vs previous volume. */
		if (i) {
			if (vol->id <= (vol-1)->id) {
				hprintf("%s has inconsistent id %d\n",
				    path, vol->id);
				return (EINVAL);
			}
			if (vol->offset != (vol-1)->offset + (vol-1)->size) {
				hprintf("%s has inconsistent offset %016llx\n",
				    path, (long long)vol->offset);
				return (EINVAL);
			}
		} else { /* first */
			if (vol->offset) {
				hprintf("%s has non zero offset %016llx\n",
				    path, (long long)vol->offset);
				return (EINVAL);
			}
		}
		/* Check size for non-last and last volumes. */
		if (i != rootvoldata->nvolumes - 1) {
			if (vol->size < HAMMER2_FREEMAP_LEVEL1_SIZE) {
				hprintf("%s's size must be >= %016llx\n",
				    path,
				    (long long)HAMMER2_FREEMAP_LEVEL1_SIZE);
				return (EINVAL);
			}
			if (vol->size & HAMMER2_FREEMAP_LEVEL1_MASK) {
				hprintf("%s's size is not %016llx aligned\n",
				    path,
				    (long long)HAMMER2_FREEMAP_LEVEL1_SIZE);
				return (EINVAL);
			}
		} else { /* last */
			if (vol->size & HAMMER2_VOLUME_ALIGNMASK64) {
				hprintf("%s's size is not %016llx aligned\n",
				    path, (long long)HAMMER2_VOLUME_ALIGN);
				return (EINVAL);
			}
		}
	}

	return (0);
}

static int
hammer2_verify_volumes(const hammer2_volume_t *volumes,
    const hammer2_volume_data_t *rootvoldata)
{
	int error;

	error = hammer2_verify_volumes_common(volumes, rootvoldata);
	if (error)
		return (error);

	if (rootvoldata->version >= HAMMER2_VOL_VERSION_MULTI_VOLUMES)
		return (hammer2_verify_volumes_2(volumes, rootvoldata));
	else
		return (hammer2_verify_volumes_1(volumes, rootvoldata));
}

/*
 * Returns zone# of returned volume header or < 0 on failure.
 */
static int
hammer2_read_volume_header(struct block_device *bdev, const char *path,
    hammer2_volume_data_t *voldata)
{
	hammer2_volume_data_t *vd;
	hammer2_crc32_t crc0, crc1;
	char *vbuf;
	loff_t mediasize;
	off_t blkoff;
	int i, zone = -1;

	KASSERTMSG(bdev != NULL, "NULL bdev");
	mediasize = bdev_nr_bytes(bdev);

	/*
	 * Read each 64K volume header via PAGE_SIZE-chunked transfers (see
	 * hammer2_dev_bread); a single 64K buffer_head is not available.
	 */
	vbuf = kvmalloc(HAMMER2_VOLUME_BYTES, GFP_KERNEL);
	if (!vbuf)
		return (-ENOMEM);

	/*
	 * Up to 4 volume header copies; iterate until we find the most
	 * recent valid one.
	 */
	for (i = 0; i < HAMMER2_NUM_VOLHDRS; ++i) {
		blkoff = (off_t)i * HAMMER2_ZONE_BYTES64;
		if (blkoff >= mediasize)
			continue;

		if (hammer2_dev_bread(bdev, blkoff, vbuf, HAMMER2_VOLUME_BYTES))
			continue;

		vd = (struct hammer2_volume_data *)vbuf;
		/* Verify volume header magic. */
		if ((vd->magic != HAMMER2_VOLUME_ID_HBO) &&
		    (vd->magic != HAMMER2_VOLUME_ID_ABO)) {
			hprintf("%s #%d: bad magic\n", path, i);
			continue;
		}
		if (vd->magic == HAMMER2_VOLUME_ID_ABO) {
			/* XXX: Reversed-endianness filesystem. */
			hprintf("%s #%d: reverse-endian filesystem detected\n",
			    path, i);
			continue;
		}

		/* Verify volume header CRC's. */
		crc0 = vd->icrc_sects[HAMMER2_VOL_ICRC_SECT0];
		crc1 = hammer2_icrc32(vbuf + HAMMER2_VOLUME_ICRC0_OFF,
		    HAMMER2_VOLUME_ICRC0_SIZE);
		if (crc0 != crc1) {
			hprintf("%s #%d: volume header crc mismatch sect0 "
			    "%08x/%08x\n",
			    path, i, crc0, crc1);
			continue;
		}
		crc0 = vd->icrc_sects[HAMMER2_VOL_ICRC_SECT1];
		crc1 = hammer2_icrc32(vbuf + HAMMER2_VOLUME_ICRC1_OFF,
		    HAMMER2_VOLUME_ICRC1_SIZE);
		if (crc0 != crc1) {
			hprintf("%s #%d: volume header crc mismatch sect1 "
			    "%08x/%08x\n",
			    path, i, crc0, crc1);
			continue;
		}
		crc0 = vd->icrc_volheader;
		crc1 = hammer2_icrc32(vbuf + HAMMER2_VOLUME_ICRCVH_OFF,
		    HAMMER2_VOLUME_ICRCVH_SIZE);
		if (crc0 != crc1) {
			hprintf("%s #%d: volume header crc mismatch vh "
			    "%08x/%08x\n",
			    path, i, crc0, crc1);
			continue;
		}

		if (zone == -1 || voldata->mirror_tid < vd->mirror_tid) {
			*voldata = *vd;
			zone = i;
		}
	}

	kvfree(vbuf);

	if (zone == -1) {
		hprintf("%s has no valid volume headers\n", path);
		return (-EINVAL);
	}
	return (zone);
}

static void
hammer2_print_uuid_mismatch(struct uuid *uuid1, struct uuid *uuid2,
    const char *id)
{
	hprintf("volume %s uuid mismatch (per-byte compare)\n", id);
	(void)uuid1;
	(void)uuid2;
}

int
hammer2_init_volumes(const hammer2_devvp_list_t *devvpl,
    hammer2_volume_t *volumes, hammer2_volume_data_t *rootvoldata,
    int *rootvolzone, struct block_device **rootvoldevvp)
{
	hammer2_volume_data_t *voldata;
	hammer2_volume_t *vol;
	hammer2_devvp_t *e;
	struct block_device *bdev;
	struct uuid fsid, fstype;
	const char *path;
	int i, zone, error = 0, v = -1, nvolumes = 0;

	for (i = 0; i < HAMMER2_MAX_VOLUMES; ++i) {
		vol = &volumes[i];
		vol->dev = NULL;
		vol->id = -1;
		vol->offset = (hammer2_off_t)-1;
		vol->size = (hammer2_off_t)-1;
	}

	voldata = hmalloc(sizeof(*voldata), M_HAMMER2, M_WAITOK | M_ZERO);
	bzero(&fsid, sizeof(fsid));
	bzero(&fstype, sizeof(fstype));
	bzero(rootvoldata, sizeof(*rootvoldata));

	TAILQ_FOREACH(e, devvpl, entry) {
		bdev = e->bdev;
		path = e->path;
		KKASSERT(bdev);

		/* Returns negative error or positive zone#. */
		error = hammer2_read_volume_header(bdev, path, voldata);
		if (error < 0) {
			hprintf("failed to read %s's volume header\n", path);
			error = -error;
			goto done;
		}
		zone = error;
		error = 0; /* Reset error. */

		/* Check volume ID. */
		if (voldata->volu_id >= HAMMER2_MAX_VOLUMES) {
			hprintf("%s has bad volume id %d\n",
			    path, voldata->volu_id);
			error = EINVAL;
			goto done;
		}
		vol = &volumes[voldata->volu_id];
		if (vol->id != -1) {
			hprintf("volume id %d already initialized\n",
			    voldata->volu_id);
			error = EINVAL;
			goto done;
		}

		/* All headers must have the same version, nvolumes and uuid. */
		if (v == -1) {
			v = voldata->version;
			nvolumes = voldata->nvolumes;
			fsid = voldata->fsid;
			fstype = voldata->fstype;
		} else {
			if (v != (int)voldata->version) {
				hprintf("volume version mismatch %d vs %d\n",
				    v, (int)voldata->version);
				error = ENXIO;
				goto done;
			}
			if (nvolumes != voldata->nvolumes) {
				hprintf("volume count mismatch %d vs %d\n",
				    nvolumes, voldata->nvolumes);
				error = ENXIO;
				goto done;
			}
			if (bcmp(&fsid, &voldata->fsid, sizeof(fsid))) {
				hammer2_print_uuid_mismatch(&fsid,
				    &voldata->fsid, "fsid");
				error = ENXIO;
				goto done;
			}
			if (bcmp(&fstype, &voldata->fstype, sizeof(fstype))) {
				hammer2_print_uuid_mismatch(&fstype,
				    &voldata->fstype, "fstype");
				error = ENXIO;
				goto done;
			}
		}
		if (v < HAMMER2_VOL_VERSION_MIN ||
		    v > HAMMER2_VOL_VERSION_WIP) {
			hprintf("bad volume version %d\n", v);
			error = EINVAL;
			goto done;
		}

		/* All per-volume tests passed. */
		vol->dev = e;
		vol->id = voldata->volu_id;
		vol->offset = voldata->volu_loff[vol->id];
		vol->size = voldata->volu_size;
		if (vol->id == HAMMER2_ROOT_VOLUME) {
			bcopy(voldata, rootvoldata, sizeof(*rootvoldata));
			*rootvolzone = zone;
			KKASSERT(*rootvoldevvp == NULL);
			*rootvoldevvp = bdev;
		}
		debug_hprintf("\"%s\" zone %d id %d offset %016llx size "
		    "%016llx\n",
		    path, zone, vol->id, (long long)vol->offset,
		    (long long)vol->size);
	}
done:
	if (error == 0) {
		if (!rootvoldata->version) {
			hprintf("root volume not found\n");
			error = EINVAL;
		}
		if (error == 0)
			error = hammer2_verify_volumes(volumes, rootvoldata);
	}
	hfree(voldata, M_HAMMER2, sizeof(*voldata));

	return (error);
}

hammer2_volume_t*
hammer2_get_volume(hammer2_dev_t *hmp, hammer2_off_t offset)
{
	hammer2_volume_t *vol, *ret = NULL;
	int i;

	offset &= ~HAMMER2_OFF_MASK_RADIX;

	/* locking is unneeded until volume-add support */
	//hammer2_voldata_lock(hmp);
	/* Do binary search if users really use this many supported volumes. */
	for (i = 0; i < hmp->nvolumes; ++i) {
		vol = &hmp->volumes[i];
		if ((offset >= vol->offset) &&
		    (offset < vol->offset + vol->size)) {
			ret = vol;
			break;
		}
	}
	//hammer2_voldata_unlock(hmp);

	if (!ret)
		hpanic("no volume for offset %016llx", (long long)offset);

	KKASSERT(ret);
	KKASSERT(ret->dev);
	KKASSERT(ret->dev->path);
	/* ret->dev->bdev is NULL until hammer2_open_devvp() is called. */
	return ret;
}

/*
 * Linux port: BSD's hammer2_access_devvp() runs VOP_ACCESS against the
 * device vnode and falls back to a privileged-mount check.  On Linux,
 * permission to open the block device is enforced by the kernel during
 * bdev_file_open_by_path() based on the caller's credentials; we have no
 * separate access pass to perform here.  hammer2_get/putw_devvp() in BSD
 * toggle GEOM consumer write-access (g_access(dcw)) which has no
 * equivalent in the Linux block layer.  Stub all four to no-ops.
 */
int
hammer2_access_devvp(struct block_device *bdev, int rdonly)
{
	(void)bdev;
	(void)rdonly;
	return 0;
}

int
hammer2_getw_devvp(struct block_device *bdev)
{
	(void)bdev;
	return 0;
}

int
hammer2_putw_devvp(struct block_device *bdev)
{
	(void)bdev;
	return 0;
}
