/*
 * GPL HEADER START
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
 * version 2 along with this program; If not, see
 * http://www.sun.com/software/products/lustre/docs/GPLv2.pdf
 *
 * Please contact Sun Microsystems, Inc., 4150 Network Circle, Santa Clara,
 * CA 95054 USA or visit www.sun.com if you need additional information or
 * have any questions.
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2002, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2011, 2013, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * lustre/llite/file.c
 *
 * Author: Peter Braam <braam@clusterfs.com>
 * Author: Phil Schwan <phil@clusterfs.com>
 * Author: Andreas Dilger <adilger@clusterfs.com>
 */

#define DEBUG_SUBSYSTEM S_LLITE
#include <lustre_dlm.h>
#include <lustre_lite.h>
#include <linux/pagemap.h>
#include <linux/file.h>
#include "llite_internal.h"
#include <lustre/ll_fiemap.h>

#include "cl_object.h"

struct ll_file_data *ll_file_data_get(void)
{
	struct ll_file_data *fd;

	OBD_SLAB_ALLOC_PTR_GFP(fd, ll_file_data_slab, GFP_NOFS);
	if (fd == NULL)
		return NULL;

	fd->fd_write_failed = false;

	return fd;
}

static void ll_file_data_put(struct ll_file_data *fd)
{
        if (fd != NULL)
                OBD_SLAB_FREE_PTR(fd, ll_file_data_slab);
}

void ll_pack_inode2opdata(struct inode *inode, struct md_op_data *op_data,
                          struct lustre_handle *fh)
{
        op_data->op_fid1 = ll_i2info(inode)->lli_fid;
        op_data->op_attr.ia_mode = inode->i_mode;
        op_data->op_attr.ia_atime = inode->i_atime;
        op_data->op_attr.ia_mtime = inode->i_mtime;
        op_data->op_attr.ia_ctime = inode->i_ctime;
        op_data->op_attr.ia_size = i_size_read(inode);
        op_data->op_attr_blocks = inode->i_blocks;
        ((struct ll_iattr *)&op_data->op_attr)->ia_attr_flags =
                                        ll_inode_to_ext_flags(inode->i_flags);
        op_data->op_ioepoch = ll_i2info(inode)->lli_ioepoch;
        if (fh)
                op_data->op_handle = *fh;
        op_data->op_capa1 = ll_mdscapa_get(inode);

	if (LLIF_DATA_MODIFIED & ll_i2info(inode)->lli_flags)
		op_data->op_bias |= MDS_DATA_MODIFIED;
}

/**
 * Closes the IO epoch and packs all the attributes into @op_data for
 * the CLOSE rpc.
 */
static void ll_prepare_close(struct inode *inode, struct md_op_data *op_data,
                             struct obd_client_handle *och)
{
        ENTRY;

	op_data->op_attr.ia_valid = ATTR_MODE | ATTR_ATIME | ATTR_ATIME_SET |
					ATTR_MTIME | ATTR_MTIME_SET |
					ATTR_CTIME | ATTR_CTIME_SET;

        if (!(och->och_flags & FMODE_WRITE))
                goto out;

        if (!exp_connect_som(ll_i2mdexp(inode)) || !S_ISREG(inode->i_mode))
                op_data->op_attr.ia_valid |= ATTR_SIZE | ATTR_BLOCKS;
        else
                ll_ioepoch_close(inode, op_data, &och, 0);

out:
        ll_pack_inode2opdata(inode, op_data, &och->och_fh);
        ll_prep_md_op_data(op_data, inode, NULL, NULL,
                           0, 0, LUSTRE_OPC_ANY, NULL);
        EXIT;
}

static int ll_close_inode_openhandle(struct obd_export *md_exp,
				     struct inode *inode,
				     struct obd_client_handle *och,
				     const __u64 *data_version)
{
        struct obd_export *exp = ll_i2mdexp(inode);
        struct md_op_data *op_data;
        struct ptlrpc_request *req = NULL;
        struct obd_device *obd = class_exp2obd(exp);
        int epoch_close = 1;
        int rc;
        ENTRY;

        if (obd == NULL) {
                /*
                 * XXX: in case of LMV, is this correct to access
                 * ->exp_handle?
                 */
                CERROR("Invalid MDC connection handle "LPX64"\n",
                       ll_i2mdexp(inode)->exp_handle.h_cookie);
                GOTO(out, rc = 0);
        }

        OBD_ALLOC_PTR(op_data);
        if (op_data == NULL)
                GOTO(out, rc = -ENOMEM); // XXX We leak openhandle and request here.

	ll_prepare_close(inode, op_data, och);
	if (data_version != NULL) {
		/* Pass in data_version implies release. */
		op_data->op_bias |= MDS_HSM_RELEASE;
		op_data->op_data_version = *data_version;
		op_data->op_lease_handle = och->och_lease_handle;
		op_data->op_attr.ia_valid |= ATTR_SIZE | ATTR_BLOCKS;
	}
        epoch_close = (op_data->op_flags & MF_EPOCH_CLOSE);
        rc = md_close(md_exp, op_data, och->och_mod, &req);
        if (rc == -EAGAIN) {
                /* This close must have the epoch closed. */
                LASSERT(epoch_close);
                /* MDS has instructed us to obtain Size-on-MDS attribute from
                 * OSTs and send setattr to back to MDS. */
                rc = ll_som_update(inode, op_data);
                if (rc) {
			CERROR("%s: inode "DFID" mdc Size-on-MDS update"
			       " failed: rc = %d\n",
			       ll_i2mdexp(inode)->exp_obd->obd_name,
			       PFID(ll_inode2fid(inode)), rc);
			rc = 0;
		}
	} else if (rc) {
		CERROR("%s: inode "DFID" mdc close failed: rc = %d\n",
		       ll_i2mdexp(inode)->exp_obd->obd_name,
		       PFID(ll_inode2fid(inode)), rc);
	}

	/* DATA_MODIFIED flag was successfully sent on close, cancel data
	 * modification flag. */
	if (rc == 0 && (op_data->op_bias & MDS_DATA_MODIFIED)) {
		struct ll_inode_info *lli = ll_i2info(inode);

		spin_lock(&lli->lli_lock);
		lli->lli_flags &= ~LLIF_DATA_MODIFIED;
		spin_unlock(&lli->lli_lock);
	}

        if (rc == 0) {
                rc = ll_objects_destroy(req, inode);
                if (rc)
			CERROR("%s: inode "DFID
			       " ll_objects destroy: rc = %d\n",
			       ll_i2mdexp(inode)->exp_obd->obd_name,
			       PFID(ll_inode2fid(inode)), rc);
        }

	if (rc == 0 && op_data->op_bias & MDS_HSM_RELEASE) {
		struct mdt_body *body;
		body = req_capsule_server_get(&req->rq_pill, &RMF_MDT_BODY);
		if (!(body->valid & OBD_MD_FLRELEASED))
			rc = -EBUSY;
	}

        ll_finish_md_op_data(op_data);
        EXIT;
out:

        if (exp_connect_som(exp) && !epoch_close &&
            S_ISREG(inode->i_mode) && (och->och_flags & FMODE_WRITE)) {
                ll_queue_done_writing(inode, LLIF_DONE_WRITING);
        } else {
                md_clear_open_replay_data(md_exp, och);
                /* Free @och if it is not waiting for DONE_WRITING. */
                och->och_fh.cookie = DEAD_HANDLE_MAGIC;
                OBD_FREE_PTR(och);
        }
        if (req) /* This is close request */
                ptlrpc_req_finished(req);
        return rc;
}

int ll_md_real_close(struct inode *inode, fmode_t fmode)
{
	struct ll_inode_info *lli = ll_i2info(inode);
	struct obd_client_handle **och_p;
	struct obd_client_handle *och;
	__u64 *och_usecount;
	int rc = 0;
	ENTRY;

	if (fmode & FMODE_WRITE) {
		och_p = &lli->lli_mds_write_och;
		och_usecount = &lli->lli_open_fd_write_count;
	} else if (fmode & FMODE_EXEC) {
		och_p = &lli->lli_mds_exec_och;
		och_usecount = &lli->lli_open_fd_exec_count;
	} else {
		LASSERT(fmode & FMODE_READ);
		och_p = &lli->lli_mds_read_och;
		och_usecount = &lli->lli_open_fd_read_count;
	}

	mutex_lock(&lli->lli_och_mutex);
	if (*och_usecount > 0) {
		/* There are still users of this handle, so skip
		 * freeing it. */
		mutex_unlock(&lli->lli_och_mutex);
		RETURN(0);
	}

	och = *och_p;
	*och_p = NULL;
	mutex_unlock(&lli->lli_och_mutex);

	if (och != NULL) {
		/* There might be a race and this handle may already
		 * be closed. */
		rc = ll_close_inode_openhandle(ll_i2sbi(inode)->ll_md_exp,
					       inode, och, NULL);
	}

	RETURN(rc);
}

int ll_md_close(struct obd_export *md_exp, struct inode *inode,
                struct file *file)
{
        struct ll_file_data *fd = LUSTRE_FPRIVATE(file);
        struct ll_inode_info *lli = ll_i2info(inode);
        int rc = 0;
        ENTRY;

        /* clear group lock, if present */
        if (unlikely(fd->fd_flags & LL_FILE_GROUP_LOCKED))
                ll_put_grouplock(inode, file, fd->fd_grouplock.cg_gid);

	if (fd->fd_lease_och != NULL) {
		bool lease_broken;

		/* Usually the lease is not released when the
		 * application crashed, we need to release here. */
		rc = ll_lease_close(fd->fd_lease_och, inode, &lease_broken);
		CDEBUG(rc ? D_ERROR : D_INODE, "Clean up lease "DFID" %d/%d\n",
			PFID(&lli->lli_fid), rc, lease_broken);

		fd->fd_lease_och = NULL;
	}

	if (fd->fd_och != NULL) {
		rc = ll_close_inode_openhandle(md_exp, inode, fd->fd_och, NULL);
		fd->fd_och = NULL;
		GOTO(out, rc);
	}

        /* Let's see if we have good enough OPEN lock on the file and if
           we can skip talking to MDS */
        if (file->f_dentry->d_inode) { /* Can this ever be false? */
                int lockmode;
		__u64 flags = LDLM_FL_BLOCK_GRANTED | LDLM_FL_TEST_LOCK;
                struct lustre_handle lockh;
                struct inode *inode = file->f_dentry->d_inode;
                ldlm_policy_data_t policy = {.l_inodebits={MDS_INODELOCK_OPEN}};

		mutex_lock(&lli->lli_och_mutex);
                if (fd->fd_omode & FMODE_WRITE) {
                        lockmode = LCK_CW;
                        LASSERT(lli->lli_open_fd_write_count);
                        lli->lli_open_fd_write_count--;
                } else if (fd->fd_omode & FMODE_EXEC) {
                        lockmode = LCK_PR;
                        LASSERT(lli->lli_open_fd_exec_count);
                        lli->lli_open_fd_exec_count--;
                } else {
                        lockmode = LCK_CR;
                        LASSERT(lli->lli_open_fd_read_count);
                        lli->lli_open_fd_read_count--;
                }
		mutex_unlock(&lli->lli_och_mutex);

                if (!md_lock_match(md_exp, flags, ll_inode2fid(inode),
                                   LDLM_IBITS, &policy, lockmode,
                                   &lockh)) {
                        rc = ll_md_real_close(file->f_dentry->d_inode,
                                              fd->fd_omode);
                }
        } else {
                CERROR("Releasing a file %p with negative dentry %p. Name %s",
                       file, file->f_dentry, file->f_dentry->d_name.name);
        }

out:
	LUSTRE_FPRIVATE(file) = NULL;
	ll_file_data_put(fd);
	ll_capa_close(inode);

	RETURN(rc);
}

/* While this returns an error code, fput() the caller does not, so we need
 * to make every effort to clean up all of our state here.  Also, applications
 * rarely check close errors and even if an error is returned they will not
 * re-try the close call.
 */
int ll_file_release(struct inode *inode, struct file *file)
{
        struct ll_file_data *fd;
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ll_inode_info *lli = ll_i2info(inode);
        int rc;
        ENTRY;

	CDEBUG(D_VFSTRACE, "VFS Op:inode="DFID"(%p)\n",
	       PFID(ll_inode2fid(inode)), inode);

#ifdef CONFIG_FS_POSIX_ACL
	if (sbi->ll_flags & LL_SBI_RMT_CLIENT &&
	    inode == inode->i_sb->s_root->d_inode) {
		struct ll_file_data *fd = LUSTRE_FPRIVATE(file);

		LASSERT(fd != NULL);
		if (unlikely(fd->fd_flags & LL_FILE_RMTACL)) {
			fd->fd_flags &= ~LL_FILE_RMTACL;
			rct_del(&sbi->ll_rct, current_pid());
			et_search_free(&sbi->ll_et, current_pid());
		}
	}
#endif

        if (inode->i_sb->s_root != file->f_dentry)
                ll_stats_ops_tally(sbi, LPROC_LL_RELEASE, 1);
        fd = LUSTRE_FPRIVATE(file);
        LASSERT(fd != NULL);

        /* The last ref on @file, maybe not the the owner pid of statahead.
         * Different processes can open the same dir, "ll_opendir_key" means:
         * it is me that should stop the statahead thread. */
        if (S_ISDIR(inode->i_mode) && lli->lli_opendir_key == fd &&
            lli->lli_opendir_pid != 0)
                ll_stop_statahead(inode, lli->lli_opendir_key);

        if (inode->i_sb->s_root == file->f_dentry) {
                LUSTRE_FPRIVATE(file) = NULL;
                ll_file_data_put(fd);
                RETURN(0);
        }

        if (!S_ISDIR(inode->i_mode)) {
		lov_read_and_clear_async_rc(lli->lli_clob);
                lli->lli_async_rc = 0;
        }

        rc = ll_md_close(sbi->ll_md_exp, inode, file);

        if (CFS_FAIL_TIMEOUT_MS(OBD_FAIL_PTLRPC_DUMP_LOG, cfs_fail_val))
                libcfs_debug_dumplog();

        RETURN(rc);
}

static int ll_intent_file_open(struct file *file, void *lmm,
                               int lmmsize, struct lookup_intent *itp)
{
        struct ll_sb_info *sbi = ll_i2sbi(file->f_dentry->d_inode);
        struct dentry *parent = file->f_dentry->d_parent;
        const char *name = file->f_dentry->d_name.name;
        const int len = file->f_dentry->d_name.len;
        struct md_op_data *op_data;
        struct ptlrpc_request *req;
        __u32 opc = LUSTRE_OPC_ANY;
        int rc;
        ENTRY;

        if (!parent)
                RETURN(-ENOENT);

        /* Usually we come here only for NFSD, and we want open lock.
           But we can also get here with pre 2.6.15 patchless kernels, and in
           that case that lock is also ok */
        /* We can also get here if there was cached open handle in revalidate_it
         * but it disappeared while we were getting from there to ll_file_open.
         * But this means this file was closed and immediatelly opened which
         * makes a good candidate for using OPEN lock */
        /* If lmmsize & lmm are not 0, we are just setting stripe info
         * parameters. No need for the open lock */
        if (lmm == NULL && lmmsize == 0) {
                itp->it_flags |= MDS_OPEN_LOCK;
                if (itp->it_flags & FMODE_WRITE)
                        opc = LUSTRE_OPC_CREATE;
        }

        op_data  = ll_prep_md_op_data(NULL, parent->d_inode,
                                      file->f_dentry->d_inode, name, len,
                                      O_RDWR, opc, NULL);
        if (IS_ERR(op_data))
                RETURN(PTR_ERR(op_data));

	itp->it_flags |= MDS_OPEN_BY_FID;
        rc = md_intent_lock(sbi->ll_md_exp, op_data, lmm, lmmsize, itp,
                            0 /*unused */, &req, ll_md_blocking_ast, 0);
        ll_finish_md_op_data(op_data);
        if (rc == -ESTALE) {
                /* reason for keep own exit path - don`t flood log
                * with messages with -ESTALE errors.
                */
                if (!it_disposition(itp, DISP_OPEN_OPEN) ||
                     it_open_error(DISP_OPEN_OPEN, itp))
                        GOTO(out, rc);
                ll_release_openhandle(file->f_dentry, itp);
                GOTO(out, rc);
        }

        if (it_disposition(itp, DISP_LOOKUP_NEG))
                GOTO(out, rc = -ENOENT);

        if (rc != 0 || it_open_error(DISP_OPEN_OPEN, itp)) {
                rc = rc ? rc : it_open_error(DISP_OPEN_OPEN, itp);
                CDEBUG(D_VFSTRACE, "lock enqueue: err: %d\n", rc);
                GOTO(out, rc);
        }

        rc = ll_prep_inode(&file->f_dentry->d_inode, req, NULL, itp);
        if (!rc && itp->d.lustre.it_lock_mode)
                ll_set_lock_data(sbi->ll_md_exp, file->f_dentry->d_inode,
                                 itp, NULL);

out:
	ptlrpc_req_finished(req);
        ll_intent_drop_lock(itp);

        RETURN(rc);
}

/**
 * Assign an obtained @ioepoch to client's inode. No lock is needed, MDS does
 * not believe attributes if a few ioepoch holders exist. Attributes for
 * previous ioepoch if new one is opened are also skipped by MDS.
 */
void ll_ioepoch_open(struct ll_inode_info *lli, __u64 ioepoch)
{
        if (ioepoch && lli->lli_ioepoch != ioepoch) {
                lli->lli_ioepoch = ioepoch;
                CDEBUG(D_INODE, "Epoch "LPU64" opened on "DFID"\n",
                       ioepoch, PFID(&lli->lli_fid));
        }
}

static int ll_och_fill(struct obd_export *md_exp, struct lookup_intent *it,
		       struct obd_client_handle *och)
{
	struct ptlrpc_request *req = it->d.lustre.it_data;
	struct mdt_body *body;

	body = req_capsule_server_get(&req->rq_pill, &RMF_MDT_BODY);
	och->och_fh = body->handle;
	och->och_fid = body->fid1;
	och->och_lease_handle.cookie = it->d.lustre.it_lock_handle;
	och->och_magic = OBD_CLIENT_HANDLE_MAGIC;
	och->och_flags = it->it_flags;

	return md_set_open_replay_data(md_exp, och, it);
}

int ll_local_open(struct file *file, struct lookup_intent *it,
                  struct ll_file_data *fd, struct obd_client_handle *och)
{
        struct inode *inode = file->f_dentry->d_inode;
        struct ll_inode_info *lli = ll_i2info(inode);
        ENTRY;

        LASSERT(!LUSTRE_FPRIVATE(file));

        LASSERT(fd != NULL);

