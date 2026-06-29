/*
 * Linux port of Hammer2 XOP (eXtended OPeration) system
 * Converted from BSD/DragonFly BSD
 */

#include <linux/slab.h>
#include <linux/string.h>
#include <linux/mutex.h>
#include <linux/wait.h>
#include <linux/list.h>
#include <linux/sched.h>
#include <linux/printk.h>
#include <linux/spinlock.h>
#include <linux/completion.h>

#include "hammer2.h"

#define H2XOPDESCRIPTOR(label)					\
	hammer2_xop_desc_t hammer2_##label##_desc = {		\
		.storage_func = hammer2_xop_##label,		\
		.id = #label					\
	}

H2XOPDESCRIPTOR(ipcluster);
H2XOPDESCRIPTOR(readdir);
H2XOPDESCRIPTOR(nresolve);
H2XOPDESCRIPTOR(unlink);
H2XOPDESCRIPTOR(nrename);
H2XOPDESCRIPTOR(scanlhc);
H2XOPDESCRIPTOR(scanall);
H2XOPDESCRIPTOR(lookup);
H2XOPDESCRIPTOR(delete);
H2XOPDESCRIPTOR(inode_mkdirent);
H2XOPDESCRIPTOR(inode_create);
H2XOPDESCRIPTOR(inode_create_det);
H2XOPDESCRIPTOR(inode_create_ins);
H2XOPDESCRIPTOR(inode_destroy);
H2XOPDESCRIPTOR(inode_chain_sync);
H2XOPDESCRIPTOR(inode_unlinkall);
H2XOPDESCRIPTOR(inode_connect);
H2XOPDESCRIPTOR(inode_flush);
H2XOPDESCRIPTOR(strategy_read);
H2XOPDESCRIPTOR(strategy_write);
H2XOPDESCRIPTOR(bmap);

/*
 * Allocate or reallocate XOP FIFO.
 */
static void
hammer2_xop_fifo_alloc(hammer2_xop_fifo_t *fifo, size_t new_nmemb,
    size_t old_nmemb)
{
	size_t new_size, old_size;
	hammer2_chain_t **array;
	int *errors;

	/* Assert new vs old nmemb requirements. */
	BUG_ON(new_nmemb <= old_nmemb);
	
	if (old_nmemb == 0) {
		BUG_ON(fifo->array || fifo->errors);
	} else {
		BUG_ON(!fifo->array);
		BUG_ON(!fifo->errors);
	}

	/* Assert new_nmemb requirements. */
	BUG_ON((new_nmemb & (new_nmemb - 1)) != 0);
	BUG_ON(new_nmemb < HAMMER2_XOPFIFO);

	/* Allocate or reallocate fifo array. */
	new_size = new_nmemb * sizeof(hammer2_chain_t *);
	old_size = old_nmemb * sizeof(hammer2_chain_t *);
	array = kmalloc(new_size, GFP_KERNEL | __GFP_ZERO);
	if (!array) {
		pr_err("hammer2: failed to allocate XOP FIFO array\n");
		return;
	}
	
	if (fifo->array) {
		memcpy(array, fifo->array, old_size);
		kfree(fifo->array);
	}
	fifo->array = array;

	/* Allocate or reallocate fifo errors. */
	new_size = new_nmemb * sizeof(int);
	old_size = old_nmemb * sizeof(int);
	errors = kmalloc(new_size, GFP_KERNEL | __GFP_ZERO);
	if (!errors) {
		pr_err("hammer2: failed to allocate XOP FIFO errors\n");
		kfree(fifo->array);
		fifo->array = NULL;
		return;
	}
	
	if (fifo->errors) {
		memcpy(errors, fifo->errors, old_size);
		kfree(fifo->errors);
	}
	fifo->errors = errors;
}

/*
 * Allocate a XOP request.
 */
