/*
 *  Intervals Library
 *
 *  Copyright 2009 Nicholas D. Matsakis.
 *
 *  This library is open source and distributed under the GPLv3 license.  
 *  Please see the file LICENSE for distribution and licensing details.
 */

#ifndef INTERVAL_H
#define INTERVAL_H

#include <stdbool.h>

#define INTERVAL_SAFETY_CHECKS_ENABLED

#pragma mark Type Definitions
typedef struct point_t point_t;
typedef struct guard_t guard_t;
typedef struct interval_t {
	point_t *start, *end;
} interval_t;
typedef void (*task_func_t)(interval_t, void*);
typedef void (^interval_block_t)(interval_t inter);
typedef enum interval_err_t {
	INTERVAL_OK,
	INTERVAL_EDGE_REQUIRED,
	INTERVAL_CYCLE,
	INTERVAL_NO_ROOT
} interval_err_t;

#pragma mark Creating Intervals
void root_interval(interval_block_t task); // invoked to start interval runtime
interval_t interval(point_t *bound, interval_block_t task); // NULL bound == bound of current interval
interval_t interval_f(point_t *bound, task_func_t task, void *userdata);
interval_err_t subinterval(interval_block_t task);
interval_err_t subinterval_f(task_func_t task, void *userdata);

#pragma mark Scheduling Intervals
interval_err_t interval_add_hb(point_t *before, point_t *after);
interval_err_t interval_lock(interval_t interval, guard_t *guard);
interval_err_t interval_schedule();

#pragma mark Querying Intervals and Points
bool point_hb(point_t *before, point_t *after);
bool point_bounded_by(point_t *point, point_t *bound);

#pragma mark Creating Guards
guard_t *guard();

#pragma mark Memory Management
point_t *point_retain(point_t *point);            // Increase ref count and return 'point'
guard_t *guard_retain(guard_t *guard);            // Increase ref count and return 'guard'
interval_t interval_retain(interval_t interval);  // Retains both points and returns 'interval'
void point_release(point_t *point);               // Release ref count on 'point', possibly freeing it
void guard_release(guard_t *guard);               // Release ref count on 'guard', possibly freeing it
void interval_release(interval_t interval);       // Releases both points

#endif