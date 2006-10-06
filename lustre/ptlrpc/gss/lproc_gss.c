/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 *  Copyright (C) 2001-2003 Cluster File Systems, Inc.
 *   Author Peter Braam <braam@clusterfs.com>
 *
 *   This file is part of the Lustre file system, http://www.lustre.org
 *   Lustre is a trademark of Cluster File Systems, Inc.
 *
 *   You may have signed or agreed to another license before downloading
 *   this software.  If so, you are bound by the terms and conditions
 *   of that agreement, and the following does not apply to you.  See the
 *   LICENSE file included with this distribution for more information.
 *
 *   If you did not agree to a different license, then this copy of Lustre
 *   is open source software; you can redistribute it and/or modify it
 *   under the terms of version 2 of the GNU General Public License as
 *   published by the Free Software Foundation.
 *
 *   In either case, Lustre is distributed in the hope that it will be
 *   useful, but WITHOUT ANY WARRANTY; without even the implied warranty
 *   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   license text for more details.
 *
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif
#define DEBUG_SUBSYSTEM S_SEC
#ifdef __KERNEL__
#include <linux/init.h>
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/dcache.h>
#include <linux/fs.h>
#include <linux/random.h>
#else
#include <liblustre.h>
#endif

#include <obd.h>
#include <obd_class.h>
#include <obd_support.h>
#include <lustre/lustre_idl.h>
#include <lustre_net.h>
#include <lustre_import.h>
#include <lprocfs_status.h>
#include <lustre_sec.h>

#include "gss_err.h"
#include "gss_internal.h"
#include "gss_api.h"

static struct proc_dir_entry *gss_proc_root = NULL;

/*
 * statistic of "out-of-sequence-window"
 */
static struct {
        spinlock_t      oos_lock;
        atomic_t        oos_cli_count;       /* client occurrence */
        int             oos_cli_behind;      /* client max seqs behind */
        atomic_t        oos_svc_replay[3];   /* server replay detected */
        atomic_t        oos_svc_pass[3];     /* server verified ok */
} gss_stat_oos = {
        .oos_lock       = SPIN_LOCK_UNLOCKED,
        .oos_cli_count  = ATOMIC_INIT(0),
        .oos_cli_behind = 0,
        .oos_svc_replay = { ATOMIC_INIT(0), },
        .oos_svc_pass   = { ATOMIC_INIT(0), },
};

void gss_stat_oos_record_cli(int behind)
{
        atomic_inc(&gss_stat_oos.oos_cli_count);

        spin_lock(&gss_stat_oos.oos_lock);
        if (behind > gss_stat_oos.oos_cli_behind)
                gss_stat_oos.oos_cli_behind = behind;
        spin_unlock(&gss_stat_oos.oos_lock);
}

void gss_stat_oos_record_svc(int phase, int replay)
{
        LASSERT(phase >= 0 && phase <= 2);

        if (replay)
                atomic_inc(&gss_stat_oos.oos_svc_replay[phase]);
        else
                atomic_inc(&gss_stat_oos.oos_svc_pass[phase]);
}

static int gss_proc_read_oos(char *page, char **start, off_t off, int count,
                             int *eof, void *data)
{
        int written;

        written = snprintf(page, count,
                        "seqwin:                %u\n"
                        "backwin:               %u\n"
                        "client fall behind seqwin\n"
                        "  occurrence:          %d\n"
                        "  max seq behind:      %d\n"
                        "server replay detected:\n"
                        "  phase 0:             %d\n"
                        "  phase 1:             %d\n"
                        "  phase 2:             %d\n"
                        "server verify ok:\n"
                        "  phase 2:             %d\n",
                        GSS_SEQ_WIN_MAIN,
                        GSS_SEQ_WIN_BACK,
                        atomic_read(&gss_stat_oos.oos_cli_count),
                        gss_stat_oos.oos_cli_behind,
                        atomic_read(&gss_stat_oos.oos_svc_replay[0]),
                        atomic_read(&gss_stat_oos.oos_svc_replay[1]),
                        atomic_read(&gss_stat_oos.oos_svc_replay[2]),
                        atomic_read(&gss_stat_oos.oos_svc_pass[2]));

        return written;
}

static int gss_proc_write_secinit(struct file *file, const char *buffer,
                                  unsigned long count, void *data)
{
        int rc;

        rc = gss_do_ctx_init_rpc((char *) buffer, count);
        if (rc) {
                LASSERT(rc < 0);
                return rc;
        }

        return ((int) count);
}

static struct lprocfs_vars gss_lprocfs_vars[] = {
        { "replays", gss_proc_read_oos, NULL },
        { "init_channel", NULL, gss_proc_write_secinit, NULL },
        { NULL }
};

int gss_init_lproc(void)
{
        int rc;
        gss_proc_root = lprocfs_register("gss", sptlrpc_proc_root,
                                         gss_lprocfs_vars, NULL);

        if (IS_ERR(gss_proc_root)) {
                rc = PTR_ERR(gss_proc_root);
                gss_proc_root = NULL;
                CERROR("failed to initialize lproc entries: %d\n", rc);
                return rc;
        }

        return 0;
}

void gss_exit_lproc(void)
{
        if (gss_proc_root) {
                lprocfs_remove(gss_proc_root);
                gss_proc_root = NULL;
        }
}