void *
hammer2_xop_alloc(hammer2_inode_t *ip, int flags)
{
	hammer2_xop_t *xop;

	xop = kmem_cache_alloc(hammer2_xop_cache, GFP_KERNEL | __GFP_ZERO);
	if (!xop)
		return NULL;

	BUG_ON(xop->head.cluster.array[0].chain != NULL);

	xop->head.ip1 = ip;
	xop->head.flags = flags;

	if (flags & HAMMER2_XOP_MODIFYING)
		xop->head.mtid = hammer2_trans_sub(ip->pmp);
	else
		xop->head.mtid = 0;

	xop->head.cluster.nchains = ip->cluster.nchains;
	xop->head.cluster.pmp = ip->pmp;
	hammer2_assert_cluster(&ip->cluster);

	/* run_mask - Frontend associated with XOP. */
	xop->head.run_mask = HAMMER2_XOPMASK_VOP;

	hammer2_xop_fifo_t *fifo = &xop->head.collect[0];
	xop->head.fifo_size = HAMMER2_XOPFIFO;
	hammer2_xop_fifo_alloc(fifo, xop->head.fifo_size, 0);

	if (!fifo->array || !fifo->errors) {
		kmem_cache_free(hammer2_xop_cache, xop);
		return NULL;
	}

	hammer2_inode_ref(ip);

	return xop;
}

void
hammer2_xop_setname(hammer2_xop_head_t *xop, const char *name, size_t name_len)
{
	xop->name1 = kmalloc(name_len + 1, GFP_KERNEL | __GFP_ZERO);
	if (!xop->name1)
		return;
	
	xop->name1_len = name_len;
	memcpy(xop->name1, name, name_len);
	xop->name1[name_len] = '\0';
}

void
hammer2_xop_setname2(hammer2_xop_head_t *xop, const char *name, size_t name_len)
{
	xop->name2 = kmalloc(name_len + 1, GFP_KERNEL | __GFP_ZERO);
	if (!xop->name2)
		return;
	
	xop->name2_len = name_len;
	memcpy(xop->name2, name, name_len);
	xop->name2[name_len] = '\0';
}

size_t
hammer2_xop_setname_inum(hammer2_xop_head_t *xop, hammer2_key_t inum)
{
	const size_t name_len = 18;

	xop->name1 = kmalloc(name_len + 1, GFP_KERNEL | __GFP_ZERO);
	if (!xop->name1)
		return 0;
	
	xop->name1_len = name_len;
	snprintf(xop->name1, name_len + 1, "0x%016llx", (unsigned long long)inum);

	return name_len;
}

void
hammer2_xop_setip2(hammer2_xop_head_t *xop, hammer2_inode_t *ip2)
{
	xop->ip2 = ip2;
	hammer2_inode_ref(ip2);
}

void
hammer2_xop_setip3(hammer2_xop_head_t *xop, hammer2_inode_t *ip3)
{
	xop->ip3 = ip3;
	hammer2_inode_ref(ip3);
}

void
hammer2_xop_setip4(hammer2_xop_head_t *xop, hammer2_inode_t *ip4)
{
	xop->ip4 = ip4;
	hammer2_inode_ref(ip4);
}

/*
 * (Backend) Returns non-zero if the frontend is still attached.
 */
static __inline int
hammer2_xop_active(const hammer2_xop_head_t *xop)
{
	return (xop->run_mask & HAMMER2_XOPMASK_VOP) ? 1 : 0;
}

/*
 * Simple hash to track inode dependencies.
 */
static int
xop_testset_ipdep(hammer2_inode_t *ip, int idx)
{
	struct hammer2_ipdep_list *ipdep;
	hammer2_inode_t *iptmp;

	BUG_ON(!mutex_is_locked(&ip->pmp->xop_lock[idx]));

	ipdep = &ip->pmp->ipdep_lists[idx];
	list_for_each_entry(iptmp, &ipdep->list, ientry) {
		if (iptmp == ip)
			return 1; /* collision */
	}

	list_add(&ip->ientry, &ipdep->list);
	return 0;
}