        if (och) {
                struct ptlrpc_request *req = it->d.lustre.it_data;
                struct mdt_body *body;
                int rc;

		rc = ll_och_fill(ll_i2sbi(inode)->ll_md_exp, it, och);
		if (rc != 0)
			RETURN(rc);

		body = req_capsule_server_get(&req->rq_pill, &RMF_MDT_BODY);
		ll_ioepoch_open(lli, body->ioepoch);
	}

	LUSTRE_FPRIVATE(file) = fd;
	ll_readahead_init(inode, &fd->fd_ras);
	fd->fd_omode = it->it_flags & (FMODE_READ | FMODE_WRITE | FMODE_EXEC);

	RETURN(0);
}

/* Open a file, and (for the very first open) create objects on the OSTs at
 * this time.  If opened with O_LOV_DELAY_CREATE, then we don't do the object
 * creation or open until ll_lov_setstripe() ioctl is called.
 *
 * If we already have the stripe MD locally then we don't request it in
 * md_open(), by passing a lmm_size = 0.
 *
 * It is up to the application to ensure no other processes open this file
 * in the O_LOV_DELAY_CREATE case, or the default striping pattern will be
 * used.  We might be able to avoid races of that sort by getting lli_open_sem
 * before returning in the O_LOV_DELAY_CREATE case and dropping it here
 * or in ll_file_release(), but I'm not sure that is desirable/necessary.
 */
int ll_file_open(struct inode *inode, struct file *file)
{
        struct ll_inode_info *lli = ll_i2info(inode);
        struct lookup_intent *it, oit = { .it_op = IT_OPEN,
                                          .it_flags = file->f_flags };
        struct obd_client_handle **och_p = NULL;
        __u64 *och_usecount = NULL;
        struct ll_file_data *fd;
        int rc = 0, opendir_set = 0;
        ENTRY;

	CDEBUG(D_VFSTRACE, "VFS Op:inode="DFID"(%p), flags %o\n",
	       PFID(ll_inode2fid(inode)), inode, file->f_flags);

        it = file->private_data; /* XXX: compat macro */
        file->private_data = NULL; /* prevent ll_local_open assertion */

	fd = ll_file_data_get();
	if (fd == NULL)
		GOTO(out_openerr, rc = -ENOMEM);

	fd->fd_file = file;
	if (S_ISDIR(inode->i_mode)) {
		spin_lock(&lli->lli_sa_lock);
		if (lli->lli_opendir_key == NULL && lli->lli_sai == NULL &&
		    lli->lli_opendir_pid == 0) {
			lli->lli_opendir_key = fd;
			lli->lli_opendir_pid = current_pid();
			opendir_set = 1;
		}
		spin_unlock(&lli->lli_sa_lock);
	}

        if (inode->i_sb->s_root == file->f_dentry) {
                LUSTRE_FPRIVATE(file) = fd;
                RETURN(0);
        }

        if (!it || !it->d.lustre.it_disposition) {
                /* Convert f_flags into access mode. We cannot use file->f_mode,
                 * because everything but O_ACCMODE mask was stripped from
                 * there */
                if ((oit.it_flags + 1) & O_ACCMODE)
                        oit.it_flags++;
                if (file->f_flags & O_TRUNC)
                        oit.it_flags |= FMODE_WRITE;

                /* kernel only call f_op->open in dentry_open.  filp_open calls
                 * dentry_open after call to open_namei that checks permissions.
                 * Only nfsd_open call dentry_open directly without checking
                 * permissions and because of that this code below is safe. */
                if (oit.it_flags & (FMODE_WRITE | FMODE_READ))
                        oit.it_flags |= MDS_OPEN_OWNEROVERRIDE;

                /* We do not want O_EXCL here, presumably we opened the file
                 * already? XXX - NFS implications? */
                oit.it_flags &= ~O_EXCL;

                /* bug20584, if "it_flags" contains O_CREAT, the file will be
                 * created if necessary, then "IT_CREAT" should be set to keep
                 * consistent with it */
                if (oit.it_flags & O_CREAT)
                        oit.it_op |= IT_CREAT;

                it = &oit;
        }

restart:
        /* Let's see if we have file open on MDS already. */
        if (it->it_flags & FMODE_WRITE) {
                och_p = &lli->lli_mds_write_och;
                och_usecount = &lli->lli_open_fd_write_count;
        } else if (it->it_flags & FMODE_EXEC) {
                och_p = &lli->lli_mds_exec_och;
                och_usecount = &lli->lli_open_fd_exec_count;
         } else {
                och_p = &lli->lli_mds_read_och;
                och_usecount = &lli->lli_open_fd_read_count;
        }

	mutex_lock(&lli->lli_och_mutex);
        if (*och_p) { /* Open handle is present */
                if (it_disposition(it, DISP_OPEN_OPEN)) {
                        /* Well, there's extra open request that we do not need,
                           let's close it somehow. This will decref request. */
                        rc = it_open_error(DISP_OPEN_OPEN, it);
                        if (rc) {
				mutex_unlock(&lli->lli_och_mutex);
                                GOTO(out_openerr, rc);
                        }

                        ll_release_openhandle(file->f_dentry, it);
                }
                (*och_usecount)++;

                rc = ll_local_open(file, it, fd, NULL);
                if (rc) {
                        (*och_usecount)--;
			mutex_unlock(&lli->lli_och_mutex);
                        GOTO(out_openerr, rc);
                }
        } else {
                LASSERT(*och_usecount == 0);
                if (!it->d.lustre.it_disposition) {
                        /* We cannot just request lock handle now, new ELC code
                           means that one of other OPEN locks for this file
                           could be cancelled, and since blocking ast handler
                           would attempt to grab och_mutex as well, that would
                           result in a deadlock */
			mutex_unlock(&lli->lli_och_mutex);
                        it->it_create_mode |= M_CHECK_STALE;
                        rc = ll_intent_file_open(file, NULL, 0, it);
                        it->it_create_mode &= ~M_CHECK_STALE;
                        if (rc)
                                GOTO(out_openerr, rc);

                        goto restart;
                }
                OBD_ALLOC(*och_p, sizeof (struct obd_client_handle));
                if (!*och_p)
                        GOTO(out_och_free, rc = -ENOMEM);

                (*och_usecount)++;

                /* md_intent_lock() didn't get a request ref if there was an
                 * open error, so don't do cleanup on the request here
                 * (bug 3430) */
                /* XXX (green): Should not we bail out on any error here, not
                 * just open error? */
		rc = it_open_error(DISP_OPEN_OPEN, it);
		if (rc != 0)
			GOTO(out_och_free, rc);

		LASSERTF(it_disposition(it, DISP_ENQ_OPEN_REF),
			 "inode %p: disposition %x, status %d\n", inode,
			 it_disposition(it, ~0), it->d.lustre.it_status);

		rc = ll_local_open(file, it, fd, *och_p);
		if (rc)
			GOTO(out_och_free, rc);
	}
	mutex_unlock(&lli->lli_och_mutex);
        fd = NULL;

        /* Must do this outside lli_och_mutex lock to prevent deadlock where
           different kind of OPEN lock for this same inode gets cancelled
           by ldlm_cancel_lru */
        if (!S_ISREG(inode->i_mode))
                GOTO(out_och_free, rc);

        ll_capa_open(inode);

	if (!lli->lli_has_smd &&
	    (cl_is_lov_delay_create(file->f_flags) ||
	     (file->f_mode & FMODE_WRITE) == 0)) {
		CDEBUG(D_INODE, "object creation was delayed\n");
		GOTO(out_och_free, rc);
	}
	cl_lov_delay_create_clear(&file->f_flags);
	GOTO(out_och_free, rc);

out_och_free:
        if (rc) {
                if (och_p && *och_p) {
                        OBD_FREE(*och_p, sizeof (struct obd_client_handle));
                        *och_p = NULL; /* OBD_FREE writes some magic there */
                        (*och_usecount)--;
                }
		mutex_unlock(&lli->lli_och_mutex);

out_openerr:
                if (opendir_set != 0)
                        ll_stop_statahead(inode, lli->lli_opendir_key);
                if (fd != NULL)
                        ll_file_data_put(fd);
        } else {
                ll_stats_ops_tally(ll_i2sbi(inode), LPROC_LL_OPEN, 1);
        }

	if (it && it_disposition(it, DISP_ENQ_OPEN_REF)) {
		ptlrpc_req_finished(it->d.lustre.it_data);
		it_clear_disposition(it, DISP_ENQ_OPEN_REF);
	}

        return rc;
}

static int ll_md_blocking_lease_ast(struct ldlm_lock *lock,
			struct ldlm_lock_desc *desc, void *data, int flag)
{
	int rc;
	struct lustre_handle lockh;
	ENTRY;

	switch (flag) {
	case LDLM_CB_BLOCKING:
		ldlm_lock2handle(lock, &lockh);
		rc = ldlm_cli_cancel(&lockh, LCF_ASYNC);
		if (rc < 0) {
			CDEBUG(D_INODE, "ldlm_cli_cancel: %d\n", rc);
			RETURN(rc);
		}
		break;
	case LDLM_CB_CANCELING:
		/* do nothing */
		break;
	}
	RETURN(0);
}

/**
 * Acquire a lease and open the file.
 */
struct obd_client_handle *ll_lease_open(struct inode *inode, struct file *file,
					fmode_t fmode, __u64 open_flags)
{
	struct lookup_intent it = { .it_op = IT_OPEN };
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	struct md_op_data *op_data;
	struct ptlrpc_request *req;
	struct lustre_handle old_handle = { 0 };
	struct obd_client_handle *och = NULL;
	int rc;
	int rc2;
	ENTRY;

	if (fmode != FMODE_WRITE && fmode != FMODE_READ)
		RETURN(ERR_PTR(-EINVAL));

	if (file != NULL) {
		struct ll_inode_info *lli = ll_i2info(inode);
		struct ll_file_data *fd = LUSTRE_FPRIVATE(file);
		struct obd_client_handle **och_p;
		__u64 *och_usecount;

		if (!(fmode & file->f_mode) || (file->f_mode & FMODE_EXEC))
			RETURN(ERR_PTR(-EPERM));

		/* Get the openhandle of the file */
		rc = -EBUSY;
		mutex_lock(&lli->lli_och_mutex);
		if (fd->fd_lease_och != NULL) {
			mutex_unlock(&lli->lli_och_mutex);
			RETURN(ERR_PTR(rc));
		}

		if (fd->fd_och == NULL) {
			if (file->f_mode & FMODE_WRITE) {
				LASSERT(lli->lli_mds_write_och != NULL);
				och_p = &lli->lli_mds_write_och;
				och_usecount = &lli->lli_open_fd_write_count;
			} else {
				LASSERT(lli->lli_mds_read_och != NULL);
				och_p = &lli->lli_mds_read_och;
				och_usecount = &lli->lli_open_fd_read_count;
			}
			if (*och_usecount == 1) {
				fd->fd_och = *och_p;
				*och_p = NULL;
				*och_usecount = 0;
				rc = 0;
			}
		}
		mutex_unlock(&lli->lli_och_mutex);
		if (rc < 0) /* more than 1 opener */
			RETURN(ERR_PTR(rc));

		LASSERT(fd->fd_och != NULL);
		old_handle = fd->fd_och->och_fh;
	}

	OBD_ALLOC_PTR(och);
	if (och == NULL)
		RETURN(ERR_PTR(-ENOMEM));

	op_data = ll_prep_md_op_data(NULL, inode, inode, NULL, 0, 0,
					LUSTRE_OPC_ANY, NULL);
	if (IS_ERR(op_data))
		GOTO(out, rc = PTR_ERR(op_data));

	/* To tell the MDT this openhandle is from the same owner */
	op_data->op_handle = old_handle;

	it.it_flags = fmode | open_flags;
	it.it_flags |= MDS_OPEN_LOCK | MDS_OPEN_BY_FID | MDS_OPEN_LEASE;
	rc = md_intent_lock(sbi->ll_md_exp, op_data, NULL, 0, &it, 0, &req,
				ll_md_blocking_lease_ast,
	/* LDLM_FL_NO_LRU: To not put the lease lock into LRU list, otherwise
	 * it can be cancelled which may mislead applications that the lease is
	 * broken;
	 * LDLM_FL_EXCL: Set this flag so that it won't be matched by normal
	 * open in ll_md_blocking_ast(). Otherwise as ll_md_blocking_lease_ast
	 * doesn't deal with openhandle, so normal openhandle will be leaked. */
				LDLM_FL_NO_LRU | LDLM_FL_EXCL);
	ll_finish_md_op_data(op_data);
	ptlrpc_req_finished(req);
	if (rc < 0)
		GOTO(out_release_it, rc);

	if (it_disposition(&it, DISP_LOOKUP_NEG))
		GOTO(out_release_it, rc = -ENOENT);

	rc = it_open_error(DISP_OPEN_OPEN, &it);
	if (rc)
		GOTO(out_release_it, rc);

	LASSERT(it_disposition(&it, DISP_ENQ_OPEN_REF));
	ll_och_fill(sbi->ll_md_exp, &it, och);

	if (!it_disposition(&it, DISP_OPEN_LEASE)) /* old server? */
		GOTO(out_close, rc = -EOPNOTSUPP);

	/* already get lease, handle lease lock */
	ll_set_lock_data(sbi->ll_md_exp, inode, &it, NULL);
	if (it.d.lustre.it_lock_mode == 0 ||
	    it.d.lustre.it_lock_bits != MDS_INODELOCK_OPEN) {
		/* open lock must return for lease */
		CERROR(DFID "lease granted but no open lock, %d/"LPU64".\n",
			PFID(ll_inode2fid(inode)), it.d.lustre.it_lock_mode,
			it.d.lustre.it_lock_bits);
		GOTO(out_close, rc = -EPROTO);
	}

	ll_intent_release(&it);
	RETURN(och);

out_close:
	/* Cancel open lock */
	if (it.d.lustre.it_lock_mode != 0) {
		ldlm_lock_decref_and_cancel(&och->och_lease_handle,
					    it.d.lustre.it_lock_mode);
		it.d.lustre.it_lock_mode = 0;
		och->och_lease_handle.cookie = 0ULL;
	}
	rc2 = ll_close_inode_openhandle(sbi->ll_md_exp, inode, och, NULL);
	if (rc2 < 0)
		CERROR("%s: error closing file "DFID": %d\n",
		       ll_get_fsname(inode->i_sb, NULL, 0),
		       PFID(&ll_i2info(inode)->lli_fid), rc2);
	och = NULL; /* och has been freed in ll_close_inode_openhandle() */
out_release_it:
	ll_intent_release(&it);
out:
	if (och != NULL)
		OBD_FREE_PTR(och);
	RETURN(ERR_PTR(rc));
}
EXPORT_SYMBOL(ll_lease_open);

/**
 * Release lease and close the file.
 * It will check if the lease has ever broken.
 */
int ll_lease_close(struct obd_client_handle *och, struct inode *inode,
			bool *lease_broken)
{
	struct ldlm_lock *lock;
	bool cancelled = true;
	int rc;
	ENTRY;

	lock = ldlm_handle2lock(&och->och_lease_handle);
	if (lock != NULL) {
		lock_res_and_lock(lock);
		cancelled = ldlm_is_cancel(lock);
		unlock_res_and_lock(lock);
		LDLM_LOCK_PUT(lock);
	}

	CDEBUG(D_INODE, "lease for "DFID" broken? %d\n",
		PFID(&ll_i2info(inode)->lli_fid), cancelled);

	if (!cancelled)
		ldlm_cli_cancel(&och->och_lease_handle, 0);
	if (lease_broken != NULL)
		*lease_broken = cancelled;

	rc = ll_close_inode_openhandle(ll_i2sbi(inode)->ll_md_exp, inode, och,
				       NULL);
	RETURN(rc);
}
EXPORT_SYMBOL(ll_lease_close);

/* Fills the obdo with the attributes for the lsm */
static int ll_lsm_getattr(struct lov_stripe_md *lsm, struct obd_export *exp,
			  struct obd_capa *capa, struct obdo *obdo,
			  __u64 ioepoch, int dv_flags)
{
        struct ptlrpc_request_set *set;
        struct obd_info            oinfo = { { { 0 } } };
        int                        rc;

        ENTRY;

        LASSERT(lsm != NULL);

        oinfo.oi_md = lsm;
        oinfo.oi_oa = obdo;
	oinfo.oi_oa->o_oi = lsm->lsm_oi;
        oinfo.oi_oa->o_mode = S_IFREG;
        oinfo.oi_oa->o_ioepoch = ioepoch;
        oinfo.oi_oa->o_valid = OBD_MD_FLID | OBD_MD_FLTYPE |
                               OBD_MD_FLSIZE | OBD_MD_FLBLOCKS |
                               OBD_MD_FLBLKSZ | OBD_MD_FLATIME |
                               OBD_MD_FLMTIME | OBD_MD_FLCTIME |
                               OBD_MD_FLGROUP | OBD_MD_FLEPOCH |
                               OBD_MD_FLDATAVERSION;
        oinfo.oi_capa = capa;
	if (dv_flags & (LL_DV_WR_FLUSH | LL_DV_RD_FLUSH)) {
		oinfo.oi_oa->o_valid |= OBD_MD_FLFLAGS;
		oinfo.oi_oa->o_flags |= OBD_FL_SRVLOCK;
		if (dv_flags & LL_DV_WR_FLUSH)
			oinfo.oi_oa->o_flags |= OBD_FL_FLUSH;
	}

        set = ptlrpc_prep_set();
        if (set == NULL) {
                CERROR("can't allocate ptlrpc set\n");
                rc = -ENOMEM;
        } else {
                rc = obd_getattr_async(exp, &oinfo, set);
                if (rc == 0)
                        rc = ptlrpc_set_wait(set);
                ptlrpc_set_destroy(set);
        }
	if (rc == 0) {
		oinfo.oi_oa->o_valid &= (OBD_MD_FLBLOCKS | OBD_MD_FLBLKSZ |
					 OBD_MD_FLATIME | OBD_MD_FLMTIME |
					 OBD_MD_FLCTIME | OBD_MD_FLSIZE |
					 OBD_MD_FLDATAVERSION | OBD_MD_FLFLAGS);
		if (dv_flags & LL_DV_WR_FLUSH &&
		    !(oinfo.oi_oa->o_valid & OBD_MD_FLFLAGS &&
		      oinfo.oi_oa->o_flags & OBD_FL_FLUSH))
			RETURN(-ENOTSUPP);
	}
	RETURN(rc);
}

/**
  * Performs the getattr on the inode and updates its fields.
  * If @sync != 0, perform the getattr under the server-side lock.
  */
