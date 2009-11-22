/*
 *  interval.c
 *  Intervals
 *
 *  Created by Niko Matsakis on 9/13/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#include "interval.h"
#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <limits.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>

#pragma mark GCC Macros

#define ALIGNED(n) __attribute__((aligned(n)))

#define atomic_xchg __sync_lock_test_and_set
#define atomic_cmpxchg __sync_bool_compare_and_swap
#define atomic_add(v, a) __sync_add_and_fetch(v, a)
#define atomic_sub(v, a) __sync_sub_and_fetch(v, a)
#define atomic_rel_lock(v) __sync_lock_release(v)

#pragma mark Debugging Macros

#ifndef NDEBUG

static dispatch_once_t init_debug;
static dispatch_queue_t debug_queue;

static void debugf(const char *fmt, ...) {
	dispatch_once(&init_debug, ^{
		debug_queue = dispatch_queue_create("ch.ethz.intervals.debug", NULL);
	});
	
	pthread_t self = pthread_self();
	
	va_list ap;
	va_start(ap, fmt);
	const int initial_size = 128, self_bytes = 18;
	char *res = (char*)malloc(initial_size + self_bytes);
	int size = vsnprintf(res + self_bytes, initial_size, fmt, ap) + 1;
	if(size >= initial_size) {
		free(res);
		res = (char*)malloc(size * sizeof(char) + self_bytes);
		vsnprintf(res + self_bytes, size, fmt, ap);
	}
	va_end(ap);
	
	snprintf(res, self_bytes, "%016lx:", (intptr_t)self);
	res[self_bytes - 1] = ' ';
	
	dispatch_async(debug_queue, ^{
		fprintf(stderr, "%s\n", res);
		free(res);
	});
}

#else

#  define debugf(...)

#endif

#pragma mark Current Interval Information

static pthread_once_t key_init = PTHREAD_ONCE_INIT;
static pthread_key_t current_interval_key;

void init_current_interval_key_helper() {
	pthread_key_create(&current_interval_key, NULL);
}

void init_current_interval_key() {
	pthread_once(&key_init, init_current_interval_key_helper);
}

interval_t *current_interval() { // Must have invoked init_current_interval_key() first
	void *result = pthread_getspecific(current_interval_key);
	if(result == NULL)
		return root_interval();	
	return (interval_t*)result;
}

interval_t *set_current_interval(interval_t *interval) { // Must have invoked init_current_interval_key() first
	void *old_value = pthread_getspecific(current_interval_key);
	pthread_setspecific(current_interval_key, interval);
	return (interval_t*)old_value;	
}

#pragma mark Data Types and Simple Accessors

#define WC_STARTED UINT32_MAX
#define EDGE_CHUNK_SIZE 3
#define ONE_REF_COUNT    (1L)
#define ONE_WAIT_COUNT   (1L << 32)
#define ONE_REF_AND_WAIT_COUNT (ONE_REF_COUNT | ONE_WAIT_COUNT)
#define REF_COUNT(v) ((uint32_t)(v))
#define WAIT_COUNT(v) ((uint32_t)((v) >> 32))
#define TO_REF_COUNT(n) (((uint64_t)(n)))
#define TO_WAIT_COUNT(n) (((uint64_t)(n)) << 32)
#define TO_COUNT(rc, wc) (TO_REF_COUNT(rc) | TO_WAIT_COUNT(wc))
#define NULL_EPOINT ((epoint_t)0)

typedef intptr_t epoint_t;
typedef struct edge_t {
	epoint_t to_points[EDGE_CHUNK_SIZE];
	struct edge_t *next;
} edge_t;

struct guard_t {
	int ref_count;      // Always modified atomically.
	point_t *last_lock; // Always exchanged atomically.
};

struct point_t {
	// Immutable fields:
	int depth;
	point_t *bound;
	task_f task;
	void *userdata;
	
	// Mutable state:
	dispatch_semaphore_t lock;
	edge_t *out_edges;
	
	// Arrival and expected counts for each point:
	//
	//   The upper 32 bits ("wait") record the number of incoming
	//   edges from points which have not yet arrived.  The lower 
	//   32 bits ("ref. count") record the number of incoming edges
	//   from points that have not yet been freed.  Modifications 
	//   to these counts take place using atomic_add and atomic_sub
	//   and the ONE_* constants #define'd above.  The only exception
	//   is that once all points have arrived, the lock is acquired
	//   and the wait count set (atomically) to WC_STARTED.  Note 
	//   that the use of int64_t limits the in-degree of a point 
	//   to 2^32.
	uint64_t counts;
};	

guard_t *create_guard() {
	guard_t *guard = (guard_t*)malloc(sizeof(guard_t));
	guard->ref_count = 1;
	guard->last_lock = NULL;
	return guard;
}

guard_t *guard_retain(guard_t *guard) {
	if(guard) {
		atomic_add(&guard->ref_count, 1);
		return guard;
	}
}

void guard_release(guard_t *guard) {
	int count = atomic_sub(&guard->ref_count, 1);
	if(count == 0) {
		free(guard);
	}
}

// Encodes in one pointer the interval and side to which an edge points.
// If the synthetic bit is true, then this edge was not directly specified
// by the user but rather resulted from locking a guard or some other such
// feature.
static inline epoint_t epoint(point_t *point, bool synthetic) {
	edge_point_t epnt = (edge_point_t)point;
	epnt |= synthetic;
	return pnt;
}

static inline point_t *point_of_epoint(epoint_t pnt) {
	intptr_t i = pnt & ~0x3;
	return (point_t*)i;
}

static inline bool is_synthetic_epoint(epoint_t pnt) {
	return (pnt & 0x1) != 0;
}

static void point_lock(point_t *interval) {
	dispatch_semaphore_wait(interval->lock, DISPATCH_TIME_FOREVER);
	/*
	 int backoff;
	 while(atomic_xchg(&(interval->lock), 1))
	 while(interval->lock)
	 ;
	 */
}