static void
xop_unset_ipdep(hammer2_inode_t *ip, int idx)
{
	struct hammer2_ipdep_list *ipdep;
	hammer2_inode_t *iptmp, *tmp;

	BUG_ON(!mutex_is_locked(&ip->pmp->xop_lock[idx]));

	ipdep = &ip->pmp->ipdep_lists[idx];
	list_for_each_entry_safe(iptmp, tmp, &ipdep->list, ientry) {
		if (iptmp == ip) {
			list_del(&ip->ientry);
			return;
		}
	}
}

static void
hammer2_xop_testset_ipdep(hammer2_inode_t *ip)
{
	hammer2_pfs_t *pmp = ip->pmp;
	struct mutex *mtx;
	wait_queue_head_t *wq;

	mtx = &pmp->xop_lock[ip->ipdep_idx];
	wq = &pmp->xop_wq[ip->ipdep_idx];

	mutex_lock(mtx);
again:
	if (xop_testset_ipdep(ip, ip->ipdep_idx)) {
		pmp->flags |= HAMMER2_PMPF_WAITING;
		mutex_unlock(mtx);
		wait_event(*wq, !(pmp->flags & HAMMER2_PMPF_WAITING));
		mutex_lock(mtx);
		goto again;
	}
	mutex_unlock(mtx);
}

static void
hammer2_xop_unset_ipdep(hammer2_inode_t *ip)
{
	hammer2_pfs_t *pmp = ip->pmp;
	struct mutex *mtx;
	wait_queue_head_t *wq;

	mtx = &pmp->xop_lock[ip->ipdep_idx];
	wq = &pmp->xop_wq[ip->ipdep_idx];

	mutex_lock(mtx);
	xop_unset_ipdep(ip, ip->ipdep_idx);
	if (pmp->flags & HAMMER2_PMPF_WAITING) {
		pmp->flags &= ~HAMMER2_PMPF_WAITING;
		wake_up(wq);
	}
	mutex_unlock(mtx);
}

#ifdef CONFIG_HAMMER2_DEBUG
//#define XOP_ADMIN_DEBUG
static __inline void
xop_storage_func(hammer2_xop_head_t *xop, hammer2_inode_t *ip, void *scratch,
    int i)
{
#ifdef XOP_ADMIN_DEBUG
	pr_debug("xop_%s inum %016llx index %d\n",
	    xop->desc->id, (unsigned long long)ip->meta.inum, i);
#endif
	xop->desc->storage_func((hammer2_xop_t *)xop, scratch, i);
#ifdef XOP_ADMIN_DEBUG
	pr_debug("xop_%s inum %016llx index %d done\n",
	    xop->desc->id, (unsigned long long)ip->meta.inum, i);
#endif
}
#else
#define xop_storage_func(xop, ip, scratch, i)	\
	xop->desc->storage_func((hammer2_xop_t *)xop, scratch, i)
#endif

/*
 * Start a XOP request, queueing it to all nodes in the cluster to
 * execute the cluster op.
 */
void
hammer2_xop_start(hammer2_xop_head_t *xop, hammer2_xop_desc_t *desc)
{
	hammer2_inode_t *ip = xop->ip1;
	uint32_t mask;
	int i;

	BUG_ON(!ip);
	hammer2_assert_cluster(&ip->cluster);
	xop->desc = desc;

	if (desc == &hammer2_strategy_write_desc) {
		size_t logical = hammer2_get_logical();
		xop->scratch = kzalloc(logical, GFP_KERNEL);
		if (!xop->scratch)
			return;
	}

	for (i = 0; i < ip->cluster.nchains; ++i) {
		mask = 1LLU << i;
		if (ip->cluster.array[i].chain) {
			atomic_set(&xop->run_mask, mask);
			atomic_set(&xop->chk_mask, mask);
		} else {
			continue;
		}

		if (hammer2_xop_active(xop)) {
			hammer2_xop_testset_ipdep(ip);
			if (xop->ip2)
				hammer2_xop_testset_ipdep(xop->ip2);
			if (xop->ip3 && xop->ip3 != xop->ip1) /* rename */
				hammer2_xop_testset_ipdep(xop->ip3);
			if (xop->ip4 && xop->ip4 != xop->ip2) /* rename */
				hammer2_xop_testset_ipdep(xop->ip4);
			xop_storage_func(xop, ip, xop->scratch, i);
			hammer2_xop_retire(xop, mask);
		} else {
			hammer2_xop_feed(xop, NULL, i, ECONNABORTED);
			hammer2_xop_retire(xop, mask);
		}
	}
}

