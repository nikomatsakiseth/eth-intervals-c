/*
 *  Intervals Library
 *
 *  Copyright 2009 Nicholas D. Matsakis.
 *
 *  This library is open source and distributed under the GPLv3 license.  
 *  Please see the file LICENSE for distribution and licensing details.
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
#include <libkern/OSAtomic.h>
#include <string.h>
#include <Block.h>

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

#pragma mark Data Types and Simple Accessors

#define RC_ROOT UINT32_MAX     // ref count on the root nodes
#define WC_STARTED UINT32_MAX  // 
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

typedef int epoch_t;
typedef intptr_t epoint_t;
typedef struct edge_t {
	epoint_t to_points[EDGE_CHUNK_SIZE];
	epoch_t from_epochs[EDGE_CHUNK_SIZE];
	struct edge_t *next;
} edge_t;

struct guard_t {
	int ref_count;      // Always modified atomically.
	point_t *last_lock; // Always exchanged atomically.
};

struct point_t {
	// Immutable fields:
	point_t *bound;
	interval_block_t task;
	int depth;
	
	// Mutable state:
	OSSpinLock lock;
	edge_t *out_edges; /* guarded by lock */
	epoch_t epoch;     /* guarded by lock */
	
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

typedef struct current_interval_info_t current_interval_info_t;
struct current_interval_info_t {
	current_interval_info_t *next; // points to the previous current_interval_info
	point_t *start;
	point_t *end;
	edge_t *unscheduled_starts;
};


#pragma mark Manipulating Edge Lists

static void arrive(point_t *interval, uint64_t count);

// Encodes in one pointer the interval and side to which an edge points.
// If the synthetic bit is true, then this edge was not directly specified
// by the user but rather resulted from locking a guard or some other such
// feature.
static inline epoint_t epoint(point_t *point, bool synthetic) {
	epoint_t epnt = (epoint_t)point;
	epnt |= synthetic;
	return epnt;
}

static inline point_t *point_of_epoint(epoint_t pnt) {
	intptr_t i = pnt & ~0x3;
	return (point_t*)i;
}

static inline bool is_synthetic_epoint(epoint_t pnt) {
	return (pnt & 0x1) != 0;
}

static inline void insert_edge(edge_t **list, epoint_t pnt) {
	edge_t *cur_edge = *list;
	if(cur_edge != NULL) {
		for(int i = 1; i < EDGE_CHUNK_SIZE; i++) {
			if(cur_edge->to_points[i] == NULL_EPOINT) {
				cur_edge->to_points[i] = pnt;
				return;
			}
		}
	}
	
	edge_t *new_edge;
	*list = new_edge = (edge_t*)malloc(sizeof(edge_t));
	new_edge->to_points[0] = pnt;
	for(int i = 1; i < EDGE_CHUNK_SIZE; i++)
		new_edge->to_points[i] = NULL_EPOINT;
	new_edge->next = cur_edge;
}

