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
 * http://www.gnu.org/licenses/gpl-2.0.html
 *
 * GPL HEADER END
 */
/*
 * Copyright (c) 2008, 2010, Oracle and/or its affiliates. All rights reserved.
 * Use is subject to license terms.
 *
 * Copyright (c) 2014, Intel Corporation.
 */
/*
 * This file is part of Lustre, http://www.lustre.org/
 * Lustre is a trademark of Sun Microsystems, Inc.
 *
 * libcfs/include/libcfs/linux/linux-time.h
 *
 * Implementation of portable time API for Linux (kernel and user-level).
 *
 * Author: Nikita Danilov <nikita@clusterfs.com>
 */

#ifndef __LIBCFS_LINUX_LINUX_TIME_H__
#define __LIBCFS_LINUX_LINUX_TIME_H__

/* Portable time API */

/*
 * Platform provides three opaque data-types:
 *
 *  cfs_time_t        represents point in time. This is internal kernel
 *                    time rather than "wall clock". This time bears no
 *                    relation to gettimeofday().
 *
 *  cfs_duration_t    represents time interval with resolution of internal
 *                    platform clock
 *
 *  cfs_time_t     cfs_time_current(void);
 *  cfs_time_t     cfs_time_add    (cfs_time_t, cfs_duration_t);
 *  cfs_duration_t cfs_time_sub    (cfs_time_t, cfs_time_t);
 *
 *  time_t         cfs_duration_sec (cfs_duration_t);
 */

#include <linux/hrtimer.h>
#include <linux/module.h>
#include <linux/kernel.h>
#include <linux/version.h>
#include <linux/jiffies.h>
#include <linux/hrtimer.h>
#include <linux/types.h>
#include <linux/time.h>
#include <asm/div64.h>

/*
 * Generic kernel stuff
 */

typedef unsigned long cfs_time_t;      /* jiffies */
typedef long cfs_duration_t;

#ifndef HAVE_TIMESPEC64

typedef __s64 time64_t;

#if __BITS_PER_LONG == 64

# define timespec64 timespec

static inline struct timespec64 timespec_to_timespec64(const struct timespec ts)
{
	return ts;
}

static inline struct timespec timespec64_to_timespec(const struct timespec64 ts)
{
	return ts;
}

#else
struct timespec64 {
	time64_t	tv_sec;		/* seconds */
	long		tv_nsec;	/* nanoseconds */
};

static inline struct timespec64 timespec_to_timespec64(const struct timespec ts)
{
	struct timespec64 ret;

	ret.tv_sec = ts.tv_sec;
	ret.tv_nsec = ts.tv_nsec;
	return ret;
}

static inline struct timespec timespec64_to_timespec(const struct timespec64 ts64)
{
	struct timespec ret;

	ret.tv_sec = (time_t)ts64.tv_sec;
	ret.tv_nsec = ts64.tv_nsec;
	return ret;
}
#endif /* __BITS_PER_LONG != 64 */

#endif /* HAVE_TIMESPEC64 */

#ifndef HAVE_KTIME_ADD
# define ktime_add(lhs, rhs) ({ (ktime_t){ .tv64 = (lhs).tv64 + (rhs).tv64 }; })
#endif /* !HAVE_KTIME_ADD */

#ifndef HAVE_KTIME_AFTER
static inline bool ktime_after(const ktime_t cmp1, const ktime_t cmp2)
{
	return cmp1.tv64 > cmp2.tv64;
}
#endif /* !HAVE_KTIME_AFTER */

#ifndef HAVE_KTIME_BEFORE
static inline bool ktime_before(const ktime_t cmp1, const ktime_t cmp2)
{
	return cmp1.tv64 < cmp2.tv64;
}
#endif /* !HAVE_KTIME_BEFORE */

#ifndef HAVE_KTIME_COMPARE
static inline int ktime_compare(const ktime_t cmp1, const ktime_t cmp2)
{
	if (cmp1.tv64 < cmp2.tv64)
		return -1;
	if (cmp1.tv64 > cmp2.tv64)
		return 1;
	return 0;
}
#endif /* !HAVE_KTIME_COMPARE */

#ifndef HAVE_KTIME_GET_TS64
void ktime_get_ts64(struct timespec64 *ts);
#endif /* HAVE_KTIME_GET_TS */