int ll_inode_getattr(struct inode *inode, struct obdo *obdo,
                     __u64 ioepoch, int sync)
{
	struct obd_capa      *capa = ll_mdscapa_get(inode);
	struct lov_stripe_md *lsm;
	int rc;
	ENTRY;

	lsm = ccc_inode_lsm_get(inode);
	rc = ll_lsm_getattr(lsm, ll_i2dtexp(inode),
				capa, obdo, ioepoch, sync ? LL_DV_RD_FLUSH : 0);
	capa_put(capa);
	if (rc == 0) {
		struct ost_id *oi = lsm ? &lsm->lsm_oi : &obdo->o_oi;

		obdo_refresh_inode(inode, obdo, obdo->o_valid);
		CDEBUG(D_INODE, "objid "DOSTID" size %llu, blocks %llu,"
		       " blksize %lu\n", POSTID(oi), i_size_read(inode),
		       (unsigned long long)inode->i_blocks,
		       (unsigned long)ll_inode_blksize(inode));
	}
	ccc_inode_lsm_put(inode, lsm);
	RETURN(rc);
}

int ll_merge_lvb(const struct lu_env *env, struct inode *inode)
{
	struct ll_inode_info *lli = ll_i2info(inode);
	struct cl_object *obj = lli->lli_clob;
	struct cl_attr *attr = ccc_env_thread_attr(env);
	struct ost_lvb lvb;
	int rc = 0;

	ENTRY;

	ll_inode_size_lock(inode);
	/* merge timestamps the most recently obtained from mds with
	   timestamps obtained from osts */
	LTIME_S(inode->i_atime) = lli->lli_lvb.lvb_atime;
	LTIME_S(inode->i_mtime) = lli->lli_lvb.lvb_mtime;
	LTIME_S(inode->i_ctime) = lli->lli_lvb.lvb_ctime;
	inode_init_lvb(inode, &lvb);

	cl_object_attr_lock(obj);
	rc = cl_object_attr_get(env, obj, attr);
	cl_object_attr_unlock(obj);

	if (rc == 0) {
		if (lvb.lvb_atime < attr->cat_atime)
			lvb.lvb_atime = attr->cat_atime;
		if (lvb.lvb_ctime < attr->cat_ctime)
			lvb.lvb_ctime = attr->cat_ctime;
		if (lvb.lvb_mtime < attr->cat_mtime)
			lvb.lvb_mtime = attr->cat_mtime;

		CDEBUG(D_VFSTRACE, DFID" updating i_size "LPU64"\n",
				PFID(&lli->lli_fid), attr->cat_size);
		cl_isize_write_nolock(inode, attr->cat_size);

		inode->i_blocks = attr->cat_blocks;

		LTIME_S(inode->i_mtime) = lvb.lvb_mtime;
		LTIME_S(inode->i_atime) = lvb.lvb_atime;
		LTIME_S(inode->i_ctime) = lvb.lvb_ctime;
	}
	ll_inode_size_unlock(inode);

	RETURN(rc);
}

int ll_glimpse_ioctl(struct ll_sb_info *sbi, struct lov_stripe_md *lsm,
                     lstat_t *st)
{
        struct obdo obdo = { 0 };
        int rc;

        rc = ll_lsm_getattr(lsm, sbi->ll_dt_exp, NULL, &obdo, 0, 0);
        if (rc == 0) {
                st->st_size   = obdo.o_size;
                st->st_blocks = obdo.o_blocks;
                st->st_mtime  = obdo.o_mtime;
                st->st_atime  = obdo.o_atime;
                st->st_ctime  = obdo.o_ctime;
        }
        return rc;
}

static bool file_is_noatime(const struct file *file)
{
	const struct vfsmount *mnt = file->f_path.mnt;
	const struct inode *inode = file->f_path.dentry->d_inode;

	/* Adapted from file_accessed() and touch_atime().*/
	if (file->f_flags & O_NOATIME)
		return true;

	if (inode->i_flags & S_NOATIME)
		return true;

	if (IS_NOATIME(inode))
		return true;

	if (mnt->mnt_flags & (MNT_NOATIME | MNT_READONLY))
		return true;

	if ((mnt->mnt_flags & MNT_NODIRATIME) && S_ISDIR(inode->i_mode))
		return true;

	if ((inode->i_sb->s_flags & MS_NODIRATIME) && S_ISDIR(inode->i_mode))
		return true;

	return false;
}

void ll_io_init(struct cl_io *io, const struct file *file, int write)
{
        struct inode *inode = file->f_dentry->d_inode;

        io->u.ci_rw.crw_nonblock = file->f_flags & O_NONBLOCK;
	if (write) {
		io->u.ci_wr.wr_append = !!(file->f_flags & O_APPEND);
		io->u.ci_wr.wr_sync = file->f_flags & O_SYNC ||
				      file->f_flags & O_DIRECT ||
				      IS_SYNC(inode);
	}
        io->ci_obj     = ll_i2info(inode)->lli_clob;
        io->ci_lockreq = CILR_MAYBE;
        if (ll_file_nolock(file)) {
                io->ci_lockreq = CILR_NEVER;
                io->ci_no_srvlock = 1;
        } else if (file->f_flags & O_APPEND) {
                io->ci_lockreq = CILR_MANDATORY;
        }

	io->ci_noatime = file_is_noatime(file);
}

static ssize_t
ll_file_io_generic(const struct lu_env *env, struct vvp_io_args *args,
		   struct file *file, enum cl_io_type iot,
		   loff_t *ppos, size_t count)
{
	struct ll_inode_info *lli = ll_i2info(file->f_dentry->d_inode);
	struct ll_file_data  *fd  = LUSTRE_FPRIVATE(file);
	struct cl_io         *io;
	ssize_t               result;
	ENTRY;

	CDEBUG(D_VFSTRACE, "file: %s, type: %d ppos: "LPU64", count: %zd\n",
		file->f_dentry->d_name.name, iot, *ppos, count);

restart:
        io = ccc_env_thread_io(env);
        ll_io_init(io, file, iot == CIT_WRITE);

        if (cl_io_rw_init(env, io, iot, *ppos, count) == 0) {
                struct vvp_io *vio = vvp_env_io(env);
                struct ccc_io *cio = ccc_env_io(env);
                int write_mutex_locked = 0;

                cio->cui_fd  = LUSTRE_FPRIVATE(file);
                vio->cui_io_subtype = args->via_io_subtype;

                switch (vio->cui_io_subtype) {
                case IO_NORMAL:
                        cio->cui_iov = args->u.normal.via_iov;
                        cio->cui_nrsegs = args->u.normal.via_nrsegs;
                        cio->cui_tot_nrsegs = cio->cui_nrsegs;
                        cio->cui_iocb = args->u.normal.via_iocb;
                        if ((iot == CIT_WRITE) &&
                            !(cio->cui_fd->fd_flags & LL_FILE_GROUP_LOCKED)) {
				if (mutex_lock_interruptible(&lli->
							lli_write_mutex))
					GOTO(out, result = -ERESTARTSYS);
				write_mutex_locked = 1;
			}
			down_read(&lli->lli_trunc_sem);
                        break;
                case IO_SPLICE:
                        vio->u.splice.cui_pipe = args->u.splice.via_pipe;
                        vio->u.splice.cui_flags = args->u.splice.via_flags;
                        break;
                default:
                        CERROR("Unknow IO type - %u\n", vio->cui_io_subtype);
                        LBUG();
                }
                result = cl_io_loop(env, io);
		if (args->via_io_subtype == IO_NORMAL)
			up_read(&lli->lli_trunc_sem);
		if (write_mutex_locked)
			mutex_unlock(&lli->lli_write_mutex);
        } else {
                /* cl_io_rw_init() handled IO */
                result = io->ci_result;
        }

        if (io->ci_nob > 0) {
                result = io->ci_nob;
                *ppos = io->u.ci_wr.wr.crw_pos;
        }
        GOTO(out, result);
out:
        cl_io_fini(env, io);
	/* If any bit been read/written (result != 0), we just return
	 * short read/write instead of restart io. */
	if ((result == 0 || result == -ENODATA) && io->ci_need_restart) {
		CDEBUG(D_VFSTRACE, "Restart %s on %s from %lld, count:%zd\n",
		       iot == CIT_READ ? "read" : "write",
		       file->f_dentry->d_name.name, *ppos, count);
		LASSERTF(io->ci_nob == 0, "%zd", io->ci_nob);
		goto restart;
	}

        if (iot == CIT_READ) {
                if (result >= 0)
                        ll_stats_ops_tally(ll_i2sbi(file->f_dentry->d_inode),
                                           LPROC_LL_READ_BYTES, result);
        } else if (iot == CIT_WRITE) {
                if (result >= 0) {
                        ll_stats_ops_tally(ll_i2sbi(file->f_dentry->d_inode),
                                           LPROC_LL_WRITE_BYTES, result);
			fd->fd_write_failed = false;
		} else if (result != -ERESTARTSYS) {
			fd->fd_write_failed = true;
		}
	}
	CDEBUG(D_VFSTRACE, "iot: %d, result: %zd\n", iot, result);

	return result;
}


/*
 * XXX: exact copy from kernel code (__generic_file_aio_write_nolock)
 */
static int ll_file_get_iov_count(const struct iovec *iov,
                                 unsigned long *nr_segs, size_t *count)
{
        size_t cnt = 0;
        unsigned long seg;

        for (seg = 0; seg < *nr_segs; seg++) {
                const struct iovec *iv = &iov[seg];

                /*
                 * If any segment has a negative length, or the cumulative
                 * length ever wraps negative then return -EINVAL.
                 */
                cnt += iv->iov_len;
                if (unlikely((ssize_t)(cnt|iv->iov_len) < 0))
                        return -EINVAL;
                if (access_ok(VERIFY_READ, iv->iov_base, iv->iov_len))
                        continue;
                if (seg == 0)
                        return -EFAULT;
                *nr_segs = seg;
                cnt -= iv->iov_len;   /* This segment is no good */
                break;
        }
        *count = cnt;
        return 0;
}

static ssize_t ll_file_aio_read(struct kiocb *iocb, const struct iovec *iov,
                                unsigned long nr_segs, loff_t pos)
{
        struct lu_env      *env;
        struct vvp_io_args *args;
        size_t              count;
        ssize_t             result;
        int                 refcheck;
        ENTRY;

        result = ll_file_get_iov_count(iov, &nr_segs, &count);
        if (result)
                RETURN(result);

        env = cl_env_get(&refcheck);
        if (IS_ERR(env))
                RETURN(PTR_ERR(env));

        args = vvp_env_args(env, IO_NORMAL);
        args->u.normal.via_iov = (struct iovec *)iov;
        args->u.normal.via_nrsegs = nr_segs;
        args->u.normal.via_iocb = iocb;

        result = ll_file_io_generic(env, args, iocb->ki_filp, CIT_READ,
                                    &iocb->ki_pos, count);
        cl_env_put(env, &refcheck);
        RETURN(result);
}

static ssize_t ll_file_read(struct file *file, char *buf, size_t count,
                            loff_t *ppos)
{
        struct lu_env *env;
        struct iovec  *local_iov;
        struct kiocb  *kiocb;
        ssize_t        result;
        int            refcheck;
        ENTRY;

        env = cl_env_get(&refcheck);
        if (IS_ERR(env))
                RETURN(PTR_ERR(env));

        local_iov = &vvp_env_info(env)->vti_local_iov;
        kiocb = &vvp_env_info(env)->vti_kiocb;
        local_iov->iov_base = (void __user *)buf;
        local_iov->iov_len = count;
        init_sync_kiocb(kiocb, file);
        kiocb->ki_pos = *ppos;
        kiocb->ki_left = count;

        result = ll_file_aio_read(kiocb, local_iov, 1, kiocb->ki_pos);
        *ppos = kiocb->ki_pos;

        cl_env_put(env, &refcheck);
        RETURN(result);
}

/*
 * Write to a file (through the page cache).
 * AIO stuff
 */
static ssize_t ll_file_aio_write(struct kiocb *iocb, const struct iovec *iov,
                                 unsigned long nr_segs, loff_t pos)
{
        struct lu_env      *env;
        struct vvp_io_args *args;
        size_t              count;
        ssize_t             result;
        int                 refcheck;
        ENTRY;

        result = ll_file_get_iov_count(iov, &nr_segs, &count);
        if (result)
                RETURN(result);

        env = cl_env_get(&refcheck);
        if (IS_ERR(env))
                RETURN(PTR_ERR(env));

        args = vvp_env_args(env, IO_NORMAL);
        args->u.normal.via_iov = (struct iovec *)iov;
        args->u.normal.via_nrsegs = nr_segs;
        args->u.normal.via_iocb = iocb;

        result = ll_file_io_generic(env, args, iocb->ki_filp, CIT_WRITE,
                                  &iocb->ki_pos, count);
        cl_env_put(env, &refcheck);
        RETURN(result);
}

static ssize_t ll_file_write(struct file *file, const char *buf, size_t count,
                             loff_t *ppos)
{
        struct lu_env *env;
        struct iovec  *local_iov;
        struct kiocb  *kiocb;
        ssize_t        result;
        int            refcheck;
        ENTRY;

        env = cl_env_get(&refcheck);
        if (IS_ERR(env))
                RETURN(PTR_ERR(env));

        local_iov = &vvp_env_info(env)->vti_local_iov;
        kiocb = &vvp_env_info(env)->vti_kiocb;
        local_iov->iov_base = (void __user *)buf;
        local_iov->iov_len = count;
        init_sync_kiocb(kiocb, file);
        kiocb->ki_pos = *ppos;
        kiocb->ki_left = count;

        result = ll_file_aio_write(kiocb, local_iov, 1, kiocb->ki_pos);
        *ppos = kiocb->ki_pos;

        cl_env_put(env, &refcheck);
        RETURN(result);
}

/*
 * Send file content (through pagecache) somewhere with helper
 */
static ssize_t ll_file_splice_read(struct file *in_file, loff_t *ppos,
                                   struct pipe_inode_info *pipe, size_t count,
                                   unsigned int flags)
{
        struct lu_env      *env;
        struct vvp_io_args *args;
        ssize_t             result;
        int                 refcheck;
        ENTRY;

        env = cl_env_get(&refcheck);
        if (IS_ERR(env))
                RETURN(PTR_ERR(env));

        args = vvp_env_args(env, IO_SPLICE);
        args->u.splice.via_pipe = pipe;
        args->u.splice.via_flags = flags;

        result = ll_file_io_generic(env, args, in_file, CIT_READ, ppos, count);
        cl_env_put(env, &refcheck);
        RETURN(result);
}

static int ll_lov_recreate(struct inode *inode, struct ost_id *oi,
                           obd_count ost_idx)
{
	struct obd_export *exp = ll_i2dtexp(inode);
	struct obd_trans_info oti = { 0 };
	struct obdo *oa = NULL;
	int lsm_size;
	int rc = 0;
	struct lov_stripe_md *lsm = NULL, *lsm2;
	ENTRY;

	OBDO_ALLOC(oa);
	if (oa == NULL)
		RETURN(-ENOMEM);

	lsm = ccc_inode_lsm_get(inode);
	if (!lsm_has_objects(lsm))
                GOTO(out, rc = -ENOENT);

        lsm_size = sizeof(*lsm) + (sizeof(struct lov_oinfo) *
                   (lsm->lsm_stripe_count));

        OBD_ALLOC_LARGE(lsm2, lsm_size);
        if (lsm2 == NULL)
                GOTO(out, rc = -ENOMEM);

	oa->o_oi = *oi;
        oa->o_nlink = ost_idx;
        oa->o_flags |= OBD_FL_RECREATE_OBJS;
        oa->o_valid = OBD_MD_FLID | OBD_MD_FLFLAGS | OBD_MD_FLGROUP;
        obdo_from_inode(oa, inode, OBD_MD_FLTYPE | OBD_MD_FLATIME |
                                   OBD_MD_FLMTIME | OBD_MD_FLCTIME);
        obdo_set_parent_fid(oa, &ll_i2info(inode)->lli_fid);
        memcpy(lsm2, lsm, lsm_size);
	ll_inode_size_lock(inode);
	rc = obd_create(NULL, exp, oa, &lsm2, &oti);
	ll_inode_size_unlock(inode);

	OBD_FREE_LARGE(lsm2, lsm_size);
	GOTO(out, rc);
out:
	ccc_inode_lsm_put(inode, lsm);
	OBDO_FREE(oa);
	return rc;
}

static int ll_lov_recreate_obj(struct inode *inode, unsigned long arg)
{
	struct ll_recreate_obj ucreat;
	struct ost_id		oi;
	ENTRY;

	if (!cfs_capable(CFS_CAP_SYS_ADMIN))
		RETURN(-EPERM);

	if (copy_from_user(&ucreat, (struct ll_recreate_obj *)arg,
			   sizeof(ucreat)))
		RETURN(-EFAULT);

	ostid_set_seq_mdt0(&oi);
	ostid_set_id(&oi, ucreat.lrc_id);
	RETURN(ll_lov_recreate(inode, &oi, ucreat.lrc_ost_idx));
}

static int ll_lov_recreate_fid(struct inode *inode, unsigned long arg)
{
	struct lu_fid	fid;
	struct ost_id	oi;
	obd_count	ost_idx;
        ENTRY;

	if (!cfs_capable(CFS_CAP_SYS_ADMIN))
		RETURN(-EPERM);

	if (copy_from_user(&fid, (struct lu_fid *)arg, sizeof(fid)))
		RETURN(-EFAULT);

	fid_to_ostid(&fid, &oi);
	ost_idx = (fid_seq(&fid) >> 16) & 0xffff;
	RETURN(ll_lov_recreate(inode, &oi, ost_idx));
}

int ll_lov_setstripe_ea_info(struct inode *inode, struct file *file,
                             __u64  flags, struct lov_user_md *lum,
			     int lum_size)
{
	struct lov_stripe_md *lsm = NULL;
	struct lookup_intent oit = {.it_op = IT_OPEN, .it_flags = flags};
	int rc = 0;
	ENTRY;

	lsm = ccc_inode_lsm_get(inode);
	if (lsm != NULL) {
		ccc_inode_lsm_put(inode, lsm);
		CDEBUG(D_IOCTL, "stripe already exists for inode "DFID"\n",
		       PFID(ll_inode2fid(inode)));
		GOTO(out, rc = -EEXIST);
	}

	ll_inode_size_lock(inode);
	rc = ll_intent_file_open(file, lum, lum_size, &oit);
	if (rc)
		GOTO(out_unlock, rc);
	rc = oit.d.lustre.it_status;
	if (rc < 0)
		GOTO(out_req_free, rc);

	ll_release_openhandle(file->f_dentry, &oit);

out_unlock:
	ll_inode_size_unlock(inode);
	ll_intent_release(&oit);
	ccc_inode_lsm_put(inode, lsm);
out:
	cl_lov_delay_create_clear(&file->f_flags);
	RETURN(rc);
out_req_free:
	ptlrpc_req_finished((struct ptlrpc_request *) oit.d.lustre.it_data);
	goto out;
}