/*
 * Retire a XOP.  Used by both the VOP frontend and by the XOP backend.
 */
void
hammer2_xop_retire(hammer2_xop_head_t *xop, uint32_t mask)
{
	hammer2_chain_t *chain, *dropch[HAMMER2_MAXCLUSTER];
	hammer2_inode_t *ip;
	hammer2_xop_fifo_t *fifo;
	uint32_t omask;
	int prior_nchains, i;

	/* Remove the frontend collector or remove a backend feeder. */
	BUG_ON(!(xop->run_mask & mask));
	omask = atomic_fetch_sub(&xop->run_mask, mask);

	/* More than one entity left. */
	if ((omask & HAMMER2_XOPMASK_ALLDONE) != mask)
		return;

	/*
	 * All collectors are gone, we can cleanup and dispose of the XOP.
	 */
	if (xop->ip1) {
		/*
		 * Cache cluster chains in a convenient inode.
		 */
		ip = xop->ip1;
		spin_lock(&ip->cluster_spin);
		prior_nchains = ip->ccache_nchains;
		for (i = 0; i < prior_nchains; ++i) {
			dropch[i] = ip->ccache[i].chain;
			ip->ccache[i].chain = NULL;
		}
		for (i = 0; i < xop->cluster.nchains; ++i) {
			ip->ccache[i] = xop->cluster.array[i];
		}
		ip->ccache_nchains = i;
		spin_unlock(&ip->cluster_spin);

		/* Drop prior cache. */
		for (i = 0; i < prior_nchains; ++i) {
			chain = dropch[i];
			if (chain)
				hammer2_chain_drop(chain);
		}
	}

	/* Drop and unhold chains in xop cluster */
	for (i = 0; i < xop->cluster.nchains; ++i) {
		xop->cluster.array[i].flags = 0;
		chain = xop->cluster.array[i].chain;
		if (chain) {
			xop->cluster.array[i].chain = NULL;
			hammer2_chain_drop_unhold(chain);
		}
	}

	/*
	 * Cleanup the fifos.
	 */
	mask = xop->chk_mask;
	for (i = 0; mask && i < HAMMER2_MAXCLUSTER; ++i) {
		fifo = &xop->collect[i];
		while (fifo->ri != fifo->wi) {
			chain = fifo->array[fifo->ri & fifo_mask(xop)];
			if (chain)
				hammer2_chain_drop_unhold(chain);
			++fifo->ri;
		}
		mask &= ~(1U << i);
	}

	/* The inode is only held at this point, simply drop it. */
	if (xop->ip1) {
		hammer2_xop_unset_ipdep(xop->ip1);
		hammer2_inode_drop(xop->ip1);
		xop->ip1 = NULL;
	}
	if (xop->ip2) {
		hammer2_xop_unset_ipdep(xop->ip2);
		hammer2_inode_drop(xop->ip2);
		xop->ip2 = NULL;
	}
	if (xop->ip3) {
		if (xop->ip3 && xop->ip3 != xop->ip1) /* rename */
			hammer2_xop_unset_ipdep(xop->ip3);
		hammer2_inode_drop(xop->ip3);
		xop->ip3 = NULL;
	}
	if (xop->ip4) {
		if (xop->ip4 && xop->ip4 != xop->ip2) /* rename */
			hammer2_xop_unset_ipdep(xop->ip4);
		hammer2_inode_drop(xop->ip4);
		xop->ip4 = NULL;
	}

	if (xop->name1) {
		kfree(xop->name1);
		xop->name1 = NULL;
		xop->name1_len = 0;
	}
	if (xop->name2) {
		kfree(xop->name2);
		xop->name2 = NULL;
		xop->name2_len = 0;
	}

	for (i = 0; i < xop->cluster.nchains; ++i) {
		fifo = &xop->collect[i];
		BUG_ON((fifo->array && xop->fifo_size == 0) ||
		       (fifo->errors && xop->fifo_size == 0));
		kfree(fifo->array);
		kfree(fifo->errors);
		fifo->array = NULL;
		fifo->errors = NULL;
	}

	if (xop->scratch)
		kfree(xop->scratch);

	kmem_cache_free(hammer2_xop_cache, xop);
}

