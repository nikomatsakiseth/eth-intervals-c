#ifndef INTERVAL_H
#define INTERVAL_H

#include <stdbool.h>

typedef struct point_t point_t;
typedef struct guard_t guard_t;
typedef void (*task_f)(interval_t*, void*);

typedef struct interval_t {
	point_t *start, *end;
} interval_t;

typedef enum dependency_kind_t {
	_DEP_END,
	
	_DEP_LOCK,
	
	_DEP_AFTER_MIN,
	_DEP_START_AFTER,
	_DEP_END_AFTER,
	_DEP_AFTER_MAX,
	
	_DEP_BEFORE_MIN,
	_DEP_START_BEFORE,
	_DEP_END_BEFORE,
	_DEP_BEFORE_MAX,
} dependency_kind_t;

typedef enum side_t {
	SIDE_START, 
	SIDE_END,
	SIDE_CNT
} side_t;

typedef struct interval_dependency_t {
	dependency_kind_t kind;
	union {
		point_t *point;
		guard_t *guard;
	} item;
} interval_dependency_t;

#define INTERVAL_NO_DEPS                 ((interval_dependency_t[]) { { _DEP_END } })
#define INTERVAL_DEPS(deps...)           ((interval_dependency_t[]) { deps, { _DEP_END } })
#define INTERVAL_CURRENT                 NULL

#define DEP_START_AFTER(pnt)  ((interval_dependency_t){ .kind=_DEP_START_AFTER,  .item.point=pnt })
#define DEP_END_AFTER(pnt)    ((interval_dependency_t){ .kind=_DEP_END_AFTER,    .item.point=pnt })

#define DEP_START_BEFORE(pnt) ((interval_dependency_t){ .kind=_DEP_START_BEFORE, .item.point=pnt })
#define DEP_END_BEFORE(pnt)   ((interval_dependency_t){ .kind=_DEP_END_BEFORE,   .item.point=pnt })

#define DEP_LOCK(g)           ((interval_dependency_t){ .kind=_DEP_LOCK,         .item.guard=g })

// Functions to create and manipulate standard intervals:
interval_t root_interval();
interval_t create_unsched_interval(point_t *bound, task_f task, void *userdata, interval_dependency_t dependencies[]); // Use INTERVAL_CURRENT for parent to use current interval
void sched_interval(point_t *start);
void sync_interval(task_f task, void *userdata, interval_dependency_t dependencies[]);  // Current interval (or root) is parent
bool point_bounded_by(interval_t *inter, interval_t *bnd); // Returns true if 'bnd' is a bounding interval of 'inter'

point_t *point_retain(point_t *point);            // Increase ref count and return 'point'
void point_release(point_t *point);               // Release ref count on 'point', possibly freeing it
interval_t interval_retain(interval_t interval);  // Retains both points and returns 'interval'
void interval_release(interval_t interval);       // Releases both points

// Functions to create and manipulate suspended intervals:
//
//   A suspended interval is one which does not complete until the
//   it is released by a call to suspended_interval_release().
//   In other ways, it acts like a normal interval.
//
//   As failing to call suspended_interval_release()
//   will cause deadlock, using suspended intervals is dangerous.
//   They are intended to be used for extending the scheduler
//   to take into account other kinds of asynchronous events.
//
//   The idea is that users may create a suspended interval and
//   integrate it into their scheduling dependencies.  Therefore,
//   one may specify that other tasks must wait for the suspended
//   interval before proceeding.
interval_t *create_suspended_interval();
void suspended_interval_release(interval_t *interval); // Also decrements ref. count!

// Functions to create and manipulate guards:
// 
//   Guards act as locks or queues.  Intervals may inform
//   the scheduler that they lock a particular guard.  All 
//   intervals which lock the same guard will be scheduled
//   in some sequential order.  Intervals may lock multiple
//   guards.  Be wary of deadlocks when using guards, 
//   however!  
guard_t *create_guard();
guard_t *guard_retain(guard_t *g);  // Always returns g.
void guard_release(guard_t *g);

#endif