int ll_lov_getstripe_ea_info(struct inode *inode, const char *filename,
                             struct lov_mds_md **lmmp, int *lmm_size,
                             struct ptlrpc_request **request)
{
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct mdt_body  *body;
        struct lov_mds_md *lmm = NULL;
        struct ptlrpc_request *req = NULL;
        struct md_op_data *op_data;
        int rc, lmmsize;

	rc = ll_get_default_mdsize(sbi, &lmmsize);
	if (rc)
		RETURN(rc);

        op_data = ll_prep_md_op_data(NULL, inode, NULL, filename,
                                     strlen(filename), lmmsize,
                                     LUSTRE_OPC_ANY, NULL);
        if (IS_ERR(op_data))
                RETURN(PTR_ERR(op_data));

        op_data->op_valid = OBD_MD_FLEASIZE | OBD_MD_FLDIREA;
        rc = md_getattr_name(sbi->ll_md_exp, op_data, &req);
        ll_finish_md_op_data(op_data);
        if (rc < 0) {
                CDEBUG(D_INFO, "md_getattr_name failed "
                       "on %s: rc %d\n", filename, rc);
                GOTO(out, rc);
        }

        body = req_capsule_server_get(&req->rq_pill, &RMF_MDT_BODY);
        LASSERT(body != NULL); /* checked by mdc_getattr_name */

        lmmsize = body->eadatasize;

        if (!(body->valid & (OBD_MD_FLEASIZE | OBD_MD_FLDIREA)) ||
                        lmmsize == 0) {
                GOTO(out, rc = -ENODATA);
        }

        lmm = req_capsule_server_sized_get(&req->rq_pill, &RMF_MDT_MD, lmmsize);
        LASSERT(lmm != NULL);

        if ((lmm->lmm_magic != cpu_to_le32(LOV_MAGIC_V1)) &&
            (lmm->lmm_magic != cpu_to_le32(LOV_MAGIC_V3))) {
                GOTO(out, rc = -EPROTO);
        }

        /*
         * This is coming from the MDS, so is probably in
         * little endian.  We convert it to host endian before
         * passing it to userspace.
         */
        if (LOV_MAGIC != cpu_to_le32(LOV_MAGIC)) {
		int stripe_count;

		stripe_count = le16_to_cpu(lmm->lmm_stripe_count);
		if (le32_to_cpu(lmm->lmm_pattern) & LOV_PATTERN_F_RELEASED)
			stripe_count = 0;

                /* if function called for directory - we should
                 * avoid swab not existent lsm objects */
                if (lmm->lmm_magic == cpu_to_le32(LOV_MAGIC_V1)) {
                        lustre_swab_lov_user_md_v1((struct lov_user_md_v1 *)lmm);
                        if (S_ISREG(body->mode))
                                lustre_swab_lov_user_md_objects(
                                 ((struct lov_user_md_v1 *)lmm)->lmm_objects,
                                 stripe_count);
                } else if (lmm->lmm_magic == cpu_to_le32(LOV_MAGIC_V3)) {
                        lustre_swab_lov_user_md_v3((struct lov_user_md_v3 *)lmm);
                        if (S_ISREG(body->mode))
                                lustre_swab_lov_user_md_objects(
                                 ((struct lov_user_md_v3 *)lmm)->lmm_objects,
                                 stripe_count);
                }
        }

out:
        *lmmp = lmm;
        *lmm_size = lmmsize;
        *request = req;
        return rc;
}

static int ll_lov_setea(struct inode *inode, struct file *file,
                            unsigned long arg)
{
	__u64			 flags = MDS_OPEN_HAS_OBJS | FMODE_WRITE;
	struct lov_user_md	*lump;
	int			 lum_size = sizeof(struct lov_user_md) +
					    sizeof(struct lov_user_ost_data);
	int			 rc;
	ENTRY;

	if (!cfs_capable(CFS_CAP_SYS_ADMIN))
		RETURN(-EPERM);

	OBD_ALLOC_LARGE(lump, lum_size);
	if (lump == NULL)
                RETURN(-ENOMEM);

	if (copy_from_user(lump, (struct lov_user_md  *)arg, lum_size)) {
		OBD_FREE_LARGE(lump, lum_size);
		RETURN(-EFAULT);
	}

	rc = ll_lov_setstripe_ea_info(inode, file, flags, lump, lum_size);

	OBD_FREE_LARGE(lump, lum_size);
	RETURN(rc);
}

static int ll_lov_setstripe(struct inode *inode, struct file *file,
			    unsigned long arg)
{
	struct lov_user_md_v3	 lumv3;
	struct lov_user_md_v1	*lumv1 = (struct lov_user_md_v1 *)&lumv3;
	struct lov_user_md_v1	*lumv1p = (struct lov_user_md_v1 *)arg;
	struct lov_user_md_v3	*lumv3p = (struct lov_user_md_v3 *)arg;
	int			 lum_size, rc;
	__u64			 flags = FMODE_WRITE;
	ENTRY;

	/* first try with v1 which is smaller than v3 */
	lum_size = sizeof(struct lov_user_md_v1);
	if (copy_from_user(lumv1, lumv1p, lum_size))
		RETURN(-EFAULT);

	if (lumv1->lmm_magic == LOV_USER_MAGIC_V3) {
		lum_size = sizeof(struct lov_user_md_v3);
		if (copy_from_user(&lumv3, lumv3p, lum_size))
			RETURN(-EFAULT);
	}

	rc = ll_lov_setstripe_ea_info(inode, file, flags, lumv1, lum_size);
	if (rc == 0) {
		struct lov_stripe_md *lsm;
		__u32 gen;

		put_user(0, &lumv1p->lmm_stripe_count);

		ll_layout_refresh(inode, &gen);
		lsm = ccc_inode_lsm_get(inode);
		rc = obd_iocontrol(LL_IOC_LOV_GETSTRIPE, ll_i2dtexp(inode),
				   0, lsm, (void *)arg);
		ccc_inode_lsm_put(inode, lsm);
	}
	RETURN(rc);
}

static int ll_lov_getstripe(struct inode *inode, unsigned long arg)
{
	struct lov_stripe_md *lsm;
	int rc = -ENODATA;
	ENTRY;

	lsm = ccc_inode_lsm_get(inode);
	if (lsm != NULL)
		rc = obd_iocontrol(LL_IOC_LOV_GETSTRIPE, ll_i2dtexp(inode), 0,
				   lsm, (void *)arg);
	ccc_inode_lsm_put(inode, lsm);
	RETURN(rc);
}

int ll_get_grouplock(struct inode *inode, struct file *file, unsigned long arg)
{
        struct ll_inode_info   *lli = ll_i2info(inode);
        struct ll_file_data    *fd = LUSTRE_FPRIVATE(file);
        struct ccc_grouplock    grouplock;
        int                     rc;
        ENTRY;

        if (ll_file_nolock(file))
                RETURN(-EOPNOTSUPP);

	spin_lock(&lli->lli_lock);
	if (fd->fd_flags & LL_FILE_GROUP_LOCKED) {
		CWARN("group lock already existed with gid %lu\n",
		      fd->fd_grouplock.cg_gid);
		spin_unlock(&lli->lli_lock);
		RETURN(-EINVAL);
	}
	LASSERT(fd->fd_grouplock.cg_lock == NULL);
	spin_unlock(&lli->lli_lock);

	rc = cl_get_grouplock(cl_i2info(inode)->lli_clob,
			      arg, (file->f_flags & O_NONBLOCK), &grouplock);
	if (rc)
		RETURN(rc);

	spin_lock(&lli->lli_lock);
	if (fd->fd_flags & LL_FILE_GROUP_LOCKED) {
		spin_unlock(&lli->lli_lock);
		CERROR("another thread just won the race\n");
		cl_put_grouplock(&grouplock);
		RETURN(-EINVAL);
	}

	fd->fd_flags |= LL_FILE_GROUP_LOCKED;
	fd->fd_grouplock = grouplock;
	spin_unlock(&lli->lli_lock);

	CDEBUG(D_INFO, "group lock %lu obtained\n", arg);
	RETURN(0);
}

int ll_put_grouplock(struct inode *inode, struct file *file, unsigned long arg)
{
	struct ll_inode_info   *lli = ll_i2info(inode);
	struct ll_file_data    *fd = LUSTRE_FPRIVATE(file);
	struct ccc_grouplock    grouplock;
	ENTRY;

	spin_lock(&lli->lli_lock);
	if (!(fd->fd_flags & LL_FILE_GROUP_LOCKED)) {
		spin_unlock(&lli->lli_lock);
                CWARN("no group lock held\n");
                RETURN(-EINVAL);
        }
        LASSERT(fd->fd_grouplock.cg_lock != NULL);

        if (fd->fd_grouplock.cg_gid != arg) {
                CWARN("group lock %lu doesn't match current id %lu\n",
                       arg, fd->fd_grouplock.cg_gid);
		spin_unlock(&lli->lli_lock);
		RETURN(-EINVAL);
	}

	grouplock = fd->fd_grouplock;
	memset(&fd->fd_grouplock, 0, sizeof(fd->fd_grouplock));
	fd->fd_flags &= ~LL_FILE_GROUP_LOCKED;
	spin_unlock(&lli->lli_lock);

	cl_put_grouplock(&grouplock);
	CDEBUG(D_INFO, "group lock %lu released\n", arg);
	RETURN(0);
}

/**
 * Close inode open handle
 *
 * \param dentry [in]     dentry which contains the inode
 * \param it     [in,out] intent which contains open info and result
 *
 * \retval 0     success
 * \retval <0    failure
 */
int ll_release_openhandle(struct dentry *dentry, struct lookup_intent *it)
{
        struct inode *inode = dentry->d_inode;
        struct obd_client_handle *och;
        int rc;
        ENTRY;

        LASSERT(inode);

        /* Root ? Do nothing. */
        if (dentry->d_inode->i_sb->s_root == dentry)
                RETURN(0);

        /* No open handle to close? Move away */
        if (!it_disposition(it, DISP_OPEN_OPEN))
                RETURN(0);

        LASSERT(it_open_error(DISP_OPEN_OPEN, it) == 0);

        OBD_ALLOC(och, sizeof(*och));
        if (!och)
                GOTO(out, rc = -ENOMEM);

	ll_och_fill(ll_i2sbi(inode)->ll_md_exp, it, och);

        rc = ll_close_inode_openhandle(ll_i2sbi(inode)->ll_md_exp,
				       inode, och, NULL);
out:
	/* this one is in place of ll_file_open */
	if (it_disposition(it, DISP_ENQ_OPEN_REF)) {
		ptlrpc_req_finished(it->d.lustre.it_data);
		it_clear_disposition(it, DISP_ENQ_OPEN_REF);
	}
	RETURN(rc);
}

/**
 * Get size for inode for which FIEMAP mapping is requested.
 * Make the FIEMAP get_info call and returns the result.
 */
static int ll_do_fiemap(struct inode *inode, struct ll_user_fiemap *fiemap,
			size_t num_bytes)
{
	struct obd_export *exp = ll_i2dtexp(inode);
	struct lov_stripe_md *lsm = NULL;
        struct ll_fiemap_info_key fm_key = { .name = KEY_FIEMAP, };
	__u32 vallen = num_bytes;
        int rc;
        ENTRY;

        /* Checks for fiemap flags */
        if (fiemap->fm_flags & ~LUSTRE_FIEMAP_FLAGS_COMPAT) {
                fiemap->fm_flags &= ~LUSTRE_FIEMAP_FLAGS_COMPAT;
                return -EBADR;
        }

        /* Check for FIEMAP_FLAG_SYNC */
        if (fiemap->fm_flags & FIEMAP_FLAG_SYNC) {
                rc = filemap_fdatawrite(inode->i_mapping);
                if (rc)
                        return rc;
        }

	lsm = ccc_inode_lsm_get(inode);
	if (lsm == NULL)
		return -ENOENT;

	/* If the stripe_count > 1 and the application does not understand
	 * DEVICE_ORDER flag, then it cannot interpret the extents correctly.
	 */
	if (lsm->lsm_stripe_count > 1 &&
	    !(fiemap->fm_flags & FIEMAP_FLAG_DEVICE_ORDER))
		GOTO(out, rc = -EOPNOTSUPP);

	fm_key.oa.o_oi = lsm->lsm_oi;
        fm_key.oa.o_valid = OBD_MD_FLID | OBD_MD_FLGROUP;

        obdo_from_inode(&fm_key.oa, inode, OBD_MD_FLSIZE);
        obdo_set_parent_fid(&fm_key.oa, &ll_i2info(inode)->lli_fid);
        /* If filesize is 0, then there would be no objects for mapping */
        if (fm_key.oa.o_size == 0) {
                fiemap->fm_mapped_extents = 0;
		GOTO(out, rc = 0);
        }

        memcpy(&fm_key.fiemap, fiemap, sizeof(*fiemap));

        rc = obd_get_info(NULL, exp, sizeof(fm_key), &fm_key, &vallen,
                          fiemap, lsm);
        if (rc)
                CERROR("obd_get_info failed: rc = %d\n", rc);

out:
	ccc_inode_lsm_put(inode, lsm);
	RETURN(rc);
}

int ll_fid2path(struct inode *inode, void *arg)
{
	struct obd_export	*exp = ll_i2mdexp(inode);
	struct getinfo_fid2path	*gfout, *gfin;
	int			 outsize, rc;
	ENTRY;

	if (!cfs_capable(CFS_CAP_DAC_READ_SEARCH) &&
	    !(ll_i2sbi(inode)->ll_flags & LL_SBI_USER_FID2PATH))
		RETURN(-EPERM);

	/* Need to get the buflen */
	OBD_ALLOC_PTR(gfin);
	if (gfin == NULL)
		RETURN(-ENOMEM);
	if (copy_from_user(gfin, arg, sizeof(*gfin))) {
		OBD_FREE_PTR(gfin);
		RETURN(-EFAULT);
	}

	outsize = sizeof(*gfout) + gfin->gf_pathlen;
	OBD_ALLOC(gfout, outsize);
	if (gfout == NULL) {
		OBD_FREE_PTR(gfin);
		RETURN(-ENOMEM);
	}
	memcpy(gfout, gfin, sizeof(*gfout));
	OBD_FREE_PTR(gfin);

	/* Call mdc_iocontrol */
	rc = obd_iocontrol(OBD_IOC_FID2PATH, exp, outsize, gfout, NULL);
	if (rc)
		GOTO(gf_free, rc);

	if (copy_to_user(arg, gfout, outsize))
		rc = -EFAULT;

gf_free:
	OBD_FREE(gfout, outsize);
	RETURN(rc);
}

static int ll_ioctl_fiemap(struct inode *inode, unsigned long arg)
{
        struct ll_user_fiemap *fiemap_s;
        size_t num_bytes, ret_bytes;
        unsigned int extent_count;
        int rc = 0;

        /* Get the extent count so we can calculate the size of
         * required fiemap buffer */
        if (get_user(extent_count,
            &((struct ll_user_fiemap __user *)arg)->fm_extent_count))
                RETURN(-EFAULT);
        num_bytes = sizeof(*fiemap_s) + (extent_count *
                                         sizeof(struct ll_fiemap_extent));

        OBD_ALLOC_LARGE(fiemap_s, num_bytes);
        if (fiemap_s == NULL)
                RETURN(-ENOMEM);

	/* get the fiemap value */
	if (copy_from_user(fiemap_s, (struct ll_user_fiemap __user *)arg,
			   sizeof(*fiemap_s)))
		GOTO(error, rc = -EFAULT);

        /* If fm_extent_count is non-zero, read the first extent since
         * it is used to calculate end_offset and device from previous
         * fiemap call. */
        if (extent_count) {
                if (copy_from_user(&fiemap_s->fm_extents[0],
                    (char __user *)arg + sizeof(*fiemap_s),
                    sizeof(struct ll_fiemap_extent)))
                        GOTO(error, rc = -EFAULT);
        }

        rc = ll_do_fiemap(inode, fiemap_s, num_bytes);
        if (rc)
                GOTO(error, rc);

        ret_bytes = sizeof(struct ll_user_fiemap);

        if (extent_count != 0)
                ret_bytes += (fiemap_s->fm_mapped_extents *
                                 sizeof(struct ll_fiemap_extent));

	if (copy_to_user((void *)arg, fiemap_s, ret_bytes))
		rc = -EFAULT;

error:
        OBD_FREE_LARGE(fiemap_s, num_bytes);
        RETURN(rc);
}

/*
 * Read the data_version for inode.
 *
 * This value is computed using stripe object version on OST.
 * Version is computed using server side locking.
 *
 * @param sync if do sync on the OST side;
 *		0: no sync
 *		LL_DV_RD_FLUSH: flush dirty pages, LCK_PR on OSTs
 *		LL_DV_WR_FLUSH: drop all caching pages, LCK_PW on OSTs
 */
int ll_data_version(struct inode *inode, __u64 *data_version, int flags)
{
	struct lov_stripe_md	*lsm = NULL;
	struct ll_sb_info	*sbi = ll_i2sbi(inode);
	struct obdo		*obdo = NULL;
	int			 rc;
	ENTRY;

	/* If no stripe, we consider version is 0. */
	lsm = ccc_inode_lsm_get(inode);
	if (!lsm_has_objects(lsm)) {
		*data_version = 0;
		CDEBUG(D_INODE, "No object for inode\n");
		GOTO(out, rc = 0);
	}

	OBD_ALLOC_PTR(obdo);
	if (obdo == NULL)
		GOTO(out, rc = -ENOMEM);

	rc = ll_lsm_getattr(lsm, sbi->ll_dt_exp, NULL, obdo, 0, flags);
	if (rc == 0) {
		if (!(obdo->o_valid & OBD_MD_FLDATAVERSION))
			rc = -EOPNOTSUPP;
		else
			*data_version = obdo->o_data_version;
	}

	OBD_FREE_PTR(obdo);
	EXIT;
out:
	ccc_inode_lsm_put(inode, lsm);
	RETURN(rc);
}

/*
 * Trigger a HSM release request for the provided inode.
 */
