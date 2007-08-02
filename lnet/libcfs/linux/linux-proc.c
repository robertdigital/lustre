/* -*- mode: c; c-basic-offset: 8; indent-tabs-mode: nil; -*-
 * vim:expandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright (C) 2001, 2002 Cluster File Systems, Inc.
 *   Author: Zach Brown <zab@zabbo.net>
 *   Author: Peter J. Braam <braam@clusterfs.com>
 *   Author: Phil Schwan <phil@clusterfs.com>
 *
 *   This file is part of Lustre, http://www.lustre.org.
 *
 *   Lustre is free software; you can redistribute it and/or
 *   modify it under the terms of version 2 of the GNU General Public
 *   License as published by the Free Software Foundation.
 *
 *   Lustre is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with Lustre; if not, write to the Free Software
 *   Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#ifndef EXPORT_SYMTAB
# define EXPORT_SYMTAB
#endif

#ifndef AUTOCONF_INCLUDED
#include <linux/config.h>
#endif
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/string.h>
#include <linux/stat.h>
#include <linux/errno.h>
#include <linux/smp_lock.h>
#include <linux/unistd.h>
#include <net/sock.h>
#include <linux/uio.h>

#include <asm/system.h>
#include <asm/uaccess.h>

#include <linux/fs.h>
#include <linux/file.h>
#include <linux/stat.h>
#include <linux/list.h>
#include <asm/uaccess.h>
#include <asm/segment.h>

#include <linux/proc_fs.h>
#include <linux/sysctl.h>

# define DEBUG_SUBSYSTEM S_LNET

#include <libcfs/kp30.h>
#include <asm/div64.h>
#include "tracefile.h"

static struct ctl_table_header *lnet_table_header = NULL;
extern char lnet_upcall[1024];

#define PSDEV_LNET  (0x100)
enum {
        PSDEV_DEBUG = 1,          /* control debugging */
        PSDEV_SUBSYSTEM_DEBUG,    /* control debugging */
        PSDEV_PRINTK,             /* force all messages to console */
        PSDEV_CONSOLE_RATELIMIT,  /* ratelimit console messages */
        PSDEV_DEBUG_PATH,         /* crashdump log location */
        PSDEV_DEBUG_DUMP_PATH,    /* crashdump tracelog location */
        PSDEV_LNET_UPCALL,        /* User mode upcall script  */
        PSDEV_LNET_MEMUSED,       /* bytes currently PORTAL_ALLOCated */
        PSDEV_LNET_CATASTROPHE,   /* if we have LBUGged or panic'd */
        PSDEV_LNET_PANIC_ON_LBUG, /* flag to panic on LBUG */
};

int LL_PROC_PROTO(proc_dobitmasks);

static struct ctl_table lnet_table[] = {
        {
                .ctl_name = PSDEV_DEBUG,
                .procname = "debug",
                .data     = &libcfs_debug,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dobitmasks
        },
        {
                .ctl_name = PSDEV_SUBSYSTEM_DEBUG,
                .procname = "subsystem_debug",
                .data     = &libcfs_subsystem_debug,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dobitmasks
        },
        {
                .ctl_name = PSDEV_PRINTK,
                .procname = "printk",
                .data     = &libcfs_printk,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dobitmasks
        },
        {
                .ctl_name = PSDEV_CONSOLE_RATELIMIT,
                .procname = "console_ratelimit",
                .data     = &libcfs_console_ratelimit,
                .maxlen   = sizeof(int),
                .mode     = 0644,
                .proc_handler = &proc_dointvec
        },

        {
                .ctl_name = PSDEV_DEBUG_PATH,
                .procname = "debug_path",
                .data     = debug_file_path,
                .maxlen   = sizeof(debug_file_path),
                .mode     = 0644,
                .proc_handler = &proc_dostring,
                .strategy =  &sysctl_string
        },