/*
 * (Backend) Feed chain data.
 */
int
hammer2_xop_feed(hammer2_xop_head_t *xop, hammer2_chain_t *chain, int clindex,
    int error)
{
	hammer2_xop_fifo_t *fifo;
	size_t old_fifo_size;

	/* Early termination (typically of xop_readir). */
	if (!hammer2_xop_active(xop)) {
		error = HAMMER2_ERROR_ABORTED;
		goto done;
	}

	/*
	 * Entry into the XOP collector.
	 */
	fifo = &xop->collect[clindex];
	while (fifo->ri == fifo->wi - xop->fifo_size) {
		if ((xop->run_mask & HAMMER2_XOPMASK_VOP) == 0) {
			error = HAMMER2_ERROR_ABORTED;
			goto done;
		}
		old_fifo_size = xop->fifo_size;
		xop->fifo_size *= 2;
		hammer2_xop_fifo_alloc(fifo, xop->fifo_size, old_fifo_size);
	}

	if (error == 0 && chain)
		error = chain->error;
	fifo->errors[fifo->wi & fifo_mask(xop)] = error;
	fifo->array[fifo->wi & fifo_mask(xop)] = chain;
	++fifo->wi;

	error = 0;
done:
	return error;
}

/*
 * (Frontend) collect a response from a running cluster op.
 */
int
hammer2_xop_collect(hammer2_xop_head_t *xop, int flags)
{
	hammer2_xop_fifo_t *fifo;
	hammer2_chain_t *chain;
	hammer2_key_t lokey;
	int i, keynull, adv, error;

	/*
	 * First loop tries to advance pieces of the cluster which
	 * are out of sync.
	 */
	lokey = HAMMER2_KEY_MAX;
	keynull = HAMMER2_CHECK_NULL;

	for (i = 0; i < xop->cluster.nchains; ++i) {
		chain = xop->cluster.array[i].chain;
		if (chain == NULL) {
			adv = 1;
		} else if (chain->bref.key < xop->collect_key) {
			adv = 1;
		} else {
			keynull &= ~HAMMER2_CHECK_NULL;
			if (lokey > chain->bref.key)
				lokey = chain->bref.key;
			adv = 0;
		}
		if (adv == 0)
			continue;

		/* Advance element if possible, advanced element may be NULL. */
		if (chain)
			hammer2_chain_drop_unhold(chain);

		fifo = &xop->collect[i];
		if (fifo->ri != fifo->wi) {
			chain = fifo->array[fifo->ri & fifo_mask(xop)];
			error = fifo->errors[fifo->ri & fifo_mask(xop)];
			++fifo->ri;
			xop->cluster.array[i].chain = chain;
			xop->cluster.array[i].error = error;
			if (chain == NULL)
				xop->cluster.array[i].flags |=
				    HAMMER2_CITEM_NULL;
			--i; /* Loop on same index. */
		} else {
			/*
			 * Retain CITEM_NULL flag.  If set just repeat EOF.
			 * If not, the NULL,0 combination indicates an
			 * operation in-progress.
			 */
			xop->cluster.array[i].chain = NULL;
			/* Retain any CITEM_NULL setting. */
		}
	}

	/*
	 * Determine whether the lowest collected key meets clustering
	 * requirements.
	 */
	error = hammer2_cluster_check(&xop->cluster, lokey, keynull);

	if (lokey == HAMMER2_KEY_MAX)
		xop->collect_key = lokey;
	else
		xop->collect_key = lokey + 1;

	return error;
}
