/* GPL HEADER START
 *
 * DO NOT ALTER OR REMOVE COPYRIGHT NOTICES OR THIS FILE HEADER.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License version 2 for more details (a copy is included
 * in the LICENSE file that accompanied this code).
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; If not, see http://www.gnu.org/licenses
 *
 * Please  visit http://www.xyratex.com/contact if you need additional
 * information or have any questions.
 *
 * GPL HEADER END
 */

/*
 * Copyright 2012 Xyratex Technology Limited
 *
 * Author: Andrew Perepechko <Andrew_Perepechko@xyratex.com>
 *
 */

#define DEBUG_SUBSYSTEM S_LLITE

#include <linux/fs.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <obd_support.h>
#include <lustre_lite.h>
#include <lustre_dlm.h>
#include <lustre_ver.h>
#include "llite_internal.h"

/* If we ever have hundreds of extended attributes, we might want to consider
 * using a hash or a tree structure instead of list for faster lookups.
 */
struct ll_xattr_entry {
	struct list_head	xe_list;    /* protected with
					     * lli_xattrs_list_rwsem */
	char			*xe_name;   /* xattr name, \0-terminated */
	char			*xe_value;  /* xattr value */
	unsigned		xe_namelen; /* strlen(xe_name) + 1 */
	unsigned		xe_vallen;  /* xattr value length */
};

static struct kmem_cache *xattr_kmem;
static struct lu_kmem_descr xattr_caches[] = {
	{
		.ckd_cache = &xattr_kmem,
		.ckd_name  = "xattr_kmem",
		.ckd_size  = sizeof(struct ll_xattr_entry)
	},
	{
		.ckd_cache = NULL
	}
};

int ll_xattr_init(void)
{
	return lu_kmem_init(xattr_caches);
}

void ll_xattr_fini(void)
{
	lu_kmem_fini(xattr_caches);
}

/**
 * Initializes xattr cache for an inode.
 *
 * This initializes the xattr list and marks cache presence.
 */
static void ll_xattr_cache_init(struct ll_inode_info *lli)
{
	ENTRY;

	LASSERT(lli != NULL);

	CFS_INIT_LIST_HEAD(&lli->lli_xattrs);
	lli->lli_flags |= LLIF_XATTR_CACHE;
}

/**
 *  This looks for a specific extended attribute.
 *
 *  Find in @cache and return @xattr_name attribute in @xattr,
 *  for the NULL @xattr_name return the first cached @xattr.
 *
 *  \retval 0        success
 *  \retval -ENODATA if not found
 */
static int ll_xattr_cache_find(struct list_head *cache,
			       const char *xattr_name,
			       struct ll_xattr_entry **xattr)
{
	struct ll_xattr_entry *entry;

	ENTRY;

	list_for_each_entry(entry, cache, xe_list) {
		/* xattr_name == NULL means look for any entry */
		if (xattr_name == NULL ||
		    strcmp(xattr_name, entry->xe_name) == 0) {
			*xattr = entry;
			CDEBUG(D_CACHE, "find: [%s]=%.*s\n",
			       entry->xe_name, entry->xe_vallen,
			       entry->xe_value);
			RETURN(0);
		}
	}

	RETURN(-ENODATA);
}

/**
 * This adds an xattr.
 *
 * Add @xattr_name attr with @xattr_val value and @xattr_val_len length,
 *
 * \retval 0       success
 * \retval -ENOMEM if no memory could be allocated for the cached attr
 * \retval -EPROTO if duplicate xattr is being added
 */
static int ll_xattr_cache_add(struct list_head *cache,
			      const char *xattr_name,
			      const char *xattr_val,
			      unsigned xattr_val_len)
{
	struct ll_xattr_entry *xattr;

	ENTRY;

	if (ll_xattr_cache_find(cache, xattr_name, &xattr) == 0) {
		CDEBUG(D_CACHE, "duplicate xattr: [%s]\n", xattr_name);
		RETURN(-EPROTO);
	}

	OBD_SLAB_ALLOC_PTR_GFP(xattr, xattr_kmem, GFP_NOFS);
	if (xattr == NULL) {
		CDEBUG(D_CACHE, "failed to allocate xattr\n");
		RETURN(-ENOMEM);
	}

	xattr->xe_namelen = strlen(xattr_name) + 1;

	OBD_ALLOC(xattr->xe_name, xattr->xe_namelen);
	if (!xattr->xe_name) {
		CDEBUG(D_CACHE, "failed to alloc xattr name %u\n",
		       xattr->xe_namelen);
		goto err_name;
	}
	OBD_ALLOC(xattr->xe_value, xattr_val_len);
	if (!xattr->xe_value) {
		CDEBUG(D_CACHE, "failed to alloc xattr value %d\n",
		       xattr_val_len);
		goto err_value;
	}

	memcpy(xattr->xe_name, xattr_name, xattr->xe_namelen);
	memcpy(xattr->xe_value, xattr_val, xattr_val_len);
	xattr->xe_vallen = xattr_val_len;
	list_add(&xattr->xe_list, cache);

	CDEBUG(D_CACHE, "set: [%s]=%.*s\n", xattr_name,
		xattr_val_len, xattr_val);

	RETURN(0);
err_value:
	OBD_FREE(xattr->xe_name, xattr->xe_namelen);
err_name:
	OBD_SLAB_FREE_PTR(xattr, xattr_kmem);

	RETURN(-ENOMEM);
}

