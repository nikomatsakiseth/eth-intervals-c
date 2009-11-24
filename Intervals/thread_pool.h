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

typedef struct interval_pool_t interval_pool_t;
typedef void (*interval_pool_func_t)(void *context);
typedef int interval_pool_latch_t;

void interval_pool_run(int workers, void (^)(interval_pool_t *pool));
void interval_pool_enqueue(point_t *start_point);

void interval_pool_init_latch(interval_pool_latch_t *latch);
void interval_pool_signal_latch(interval_pool_latch_t *latch); // should occur precisely once
void interval_pool_wait_latch(interval_pool_latch_t *latch);   // also frees any memory associated with the latch

#endif

