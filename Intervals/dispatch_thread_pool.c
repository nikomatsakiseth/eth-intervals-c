/*
 *  Intervals Library
 *
 *  Copyright 2009 Nicholas D. Matsakis.
 *
 *  This library is open source and distributed under the GPLv3 license.  
 *  Please see the file LICENSE for distribution and licensing details.
 */

#ifdef INTERVALS_USE_LIB_DISPATCH

#include "thread_pool.h"
#include "internal.h"

void interval_pool_run(int workers, void (^blk)())
{
	blk();
}

static void interval_pool_execute(void *data)
{
	point_t *start_point = (point_t*)data;
	interval_execute(start_point);
}

void interval_pool_enqueue(point_t *start_point)
{
	dispatch_async_f(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), start_point, interval_pool_execute);
}

void interval_pool_init_latch(interval_pool_latch_t *latch)
{
	*latch = dispatch_semaphore_create(0);
}

void interval_pool_signal_latch(interval_pool_latch_t *latch)
{
	dispatch_semaphore_signal(*latch);
}

void interval_pool_wait_latch(interval_pool_latch_t *latch)
{
	dispatch_semaphore_wait(*latch, DISPATCH_TIME_FOREVER);
	dispatch_release(*latch);
	*latch = NULL;
}


#endif