int ll_hsm_release(struct inode *inode)
{
	struct cl_env_nest nest;
	struct lu_env *env;
	struct obd_client_handle *och = NULL;
	__u64 data_version = 0;
	int rc;
	ENTRY;

	CDEBUG(D_INODE, "%s: Releasing file "DFID".\n",
	       ll_get_fsname(inode->i_sb, NULL, 0),
	       PFID(&ll_i2info(inode)->lli_fid));

	och = ll_lease_open(inode, NULL, FMODE_WRITE, MDS_OPEN_RELEASE);
	if (IS_ERR(och))
		GOTO(out, rc = PTR_ERR(och));

	/* Grab latest data_version and [am]time values */
	rc = ll_data_version(inode, &data_version, LL_DV_WR_FLUSH);
	if (rc != 0)
		GOTO(out, rc);

	env = cl_env_nested_get(&nest);
	if (IS_ERR(env))
		GOTO(out, rc = PTR_ERR(env));

	ll_merge_lvb(env, inode);
	cl_env_nested_put(&nest, env);

	/* Release the file.
	 * NB: lease lock handle is released in mdc_hsm_release_pack() because
	 * we still need it to pack l_remote_handle to MDT. */
	rc = ll_close_inode_openhandle(ll_i2sbi(inode)->ll_md_exp, inode, och,
				       &data_version);
	och = NULL;

	EXIT;
out:
	if (och != NULL && !IS_ERR(och)) /* close the file */
		ll_lease_close(och, inode, NULL);

	return rc;
}

struct ll_swap_stack {
	struct iattr		 ia1, ia2;
	__u64			 dv1, dv2;
	struct inode		*inode1, *inode2;
	bool			 check_dv1, check_dv2;
};

static int ll_swap_layouts(struct file *file1, struct file *file2,
			   struct lustre_swap_layouts *lsl)
{
	struct mdc_swap_layouts	 msl;
	struct md_op_data	*op_data;
	__u32			 gid;
	__u64			 dv;
	struct ll_swap_stack	*llss = NULL;
	int			 rc;

	OBD_ALLOC_PTR(llss);
	if (llss == NULL)
		RETURN(-ENOMEM);

	llss->inode1 = file1->f_dentry->d_inode;
	llss->inode2 = file2->f_dentry->d_inode;

	if (!S_ISREG(llss->inode2->i_mode))
		GOTO(free, rc = -EINVAL);

	if (inode_permission(llss->inode1, MAY_WRITE) ||
	    inode_permission(llss->inode2, MAY_WRITE))
		GOTO(free, rc = -EPERM);

	if (llss->inode2->i_sb != llss->inode1->i_sb)
		GOTO(free, rc = -EXDEV);

	/* we use 2 bool because it is easier to swap than 2 bits */
	if (lsl->sl_flags & SWAP_LAYOUTS_CHECK_DV1)
		llss->check_dv1 = true;

	if (lsl->sl_flags & SWAP_LAYOUTS_CHECK_DV2)
		llss->check_dv2 = true;

	/* we cannot use lsl->sl_dvX directly because we may swap them */
	llss->dv1 = lsl->sl_dv1;
	llss->dv2 = lsl->sl_dv2;

	rc = lu_fid_cmp(ll_inode2fid(llss->inode1), ll_inode2fid(llss->inode2));
	if (rc == 0) /* same file, done! */
		GOTO(free, rc = 0);

	if (rc < 0) { /* sequentialize it */
		swap(llss->inode1, llss->inode2);
		swap(file1, file2);
		swap(llss->dv1, llss->dv2);
		swap(llss->check_dv1, llss->check_dv2);
	}

	gid = lsl->sl_gid;
	if (gid != 0) { /* application asks to flush dirty cache */
		rc = ll_get_grouplock(llss->inode1, file1, gid);
		if (rc < 0)
			GOTO(free, rc);

		rc = ll_get_grouplock(llss->inode2, file2, gid);
		if (rc < 0) {
			ll_put_grouplock(llss->inode1, file1, gid);
			GOTO(free, rc);
		}
	}

	/* to be able to restore mtime and atime after swap
	 * we need to first save them */
	if (lsl->sl_flags &
	    (SWAP_LAYOUTS_KEEP_MTIME | SWAP_LAYOUTS_KEEP_ATIME)) {
		llss->ia1.ia_mtime = llss->inode1->i_mtime;
		llss->ia1.ia_atime = llss->inode1->i_atime;
		llss->ia1.ia_valid = ATTR_MTIME | ATTR_ATIME;
		llss->ia2.ia_mtime = llss->inode2->i_mtime;
		llss->ia2.ia_atime = llss->inode2->i_atime;
		llss->ia2.ia_valid = ATTR_MTIME | ATTR_ATIME;
	}

	/* ultimate check, before swaping the layouts we check if
	 * dataversion has changed (if requested) */
	if (llss->check_dv1) {
		rc = ll_data_version(llss->inode1, &dv, 0);
		if (rc)
			GOTO(putgl, rc);
		if (dv != llss->dv1)
			GOTO(putgl, rc = -EAGAIN);
	}

	if (llss->check_dv2) {
		rc = ll_data_version(llss->inode2, &dv, 0);
		if (rc)
			GOTO(putgl, rc);
		if (dv != llss->dv2)
			GOTO(putgl, rc = -EAGAIN);
	}

	/* struct md_op_data is used to send the swap args to the mdt
	 * only flags is missing, so we use struct mdc_swap_layouts
	 * through the md_op_data->op_data */
	/* flags from user space have to be converted before they are send to
	 * server, no flag is sent today, they are only used on the client */
	msl.msl_flags = 0;
	rc = -ENOMEM;
	op_data = ll_prep_md_op_data(NULL, llss->inode1, llss->inode2, NULL, 0,
				     0, LUSTRE_OPC_ANY, &msl);
	if (IS_ERR(op_data))
		GOTO(free, rc = PTR_ERR(op_data));

	rc = obd_iocontrol(LL_IOC_LOV_SWAP_LAYOUTS, ll_i2mdexp(llss->inode1),
			   sizeof(*op_data), op_data, NULL);
	ll_finish_md_op_data(op_data);

putgl:
	if (gid != 0) {
		ll_put_grouplock(llss->inode2, file2, gid);
		ll_put_grouplock(llss->inode1, file1, gid);
	}

	/* rc can be set from obd_iocontrol() or from a GOTO(putgl, ...) */
	if (rc != 0)
		GOTO(free, rc);

	/* clear useless flags */
	if (!(lsl->sl_flags & SWAP_LAYOUTS_KEEP_MTIME)) {
		llss->ia1.ia_valid &= ~ATTR_MTIME;
		llss->ia2.ia_valid &= ~ATTR_MTIME;
	}

	if (!(lsl->sl_flags & SWAP_LAYOUTS_KEEP_ATIME)) {
		llss->ia1.ia_valid &= ~ATTR_ATIME;
		llss->ia2.ia_valid &= ~ATTR_ATIME;
	}

	/* update time if requested */
	rc = 0;
	if (llss->ia2.ia_valid != 0) {
		mutex_lock(&llss->inode1->i_mutex);
		rc = ll_setattr(file1->f_dentry, &llss->ia2);
		mutex_unlock(&llss->inode1->i_mutex);
	}

	if (llss->ia1.ia_valid != 0) {
		int rc1;

		mutex_lock(&llss->inode2->i_mutex);
		rc1 = ll_setattr(file2->f_dentry, &llss->ia1);
		mutex_unlock(&llss->inode2->i_mutex);
		if (rc == 0)
			rc = rc1;
	}

free:
	if (llss != NULL)
		OBD_FREE_PTR(llss);

	RETURN(rc);
}

static int ll_hsm_state_set(struct inode *inode, struct hsm_state_set *hss)
{
	struct md_op_data	*op_data;
	int			 rc;

	/* Non-root users are forbidden to set or clear flags which are
	 * NOT defined in HSM_USER_MASK. */
	if (((hss->hss_setmask | hss->hss_clearmask) & ~HSM_USER_MASK) &&
	    !cfs_capable(CFS_CAP_SYS_ADMIN))
		RETURN(-EPERM);

	op_data = ll_prep_md_op_data(NULL, inode, NULL, NULL, 0, 0,
				     LUSTRE_OPC_ANY, hss);
	if (IS_ERR(op_data))
		RETURN(PTR_ERR(op_data));

	rc = obd_iocontrol(LL_IOC_HSM_STATE_SET, ll_i2mdexp(inode),
			   sizeof(*op_data), op_data, NULL);

	ll_finish_md_op_data(op_data);

	RETURN(rc);
}

static int ll_hsm_import(struct inode *inode, struct file *file,
			 struct hsm_user_import *hui)
{
	struct hsm_state_set	*hss = NULL;
	struct iattr		*attr = NULL;
	int			 rc;
	ENTRY;

	if (!S_ISREG(inode->i_mode))
		RETURN(-EINVAL);

	/* set HSM flags */
	OBD_ALLOC_PTR(hss);
	if (hss == NULL)
		GOTO(out, rc = -ENOMEM);

	hss->hss_valid = HSS_SETMASK | HSS_ARCHIVE_ID;
	hss->hss_archive_id = hui->hui_archive_id;
	hss->hss_setmask = HS_ARCHIVED | HS_EXISTS | HS_RELEASED;
	rc = ll_hsm_state_set(inode, hss);
	if (rc != 0)
		GOTO(out, rc);

	OBD_ALLOC_PTR(attr);
	if (attr == NULL)
		GOTO(out, rc = -ENOMEM);

	attr->ia_mode = hui->hui_mode & (S_IRWXU | S_IRWXG | S_IRWXO);
	attr->ia_mode |= S_IFREG;
	attr->ia_uid = hui->hui_uid;
	attr->ia_gid = hui->hui_gid;
	attr->ia_size = hui->hui_size;
	attr->ia_mtime.tv_sec = hui->hui_mtime;
	attr->ia_mtime.tv_nsec = hui->hui_mtime_ns;
	attr->ia_atime.tv_sec = hui->hui_atime;
	attr->ia_atime.tv_nsec = hui->hui_atime_ns;

	attr->ia_valid = ATTR_SIZE | ATTR_MODE | ATTR_FORCE |
			 ATTR_UID | ATTR_GID |
			 ATTR_MTIME | ATTR_MTIME_SET |
			 ATTR_ATIME | ATTR_ATIME_SET;

	rc = ll_setattr_raw(file->f_dentry, attr, true);
	if (rc == -ENODATA)
		rc = 0;

out:
	if (hss != NULL)
		OBD_FREE_PTR(hss);

	if (attr != NULL)
		OBD_FREE_PTR(attr);

	RETURN(rc);
}

long ll_file_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct inode		*inode = file->f_dentry->d_inode;
	struct ll_file_data	*fd = LUSTRE_FPRIVATE(file);
	int			 flags, rc;
	ENTRY;

	CDEBUG(D_VFSTRACE, "VFS Op:inode="DFID"(%p), cmd=%x\n",
	       PFID(ll_inode2fid(inode)), inode, cmd);
        ll_stats_ops_tally(ll_i2sbi(inode), LPROC_LL_IOCTL, 1);

        /* asm-ppc{,64} declares TCGETS, et. al. as type 't' not 'T' */
        if (_IOC_TYPE(cmd) == 'T' || _IOC_TYPE(cmd) == 't') /* tty ioctls */
                RETURN(-ENOTTY);

        switch(cmd) {
        case LL_IOC_GETFLAGS:
                /* Get the current value of the file flags */
                return put_user(fd->fd_flags, (int *)arg);
        case LL_IOC_SETFLAGS:
        case LL_IOC_CLRFLAGS:
                /* Set or clear specific file flags */
                /* XXX This probably needs checks to ensure the flags are
                 *     not abused, and to handle any flag side effects.
                 */
                if (get_user(flags, (int *) arg))
                        RETURN(-EFAULT);

                if (cmd == LL_IOC_SETFLAGS) {
                        if ((flags & LL_FILE_IGNORE_LOCK) &&
                            !(file->f_flags & O_DIRECT)) {
                                CERROR("%s: unable to disable locking on "
                                       "non-O_DIRECT file\n", current->comm);
                                RETURN(-EINVAL);
                        }

                        fd->fd_flags |= flags;
                } else {
                        fd->fd_flags &= ~flags;
                }
                RETURN(0);
        case LL_IOC_LOV_SETSTRIPE:
                RETURN(ll_lov_setstripe(inode, file, arg));
        case LL_IOC_LOV_SETEA:
                RETURN(ll_lov_setea(inode, file, arg));
	case LL_IOC_LOV_SWAP_LAYOUTS: {
		struct file *file2;
		struct lustre_swap_layouts lsl;

		if (copy_from_user(&lsl, (char *)arg,
				       sizeof(struct lustre_swap_layouts)))
			RETURN(-EFAULT);

		if ((file->f_flags & O_ACCMODE) == 0) /* O_RDONLY */
			RETURN(-EPERM);

		file2 = fget(lsl.sl_fd);
		if (file2 == NULL)
			RETURN(-EBADF);

		rc = -EPERM;
		if ((file2->f_flags & O_ACCMODE) != 0) /* O_WRONLY or O_RDWR */
			rc = ll_swap_layouts(file, file2, &lsl);
		fput(file2);
		RETURN(rc);
	}
        case LL_IOC_LOV_GETSTRIPE:
                RETURN(ll_lov_getstripe(inode, arg));
        case LL_IOC_RECREATE_OBJ:
                RETURN(ll_lov_recreate_obj(inode, arg));
        case LL_IOC_RECREATE_FID:
                RETURN(ll_lov_recreate_fid(inode, arg));
        case FSFILT_IOC_FIEMAP:
                RETURN(ll_ioctl_fiemap(inode, arg));
        case FSFILT_IOC_GETFLAGS:
        case FSFILT_IOC_SETFLAGS:
                RETURN(ll_iocontrol(inode, file, cmd, arg));
        case FSFILT_IOC_GETVERSION_OLD:
        case FSFILT_IOC_GETVERSION:
                RETURN(put_user(inode->i_generation, (int *)arg));
        case LL_IOC_GROUP_LOCK:
                RETURN(ll_get_grouplock(inode, file, arg));
        case LL_IOC_GROUP_UNLOCK:
                RETURN(ll_put_grouplock(inode, file, arg));
        case IOC_OBD_STATFS:
                RETURN(ll_obd_statfs(inode, (void *)arg));

        /* We need to special case any other ioctls we want to handle,
         * to send them to the MDS/OST as appropriate and to properly
         * network encode the arg field.
        case FSFILT_IOC_SETVERSION_OLD:
        case FSFILT_IOC_SETVERSION:
        */
	case LL_IOC_FLUSHCTX:
		RETURN(ll_flush_ctx(inode));
	case LL_IOC_PATH2FID: {
		if (copy_to_user((void *)arg, ll_inode2fid(inode),
				 sizeof(struct lu_fid)))
			RETURN(-EFAULT);

		RETURN(0);
	}
	case OBD_IOC_FID2PATH:
		RETURN(ll_fid2path(inode, (void *)arg));
	case LL_IOC_DATA_VERSION: {
		struct ioc_data_version	idv;
		int rc;

		if (copy_from_user(&idv, (char *)arg, sizeof(idv)))
			RETURN(-EFAULT);

		idv.idv_flags &= LL_DV_RD_FLUSH | LL_DV_WR_FLUSH;
		rc = ll_data_version(inode, &idv.idv_version, idv.idv_flags);

		if (rc == 0 && copy_to_user((char *) arg, &idv, sizeof(idv)))
			RETURN(-EFAULT);

		RETURN(rc);
	}

        case LL_IOC_GET_MDTIDX: {
                int mdtidx;

                mdtidx = ll_get_mdt_idx(inode);
                if (mdtidx < 0)
                        RETURN(mdtidx);

                if (put_user((int)mdtidx, (int*)arg))
                        RETURN(-EFAULT);

                RETURN(0);
        }
	case OBD_IOC_GETDTNAME:
	case OBD_IOC_GETMDNAME:
		RETURN(ll_get_obd_name(inode, cmd, arg));
	case LL_IOC_HSM_STATE_GET: {
		struct md_op_data	*op_data;
		struct hsm_user_state	*hus;
		int			 rc;

		OBD_ALLOC_PTR(hus);
		if (hus == NULL)
			RETURN(-ENOMEM);

		op_data = ll_prep_md_op_data(NULL, inode, NULL, NULL, 0, 0,
					     LUSTRE_OPC_ANY, hus);
		if (IS_ERR(op_data)) {
			OBD_FREE_PTR(hus);
			RETURN(PTR_ERR(op_data));
		}

		rc = obd_iocontrol(cmd, ll_i2mdexp(inode), sizeof(*op_data),
				   op_data, NULL);

		if (copy_to_user((void *)arg, hus, sizeof(*hus)))
			rc = -EFAULT;

		ll_finish_md_op_data(op_data);
		OBD_FREE_PTR(hus);
		RETURN(rc);
	}
	case LL_IOC_HSM_STATE_SET: {
		struct hsm_state_set	*hss;
		int			 rc;

		OBD_ALLOC_PTR(hss);
		if (hss == NULL)
			RETURN(-ENOMEM);

		if (copy_from_user(hss, (char *)arg, sizeof(*hss))) {
			OBD_FREE_PTR(hss);
			RETURN(-EFAULT);
		}

		rc = ll_hsm_state_set(inode, hss);

		OBD_FREE_PTR(hss);
		RETURN(rc);
	}
	case LL_IOC_HSM_ACTION: {
		struct md_op_data		*op_data;
		struct hsm_current_action	*hca;
		int				 rc;

		OBD_ALLOC_PTR(hca);
		if (hca == NULL)
			RETURN(-ENOMEM);

		op_data = ll_prep_md_op_data(NULL, inode, NULL, NULL, 0, 0,
					     LUSTRE_OPC_ANY, hca);
		if (IS_ERR(op_data)) {
			OBD_FREE_PTR(hca);
			RETURN(PTR_ERR(op_data));
		}

		rc = obd_iocontrol(cmd, ll_i2mdexp(inode), sizeof(*op_data),
				   op_data, NULL);

		if (copy_to_user((char *)arg, hca, sizeof(*hca)))
			rc = -EFAULT;

		ll_finish_md_op_data(op_data);
		OBD_FREE_PTR(hca);
		RETURN(rc);
	}
	case LL_IOC_SET_LEASE: {
		struct ll_inode_info *lli = ll_i2info(inode);
		struct obd_client_handle *och = NULL;
		bool lease_broken;
		fmode_t mode = 0;

		switch (arg) {
		case F_WRLCK:
			if (!(file->f_mode & FMODE_WRITE))
				RETURN(-EPERM);
			mode = FMODE_WRITE;
			break;
		case F_RDLCK:
			if (!(file->f_mode & FMODE_READ))
				RETURN(-EPERM);
			mode = FMODE_READ;
			break;
		case F_UNLCK:
			mutex_lock(&lli->lli_och_mutex);
			if (fd->fd_lease_och != NULL) {
				och = fd->fd_lease_och;
				fd->fd_lease_och = NULL;
			}
			mutex_unlock(&lli->lli_och_mutex);

			if (och != NULL) {
				mode = och->och_flags &(FMODE_READ|FMODE_WRITE);
				rc = ll_lease_close(och, inode, &lease_broken);
				if (rc == 0 && lease_broken)
					mode = 0;
			} else {
				rc = -ENOLCK;
			}

			/* return the type of lease or error */
			RETURN(rc < 0 ? rc : (int)mode);
		default:
			RETURN(-EINVAL);
		}

		CDEBUG(D_INODE, "Set lease with mode %d\n", mode);

		/* apply for lease */
		och = ll_lease_open(inode, file, mode, 0);
		if (IS_ERR(och))
			RETURN(PTR_ERR(och));

		rc = 0;
		mutex_lock(&lli->lli_och_mutex);
		if (fd->fd_lease_och == NULL) {
			fd->fd_lease_och = och;
			och = NULL;
		}
		mutex_unlock(&lli->lli_och_mutex);
		if (och != NULL) {
			/* impossible now that only excl is supported for now */
			ll_lease_close(och, inode, &lease_broken);
			rc = -EBUSY;
		}
		RETURN(rc);
	}
	case LL_IOC_GET_LEASE: {
		struct ll_inode_info *lli = ll_i2info(inode);
		struct ldlm_lock *lock = NULL;

		rc = 0;
		mutex_lock(&lli->lli_och_mutex);
		if (fd->fd_lease_och != NULL) {
			struct obd_client_handle *och = fd->fd_lease_och;

			lock = ldlm_handle2lock(&och->och_lease_handle);
			if (lock != NULL) {
				lock_res_and_lock(lock);
				if (!ldlm_is_cancel(lock))
					rc = och->och_flags &
						(FMODE_READ | FMODE_WRITE);
				unlock_res_and_lock(lock);
				LDLM_LOCK_PUT(lock);
			}
		}
		mutex_unlock(&lli->lli_och_mutex);
		RETURN(rc);
	}
	case LL_IOC_HSM_IMPORT: {
		struct hsm_user_import *hui;

		OBD_ALLOC_PTR(hui);
		if (hui == NULL)
			RETURN(-ENOMEM);

		if (copy_from_user(hui, (void *)arg, sizeof(*hui))) {
			OBD_FREE_PTR(hui);
			RETURN(-EFAULT);
		}

		rc = ll_hsm_import(inode, file, hui);

		OBD_FREE_PTR(hui);
		RETURN(rc);
	}
	default: {
		int err;

		if (LLIOC_STOP ==
		     ll_iocontrol_call(inode, file, cmd, arg, &err))
			RETURN(err);

		RETURN(obd_iocontrol(cmd, ll_i2dtexp(inode), 0, NULL,
				     (void *)arg));
	}
	}
}