static void point_unlock(point_t *interval) {
	dispatch_semaphore_signal(interval->lock);
	/*atomic_rel_lock(&interval->lock);*/
}

static void free_edges(edge_t *edge) {
	if(edge) {
		free_edges(edge->next);
		for(int i = 0; i < EDGE_CHUNK_SIZE; i++) {
			epoint_t epnt = edge->to_points[i];
			point_release(point_of_epoint(epnt));
		}
		free(edge);
	}
}

point_t *point_retain(point_t *point) {
	if(point) {
		atomic_add(&point->counts, ONE_REF_COUNT);
		return point;
	}
}

void point_release(point_t *point) {
	if(point) {
		uint64_t c = atomic_sub(&point->counts, ONE_REF_COUNT);
		if(REF_COUNT(c) == 0) {
			dispatch_release(point->lock);
			free_edges(point->out_edges);
			point_release(point->bound);
			free(point);
		}
	}
}

interval_t interval_retain(interval_t interval) {
	point_retain(interval.start);
	point_retain(interval.end);
	return interval;
}

void interval_release(interval_t interval) {
	point_release(interval.start);
	point_release(interval.end);
}

interval_t *root_interval() {
	return &s_root_interval;
}

#pragma mark Creating New Intervals

side_t new_sides[] = { SIDE_START, SIDE_START, SIDE_END, SIDE_END };
side_t existing_sides[] = { SIDE_START, SIDE_END, SIDE_START, SIDE_END };

static void arrive(point_t *interval, uint64_t count);

static void arrive_edge(edge_t *edge, uint64_t count) {
	for(int i = 0; i < EDGE_CHUNK_SIZE; i++) {
		epoint_t pnt = edge->to_points[i];
		if(pnt == NULL_EPOINT) return;
		arrive(point_of_epoint(pnt), count);
	}
}

// Stub which executes the interval's task and then frees the interval.
static void execute_interval(void *ctx) {
	interval_t *interval = (interval_t*) ctx;	
	interval_t *old_interval = set_current_interval(interval);
	interval->task(interval, interval->userdata); // Argh, double dispatch.  But what can you do?
	arrive(interval, SIDE_END, ONE_WAIT_COUNT); 
	set_current_interval(old_interval);
}