static void arrive_edge(edge_t *edge, uint64_t count) {
	for(int i = 0; i < EDGE_CHUNK_SIZE; i++) {
		epoint_t pnt = edge->to_points[i];
		if(pnt == NULL_EPOINT) return;
		arrive(point_of_epoint(pnt), count);
	}
	
	arrive_edge(edge->next, count);
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

#pragma mark Current Interval Information

static pthread_once_t key_init = PTHREAD_ONCE_INIT;
static pthread_key_t current_interval_key;

static void init_current_interval_key_helper() {
	pthread_key_create(&current_interval_key, NULL);
}

static void init_current_interval_key() {
	pthread_once(&key_init, init_current_interval_key_helper);
}

static current_interval_info_t *current_interval_info() { // Must have invoked init_current_interval_key() first
	void *result = pthread_getspecific(current_interval_key);
	return (current_interval_info_t*)result;
}

static void push_current_interval_info(current_interval_info_t *info, point_t *start, point_t *end) {
	info->start = start;
	info->end = end;
	info->next = current_interval_info();
	pthread_setspecific(current_interval_key, info);
}

static void pop_current_interval_info(current_interval_info_t *info) {
	pthread_setspecific(current_interval_key, info->next);
}

static bool is_unscheduled(current_interval_info_t *info, point_t *tar) {
	for(edge_t *edge = info->unscheduled_starts; edge != NULL; edge = edge->next)
	{
		for(int i = 0; i < EDGE_CHUNK_SIZE; i++) {
			epoint_t epnt = edge->to_points[i];
			if(epnt == NULL_EPOINT) 
				return false;
			
			point_t *pnt = point_of_epoint(epnt);
			if(pnt == tar || pnt->bound == tar)
				return true;
		}
	}
	return false;
}

#pragma mark Manipulating Points

static point_t *point(point_t *bound, interval_block_t task, uint64_t counts)
{
	point_t *result = (point_t*)malloc(sizeof(point_t));
	result->bound = bound;
	result->depth = (bound ? bound->depth + 1 : 0);
	result->task = task;
	result->lock = 0;
	result->out_edges = NULL;
	result->epoch = 0;
	result->counts = counts;
	return result;
}

static inline void point_add_count(point_t *point, uint64_t amnt) {
	atomic_add(&point->counts, amnt);
}

static inline void point_lock(point_t *point) {
	OSSpinLockLock(&point->lock);
}

static inline void point_unlock(point_t *point) {
	OSSpinLockUnlock(&point->lock);
}

#pragma mark Tracking Wait Counts

// Stub which executes the interval's task and then frees the interval.
static void execute_interval(void *ctx) {
	point_t *start = (point_t*)ctx;
	interval_t inter = { .start = start, .end = start->bound };
	
	current_interval_info_t info;
	push_current_interval_info(&info, start, start->bound);
	
	start->task(inter);
	arrive(start->bound, ONE_WAIT_COUNT);
	point_release(start);
	
	pop_current_interval_info(&info);
}

// Decrements the count(s) of 'interval.side' by 'count'.  Note that
// count must not result in the point's ref. count becoming 0! This
// should never happen in any case, as one ref. belongs to the scheduler,
// and it is only released in this function once the wait count becomes 0.
static void arrive(point_t *point, uint64_t count) {
	uint64_t new_count = atomic_sub(&point->counts, count);
	uint32_t new_wait_count = WAIT_COUNT(new_count);
	
	debugf("arrive(%p, %llx) new_count=%llx", point, count, new_count);
	
	if(new_wait_count == 0) {
		edge_t *notify;
		
		// We must add to counts[side] atomically in case of other, simultaneous
		// threads adjusting the ref count.
		point_lock(point);
		atomic_add(&point->counts, TO_WAIT_COUNT(WC_STARTED));		
		notify = point->out_edges;
		point_unlock(point);
		
		// Notify those coming after us
		arrive_edge(notify, ONE_WAIT_COUNT);
		
		if(point->task)
			dispatch_async_f(dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0), point, execute_interval);
		arrive(point->bound, ONE_WAIT_COUNT);
		
		// Release ref. count from scheduler.
		point_release(point);
	}
}

#pragma mark Safety Checks

static void interval_add_hb_unchecked(point_t *before, point_t *after, bool synthetic);

static interval_err_t check_can_add_dep(current_interval_info_t *info, point_t *pnt) 
{
#   ifdef INTERVAL_SAFETY_CHECKS_ENABLED
	if(info->end != pnt &&
	   !is_unscheduled(info, pnt) &&
	   !point_hb(info->end, pnt))
	{
		return INTERVAL_EDGE_REQUIRED;
	}
#   endif
	return INTERVAL_OK;
}

#pragma mark Creating Intervals 

void root_interval(interval_block_t task)
{
	init_current_interval_key();
	assert(current_interval_info() == NULL);

	point_t *root_end = point(NULL, NULL, TO_COUNT(RC_ROOT, 1));
	point_t *root_start = point(root_end, NULL, TO_COUNT(RC_ROOT, WC_STARTED));
	
	current_interval_info_t root_info;	
	push_current_interval_info(&root_info, root_start, root_end);

	interval_t root = { .start = root_start, .end = root_end };
	task(root);
	
	pop_current_interval_info(&root_info);
}

interval_t interval(point_t *bound, interval_block_t task)
{
	current_interval_info_t *info = current_interval_info();
	if(info != NULL) {
		if(check_can_add_dep(info, bound) == INTERVAL_OK) {	
			task = Block_copy(task);
			point_add_count(bound, ONE_REF_AND_WAIT_COUNT);    // from end point
			point_t *end = point(bound, NULL, TO_COUNT(2, 2)); // refs held by: user, start.  Waiting on start, task.
			point_t *start = point(end, task, TO_COUNT(2, 1)); // refs held by: user, scheduler.  Waiting on scheduler.
			return (interval_t) { .start = start, .end = end };
		}
	}
	return (interval_t) { .start = NULL, .end = NULL };
}

interval_t interval_f(point_t *bound, task_func_t task, void *userdata)
{
	return interval(bound, ^(interval_t inter) {
		task(inter, userdata);
	});
}

