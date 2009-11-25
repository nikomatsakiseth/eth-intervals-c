/*
 *  Intervals Library
 *
 *  Copyright 2009 Nicholas D. Matsakis.
 *
 *  This library is open source and distributed under the GPLv3 license.  
 *  Please see the file LICENSE for distribution and licensing details.
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

void interval_pool_run(int workers, void (^)());
void interval_pool_enqueue(point_t *start_point);

void interval_pool_init_latch(interval_pool_latch_t *latch);
void interval_pool_signal_latch(interval_pool_latch_t *latch); // should occur precisely once
void interval_pool_wait_latch(interval_pool_latch_t *latch);   // also frees any memory associated with the latch


#endif