/**
 * This removes an extended attribute from cache.
 *
 * Remove @xattr_name attribute from @cache.
 *
 * \retval 0        success
 * \retval -ENODATA if @xattr_name is not cached
 */
static int ll_xattr_cache_del(struct list_head *cache,
			      const char *xattr_name)
{
	struct ll_xattr_entry *xattr;

	ENTRY;

	CDEBUG(D_CACHE, "del xattr: %s\n", xattr_name);

	if (ll_xattr_cache_find(cache, xattr_name, &xattr) == 0) {
		list_del(&xattr->xe_list);
		OBD_FREE(xattr->xe_name, xattr->xe_namelen);
		OBD_FREE(xattr->xe_value, xattr->xe_vallen);
		OBD_SLAB_FREE_PTR(xattr, xattr_kmem);

		RETURN(0);
	}

	RETURN(-ENODATA);
}

/**
 * This iterates cached extended attributes.
 *
 * Walk over cached attributes in @cache and
 * fill in @xld_buffer or only calculate buffer
 * size if @xld_buffer is NULL.
 *
 * \retval >= 0     buffer list size
 * \retval -ENODATA if the list cannot fit @xld_size buffer
 */
static int ll_xattr_cache_list(struct list_head *cache,
			       char *xld_buffer,
			       int xld_size)
{
	struct ll_xattr_entry *xattr, *tmp;
	int xld_tail = 0;

	ENTRY;

	list_for_each_entry_safe(xattr, tmp, cache, xe_list) {
		CDEBUG(D_CACHE, "list: buffer=%p[%d] name=%s\n",
			xld_buffer, xld_tail, xattr->xe_name);

		if (xld_buffer) {
			xld_size -= xattr->xe_namelen;
			if (xld_size < 0)
				break;
			memcpy(&xld_buffer[xld_tail],
			       xattr->xe_name, xattr->xe_namelen);
		}
		xld_tail += xattr->xe_namelen;
	}

	if (xld_size < 0)
		RETURN(-ERANGE);

	RETURN(xld_tail);
}

/**
 * Check if the xattr cache is initialized (filled).
 *
 * \retval 0 @cache is not initialized
 * \retval 1 @cache is initialized
 */
static int ll_xattr_cache_valid(struct ll_inode_info *lli)
{
	return !!(lli->lli_flags & LLIF_XATTR_CACHE);
}

/**
 * This finalizes the xattr cache.
 *
 * Free all xattr memory. @lli is the inode info pointer.
 *
 * \retval 0 no error occured
 */
static int ll_xattr_cache_destroy_locked(struct ll_inode_info *lli)
{
	ENTRY;

	if (!ll_xattr_cache_valid(lli))
		RETURN(0);

	while (ll_xattr_cache_del(&lli->lli_xattrs, NULL) == 0)
		/* empty loop */ ;
	lli->lli_flags &= ~LLIF_XATTR_CACHE;

	RETURN(0);
}

int ll_xattr_cache_destroy(struct inode *inode)
{
	struct ll_inode_info *lli = ll_i2info(inode);
	int rc;

	ENTRY;

	down_write(&lli->lli_xattrs_list_rwsem);
	rc = ll_xattr_cache_destroy_locked(lli);
	up_write(&lli->lli_xattrs_list_rwsem);

	RETURN(rc);
}

/**
 * Match or enqueue a PR lock.
 *
 * Find or request an LDLM lock with xattr data.
 * Since LDLM does not provide API for atomic match_or_enqueue,
 * the function handles it with a separate enq lock.
 * If successful, the function exits with the list lock held.
 *
 * \retval 0       no error occured
 * \retval -ENOMEM not enough memory
 */
static int ll_xattr_find_get_lock(struct inode *inode,
				  struct lookup_intent *oit,
				  struct ptlrpc_request **req)
{
	ldlm_mode_t mode;
	struct lustre_handle lockh = { 0 };
	struct md_op_data *op_data;
	struct ll_inode_info *lli = ll_i2info(inode);
	struct ldlm_enqueue_info einfo = { .ei_type = LDLM_IBITS,
					   .ei_mode = it_to_lock_mode(oit),
					   .ei_cb_bl = ll_md_blocking_ast,
					   .ei_cb_cp = ldlm_completion_ast };
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	struct obd_export *exp = sbi->ll_md_exp;
	int rc;