interval_err_t subinterval(interval_block_t task)
{
	current_interval_info_t *info = current_interval_info();
	if(info == NULL)
		return INTERVAL_NO_ROOT;
	
	point_add_count(info->end, ONE_REF_AND_WAIT_COUNT);    // from end point
	point_t *end = point(info->end, NULL, TO_COUNT(1, 2)); // refs held by: start.  Waiting on start, task.
	point_t *start = point(end, task, TO_COUNT(1, 1));     // refs held by: scheduler.  Waiting on scheduler.
	interval_add_hb_unchecked(info->start, start, false);
	
	// Create a "notify" point which, once 'end' has occurred, will
	// signal the semaphore 'signal'.
	dispatch_semaphore_t signal = dispatch_semaphore_create(0);	
	point_t *notify = point(info->end, ^(interval_t inter) {
		dispatch_semaphore_signal(signal);
	}, TO_COUNT(1, 1));	// 1 ref from end and 1 wait from end.  Note: no ref from us!
	insert_edge(&end->out_edges, epoint(notify, false));
	
	// Execute the user's code then wait until 'end' has occurred
	// (and, hence, executed notify).
	execute_interval(start);
	dispatch_semaphore_wait(signal, DISPATCH_TIME_FOREVER); // Signal is sent from sync_func()	
	dispatch_release(signal);
	return INTERVAL_OK;
}

interval_err_t subinterval_f(task_func_t task, void *userdata)
{
	return subinterval(^(interval_t inter) {
		task(inter, userdata);
	});
}

#pragma mark Scheduling Intervals
static void interval_add_hb_unchecked(point_t *before, point_t *after, bool synthetic) {
	uint64_t before_counts;

	// XXX
	//
	// This is completely correct, but annoying.  In the common case
	// that either before or after is unscheduled, this is less
	// efficient than it needs to be!  
	
	atomic_add(&after->counts, ONE_REF_AND_WAIT_COUNT);
	
	point_lock(before);
	insert_edge(&before->out_edges, epoint(after, synthetic));
	before_counts = before->counts;
	point_unlock(before);

	if(WAIT_COUNT(before_counts) == WC_STARTED)
		arrive(after, ONE_WAIT_COUNT);
}

interval_err_t interval_add_hb(point_t *before, point_t *after) {
	current_interval_info_t *info = current_interval_info();
	if(info == NULL)
		return INTERVAL_NO_ROOT;
	
	interval_err_t err;
	if((err = check_can_add_dep(info, after)) != INTERVAL_OK)
		return err;
	
	interval_add_hb_unchecked(before, after, false);
	return INTERVAL_OK;
}

interval_err_t interval_lock(interval_t interval, guard_t *guard) {
	current_interval_info_t *info = current_interval_info();
	if(info == NULL)
		return INTERVAL_NO_ROOT;
	
	interval_err_t err;
	if((err = check_can_add_dep(info, interval.start)) != INTERVAL_OK)
	   return err;
	
	point_t *pnt = atomic_xchg(&guard->last_lock, interval.end);
	if(pnt != NULL)
		interval_add_hb_unchecked(pnt, interval.start, true);
	return INTERVAL_OK;
}

interval_err_t interval_schedule() {
	current_interval_info_t *info = current_interval_info();
	if(info == NULL)
		return INTERVAL_NO_ROOT;

	arrive_edge(info->unscheduled_starts, ONE_WAIT_COUNT);
	free_edges(info->unscheduled_starts);
	info->unscheduled_starts = NULL;
	
	return INTERVAL_OK;
}

#pragma mark Walking the Point Graph

typedef struct point_walk_entry_t point_walk_entry_t;
typedef struct point_walk_t point_walk_t;

struct point_walk_entry_t {
	point_t *pnt;
	point_walk_entry_t *next_hash;
	point_walk_entry_t *next_queue;
};

struct point_walk_t {
	point_walk_entry_t **hash;
	unsigned hash_cnt;
	intptr_t hash_mask;
	
	point_walk_entry_t *queue_first;
	point_walk_entry_t *queue_last;
};

static unsigned point_walk_hash_index(point_walk_t *pnt_walk, point_t *pnt) {
	return (unsigned)((((intptr_t)pnt) >> 2) & pnt_walk->hash_mask);
}

static void point_walk_init(point_walk_t *pnt_walk) {
	const unsigned size = 1024; // must be a power of 2	
	const unsigned bytes = size * sizeof(point_walk_entry_t*);
	pnt_walk->hash = (point_walk_entry_t**) malloc(bytes);
	pnt_walk->hash_cnt = size;
	pnt_walk->hash_mask = ~(size+1);
	
	memset(pnt_walk->hash, 0, bytes);
	pnt_walk->queue_first = pnt_walk->queue_last = NULL;
}