// Decrements the count(s) of 'interval.side' by 'count'.  Note that
// count must not result in the point's ref. count becoming 0! This
// should never happen in any case, as one ref. belongs to the scheduler,
// and it is only released in this function once the wait count becomes 0.
static void arrive(point_t *point, uint64_t count) {
	if(interval == &s_root_interval)
		return;
	
	uint64_t new_count = atomic_sub(point->counts, count);
	uint32_t new_wait_count = WAIT_COUNT(new_count);
	
	debugf("arrive(%p, %llx) new_count=%llx", point, count, new_count);
	
	if(new_wait_count == 0) {
		edge_t *notify0, *p, *pn;
		
		// We must add to counts[side] atomically in case of other, simultaneous
		// threads adjusting the ref count.
		point_lock(point);
		atomic_add(&interval->counts[side], TO_WAIT_COUNT(WC_STARTED));		
		notify0 = interval->out_edges[side];
		point_unlock(point);
		
		// Notify those coming after us
		for(p = notify0; p != NULL; p = p->next)
			arrive_edge(p, ONE_WAIT_COUNT);

		if(point->task)
			dispatch_async_f(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), interval, execute_interval);
		arrive(point->bound, ONE_WAIT_COUNT);

		// Release ref. count from scheduler.
		point_release(interval, side);
	}
}

static inline void insert_edge(edge_t **list, epoint_t pnt) {
	edge_t *cur_edge = *list;
	if(cur_edge != NULL) {
		for(int i = 1; i < EDGE_CHUNK_SIZE; i++) {
			if(cur_edge->to_points[i] == NULL_POINT) {
				cur_edge->to_points[i] = pnt;
				return;
			}
		}
	}
			
	edge_t *new_edge;
	*list = new_edge = (edge_t*)malloc(sizeof(edge_t));
	new_edge->to_points[0] = pnt;
	for(int i = 1; i < EDGE_CHUNK_SIZE; i++)
		new_edge->to_points[i] = NULL_POINT;
	new_edge->next = cur_edge;
}

static bool check_path_or_same(interval_t *from_inter, side_t from_side,
							   interval_t *to_inter, side_t to_side)
{
	if(from_inter == to_inter && from_side == to_side)
		return true;
	
	// XXX Implement path checking.
	
	return true;
}

bool point_bounded_by(point_t *inter, point_t *bnd) {
	int d = inter->depth - bnd->depth;
	for(int i = 0; i < d; i++) 
		inter = inter->parent;
	return inter == bnd;
}

static inline void add_incoming_edge(interval_t *existing_inter, side_t existing_side, 
									 interval_t *new_inter, side_t new_side,
									 bool synthetic,
									 uint64_t *waits) 
{
	uint64_t count;
	interval_lock(existing_inter);
	count = existing_inter->counts[existing_side];
	insert_edge(&existing_inter->out_edges[existing_side], point(new_inter, new_side, synthetic));
	interval_unlock(existing_inter);
	
	// If we managed to insert the edge before the existing point
	// occurred, then we will be expecting it to notify us.  In any
	// case, it now holds a ref on us -- we know that it itself cannot
	// be dead, because the existing interval is not dead.
	if(WAIT_COUNT(count) != WC_STARTED)
		waits[new_side] += ONE_REF_AND_WAIT_COUNT;
	else
		waits[new_side] += ONE_REF_COUNT;
	
	debugf("%p : %p.%d (%llx) -> new.%d (%llx) synthetic=%d", 
		   new_inter, 
		   existing_inter, existing_side, count,
		   new_side, waits[new_side], synthetic);
}