#ifndef HAVE_FILE_LLSEEK_SIZE
static inline loff_t
llseek_execute(struct file *file, loff_t offset, loff_t maxsize)
{
	if (offset < 0 && !(file->f_mode & FMODE_UNSIGNED_OFFSET))
		return -EINVAL;
	if (offset > maxsize)
		return -EINVAL;

	if (offset != file->f_pos) {
		file->f_pos = offset;
		file->f_version = 0;
	}
	return offset;
}

static loff_t
generic_file_llseek_size(struct file *file, loff_t offset, int origin,
                loff_t maxsize, loff_t eof)
{
	struct inode *inode = file->f_dentry->d_inode;

	switch (origin) {
	case SEEK_END:
		offset += eof;
		break;
	case SEEK_CUR:
		/*
		 * Here we special-case the lseek(fd, 0, SEEK_CUR)
		 * position-querying operation.  Avoid rewriting the "same"
		 * f_pos value back to the file because a concurrent read(),
		 * write() or lseek() might have altered it
		 */
		if (offset == 0)
			return file->f_pos;
		/*
		 * f_lock protects against read/modify/write race with other
		 * SEEK_CURs. Note that parallel writes and reads behave
		 * like SEEK_SET.
		 */
		mutex_lock(&inode->i_mutex);
		offset = llseek_execute(file, file->f_pos + offset, maxsize);
		mutex_unlock(&inode->i_mutex);
		return offset;
	case SEEK_DATA:
		/*
		 * In the generic case the entire file is data, so as long as
		 * offset isn't at the end of the file then the offset is data.
		 */
		if (offset >= eof)
			return -ENXIO;
		break;
	case SEEK_HOLE:
		/*
		 * There is a virtual hole at the end of the file, so as long as
		 * offset isn't i_size or larger, return i_size.
		 */
		if (offset >= eof)
			return -ENXIO;
		offset = eof;
		break;
	}

	return llseek_execute(file, offset, maxsize);
}
#endif

loff_t ll_file_seek(struct file *file, loff_t offset, int origin)
{
	struct inode *inode = file->f_dentry->d_inode;
	loff_t retval, eof = 0;

	ENTRY;
	retval = offset + ((origin == SEEK_END) ? i_size_read(inode) :
			   (origin == SEEK_CUR) ? file->f_pos : 0);
	CDEBUG(D_VFSTRACE, "VFS Op:inode="DFID"(%p), to=%llu=%#llx(%d)\n",
	       PFID(ll_inode2fid(inode)), inode, retval, retval,
	       origin);
	ll_stats_ops_tally(ll_i2sbi(inode), LPROC_LL_LLSEEK, 1);

	if (origin == SEEK_END || origin == SEEK_HOLE || origin == SEEK_DATA) {
		retval = ll_glimpse_size(inode);
		if (retval != 0)
			RETURN(retval);
		eof = i_size_read(inode);
	}

	retval = ll_generic_file_llseek_size(file, offset, origin,
					  ll_file_maxbytes(inode), eof);
	RETURN(retval);
}

int ll_flush(struct file *file, fl_owner_t id)
{
	struct inode *inode = file->f_dentry->d_inode;
	struct ll_inode_info *lli = ll_i2info(inode);
	struct ll_file_data *fd = LUSTRE_FPRIVATE(file);
	int rc, err;

	LASSERT(!S_ISDIR(inode->i_mode));

	/* catch async errors that were recorded back when async writeback
	 * failed for pages in this mapping. */
	rc = lli->lli_async_rc;
	lli->lli_async_rc = 0;
	err = lov_read_and_clear_async_rc(lli->lli_clob);
	if (rc == 0)
		rc = err;

	/* The application has been told write failure already.
	 * Do not report failure again. */
	if (fd->fd_write_failed)
		return 0;
	return rc ? -EIO : 0;
}

/**
 * Called to make sure a portion of file has been written out.
 * if @mode is not CL_FSYNC_LOCAL, it will send OST_SYNC RPCs to OST.
 *
 * Return how many pages have been written.
 */
int cl_sync_file_range(struct inode *inode, loff_t start, loff_t end,
		       enum cl_fsync_mode mode, int ignore_layout)
{
	struct cl_env_nest nest;
	struct lu_env *env;
	struct cl_io *io;
	struct obd_capa *capa = NULL;
	struct cl_fsync_io *fio;
	int result;
	ENTRY;

	if (mode != CL_FSYNC_NONE && mode != CL_FSYNC_LOCAL &&
	    mode != CL_FSYNC_DISCARD && mode != CL_FSYNC_ALL)
		RETURN(-EINVAL);

	env = cl_env_nested_get(&nest);
	if (IS_ERR(env))
		RETURN(PTR_ERR(env));

	capa = ll_osscapa_get(inode, CAPA_OPC_OSS_WRITE);

	io = ccc_env_thread_io(env);
	io->ci_obj = cl_i2info(inode)->lli_clob;
	io->ci_ignore_layout = ignore_layout;

	/* initialize parameters for sync */
	fio = &io->u.ci_fsync;
	fio->fi_capa = capa;
	fio->fi_start = start;
	fio->fi_end = end;
	fio->fi_fid = ll_inode2fid(inode);
	fio->fi_mode = mode;
	fio->fi_nr_written = 0;

	if (cl_io_init(env, io, CIT_FSYNC, io->ci_obj) == 0)
		result = cl_io_loop(env, io);
	else
		result = io->ci_result;
	if (result == 0)
		result = fio->fi_nr_written;
	cl_io_fini(env, io);
	cl_env_nested_put(&nest, env);

	capa_put(capa);

	RETURN(result);
}

/*
 * When dentry is provided (the 'else' case), *file->f_dentry may be
 * null and dentry must be used directly rather than pulled from
 * *file->f_dentry as is done otherwise.
 */