#ifndef HAVE_KTIME_GET_REAL_TS64
void ktime_get_real_ts64(struct timespec64 *ts);
#endif /* HAVE_KTIME_GET_REAL_TS */

#ifndef HAVE_KTIME_GET_REAL_SECONDS
time64_t ktime_get_real_seconds(void);
#endif /* HAVE_KTIME_GET_REAL_SECONDS */

#ifndef HAVE_KTIME_GET_SECONDS
time64_t ktime_get_seconds(void);
#endif /* HAVE_KTIME_GET_SECONDS */

#ifdef NEED_KTIME_GET_NS
static inline u64 ktime_get_ns(void)
{
	return ktime_to_ns(ktime_get());
}
#endif /* NEED_KTIME_GET_NS */

#ifdef NEED_KTIME_GET_REAL_NS
static inline u64 ktime_get_real_ns(void)
{
	return ktime_to_ns(ktime_get_real());
}
#endif /* NEED_KTIME_GET_REAL_NS */

#ifndef HAVE_KTIME_MS_DELTA
static inline s64 ktime_ms_delta(const ktime_t later, const ktime_t earlier)
{
	return ktime_to_ms(ktime_sub(later, earlier));
}
#endif /* HAVE_KTIME_MS_DELTA */

#ifndef HAVE_KTIME_TO_TIMESPEC64
static inline struct timespec64 ktime_to_timespec64(ktime_t kt)
{
	struct timespec ts = ns_to_timespec((kt).tv64);

	return timespec_to_timespec64(ts);
}
#endif /* HAVE_KTIME_TO_TIMESPEC64 */

#ifndef HAVE_TIMESPEC64_SUB
static inline struct timespec64
timespec64_sub(struct timespec64 later, struct timespec64 earlier)
{
	struct timespec diff;

	diff = timespec_sub(timespec64_to_timespec(later),
			    timespec64_to_timespec(earlier));
	return timespec_to_timespec64(diff);
}
#endif

#ifndef HAVE_TIMESPEC64_TO_KTIME
static inline ktime_t timespec64_to_ktime(struct timespec64 ts)
{
	return ktime_set(ts.tv_sec, ts.tv_nsec);
}
#endif

static inline int cfs_time_before(cfs_time_t t1, cfs_time_t t2)
{
        return time_before(t1, t2);
}

static inline int cfs_time_beforeq(cfs_time_t t1, cfs_time_t t2)
{
        return time_before_eq(t1, t2);
}

static inline cfs_time_t cfs_time_current(void)
{
        return jiffies;
}

static inline time_t cfs_time_current_sec(void)
{
	return get_seconds();
}

static inline cfs_duration_t cfs_time_seconds(int seconds)
{
	return ((cfs_duration_t)seconds) * msecs_to_jiffies(MSEC_PER_SEC);
}

static inline time_t cfs_duration_sec(cfs_duration_t d)
{
	return d / msecs_to_jiffies(MSEC_PER_SEC);
}

#ifdef HAVE_NEW_DEFINE_TIMER
# ifndef TIMER_DATA_TYPE
# define TIMER_DATA_TYPE struct timer_list *
# endif

#define CFS_DEFINE_TIMER(_name, _function, _expires, _data) \
	DEFINE_TIMER((_name), (_function))
#else
# ifndef TIMER_DATA_TYPE
# define TIMER_DATA_TYPE unsigned long
# endif

#define CFS_DEFINE_TIMER(_name, _function, _expires, _data) \
	DEFINE_TIMER((_name), (_function), (_expires), (_data))
#endif

#ifdef HAVE_TIMER_SETUP
#define cfs_timer_cb_arg_t struct timer_list *
#define cfs_from_timer(var, callback_timer, timer_fieldname) \
	from_timer(var, callback_timer, timer_fieldname)
#define cfs_timer_setup(timer, callback, data, flags) \
	timer_setup((timer), (callback), (flags))
#define cfs_timer_cb_arg(var, timer_fieldname) (&(var)->timer_fieldname)
#else
#define cfs_timer_cb_arg_t unsigned long
#define cfs_from_timer(var, data, timer_fieldname) (typeof(var))(data)
#define cfs_timer_setup(timer, callback, data, flags) \
	setup_timer((timer), (callback), (data))
#define cfs_timer_cb_arg(var, timer_fieldname) (cfs_timer_cb_arg_t)(var)
#endif

#endif /* __LIBCFS_LINUX_LINUX_TIME_H__ */