        {
                .ctl_name = PSDEV_LNET_UPCALL,
                .procname = "upcall",
                .data     = lnet_upcall,
                .maxlen   = sizeof(lnet_upcall),
                .mode     = 0644,
                .proc_handler = &proc_dostring,
                .strategy =  &sysctl_string
        },
        {
                .ctl_name = PSDEV_LNET_MEMUSED,
                .procname = "memused",
                .data     = (int *)&libcfs_kmemory.counter,
                .maxlen   = sizeof(int),
                .mode     = 0444,
                .proc_handler = &proc_dointvec
        },
        {
                .ctl_name = PSDEV_LNET_CATASTROPHE,
                .procname = "catastrophe",
                .data     = &libcfs_catastrophe,
                .maxlen   = sizeof(int),
                .mode     = 0444,
                .proc_handler = &proc_dointvec
        },
        {
                .ctl_name = PSDEV_LNET_PANIC_ON_LBUG,
                .procname = "panic_on_lbug",
                .data     = &libcfs_panic_on_lbug,
                .maxlen   = sizeof(int),
                .mode     = 0444,
                .proc_handler = &proc_dointvec
        },
        {0}
};

static struct ctl_table top_table[2] = {
        {
                .ctl_name = PSDEV_LNET,
                .procname = "lnet",
                .data     = NULL,
                .maxlen   = 0,
                .mode     = 0555,
                .child    = lnet_table
        },
        {0}
};

int LL_PROC_PROTO(proc_dobitmasks)
{
        const int     tmpstrlen = 512;
        char         *str;
        int           rc = 0;
        /* the proc filling api stumps me always, coax proc_dointvec
         * and proc_dostring into doing the drudgery by cheating
         * with a dummy ctl_table
         */
        struct ctl_table dummy = *table;
        unsigned int *mask = (unsigned int *)table->data;
        int           is_subsys = (mask == &libcfs_subsystem_debug) ? 1 : 0;

	str = kmalloc(tmpstrlen, GFP_USER);
        if (str == NULL)
                return -ENOMEM;

        if (write) {
                size_t oldlen = *lenp;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,8)
                loff_t oldpos = *ppos;
#endif

                dummy.proc_handler = &proc_dointvec;

                /* old proc interface allows user to specify just an int
                 * value; be compatible and don't break userland.
                 */
                rc = ll_proc_dointvec(&dummy, write, filp, buffer, lenp, ppos);

                if (rc != -EINVAL)
                        goto out;

                /* using new interface */
                dummy.data = str;
                dummy.maxlen = tmpstrlen;
                dummy.proc_handler = &proc_dostring;

                /* proc_dointvec might have changed these */
                *lenp = oldlen;
#if LINUX_VERSION_CODE >= KERNEL_VERSION(2,6,8)
                *ppos = oldpos;
#endif

                rc = ll_proc_dostring(&dummy, write, filp, buffer, lenp, ppos);

                if (rc != 0)
                        goto out;

                rc = libcfs_debug_str2mask(mask, dummy.data, is_subsys);
        } else {
                dummy.data = str;
                dummy.maxlen = tmpstrlen;
                dummy.proc_handler = &proc_dostring;

                libcfs_debug_mask2str(dummy.data, dummy.maxlen,*mask,is_subsys);

                rc = ll_proc_dostring(&dummy, write, filp, buffer, lenp, ppos);
        }

out:
        kfree(str);
        return rc;
}

int insert_proc(void)
{
        struct proc_dir_entry *ent;

#ifdef CONFIG_SYSCTL
        if (!lnet_table_header)
                lnet_table_header = cfs_register_sysctl_table(top_table, 0);
#endif

        ent = create_proc_entry("sys/lnet/dump_kernel", 0, NULL);
        if (ent == NULL) {
                CERROR("couldn't register dump_kernel\n");
                return -1;
        }
        ent->write_proc = trace_dk;

        ent = create_proc_entry("sys/lnet/daemon_file", 0, NULL);
        if (ent == NULL) {
                CERROR("couldn't register daemon_file\n");
                return -1;
        }
        ent->write_proc = trace_write_daemon_file;
        ent->read_proc = trace_read_daemon_file;

        ent = create_proc_entry("sys/lnet/debug_mb", 0, NULL);
        if (ent == NULL) {
                CERROR("couldn't register debug_mb\n");
                return -1;
        }
        ent->write_proc = trace_write_debug_mb;
        ent->read_proc = trace_read_debug_mb;

        return 0;
}

void remove_proc(void)
{
        remove_proc_entry("sys/lnet/dump_kernel", NULL);
        remove_proc_entry("sys/lnet/daemon_file", NULL);
        remove_proc_entry("sys/lnet/debug_mb", NULL);

#ifdef CONFIG_SYSCTL
        if (lnet_table_header)
                cfs_unregister_sysctl_table(lnet_table_header);
        lnet_table_header = NULL;
#endif
}