	ENTRY;

	mutex_lock(&lli->lli_xattrs_enq_lock);
	/* inode may have been shrunk and recreated, so data is gone, match lock
	 * only when data exists. */
	if (ll_xattr_cache_valid(lli)) {
		/* Try matching first. */
		mode = ll_take_md_lock(inode, MDS_INODELOCK_XATTR, &lockh, 0,
					LCK_PR);
		if (mode != 0) {
			/* fake oit in mdc_revalidate_lock() manner */
			oit->d.lustre.it_lock_handle = lockh.cookie;
			oit->d.lustre.it_lock_mode = mode;
			goto out;
		}
	}

	/* Enqueue if the lock isn't cached locally. */
	op_data = ll_prep_md_op_data(NULL, inode, NULL, NULL, 0, 0,
				     LUSTRE_OPC_ANY, NULL);
	if (IS_ERR(op_data)) {
		mutex_unlock(&lli->lli_xattrs_enq_lock);
		RETURN(PTR_ERR(op_data));
	}

	op_data->op_valid = OBD_MD_FLXATTR | OBD_MD_FLXATTRLS;

	rc = md_enqueue(exp, &einfo, oit, op_data, &lockh, NULL, 0, NULL, 0);
	ll_finish_md_op_data(op_data);

	if (rc < 0) {
		CDEBUG(D_CACHE, "md_intent_lock failed with %d for fid "DFID"\n",
		       rc, PFID(ll_inode2fid(inode)));
		mutex_unlock(&lli->lli_xattrs_enq_lock);
		RETURN(rc);
	}

	*req = (struct ptlrpc_request *)oit->d.lustre.it_data;
out:
	down_write(&lli->lli_xattrs_list_rwsem);
	mutex_unlock(&lli->lli_xattrs_enq_lock);

	RETURN(0);
}

/**
 * Refill the xattr cache.
 *
 * Fetch and cache the whole of xattrs for @inode, acquiring
 * a read or a write xattr lock depending on operation in @oit.
 * Intent is dropped on exit unless the operation is setxattr.
 *
 * \retval 0       no error occured
 * \retval -EPROTO network protocol error
 * \retval -ENOMEM not enough memory for the cache
 */