static interval_t *_create_async_interval(interval_t *parent, 
										  task_f task, void *userdata, 
										  interval_dependency_t dependencies[],
										  dispatch_semaphore_t signal)
{
	init_current_interval_key();
	interval_t *current_inter = current_interval();
	if(parent == NULL) parent = current_inter;
	
	// Check safety conditions.
#   ifndef OMIT_SAFETY_CHECKS
	if(!check_path_or_same(current_inter, SIDE_END, parent, SIDE_END))
		return NULL;
	for(int i = 0; dependencies[i].kind != _DEP_END; i++) {
		dependency_kind_t kind = dependencies[i].kind;
		
		if(kind > _DEP_BEFORE_MIN && kind < _DEP_BEFORE_MAX) {
			// new_inter.new_side -> existing_inter.existing_side
			side_t existing_side = existing_sides[kind - _DEP_BEFORE_MIN];
			interval_t *existing_inter = dependencies[i].item.interval;
			if(!inter_bounded_by(existing_inter, parent))
				return NULL;
			if(!check_path_or_same(current_inter, SIDE_END, existing_inter, existing_side))
				return NULL;
		}
	}
#   endif
	
	// Start the wait count at the highest possible value.
	// We will be adding pointers from points as we go; these points may
	// well decrement the interval's wait count, but it will never reach 0
	// and hence never activate.  Meanwhile, we track internally what the appropriate
	// wait count ought to be, and at the very end we adjust accordingly.  If
	// the points that the new interval was waiting for have already arrived,
	// that will cause it to begin right then and there!  Cute, huh?
	const uint64_t initial_wait_count = TO_REF_COUNT(UINT32_MAX - 1) | TO_WAIT_COUNT(UINT32_MAX - 1);
	
	point_t *new_end = (point_t*)malloc(sizeof(point_t));
	new_end->depth = bound->depth + 1;
	new_end->bound = bound;
	new_end->task = NULL;
	new_end->userdata = NULL;
	
	point_t *new_start = (point_t*)malloc(sizeof(point_t));
	
	interval_t *new_inter = (interval_t*)malloc(sizeof(interval_t));
	new_inter->depth = parent->depth + 1;
	new_inter->parent = parent;
	new_inter->task = task;
	new_inter->userdata = userdata;
	new_inter->lock = dispatch_semaphore_create(1);
	new_inter->signal = signal;
	new_inter->ref_count = 3; // 1 for the user, 1 from each point
	
	new_inter->counts[SIDE_START] = initial_wait_count;
	new_inter->out_edges[SIDE_START] = NULL;
	new_inter->counts[SIDE_END] = initial_wait_count;
	new_inter->out_edges[SIDE_END] = NULL;
	new_inter->pending_children = NULL;
	
	debugf("%p = create_async_interval(parent=%p, task=%p, userdata=%p, signal=%p)", 
		   new_inter, parent, task, userdata, signal);
	
	// Track how many things each side should wait for.
	// The start and end points initially have 1 ref from the scheduler.
	// The end point initially has 1 wait count (from the start point).
	uint64_t waits[SIDE_CNT] = {ONE_REF_COUNT, ONE_REF_AND_WAIT_COUNT}; 

	// Check whether the parent has started.  If not, insert an edge to us 
	// into the parent's list of pending children.
	if(WAIT_COUNT(parent->counts[SIDE_START]) != WC_STARTED) {
		interval_lock(parent);
		if(WAIT_COUNT(parent->counts[SIDE_START]) != WC_STARTED) {
			insert_edge(&parent->pending_children, point(new_inter, SIDE_START, false));
			waits[SIDE_START] += ONE_WAIT_COUNT;
		} 
		interval_unlock(parent);
	}
	
	// Tell the parent to wait for us (and hold a ref on them as well).
	if(parent != &s_root_interval)
		atomic_add(&parent->counts[SIDE_END], ONE_REF_AND_WAIT_COUNT);
	
	// Process the dependencies.
	for(int i = 0; dependencies[i].kind != _DEP_END; i++) {
		dependency_kind_t kind = dependencies[i].kind;
		
		if(kind > _DEP_AFTER_MIN && kind < _DEP_AFTER_MAX) {
			// existing_inter.existing_side -> new_inter.new_side
			side_t new_side = new_sides[kind - _DEP_AFTER_MIN];
			side_t existing_side = existing_sides[kind - _DEP_AFTER_MIN];
			interval_t *existing_inter = dependencies[i].item.interval;			
			add_incoming_edge(existing_inter, existing_side, new_inter, new_side, false, waits);
		} else if (kind > _DEP_BEFORE_MIN && kind < _DEP_BEFORE_MAX) {
			// new_inter.new_side -> existing_inter.existing_side
			side_t new_side = new_sides[kind - _DEP_BEFORE_MIN];
			side_t existing_side = existing_sides[kind - _DEP_BEFORE_MIN];
			interval_t *existing_inter = dependencies[i].item.interval;
			
			// any edge to root must be (a) to root.end and (b) redundant
			if(existing_inter != &s_root_interval) {
				uint64_t c = atomic_add(&existing_inter->counts[existing_side], ONE_REF_AND_WAIT_COUNT);
				insert_edge(&new_inter->out_edges[new_side], point(existing_inter, existing_side, false));
				
				debugf("%p : new.%d -> %p.%d (%llx)", 
					   new_inter, new_side, 
					   existing_inter, existing_side, c);
			}			
		} 
	}
	
	// Process locks separately, because when the new_inter object becomes the last_lock
	// of the guard, other interval creations may concurrently try to add edges to
	// new_inter.  This way we don't require a lock for new outgoing edges in the
	// previous loop.
	for(int i = 0; dependencies[i].kind != _DEP_END; i++) {
		dependency_kind_t kind = dependencies[i].kind;
		if (kind == _DEP_LOCK) {
			guard_t *guard = dependencies[i].item.guard;
			interval_t *inter = atomic_xchg(&guard->last_lock, new_inter);
			add_incoming_edge(inter, SIDE_END, new_inter, SIDE_START, true, waits);
		}
	}
	
	debugf("%p = waits = { %llx, %llx }, initial_wait_count = %llx", 
		   new_inter, waits[SIDE_START], waits[SIDE_END], initial_wait_count);
	
	// Adjust wait counts appropriately.  Note that some of the things
	// we were waiting for may have arrived in the meantime!
	arrive(new_inter, SIDE_END, initial_wait_count - waits[SIDE_END]);
	arrive(new_inter, SIDE_START, initial_wait_count - waits[SIDE_START]);
	
	return new_inter;
}

