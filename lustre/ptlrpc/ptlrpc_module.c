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
 * Copyright (c) 2003, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 */

#define DEBUG_SUBSYSTEM S_RPC

#ifndef __KERNEL__
# include <liblustre.h>
#endif

#include <obd_support.h>
#include <obd_class.h>
#include <lustre_net.h>
#include <lustre_req_layout.h>

#include "ptlrpc_internal.h"

extern cfs_spinlock_t ptlrpc_last_xid_lock;
#if RS_DEBUG
extern cfs_spinlock_t ptlrpc_rs_debug_lock;
#endif
extern cfs_spinlock_t ptlrpc_all_services_lock;
extern cfs_mutex_t pinger_mutex;
extern cfs_mutex_t ptlrpcd_mutex;

__init int ptlrpc_init(void)
{
        int rc, cleanup_phase = 0;
        ENTRY;

        lustre_assert_wire_constants();
#if RS_DEBUG
        cfs_spin_lock_init(&ptlrpc_rs_debug_lock);
#endif
        cfs_spin_lock_init(&ptlrpc_all_services_lock);
        cfs_mutex_init(&pinger_mutex);
        cfs_mutex_init(&ptlrpcd_mutex);
        ptlrpc_init_xid();

	rc = req_layout_init();
	if (rc)
		RETURN(rc);

	rc = ptlrpc_hr_init();
	if (rc)
		RETURN(rc);

	cleanup_phase = 1;
	rc = ptlrpc_request_cache_init();
	if (rc)
		GOTO(cleanup, rc);

	cleanup_phase = 2;
	rc = ptlrpc_init_portals();
	if (rc)
		GOTO(cleanup, rc);

	cleanup_phase = 3;

	rc = ptlrpc_connection_init();
	if (rc)
		GOTO(cleanup, rc);

	cleanup_phase = 4;
	ptlrpc_put_connection_superhack = ptlrpc_connection_put;

	rc = ptlrpc_start_pinger();
	if (rc)
		GOTO(cleanup, rc);

	cleanup_phase = 5;
	rc = ldlm_init();
	if (rc)
		GOTO(cleanup, rc);

	cleanup_phase = 6;
	rc = sptlrpc_init();
	if (rc)
		GOTO(cleanup, rc);

	cleanup_phase = 7;
	rc = llog_recov_init();
	if (rc)
		GOTO(cleanup, rc);

	RETURN(0);

cleanup:
	CDEBUG(D_NET, "ptlrpc_init failed, phase %d\n", cleanup_phase);
	switch(cleanup_phase) {
	case 7:
		sptlrpc_fini();
	case 6:
		ldlm_exit();
	case 5:
		ptlrpc_stop_pinger();
	case 4:
		ptlrpc_connection_fini();
	case 3:
		ptlrpc_exit_portals();
	case 2:
		ptlrpc_request_cache_fini();
	case 1:
		ptlrpc_hr_fini();
		req_layout_fini();
	default: ;
	}

	return rc;
}

#ifdef __KERNEL__
static void __exit ptlrpc_exit(void)
{
        llog_recov_fini();
        sptlrpc_fini();
        ldlm_exit();
        ptlrpc_stop_pinger();
        ptlrpc_exit_portals();
	ptlrpc_request_cache_fini();
        ptlrpc_hr_fini();
        ptlrpc_connection_fini();
}

MODULE_AUTHOR("Sun Microsystems, Inc. <http://www.lustre.org/>");
MODULE_DESCRIPTION("Lustre Request Processor and Lock Management");
MODULE_LICENSE("GPL");

cfs_module(ptlrpc, "1.0.0", ptlrpc_init, ptlrpc_exit);
#endif
