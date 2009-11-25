/*
 *  Intervals Library
 *
 *  Copyright 2009 Nicholas D. Matsakis.
 *
 *  This library is open source and distributed under the GPLv3 license.  
 *  Please see the file LICENSE for distribution and licensing details.
 */

/*
 
 This file defines the interval thread pool interface used by
 interval.c.   This interface is specialized to intervals and
 is not intended for general use (that's what intervals are for,
 after all).
 
 */

#ifndef INTERVALS_THREAD_POOL_H
#define INTERVALS_THREAD_POOL_H

#include "interval.h"

#ifndef INTERVALS_USE_LIB_DISPATCH
typedef int interval_pool_latch_t;
#else
#include <dispatch/dispatch.h>
typedef dispatch_semaphore_t interval_pool_latch_t;
#endif

// Interval Pool:
//
// \c interval_pool_run() starts up the interval pool and 
// executes \c blk.  
// 
// \c interval_pool_enqueue() adds new work to the pool for
// eventual execution. Must be invoked from \c blk.  The work
// takes the form of a start point: when the start point is
// scheduled, the function \c interval_execute() (defined
// in \c internal.h) will be called with \c start_point as an
// argument.

void interval_pool_run(int workers, void (^blk)());
void interval_pool_enqueue(point_t *start_point);

// Latches:
//
// Latches allow one piece of code to signal another.  They
// are used to implement blocking intervals.  We assume each
// latch is signal'd and wait'd exactly once.  Signalled a latch
// happens before the return of wait.

void interval_pool_init_latch(interval_pool_latch_t *latch);
void interval_pool_signal_latch(interval_pool_latch_t *latch); // should occur precisely once
void interval_pool_wait_latch(interval_pool_latch_t *latch);   // also frees any memory associated with the latch


#endif