interval_t *create_async_interval(interval_t *parent, task_f task, void *userdata, interval_dependency_t dependencies[])
{
	return _create_async_interval(parent, task, userdata, dependencies, NULL);
}

void sync_interval(task_f task, void *userdata, interval_dependency_t dependencies[])
{
	dispatch_semaphore_t signal = dispatch_semaphore_create(0);	
	interval_t *result_inter = _create_async_interval(INTERVAL_CURRENT, task, userdata, dependencies, signal);
	debugf("waiting for %p (signal = %p)", result_inter, signal);
	dispatch_semaphore_wait(signal, DISPATCH_TIME_FOREVER); // Signal is sent from sync_func()
	dispatch_release(signal);
	interval_release(result_inter);
}

#pragma mark -
#pragma mark Suspended Intervals

static void suspended_function(interval_t *current) {} // Dummy

interval_t *create_suspended_interval() {
	const interval_t *parent = &s_root_interval;
	
	interval_t *new_inter = (interval_t*)malloc(sizeof(interval_t));
	new_inter->depth = parent->depth + 1;
	new_inter->parent = parent;
	new_inter->task = suspended_function;
	new_inter->userdata = NULL;
	new_inter->lock = dispatch_semaphore_create(1);
	new_inter->signal = NULL;
	new_inter->ref_count = 2; // 1 for the user, 1 from end point
	
	new_inter->counts[SIDE_START] = TO_COUNT(0, WC_STARTED);
	new_inter->out_edges[SIDE_START] = NULL;
	new_inter->counts[SIDE_END] = TO_COUNT(1, 1);
	new_inter->out_edges[SIDE_END] = NULL;
	new_inter->pending_children = NULL;
}

void suspended_interval_release(interval_t *interval) {
	assert(interval->task == suspended_function);
	arrive(interval, SIDE_END, 1);
	interval_release(interval);
}