#ifdef HAVE_FILE_FSYNC_4ARGS
int ll_fsync(struct file *file, loff_t start, loff_t end, int datasync)
{
	struct dentry *dentry = file->f_dentry;
#elif defined(HAVE_FILE_FSYNC_2ARGS)
int ll_fsync(struct file *file, int datasync)
{
	struct dentry *dentry = file->f_dentry;
	loff_t start = 0;
	loff_t end = LLONG_MAX;
#else
int ll_fsync(struct file *file, struct dentry *dentry, int datasync)
{
	loff_t start = 0;
	loff_t end = LLONG_MAX;
#endif
	struct inode *inode = dentry->d_inode;
	struct ll_inode_info *lli = ll_i2info(inode);
	struct ptlrpc_request *req;
	struct obd_capa *oc;
	int rc, err;
	ENTRY;

	CDEBUG(D_VFSTRACE, "VFS Op:inode="DFID"(%p)\n",
	       PFID(ll_inode2fid(inode)), inode);
	ll_stats_ops_tally(ll_i2sbi(inode), LPROC_LL_FSYNC, 1);

#ifdef HAVE_FILE_FSYNC_4ARGS
	rc = filemap_write_and_wait_range(inode->i_mapping, start, end);
	mutex_lock(&inode->i_mutex);
#else
	/* fsync's caller has already called _fdata{sync,write}, we want
	 * that IO to finish before calling the osc and mdc sync methods */
	rc = filemap_fdatawait(inode->i_mapping);
#endif

	/* catch async errors that were recorded back when async writeback
	 * failed for pages in this mapping. */
	if (!S_ISDIR(inode->i_mode)) {
		err = lli->lli_async_rc;
		lli->lli_async_rc = 0;
		if (rc == 0)
			rc = err;
		err = lov_read_and_clear_async_rc(lli->lli_clob);
		if (rc == 0)
			rc = err;
	}

	oc = ll_mdscapa_get(inode);
	err = md_fsync(ll_i2sbi(inode)->ll_md_exp, ll_inode2fid(inode), oc,
		       &req);
	capa_put(oc);
	if (!rc)
		rc = err;
	if (!err)
		ptlrpc_req_finished(req);

	if (S_ISREG(inode->i_mode)) {
		struct ll_file_data *fd = LUSTRE_FPRIVATE(file);

		err = cl_sync_file_range(inode, start, end, CL_FSYNC_ALL, 0);
		if (rc == 0 && err < 0)
			rc = err;
		if (rc < 0)
			fd->fd_write_failed = true;
		else
			fd->fd_write_failed = false;
	}

#ifdef HAVE_FILE_FSYNC_4ARGS
	mutex_unlock(&inode->i_mutex);
#endif
	RETURN(rc);
}

static int ll_file_flc2policy(struct file_lock *file_lock, int cmd,
		       ldlm_policy_data_t *flock)
{
	ENTRY;

	if (file_lock->fl_flags & FL_FLOCK) {
		LASSERT((cmd == F_SETLKW) || (cmd == F_SETLK));
		/* flocks are whole-file locks */
		flock->l_flock.end = OFFSET_MAX;
		/* For flocks owner is determined by the local file desctiptor*/
		flock->l_flock.owner = (unsigned long)file_lock->fl_file;
	} else if (file_lock->fl_flags & FL_POSIX) {
		flock->l_flock.owner = (unsigned long)file_lock->fl_owner;
		flock->l_flock.start = file_lock->fl_start;
		flock->l_flock.end = file_lock->fl_end;
	} else {
		RETURN(-EINVAL);
	}
	flock->l_flock.pid = file_lock->fl_pid;

	/* Somewhat ugly workaround for svc lockd.
	 * lockd installs custom fl_lmops->fl_compare_owner that checks
	 * for the fl_owner to be the same (which it always is on local node
	 * I guess between lockd processes) and then compares pid.
	 * As such we assign pid to the owner field to make it all work,
	 * conflict with normal locks is unlikely since pid space and
	 * pointer space for current->files are not intersecting */
	if (file_lock->fl_lmops && file_lock->fl_lmops->fl_compare_owner)
		flock->l_flock.owner = (unsigned long)file_lock->fl_pid;

	RETURN(0);
}

static void ll_file_flock_lock(struct file *file, struct file_lock *file_lock)
{
	int rc = -EINVAL;

	/* We can sleep here only if lock enqueue reply comes earlier than
	 * unlock reply. Fortunatelly unlocks can't block therefore we can't
	 * deadlock here. */
	file_lock->fl_flags |= FL_SLEEP;
	if (file_lock->fl_flags & FL_FLOCK)
		rc = flock_lock_file_wait(file, file_lock);
	if (file_lock->fl_flags & FL_POSIX)
		rc = posix_lock_file_wait(file, file_lock);
	if (rc)
		CDEBUG(D_ERROR, "kernel lock failed rc=%d\n", rc);
}

static int ll_flock_upcall(void *cookie, int err);
int
ll_flock_completion_ast_async(struct ldlm_lock *lock, __u64 flags, void *data);

static int ll_file_flock_async_unlock(struct inode *inode,
				      struct file_lock *file_lock)
{
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	struct ldlm_enqueue_info einfo = { .ei_type = LDLM_FLOCK,
					   .ei_cb_cp =
					      ll_flock_completion_ast_async,
					   .ei_mode = LCK_NL,
					   .ei_cbdata = NULL };
	ldlm_policy_data_t flock = {{0}};
	struct md_op_data *op_data;
	int rc;
	ENTRY;

	rc = ll_file_flc2policy(file_lock, F_SETLK, &flock);
	if (rc)
		RETURN(rc);

	op_data = ll_prep_md_op_data(NULL, inode, NULL, NULL, 0, 0,
				     LUSTRE_OPC_ANY, NULL);
	if (IS_ERR(op_data))
		RETURN(PTR_ERR(op_data));

	rc = md_enqueue_async(sbi->ll_md_exp, &einfo, ll_flock_upcall,
			      op_data, &flock, 0);

	ll_finish_md_op_data(op_data);

	RETURN(rc);
}

/* This function is called only once after ldlm callback. Args are already
 * detached from lock. So, locking isn't needed.
 * It should only report lock status to kernel. */
static void ll_file_flock_async_cb(struct ldlm_flock_info *args, int err)
{
	struct file_lock *file_lock = args->fa_fl;
	struct file_lock *flc = &args->fa_flc;
	struct file *file = args->fa_file;
	struct inode *inode = file->f_dentry->d_inode;
	int (*notify)(struct file_lock *, struct file_lock *, int);
	int rc = 0;
	int rc2;
	ENTRY;

	CDEBUG(D_INFO, "err=%d file_lock=%p file=%p start=%llu end=%llu\n",
	       err, file_lock, file, flc->fl_start, flc->fl_end);

	/* The kernel is responsible for resolving grant vs F_CANCELK or
	 * grant vs. cleanup races, it may happen that CANCELED flag
	 * isn't set and err == 0, because f_CANCELK/cleanup happens between
	 * ldlm_flock_completion_ast_async() and ll_flock_run_flock_cb().
	 * In this case notify() returns error for already canceled flock. */
	if (!(args->fa_flags & FA_FL_CANCELED)) {
		if (err == 0)
			ll_file_flock_lock(file, flc);
		notify = args->fa_notify;
		rc2 = notify(file_lock, NULL, err);
		if (rc2)
			CDEBUG(D_ERROR, "notify failed file_lock=%p err=%d\n",
			       file_lock, err);
		if (rc2 && err == 0) {
			flc->fl_type = F_UNLCK;
			ll_file_flock_lock(file, flc);
			rc = ll_file_flock_async_unlock(inode, flc);
		}
	}

	fput(file);

	EXIT;
}

static void ll_flock_run_flock_cb(struct ldlm_lock *lock, int err)
{
	struct ldlm_flock_info *args;

	lock_res_and_lock(lock);
	args = lock->l_ast_data;
	lock->l_ast_data = NULL;
	unlock_res_and_lock(lock);

	if (args) {
		ll_file_flock_async_cb(args, err);
		OBD_FREE_PTR(args);
	}
}

static int ll_flock_upcall(void *cookie, int err)
{
	struct ldlm_lock *lock = cookie;

	if (err) {
		CERROR("ldlm_cli_enqueue_fini: %d lock=%p\n", err, lock);
		ll_flock_run_flock_cb(lock, err);
	}

	return 0;
}

int
ll_flock_completion_ast_async(struct ldlm_lock *lock, __u64 flags, void *data)
{
	int rc;
	ENTRY;

	if (flags & (LDLM_FL_BLOCK_WAIT | LDLM_FL_BLOCK_GRANTED |
		     LDLM_FL_BLOCK_CONV)) {
		LDLM_DEBUG(lock, "client-side enqueue returned a blocked lock");
		RETURN(0);
	}

	rc = ldlm_flock_completion_ast_async(lock, flags, data);
	ll_flock_run_flock_cb(lock, rc);

	RETURN(0);
}

int ll_file_flock(struct file *file, int cmd, struct file_lock *file_lock)
{
	struct inode *inode = file->f_dentry->d_inode;
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	struct ldlm_enqueue_info einfo = {
		.ei_type	= LDLM_FLOCK,
		.ei_cb_cp	= ldlm_flock_completion_ast,
		.ei_cbdata	= NULL,
	};
	struct md_op_data *op_data;
	struct lustre_handle lockh = {0};
	ldlm_policy_data_t flock = {{0}};
	int fl_type = file_lock->fl_type;
	__u64 flags = 0;
	struct ldlm_flock_info *cb_data = NULL;
	int rc;
        ENTRY;

	CDEBUG(D_VFSTRACE, "VFS Op:inode="DFID" file_lock=%p\n",
	       PFID(ll_inode2fid(inode)), file_lock);

        ll_stats_ops_tally(ll_i2sbi(inode), LPROC_LL_FLOCK, 1);

	rc = ll_file_flc2policy(file_lock, cmd, &flock);
	if (rc)
		RETURN(rc);

	switch (fl_type) {
        case F_RDLCK:
                einfo.ei_mode = LCK_PR;
                break;
        case F_UNLCK:
                /* An unlock request may or may not have any relation to
                 * existing locks so we may not be able to pass a lock handle
                 * via a normal ldlm_lock_cancel() request. The request may even
                 * unlock a byte range in the middle of an existing lock. In
                 * order to process an unlock request we need all of the same
                 * information that is given with a normal read or write record
                 * lock request. To avoid creating another ldlm unlock (cancel)
                 * message we'll treat a LCK_NL flock request as an unlock. */
                einfo.ei_mode = LCK_NL;
                break;
        case F_WRLCK:
                einfo.ei_mode = LCK_PW;
                break;
        default:
		CDEBUG(D_INFO, "Unknown fcntl lock type: %d\n", fl_type);
                RETURN (-ENOTSUPP);
        }

        switch (cmd) {
        case F_SETLKW:
#ifdef F_SETLKW64
        case F_SETLKW64:
#endif
                flags = 0;
                break;
        case F_SETLK:
#ifdef F_SETLK64
        case F_SETLK64:
#endif
                flags = LDLM_FL_BLOCK_NOWAIT;
                break;
        case F_GETLK:
#ifdef F_GETLK64
        case F_GETLK64:
#endif
                flags = LDLM_FL_TEST_LOCK;
                break;
	case F_CANCELLK:
		CDEBUG(D_DLMTRACE, "F_CANCELLK owner=%llx %llu-%llu\n",
		       flock.l_flock.owner, flock.l_flock.start,
		       flock.l_flock.end);
		flags = 0;
		file_lock->fl_type = F_UNLCK;
		einfo.ei_mode = LCK_NL;
		break;
        default:
                CERROR("unknown fcntl lock command: %d\n", cmd);
                RETURN (-EINVAL);
        }

	CDEBUG(D_DLMTRACE, "inode="DFID", pid=%u, owner="LPX64", flags="LPX64","
			   " mode=%u, start="LPU64", end="LPU64"\n",
	       PFID(ll_inode2fid(inode)), flock.l_flock.pid,
	       flock.l_flock.owner, flags, einfo.ei_mode,
	       flock.l_flock.start, flock.l_flock.end);

        op_data = ll_prep_md_op_data(NULL, inode, NULL, NULL, 0, 0,
                                     LUSTRE_OPC_ANY, NULL);
        if (IS_ERR(op_data))
                RETURN(PTR_ERR(op_data));

	OBD_ALLOC_PTR(cb_data);
	if (cb_data == NULL)
		GOTO(out, rc = -ENOMEM);

	cb_data->fa_file = file;
	cb_data->fa_fl = file_lock;
	cb_data->fa_mode = einfo.ei_mode;
	locks_init_lock(&cb_data->fa_flc);
	locks_copy_lock(&cb_data->fa_flc, file_lock);
	if (cmd == F_CANCELLK)
		cb_data->fa_flags |= FA_FL_CANCEL_RQST;
	einfo.ei_cbdata = cb_data;

	if (file_lock->fl_lmops && file_lock->fl_lmops->fl_grant &&
	    file_lock->fl_type != F_UNLCK &&
	    flags == LDLM_FL_BLOCK_NOWAIT /* F_SETLK/F_SETLK64 */) {

		cb_data->fa_notify = file_lock->fl_lmops->fl_grant;
		flags = (file_lock->fl_flags & FL_SLEEP) ?
			0 : LDLM_FL_BLOCK_NOWAIT;
		einfo.ei_cb_cp = ll_flock_completion_ast_async;
		get_file(file);

		rc = md_enqueue_async(sbi->ll_md_exp, &einfo,
				      ll_flock_upcall, op_data, &flock, flags);
		if (rc) {
			fput(file);
			OBD_FREE_PTR(cb_data);
		} else {
			rc = FILE_LOCK_DEFERRED;
		}
	} else {
		rc = md_enqueue(sbi->ll_md_exp, &einfo, NULL, op_data,
				&lockh, &flock, 0, NULL /* req */, flags);
		OBD_FREE_PTR(cb_data);

		if (!(flags & LDLM_FL_TEST_LOCK) && fl_type == F_UNLCK && rc) {
			if (s2lsi(inode->i_sb)->lsi_flags & LSI_UMOUNT_FORCE) {
				/* There is no possibility to recover from
				 * forced umount */
				CDEBUG(D_ERROR, "unlock failed (%d) during "
						"forced umount inode %lu "
						"file_lock %p\n",
				       rc, inode->i_ino, file_lock);
				rc = 0;
			} else if (file_lock->fl_flags & FL_CLOSE) {
				CDEBUG(D_ERROR, "unlock failed (%d) during "
						"close inode %lu "
						"file_lock %p\n",
				       rc, inode->i_ino, file_lock);
				rc = 0;
			}
		}
		if (!(flags & LDLM_FL_TEST_LOCK) && rc == 0)
			ll_file_flock_lock(file, file_lock);
	}
out:
	ll_finish_md_op_data(op_data);

        RETURN(rc);
}

int ll_file_noflock(struct file *file, int cmd, struct file_lock *file_lock)
{
        ENTRY;

        RETURN(-ENOSYS);
}

/**
 * test if some locks matching bits and l_req_mode are acquired
 * - bits can be in different locks
 * - if found clear the common lock bits in *bits
 * - the bits not found, are kept in *bits
 * \param inode [IN]
 * \param bits [IN] searched lock bits [IN]
 * \param l_req_mode [IN] searched lock mode
 * \retval boolean, true iff all bits are found
 */
int ll_have_md_lock(struct inode *inode, __u64 *bits,  ldlm_mode_t l_req_mode)
{
        struct lustre_handle lockh;
        ldlm_policy_data_t policy;
        ldlm_mode_t mode = (l_req_mode == LCK_MINMODE) ?
                                (LCK_CR|LCK_CW|LCK_PR|LCK_PW) : l_req_mode;
        struct lu_fid *fid;
	__u64 flags;
        int i;
        ENTRY;

        if (!inode)
               RETURN(0);

        fid = &ll_i2info(inode)->lli_fid;
        CDEBUG(D_INFO, "trying to match res "DFID" mode %s\n", PFID(fid),
               ldlm_lockname[mode]);

	flags = LDLM_FL_BLOCK_GRANTED | LDLM_FL_CBPENDING | LDLM_FL_TEST_LOCK;
	for (i = 0; i <= MDS_INODELOCK_MAXSHIFT && *bits != 0; i++) {
		policy.l_inodebits.bits = *bits & (1 << i);
		if (policy.l_inodebits.bits == 0)
			continue;

                if (md_lock_match(ll_i2mdexp(inode), flags, fid, LDLM_IBITS,
                                  &policy, mode, &lockh)) {
                        struct ldlm_lock *lock;

                        lock = ldlm_handle2lock(&lockh);
                        if (lock) {
                                *bits &=
                                      ~(lock->l_policy_data.l_inodebits.bits);
                                LDLM_LOCK_PUT(lock);
                        } else {
                                *bits &= ~policy.l_inodebits.bits;
                        }
                }
        }
        RETURN(*bits == 0);
}

ldlm_mode_t ll_take_md_lock(struct inode *inode, __u64 bits,
			    struct lustre_handle *lockh, __u64 flags,
			    ldlm_mode_t mode)
{
        ldlm_policy_data_t policy = { .l_inodebits = {bits}};
        struct lu_fid *fid;
        ldlm_mode_t rc;
        ENTRY;

        fid = &ll_i2info(inode)->lli_fid;
        CDEBUG(D_INFO, "trying to match res "DFID"\n", PFID(fid));

	rc = md_lock_match(ll_i2mdexp(inode), LDLM_FL_BLOCK_GRANTED|flags,
			   fid, LDLM_IBITS, &policy, mode, lockh);

	RETURN(rc);
}

static int ll_inode_revalidate_fini(struct inode *inode, int rc)
{
	/* Already unlinked. Just update nlink and return success */
	if (rc == -ENOENT) {
		clear_nlink(inode);
		/* This path cannot be hit for regular files unless in
		 * case of obscure races, so no need to to validate
		 * size. */
		if (!S_ISREG(inode->i_mode) && !S_ISDIR(inode->i_mode))
			return 0;
	} else if (rc != 0) {
		CDEBUG_LIMIT((rc == -EACCES || rc == -EIDRM) ? D_INFO : D_ERROR,
			     "%s: revalidate FID "DFID" error: rc = %d\n",
			     ll_get_fsname(inode->i_sb, NULL, 0),
			     PFID(ll_inode2fid(inode)), rc);
	}

	return rc;
}

int __ll_inode_revalidate_it(struct dentry *dentry, struct lookup_intent *it,
                             __u64 ibits)
{
        struct inode *inode = dentry->d_inode;
        struct ptlrpc_request *req = NULL;
        struct obd_export *exp;
        int rc = 0;
        ENTRY;

        LASSERT(inode != NULL);

	CDEBUG(D_VFSTRACE, "VFS Op:inode="DFID"(%p),name=%s\n",
	       PFID(ll_inode2fid(inode)), inode, dentry->d_name.name);

        exp = ll_i2mdexp(inode);

        /* XXX: Enable OBD_CONNECT_ATTRFID to reduce unnecessary getattr RPC.
         *      But under CMD case, it caused some lock issues, should be fixed
         *      with new CMD ibits lock. See bug 12718 */
	if (exp_connect_flags(exp) & OBD_CONNECT_ATTRFID) {
                struct lookup_intent oit = { .it_op = IT_GETATTR };
                struct md_op_data *op_data;

                if (ibits == MDS_INODELOCK_LOOKUP)
                        oit.it_op = IT_LOOKUP;

                /* Call getattr by fid, so do not provide name at all. */
                op_data = ll_prep_md_op_data(NULL, dentry->d_inode,
                                             dentry->d_inode, NULL, 0, 0,
                                             LUSTRE_OPC_ANY, NULL);
                if (IS_ERR(op_data))
                        RETURN(PTR_ERR(op_data));

                oit.it_create_mode |= M_CHECK_STALE;
                rc = md_intent_lock(exp, op_data, NULL, 0,
                                    /* we are not interested in name
                                       based lookup */
                                    &oit, 0, &req,
                                    ll_md_blocking_ast, 0);
                ll_finish_md_op_data(op_data);
                oit.it_create_mode &= ~M_CHECK_STALE;
                if (rc < 0) {
                        rc = ll_inode_revalidate_fini(inode, rc);
                        GOTO (out, rc);
                }

                rc = ll_revalidate_it_finish(req, &oit, dentry);
                if (rc != 0) {
                        ll_intent_release(&oit);
                        GOTO(out, rc);
                }

                /* Unlinked? Unhash dentry, so it is not picked up later by
                   do_lookup() -> ll_revalidate_it(). We cannot use d_drop
                   here to preserve get_cwd functionality on 2.6.
                   Bug 10503 */
		if (!dentry->d_inode->i_nlink)
			d_lustre_invalidate(dentry, 0);

                ll_lookup_finish_locks(&oit, dentry);
        } else if (!ll_have_md_lock(dentry->d_inode, &ibits, LCK_MINMODE)) {
                struct ll_sb_info *sbi = ll_i2sbi(dentry->d_inode);
                obd_valid valid = OBD_MD_FLGETATTR;
                struct md_op_data *op_data;
                int ealen = 0;

		if (S_ISREG(inode->i_mode)) {
			rc = ll_get_default_mdsize(sbi, &ealen);
			if (rc)
				RETURN(rc);
			valid |= OBD_MD_FLEASIZE | OBD_MD_FLMODEASIZE;
		}

                op_data = ll_prep_md_op_data(NULL, inode, NULL, NULL,
                                             0, ealen, LUSTRE_OPC_ANY,
                                             NULL);
                if (IS_ERR(op_data))
                        RETURN(PTR_ERR(op_data));

                op_data->op_valid = valid;
                /* Once OBD_CONNECT_ATTRFID is not supported, we can't find one
                 * capa for this inode. Because we only keep capas of dirs
                 * fresh. */
                rc = md_getattr(sbi->ll_md_exp, op_data, &req);
                ll_finish_md_op_data(op_data);
                if (rc) {
                        rc = ll_inode_revalidate_fini(inode, rc);
                        RETURN(rc);
                }

                rc = ll_prep_inode(&inode, req, NULL, NULL);
        }
out:
        ptlrpc_req_finished(req);
        return rc;
}

int ll_inode_revalidate_it(struct dentry *dentry, struct lookup_intent *it,
			   __u64 ibits)
{
	struct inode	*inode = dentry->d_inode;
	int		 rc;
	ENTRY;

	rc = __ll_inode_revalidate_it(dentry, it, ibits);
	if (rc != 0)
		RETURN(rc);

	/* if object isn't regular file, don't validate size */
	if (!S_ISREG(inode->i_mode)) {
		LTIME_S(inode->i_atime) = ll_i2info(inode)->lli_lvb.lvb_atime;
		LTIME_S(inode->i_mtime) = ll_i2info(inode)->lli_lvb.lvb_mtime;
		LTIME_S(inode->i_ctime) = ll_i2info(inode)->lli_lvb.lvb_ctime;
	} else {
		/* In case of restore, the MDT has the right size and has
		 * already send it back without granting the layout lock,
		 * inode is up-to-date so glimpse is useless.
		 * Also to glimpse we need the layout, in case of a running
		 * restore the MDT holds the layout lock so the glimpse will
		 * block up to the end of restore (getattr will block)
		 */
		if (!(ll_i2info(inode)->lli_flags & LLIF_FILE_RESTORING))
			rc = ll_glimpse_size(inode);
	}
	RETURN(rc);
}

int ll_getattr_it(struct vfsmount *mnt, struct dentry *de,
                  struct lookup_intent *it, struct kstat *stat)
{
        struct inode *inode = de->d_inode;
        struct ll_sb_info *sbi = ll_i2sbi(inode);
        struct ll_inode_info *lli = ll_i2info(inode);
        int res = 0;

        res = ll_inode_revalidate_it(de, it, MDS_INODELOCK_UPDATE |
                                             MDS_INODELOCK_LOOKUP);
        ll_stats_ops_tally(sbi, LPROC_LL_GETATTR, 1);

        if (res)
                return res;

        stat->dev = inode->i_sb->s_dev;
        if (ll_need_32bit_api(sbi))
                stat->ino = cl_fid_build_ino(&lli->lli_fid, 1);
        else
                stat->ino = inode->i_ino;
        stat->mode = inode->i_mode;
        stat->nlink = inode->i_nlink;
        stat->uid = inode->i_uid;
        stat->gid = inode->i_gid;
	stat->rdev = inode->i_rdev;
        stat->atime = inode->i_atime;
        stat->mtime = inode->i_mtime;
        stat->ctime = inode->i_ctime;
	stat->blksize = 1 << inode->i_blkbits;

        stat->size = i_size_read(inode);
        stat->blocks = inode->i_blocks;

        return 0;
}
int ll_getattr(struct vfsmount *mnt, struct dentry *de, struct kstat *stat)
{
        struct lookup_intent it = { .it_op = IT_GETATTR };

        return ll_getattr_it(mnt, de, &it, stat);
}

int ll_fiemap(struct inode *inode, struct fiemap_extent_info *fieinfo,
                __u64 start, __u64 len)
{
        int rc;
        size_t num_bytes;
        struct ll_user_fiemap *fiemap;
        unsigned int extent_count = fieinfo->fi_extents_max;

        num_bytes = sizeof(*fiemap) + (extent_count *
                                       sizeof(struct ll_fiemap_extent));
        OBD_ALLOC_LARGE(fiemap, num_bytes);

        if (fiemap == NULL)
                RETURN(-ENOMEM);

        fiemap->fm_flags = fieinfo->fi_flags;
        fiemap->fm_extent_count = fieinfo->fi_extents_max;
        fiemap->fm_start = start;
        fiemap->fm_length = len;
	if (extent_count > 0)
		memcpy(&fiemap->fm_extents[0], fieinfo->fi_extents_start,
		       sizeof(struct ll_fiemap_extent));

	rc = ll_do_fiemap(inode, fiemap, num_bytes);

	fieinfo->fi_flags = fiemap->fm_flags;
	fieinfo->fi_extents_mapped = fiemap->fm_mapped_extents;
	if (extent_count > 0)
		memcpy(fieinfo->fi_extents_start, &fiemap->fm_extents[0],
		       fiemap->fm_mapped_extents *
		       sizeof(struct ll_fiemap_extent));

	OBD_FREE_LARGE(fiemap, num_bytes);
	return rc;
}

struct posix_acl * ll_get_acl(struct inode *inode, int type)
{
	struct ll_inode_info *lli = ll_i2info(inode);
	struct posix_acl *acl = NULL;
	ENTRY;

	spin_lock(&lli->lli_lock);
	/* VFS' acl_permission_check->check_acl will release the refcount */
	acl = posix_acl_dup(lli->lli_posix_acl);
	spin_unlock(&lli->lli_lock);

	RETURN(acl);
}

#ifndef HAVE_GENERIC_PERMISSION_2ARGS
static int
# ifdef HAVE_GENERIC_PERMISSION_4ARGS
ll_check_acl(struct inode *inode, int mask, unsigned int flags)
# else
ll_check_acl(struct inode *inode, int mask)
# endif
{
# ifdef CONFIG_FS_POSIX_ACL
	struct posix_acl *acl;
	int rc;
	ENTRY;

#  ifdef HAVE_GENERIC_PERMISSION_4ARGS
	if (flags & IPERM_FLAG_RCU)
		return -ECHILD;
#  endif
	acl = ll_get_acl(inode, ACL_TYPE_ACCESS);

	if (!acl)
		RETURN(-EAGAIN);

	rc = posix_acl_permission(inode, acl, mask);
	posix_acl_release(acl);

	RETURN(rc);
# else /* !CONFIG_FS_POSIX_ACL */
	return -EAGAIN;
# endif /* CONFIG_FS_POSIX_ACL */
}
#endif /* HAVE_GENERIC_PERMISSION_2ARGS */

#ifdef HAVE_GENERIC_PERMISSION_4ARGS
int ll_inode_permission(struct inode *inode, int mask, unsigned int flags)
#else
# ifdef HAVE_INODE_PERMISION_2ARGS
int ll_inode_permission(struct inode *inode, int mask)
# else
int ll_inode_permission(struct inode *inode, int mask, struct nameidata *nd)
# endif
#endif
{
        int rc = 0;
        ENTRY;

#ifdef MAY_NOT_BLOCK
	if (mask & MAY_NOT_BLOCK)
		return -ECHILD;
#elif defined(HAVE_GENERIC_PERMISSION_4ARGS)
	if (flags & IPERM_FLAG_RCU)
		return -ECHILD;
#endif

       /* as root inode are NOT getting validated in lookup operation,
        * need to do it before permission check. */

        if (inode == inode->i_sb->s_root->d_inode) {
                struct lookup_intent it = { .it_op = IT_LOOKUP };

                rc = __ll_inode_revalidate_it(inode->i_sb->s_root, &it,
                                              MDS_INODELOCK_LOOKUP);
                if (rc)
                        RETURN(rc);
        }

	CDEBUG(D_VFSTRACE, "VFS Op:inode="DFID"(%p), inode mode %x mask %o\n",
	       PFID(ll_inode2fid(inode)), inode, inode->i_mode, mask);

	if (ll_i2sbi(inode)->ll_flags & LL_SBI_RMT_CLIENT)
		return lustre_check_remote_perm(inode, mask);

	ll_stats_ops_tally(ll_i2sbi(inode), LPROC_LL_INODE_PERM, 1);
	rc = ll_generic_permission(inode, mask, flags, ll_check_acl);

	RETURN(rc);
}

/* -o localflock - only provides locally consistent flock locks */
struct file_operations ll_file_operations = {
        .read           = ll_file_read,
	.aio_read    = ll_file_aio_read,
        .write          = ll_file_write,
	.aio_write   = ll_file_aio_write,
        .unlocked_ioctl = ll_file_ioctl,
        .open           = ll_file_open,
        .release        = ll_file_release,
        .mmap           = ll_file_mmap,
        .llseek         = ll_file_seek,
        .splice_read    = ll_file_splice_read,
        .fsync          = ll_fsync,
        .flush          = ll_flush
};

struct file_operations ll_file_operations_flock = {
        .read           = ll_file_read,
	.aio_read    = ll_file_aio_read,
        .write          = ll_file_write,
	.aio_write   = ll_file_aio_write,
        .unlocked_ioctl = ll_file_ioctl,
        .open           = ll_file_open,
        .release        = ll_file_release,
        .mmap           = ll_file_mmap,
        .llseek         = ll_file_seek,
        .splice_read    = ll_file_splice_read,
        .fsync          = ll_fsync,
        .flush          = ll_flush,
        .flock          = ll_file_flock,
        .lock           = ll_file_flock
};

/* These are for -o noflock - to return ENOSYS on flock calls */
struct file_operations ll_file_operations_noflock = {
        .read           = ll_file_read,
	.aio_read    = ll_file_aio_read,
        .write          = ll_file_write,
	.aio_write   = ll_file_aio_write,
        .unlocked_ioctl = ll_file_ioctl,
        .open           = ll_file_open,
        .release        = ll_file_release,
        .mmap           = ll_file_mmap,
        .llseek         = ll_file_seek,
        .splice_read    = ll_file_splice_read,
        .fsync          = ll_fsync,
        .flush          = ll_flush,
        .flock          = ll_file_noflock,
        .lock           = ll_file_noflock
};

struct inode_operations ll_file_inode_operations = {
	.setattr	= ll_setattr,
	.getattr	= ll_getattr,
	.permission	= ll_inode_permission,
	.setxattr	= ll_setxattr,
	.getxattr	= ll_getxattr,
	.listxattr	= ll_listxattr,
	.removexattr	= ll_removexattr,
	.fiemap		= ll_fiemap,
#ifdef HAVE_IOP_GET_ACL
	.get_acl	= ll_get_acl,
#endif
};

/* dynamic ioctl number support routins */
static struct llioc_ctl_data {
	struct rw_semaphore	ioc_sem;
        cfs_list_t              ioc_head;
} llioc = {
        __RWSEM_INITIALIZER(llioc.ioc_sem),
        CFS_LIST_HEAD_INIT(llioc.ioc_head)
};


struct llioc_data {
        cfs_list_t              iocd_list;
        unsigned int            iocd_size;
        llioc_callback_t        iocd_cb;
        unsigned int            iocd_count;
        unsigned int            iocd_cmd[0];
};

void *ll_iocontrol_register(llioc_callback_t cb, int count, unsigned int *cmd)
{
        unsigned int size;
        struct llioc_data *in_data = NULL;
        ENTRY;

        if (cb == NULL || cmd == NULL ||
            count > LLIOC_MAX_CMD || count < 0)
                RETURN(NULL);

        size = sizeof(*in_data) + count * sizeof(unsigned int);
        OBD_ALLOC(in_data, size);
        if (in_data == NULL)
                RETURN(NULL);

        memset(in_data, 0, sizeof(*in_data));
        in_data->iocd_size = size;
        in_data->iocd_cb = cb;
        in_data->iocd_count = count;
        memcpy(in_data->iocd_cmd, cmd, sizeof(unsigned int) * count);

	down_write(&llioc.ioc_sem);
        cfs_list_add_tail(&in_data->iocd_list, &llioc.ioc_head);
	up_write(&llioc.ioc_sem);

        RETURN(in_data);
}

void ll_iocontrol_unregister(void *magic)
{
        struct llioc_data *tmp;

        if (magic == NULL)
                return;

	down_write(&llioc.ioc_sem);
        cfs_list_for_each_entry(tmp, &llioc.ioc_head, iocd_list) {
                if (tmp == magic) {
                        unsigned int size = tmp->iocd_size;

                        cfs_list_del(&tmp->iocd_list);
			up_write(&llioc.ioc_sem);

                        OBD_FREE(tmp, size);
                        return;
                }
        }
	up_write(&llioc.ioc_sem);

        CWARN("didn't find iocontrol register block with magic: %p\n", magic);
}

EXPORT_SYMBOL(ll_iocontrol_register);
EXPORT_SYMBOL(ll_iocontrol_unregister);

enum llioc_iter ll_iocontrol_call(struct inode *inode, struct file *file,
                        unsigned int cmd, unsigned long arg, int *rcp)
{
        enum llioc_iter ret = LLIOC_CONT;
        struct llioc_data *data;
        int rc = -EINVAL, i;

	down_read(&llioc.ioc_sem);
        cfs_list_for_each_entry(data, &llioc.ioc_head, iocd_list) {
                for (i = 0; i < data->iocd_count; i++) {
                        if (cmd != data->iocd_cmd[i])
                                continue;

                        ret = data->iocd_cb(inode, file, cmd, arg, data, &rc);
                        break;
                }

                if (ret == LLIOC_STOP)
                        break;
        }
	up_read(&llioc.ioc_sem);

        if (rcp)
                *rcp = rc;
        return ret;
}

int ll_layout_conf(struct inode *inode, const struct cl_object_conf *conf)
{
	struct ll_inode_info *lli = ll_i2info(inode);
	struct cl_env_nest nest;
	struct lu_env *env;
	int result;
	ENTRY;

	if (lli->lli_clob == NULL)
		RETURN(0);

	env = cl_env_nested_get(&nest);
	if (IS_ERR(env))
		RETURN(PTR_ERR(env));

	result = cl_conf_set(env, lli->lli_clob, conf);
	cl_env_nested_put(&nest, env);

	if (conf->coc_opc == OBJECT_CONF_SET) {
		struct ldlm_lock *lock = conf->coc_lock;

		LASSERT(lock != NULL);
		LASSERT(ldlm_has_layout(lock));
		if (result == 0) {
			/* it can only be allowed to match after layout is
			 * applied to inode otherwise false layout would be
			 * seen. Applying layout shoud happen before dropping
			 * the intent lock. */
			ldlm_lock_allow_match(lock);
		}
	}
	RETURN(result);
}

/* Fetch layout from MDT with getxattr request, if it's not ready yet */
static int ll_layout_fetch(struct inode *inode, struct ldlm_lock *lock)

{
	struct ll_sb_info *sbi = ll_i2sbi(inode);
	struct obd_capa *oc;
	struct ptlrpc_request *req;
	struct mdt_body *body;
	void *lvbdata;
	void *lmm;
	int lmmsize;
	int rc;
	ENTRY;

	CDEBUG(D_INODE, DFID" LVB_READY=%d l_lvb_data=%p l_lvb_len=%d\n",
	       PFID(ll_inode2fid(inode)), !!(lock->l_flags & LDLM_FL_LVB_READY),
	       lock->l_lvb_data, lock->l_lvb_len);

	if ((lock->l_lvb_data != NULL) && (lock->l_flags & LDLM_FL_LVB_READY))
		RETURN(0);

	/* if layout lock was granted right away, the layout is returned
	 * within DLM_LVB of dlm reply; otherwise if the lock was ever
	 * blocked and then granted via completion ast, we have to fetch
	 * layout here. Please note that we can't use the LVB buffer in
	 * completion AST because it doesn't have a large enough buffer */
	oc = ll_mdscapa_get(inode);
	rc = ll_get_default_mdsize(sbi, &lmmsize);
	if (rc == 0)
		rc = md_getxattr(sbi->ll_md_exp, ll_inode2fid(inode), oc,
				OBD_MD_FLXATTR, XATTR_NAME_LOV, NULL, 0,
				lmmsize, 0, &req);
	capa_put(oc);
	if (rc < 0)
		RETURN(rc);

	body = req_capsule_server_get(&req->rq_pill, &RMF_MDT_BODY);
	if (body == NULL)
		GOTO(out, rc = -EPROTO);

	lmmsize = body->eadatasize;
	if (lmmsize == 0) /* empty layout */
		GOTO(out, rc = 0);

	lmm = req_capsule_server_sized_get(&req->rq_pill, &RMF_EADATA, lmmsize);
	if (lmm == NULL)
		GOTO(out, rc = -EFAULT);

	OBD_ALLOC_LARGE(lvbdata, lmmsize);
	if (lvbdata == NULL)
		GOTO(out, rc = -ENOMEM);

	memcpy(lvbdata, lmm, lmmsize);
	lock_res_and_lock(lock);
	if (lock->l_lvb_data != NULL)
		OBD_FREE_LARGE(lock->l_lvb_data, lock->l_lvb_len);

	lock->l_lvb_data = lvbdata;
	lock->l_lvb_len = lmmsize;
	unlock_res_and_lock(lock);

	EXIT;

out:
	ptlrpc_req_finished(req);
	return rc;
}

/**
 * Apply the layout to the inode. Layout lock is held and will be released
 * in this function.
 */
static int ll_layout_lock_set(struct lustre_handle *lockh, ldlm_mode_t mode,
				struct inode *inode, __u32 *gen, bool reconf)
{
	struct ll_inode_info *lli = ll_i2info(inode);
	struct ll_sb_info    *sbi = ll_i2sbi(inode);
	struct ldlm_lock *lock;
	struct lustre_md md = { NULL };
	struct cl_object_conf conf;
	int rc = 0;
	bool lvb_ready;
	bool wait_layout = false;
	ENTRY;

	LASSERT(lustre_handle_is_used(lockh));

	lock = ldlm_handle2lock(lockh);
	LASSERT(lock != NULL);
	LASSERT(ldlm_has_layout(lock));

	LDLM_DEBUG(lock, "file "DFID"(%p) being reconfigured: %d\n",
		   PFID(&lli->lli_fid), inode, reconf);

	/* in case this is a caching lock and reinstate with new inode */
	md_set_lock_data(sbi->ll_md_exp, &lockh->cookie, inode, NULL);

	lock_res_and_lock(lock);
	lvb_ready = !!(lock->l_flags & LDLM_FL_LVB_READY);
	unlock_res_and_lock(lock);
	/* checking lvb_ready is racy but this is okay. The worst case is
	 * that multi processes may configure the file on the same time. */

	if (lvb_ready || !reconf) {
		rc = -ENODATA;
		if (lvb_ready) {
			/* layout_gen must be valid if layout lock is not
			 * cancelled and stripe has already set */
			*gen = lli->lli_layout_gen;
			rc = 0;
		}
		GOTO(out, rc);
	}

	rc = ll_layout_fetch(inode, lock);
	if (rc < 0)
		GOTO(out, rc);

	/* for layout lock, lmm is returned in lock's lvb.
	 * lvb_data is immutable if the lock is held so it's safe to access it
	 * without res lock. See the description in ldlm_lock_decref_internal()
	 * for the condition to free lvb_data of layout lock */
	if (lock->l_lvb_data != NULL) {
		rc = obd_unpackmd(sbi->ll_dt_exp, &md.lsm,
				  lock->l_lvb_data, lock->l_lvb_len);
		if (rc >= 0) {
			*gen = LL_LAYOUT_GEN_EMPTY;
			if (md.lsm != NULL)
				*gen = md.lsm->lsm_layout_gen;
			rc = 0;
		} else {
			CERROR("%s: file "DFID" unpackmd error: %d\n",
				ll_get_fsname(inode->i_sb, NULL, 0),
				PFID(&lli->lli_fid), rc);
		}
	}
	if (rc < 0)
		GOTO(out, rc);

	/* set layout to file. Unlikely this will fail as old layout was
	 * surely eliminated */
	memset(&conf, 0, sizeof conf);
	conf.coc_opc = OBJECT_CONF_SET;
	conf.coc_inode = inode;
	conf.coc_lock = lock;
	conf.u.coc_md = &md;
	rc = ll_layout_conf(inode, &conf);

	if (md.lsm != NULL)
		obd_free_memmd(sbi->ll_dt_exp, &md.lsm);

	/* refresh layout failed, need to wait */
	wait_layout = rc == -EBUSY;
	EXIT;

out:
	LDLM_LOCK_PUT(lock);
	ldlm_lock_decref(lockh, mode);

	/* wait for IO to complete if it's still being used. */
	if (wait_layout) {
		CDEBUG(D_INODE, "%s: "DFID"(%p) wait for layout reconf\n",
		       ll_get_fsname(inode->i_sb, NULL, 0),
		       PFID(&lli->lli_fid), inode);

		memset(&conf, 0, sizeof conf);
		conf.coc_opc = OBJECT_CONF_WAIT;
		conf.coc_inode = inode;
		rc = ll_layout_conf(inode, &conf);
		if (rc == 0)
			rc = -EAGAIN;

		CDEBUG(D_INODE, "%s file="DFID" waiting layout return: %d\n",
		       ll_get_fsname(inode->i_sb, NULL, 0),
		       PFID(&lli->lli_fid), rc);
	}
	RETURN(rc);
}

/**
 * This function checks if there exists a LAYOUT lock on the client side,
 * or enqueues it if it doesn't have one in cache.
 *
 * This function will not hold layout lock so it may be revoked any time after
 * this function returns. Any operations depend on layout should be redone
 * in that case.
 *
 * This function should be called before lov_io_init() to get an uptodate
 * layout version, the caller should save the version number and after IO
 * is finished, this function should be called again to verify that layout
 * is not changed during IO time.
 */
int ll_layout_refresh(struct inode *inode, __u32 *gen)
{
	struct ll_inode_info  *lli = ll_i2info(inode);
	struct ll_sb_info     *sbi = ll_i2sbi(inode);
	struct md_op_data     *op_data;
	struct lookup_intent   it;
	struct lustre_handle   lockh;
	ldlm_mode_t	       mode;
	struct ldlm_enqueue_info einfo = {
		.ei_type = LDLM_IBITS,
		.ei_mode = LCK_CR,
		.ei_cb_bl = ll_md_blocking_ast,
		.ei_cb_cp = ldlm_completion_ast,
	};
	int rc;
	ENTRY;

	*gen = lli->lli_layout_gen;
	if (!(sbi->ll_flags & LL_SBI_LAYOUT_LOCK))
		RETURN(0);

	/* sanity checks */
	LASSERT(fid_is_sane(ll_inode2fid(inode)));
	LASSERT(S_ISREG(inode->i_mode));

	/* mostly layout lock is caching on the local side, so try to match
	 * it before grabbing layout lock mutex. */
	mode = ll_take_md_lock(inode, MDS_INODELOCK_LAYOUT, &lockh, 0,
			       LCK_CR | LCK_CW | LCK_PR | LCK_PW);
	if (mode != 0) { /* hit cached lock */
		rc = ll_layout_lock_set(&lockh, mode, inode, gen, false);
		if (rc == 0)
			RETURN(0);

		/* better hold lli_layout_mutex to try again otherwise
		 * it will have starvation problem. */
	}

	/* take layout lock mutex to enqueue layout lock exclusively. */
	mutex_lock(&lli->lli_layout_mutex);

again:
	/* try again. Maybe somebody else has done this. */
	mode = ll_take_md_lock(inode, MDS_INODELOCK_LAYOUT, &lockh, 0,
			       LCK_CR | LCK_CW | LCK_PR | LCK_PW);
	if (mode != 0) { /* hit cached lock */
		rc = ll_layout_lock_set(&lockh, mode, inode, gen, true);
		if (rc == -EAGAIN)
			goto again;

		mutex_unlock(&lli->lli_layout_mutex);
		RETURN(rc);
	}

	op_data = ll_prep_md_op_data(NULL, inode, inode, NULL,
				     0, 0, LUSTRE_OPC_ANY, NULL);
	if (IS_ERR(op_data)) {
		mutex_unlock(&lli->lli_layout_mutex);
		RETURN(PTR_ERR(op_data));
	}

	/* have to enqueue one */
	memset(&it, 0, sizeof(it));
	it.it_op = IT_LAYOUT;
	lockh.cookie = 0ULL;

	LDLM_DEBUG_NOLOCK("%s: requeue layout lock for file "DFID"(%p)\n",
			  ll_get_fsname(inode->i_sb, NULL, 0),
			  PFID(&lli->lli_fid), inode);

	rc = md_enqueue(sbi->ll_md_exp, &einfo, &it, op_data, &lockh,
			NULL, 0, NULL, 0);
	if (it.d.lustre.it_data != NULL)
		ptlrpc_req_finished(it.d.lustre.it_data);
	it.d.lustre.it_data = NULL;

	ll_finish_md_op_data(op_data);

	mode = it.d.lustre.it_lock_mode;
	it.d.lustre.it_lock_mode = 0;
	ll_intent_drop_lock(&it);

	if (rc == 0) {
		/* set lock data in case this is a new lock */
		ll_set_lock_data(sbi->ll_md_exp, inode, &it, NULL);
		rc = ll_layout_lock_set(&lockh, mode, inode, gen, true);
		if (rc == -EAGAIN)
			goto again;
	}
	mutex_unlock(&lli->lli_layout_mutex);

	RETURN(rc);
}

/**
 *  This function send a restore request to the MDT
 */
int ll_layout_restore(struct inode *inode, loff_t offset, __u64 length)
{
	struct hsm_user_request	*hur;
	int			 len, rc;
	ENTRY;

	len = sizeof(struct hsm_user_request) +
	      sizeof(struct hsm_user_item);
	OBD_ALLOC(hur, len);
	if (hur == NULL)
		RETURN(-ENOMEM);

	hur->hur_request.hr_action = HUA_RESTORE;
	hur->hur_request.hr_archive_id = 0;
	hur->hur_request.hr_flags = 0;
	memcpy(&hur->hur_user_item[0].hui_fid, &ll_i2info(inode)->lli_fid,
	       sizeof(hur->hur_user_item[0].hui_fid));
	hur->hur_user_item[0].hui_extent.offset = offset;
	hur->hur_user_item[0].hui_extent.length = length;
	hur->hur_request.hr_itemcount = 1;
	rc = obd_iocontrol(LL_IOC_HSM_REQUEST, cl_i2sbi(inode)->ll_md_exp,
			   len, hur, NULL);
	OBD_FREE(hur, len);
	RETURN(rc);
}