// If 'pnt' is not yet visited, adds it to the list of visited
// nodes and to the end of the queue.
static void point_walk_enqueue(point_walk_t *pnt_walk, point_t *pnt) {
	unsigned pnt_idx = point_walk_hash_index(pnt_walk, pnt);
	for(point_walk_entry_t *entry = pnt_walk->hash[pnt_idx]; entry != NULL; entry = entry->next_hash)
	{
		if(entry->pnt == pnt)
			return;
	}
	
	point_walk_entry_t *entry = (point_walk_entry_t*)malloc(sizeof(point_walk_entry_t));
	entry->pnt = pnt;
	entry->next_hash = pnt_walk->hash[pnt_idx];
	entry->next_queue = NULL;
	
	// TODO-- consider rehashing?
	pnt_walk->hash[pnt_idx] = entry;
	
	if(pnt_walk->queue_last) {
		pnt_walk->queue_last->next_queue = entry;
	} else {
		pnt_walk->queue_first = pnt_walk->queue_last = entry;
	}
}

static point_t *point_walk_dequeue(point_walk_t *pnt_walk) {
	point_walk_entry_t *first = pnt_walk->queue_first;
	
	if(first != NULL) {
		point_t *pnt = first->pnt;
		
		point_walk_entry_t *next = first->next_queue;
		pnt_walk->queue_first = next;
		if(next == NULL)
			pnt_walk->queue_last = NULL;
		
		return pnt;
	}
	
	return NULL;
}

static void point_walk_free(point_walk_t *pnt_walk) {
	for(unsigned i = 0; i < pnt_walk->hash_cnt; i++) {
		point_walk_entry_t *entry = pnt_walk->hash[i]; 
		while(entry != NULL){
			point_walk_entry_t *to_free = entry;
			entry = entry->next_hash;
			free(to_free);
		}
	}
	free(pnt_walk->hash);
}

static bool enqueue_neighbors(point_walk_t *walk, point_t *p, point_t *target) {
	for(edge_t *edge = p->out_edges; edge != NULL; edge = edge->next) {
		for(int i = 0; i < EDGE_CHUNK_SIZE; i++) {
			epoint_t epnt = edge->to_points[i];
			if(epnt == NULL_EPOINT)
				return false;
			
			point_t *pnt = point_of_epoint(epnt);			
			if(pnt == target)
				return true;			
			point_walk_enqueue(walk, pnt);
		}
	}
	return false;
}

static bool point_hb_internal(point_t *before, point_t *after, bool only_deterministic) {
	if(before == after)
		return false;
	if(before->bound == after)
		return true;
	
	point_walk_t walk;
	
	// Breadth-first search:
	
	point_walk_init(&walk);
	point_walk_enqueue(&walk, before);
	
	bool result = false;
	point_t *p;	
	while((p = point_walk_dequeue(&walk)) != NULL) {
		if(enqueue_neighbors(&walk, p, after)) {
			result = true;
			break;
		}
	}
	
	point_walk_free(&walk);
	return result;
}

#pragma mark Querying Intervals and Points

bool point_hb(point_t *before, point_t *after) {
	return point_hb_internal(before, after, true);
}

bool point_bounded_by(point_t *pnt, point_t *bnd) {
	while(pnt->depth > bnd->depth)
		pnt = pnt->bound;
	return (pnt == bnd);
}

#pragma mark Creating Guards
guard_t *guard() {
	guard_t *guard = (guard_t*)malloc(sizeof(guard_t));
	guard->ref_count = 1;
	guard->last_lock = NULL;
	return guard;
}	

#pragma mark Memory Management
point_t *point_retain(point_t *point) {
	if(point)
		atomic_add(&point->counts, ONE_REF_COUNT);
	return point;
}

guard_t *guard_retain(guard_t *guard) {
	if(guard)
		atomic_add(&guard->ref_count, 1);
	return guard;
}

interval_t interval_retain(interval_t interval) {
	point_retain(interval.start);
	point_retain(interval.end);
	return interval;
}
void point_release(point_t *point) {
	if(point) {
		uint64_t c = atomic_sub(&point->counts, ONE_REF_COUNT);
		if(REF_COUNT(c) == 0) {
			free_edges(point->out_edges);
			point_release(point->bound);
			if(point->task)
				Block_release(point->task);
			free(point);
		}
	}
}
void guard_release(guard_t *guard) {
	int count = atomic_sub(&guard->ref_count, 1);
	if(count == 0) {
		free(guard);
	}
}

void interval_release(interval_t interval) {
	point_release(interval.start);
	point_release(interval.end);
}