static int ll_xattr_cache_refill(struct inode *inode, struct lookup_intent *oit)
{
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	struct ptlrpc_request *req = NULL;
	const char *xdata, *xval, *xtail, *xvtail;
	struct ll_inode_info *lli = ll_i2info(inode);
	struct mdt_body *body;
	__u32 *xsizes;
	int rc = 0, i;

	ENTRY;

	rc = ll_xattr_find_get_lock(inode, oit, &req);
	if (rc)
		GOTO(out_no_unlock, rc);

	/* Do we have the data at this point? */
	if (ll_xattr_cache_valid(lli)) {
		ll_stats_ops_tally(sbi, LPROC_LL_GETXATTR_HITS, 1);
		GOTO(out_maybe_drop, rc = 0);
	}

	/* Matched but no cache? Cancelled on error by a parallel refill. */
	if (unlikely(req == NULL)) {
		CDEBUG(D_CACHE, "cancelled by a parallel getxattr\n");
		GOTO(out_maybe_drop, rc = -EIO);
	}

	if (oit->d.lustre.it_status < 0) {
		CDEBUG(D_CACHE, "getxattr intent returned %d for fid "DFID"\n",
		       oit->d.lustre.it_status, PFID(ll_inode2fid(inode)));
		rc = oit->d.lustre.it_status;
		/* xattr data is so large that we don't want to cache it */
		if (rc == -ERANGE)
			rc = -EAGAIN;
		GOTO(out_destroy, rc);
	}

	body = req_capsule_server_get(&req->rq_pill, &RMF_MDT_BODY);
	if (body == NULL) {
		CERROR("no MDT BODY in the refill xattr reply\n");
		GOTO(out_destroy, rc = -EPROTO);
	}
	/* do not need swab xattr data */
	xdata = req_capsule_server_sized_get(&req->rq_pill, &RMF_EADATA,
						body->eadatasize);
	xval = req_capsule_server_sized_get(&req->rq_pill, &RMF_EAVALS,
						body->aclsize);
	xsizes = req_capsule_server_sized_get(&req->rq_pill, &RMF_EAVALS_LENS,
					      body->max_mdsize * sizeof(__u32));
	if (xdata == NULL || xval == NULL || xsizes == NULL) {
		CERROR("wrong setxattr reply\n");
		GOTO(out_destroy, rc = -EPROTO);
	}

	xtail = xdata + body->eadatasize;
	xvtail = xval + body->aclsize;

	CDEBUG(D_CACHE, "caching: xdata=%p xtail=%p\n", xdata, xtail);

	ll_xattr_cache_init(lli);

	for (i = 0; i < body->max_mdsize; i++) {
		CDEBUG(D_CACHE, "caching [%s]=%.*s\n", xdata, *xsizes, xval);
		/* Perform consistency checks: attr names and vals in pill */
		if (memchr(xdata, 0, xtail - xdata) == NULL) {
			CERROR("xattr protocol violation (names are broken)\n");
			rc = -EPROTO;
		} else if (xval + *xsizes > xvtail) {
			CERROR("xattr protocol violation (vals are broken)\n");
			rc = -EPROTO;
		} else if (OBD_FAIL_CHECK(OBD_FAIL_LLITE_XATTR_ENOMEM)) {
			rc = -ENOMEM;
		} else if (!strcmp(xdata, XATTR_NAME_ACL_ACCESS)) {
			/* Filter out ACL ACCESS since it's cached separately */
			CDEBUG(D_CACHE, "not caching %s\n",
			       XATTR_NAME_ACL_ACCESS);
			rc = 0;
		} else {
			rc = ll_xattr_cache_add(&lli->lli_xattrs, xdata, xval,
						*xsizes);
		}
		if (rc < 0) {
			ll_xattr_cache_destroy_locked(lli);
			GOTO(out_destroy, rc);
		}
		xdata += strlen(xdata) + 1;
		xval  += *xsizes;
		xsizes++;
	}

	if (xdata != xtail || xval != xvtail)
		CERROR("a hole in xattr data\n");

	ll_set_lock_data(sbi->ll_md_exp, inode, oit, NULL);

	GOTO(out_maybe_drop, rc);
out_maybe_drop:

	ll_intent_drop_lock(oit);

	if (rc != 0)
		up_write(&lli->lli_xattrs_list_rwsem);
out_no_unlock:
	ptlrpc_req_finished(req);

	return rc;

out_destroy:
	up_write(&lli->lli_xattrs_list_rwsem);

	ldlm_lock_decref_and_cancel((struct lustre_handle *)
					&oit->d.lustre.it_lock_handle,
					oit->d.lustre.it_lock_mode);

	goto out_no_unlock;
}

/**
 * Get an xattr value or list xattrs using the write-through cache.
 *
 * Get the xattr value (@valid has OBD_MD_FLXATTR set) of @name or
 * list xattr names (@valid has OBD_MD_FLXATTRLS set) for @inode.
 * The resulting value/list is stored in @buffer if the former
 * is not larger than @size.
 *
 * \retval 0        no error occured
 * \retval -EPROTO  network protocol error
 * \retval -ENOMEM  not enough memory for the cache
 * \retval -ERANGE  the buffer is not large enough
 * \retval -ENODATA no such attr or the list is empty
 */
int ll_xattr_cache_get(struct inode *inode,
			const char *name,
			char *buffer,
			size_t size,
			__u64 valid)
{
	struct lookup_intent oit = { .it_op = IT_GETXATTR };
	struct ll_inode_info *lli = ll_i2info(inode);
	int rc = 0;

	ENTRY;

	LASSERT(!!(valid & OBD_MD_FLXATTR) ^ !!(valid & OBD_MD_FLXATTRLS));

	down_read(&lli->lli_xattrs_list_rwsem);
	if (!ll_xattr_cache_valid(lli)) {
		up_read(&lli->lli_xattrs_list_rwsem);
		rc = ll_xattr_cache_refill(inode, &oit);
		if (rc)
			RETURN(rc);
		downgrade_write(&lli->lli_xattrs_list_rwsem);
	} else {
		ll_stats_ops_tally(ll_i2sbi(inode), LPROC_LL_GETXATTR_HITS, 1);
	}

	if (valid & OBD_MD_FLXATTR) {
		struct ll_xattr_entry *xattr;

		rc = ll_xattr_cache_find(&lli->lli_xattrs, name, &xattr);
		if (rc == 0) {
			rc = xattr->xe_vallen;
			/* zero size means we are only requested size in rc */
			if (size != 0) {
				if (size >= xattr->xe_vallen)
					memcpy(buffer, xattr->xe_value,
						xattr->xe_vallen);
				else
					rc = -ERANGE;
			}
		}
	} else if (valid & OBD_MD_FLXATTRLS) {
		rc = ll_xattr_cache_list(&lli->lli_xattrs,
					 size ? buffer : NULL, size);
	}

	GOTO(out, rc);
out:
	up_read(&lli->lli_xattrs_list_rwsem);

	return rc;
}

