/*
 *  Intervals Library
 *
 *  Copyright 2009 Nicholas D. Matsakis.
 *
 *  This library is open source and distributed under the GPLv3 license.  
 *  Please see the file LICENSE for distribution and licensing details.
 */

/*
 
 Memory Management Design
 
 Knowing when it is safe to free a point is rather tricky.  There is
 a combination of two factors that must be considered -- first, the
 scheduler, and second, user references.  In addition, the scheme
 should be efficient and permit points to be freed as soon as possible.
 
 The general idea of the scheme is that points go through a simple
 state progression: UNSCHEDULED, SCHEDULED, OCCURRED, FREED.
 
 The UNSCHEDULED state is when the point is first created.  Once
 the interval_schedule() method is invoked, the point then becomes
 SCHEDULED.  Once all predecessors in the interval graph OCCURRED,
 the point becomes OCCURRED.  Once all references are released, the
 point is FREED.
 
 Before the OCCURRED state, the point P holds a "wait count" (WC) on 
 each of its successors S.  In the OCCURRED state, this WC
 is transformed into a "ref count" (RC), which potentially causes the
 successor S to enter the OCCURRED state.  In the FREED state, 
 the "ref count" is released (potentially freeing S).  
 
 The transition from WC to RC that occurs when
 entering the OCCURRED state is a bit subtle.  Furthermore, for 
 optimization purposes, when possible we skip the OCCURRED state 
 altogether.
 
 Before the OCCURRED state, adjustments to the RC simply
 affect the point locally.  The point does not hold a RC on its
 successors -- it doesn't need to, because it holds a WC.
 
 When a point P is about to occur, the arrive() method looks at P's RC:  
 * If P's RC is 1, then the scheduler holds the only ref. on P.
   In that case, we follow a streamlined path: the wait count of
   all successors S of P is decremented, but their RC is unaffected.
   This may cause some of S to arrive.  
 * If P's RC is >1, then the scheduler must 
   increment the RC of each successor before 
   decrementing its WC.
 Once the RC/WC of all successors have been adjusted, the scheduler
 releases its ref on P.  
 
 One side effect of this is that the correct effect of adding an edge
 P->Q to the graph varies depending on the state of P and Q.
 First, we note that Q must be in an UNSCHEDULED or SCHEDULED state.
 Similarly, P must either:
 * be in the OCCURRED state and the user holds a ref.  In this case,
   we increment the RC of Q.  This increment is never propagated
   to Q's successors because Q cannot yet be in the OCCURRED state.
 * be in the UNSCHEDULED or SCHEDULED state.  In this case, we
   increment the WC of Q, but not its RC.
 */

/*
 Some Notes on the Design:
 
 (1) Memory management.  Each point has both a ref and a wait count. 
 The point is freed when its ref count reaches zero.  The point
 "occurs" when its wait count reaches zero.  Because the point cannot
 be freed until it has occurred, the ref count should always be
 >= the wait count.  Generally, anyone for whom the point is waiting
 should hold a reference to the point.  In addition, there is always
 one ref count for the "scheduler": this reference is released when
 the point "occurs" and its task is executed.
 
 (2) The above scheme implies that creating a new edge from one point
 to another always requires that the ref count of the target point
 be incremented.  The wait count need only be incremented if the source
 point has not yet occurred.
 
 (3) Note that intervals are returned to the user with a "temporary" reference
 held by the scheduler.  If the user wishes to retain a reference after
 the interval is scheduled, they must use interval_retain.
 */

#include <stdlib.h>
#include <stdarg.h>
#include <assert.h>
#include <limits.h>
#include <pthread.h>
#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <Block.h>

#include "interval.h"
#include "thread_pool.h"

#include "atomic.h"
#include "internal.h"

#ifndef NDEBUG
static uint64_t live_points; // tracks number of live intervals when debugging
#endif

#pragma mark Data Types and Simple Accessors

#define WC_STARTED UINT64_MAX  // 
#define EDGE_CHUNK_SIZE 3
#define NULL_EPOINT ((epoint_t)0)

typedef int epoch_t;
typedef intptr_t epoint_t;
typedef intptr_t interval_task_t;

typedef struct edge_t {
	epoint_t to_points[EDGE_CHUNK_SIZE];
	struct edge_t *next;
} edge_t;

struct guard_t {
	int ref_count;      // Always modified atomically.
	point_t *last_lock; // Always exchanged atomically.
};

struct point_t {
	/// Bound of this point.  Immutable.  Only \c NULL for end of root.
	point_t *bound;
	
	/// Points are structured into a tree based on their bounds.
	/// This depth is the depth in the tree.
	int depth;
	
	/// Task to execute when scheduled (if any).  See \c task().
	interval_task_t task;
	
	/// Lock used when performing guarded operations.
	OSSpinLock lock;
	
	/// List of outgoing edges.  Guarded by \c lock.
	edge_t *out_edges;
	
	/// Wait count: number of events which must occur
	/// before we can execute.  The point cannot be freed
	/// until this reaches zero.  Manipulated using
	/// atomic instructions, not locks!
	uint64_t wait_count;
	
	/// Ref count: number of times points has been retained
	/// but not released.  Note that a point \c P in the
	/// interval graph do \em not retain its successors
	/// until \c P's \c ref_count become's 1.  Manipulated using
	/// atomic instructions, not locks!
	uint64_t ref_count;
};

/// Information about the currently executed interval.
typedef struct current_interval_info_t current_interval_info_t;
struct current_interval_info_t {
	current_interval_info_t *next; ///< points to the previous current_interval_info
	point_t *start;                ///< a point which \em happens before current task, may be \c NULL
	point_t *end;                  ///< bound of current task
	edge_t *unscheduled_starts;    ///< list of start points created but not yet scheduled
};

#pragma mark Manipulating Interval Tasks

#define EMPTY_TASK            0
#define TASK_LATCH_TAG        1
#define TASK_BLOCK_TAG        2
#define TASK_COPIED_BLOCK_TAG 3
#define TASK_TAG              3

static interval_task_t task(void *ptr, int tag) {
	intptr_t task = (intptr_t)ptr;
	task |= tag;
	return task;
}

// Dispatches the task for 'pnt' once 'pnt' arrives.  
// Responsible for releasing the scheduler's reference on 'pnt'
// once the task fully completes.
static void task_dispatch(point_t *pnt, interval_task_t task) {
	if(task != EMPTY_TASK) {
		int tag = (task & TASK_TAG);
		intptr_t ptr = (task & ~TASK_TAG);
		
		if(tag == TASK_LATCH_TAG) {
			interval_pool_latch_t *latch = (interval_pool_latch_t*)ptr;
			interval_pool_signal_latch(latch);
			point_release(pnt);
		} else if(tag == TASK_BLOCK_TAG || tag == TASK_COPIED_BLOCK_TAG) {
			// execute_interval will invoke task_execute (below) which 
			// will release the ref.
			interval_pool_enqueue(pnt);
		}		
	} else {
		point_release(pnt);
	}		
}

// Invoked from execute_interval when a point's task is executed.
// Note that this method is only called when the interval's task
// is a block.
static void task_execute(point_t *pnt, interval_task_t task, point_t *arg) {
	assert(task != EMPTY_TASK);
	int tag = (task & TASK_TAG);
	intptr_t ptr = (task & ~TASK_TAG);
	
	assert(tag == TASK_BLOCK_TAG || tag == TASK_COPIED_BLOCK_TAG);
	interval_block_t blk = (interval_block_t)ptr;
	blk(arg);
	if(tag == TASK_COPIED_BLOCK_TAG)
		Block_release(blk);
	point_release(pnt);
}

#pragma mark Manipulating Edge Lists

static void arrive(point_t *interval, uint64_t sub_waits, uint64_t add_refs);

// Encodes in one pointer the interval and side to which an edge points.
// If the synthetic bit is true, then this edge was not directly specified
// by the user but rather resulted from locking a guard or some other such
// feature.
static inline epoint_t epoint(point_t *point, bool synthetic) {
	epoint_t epnt = (epoint_t)point;
	epnt |= synthetic;
	return epnt;
}

/// Extract point from \c pnt.
static inline point_t *point_of_epoint(epoint_t pnt) {
	intptr_t i = pnt & ~0x3;
	return (point_t*)i;
}

/// Extract synthetic flag from \c pnt.
static inline bool is_synthetic_epoint(epoint_t pnt) {
	return (pnt & 0x1) != 0;
}

/// Adds \c epnt to \c *list, possibly overwriting
/// \c *list if allocation is necessary.  Generally executed
/// while holding an appropriate lock on the owner of \c *list.
static inline void insert_edge(edge_t **list, epoint_t epnt) {
	edge_t *cur_edge = *list;
	if(cur_edge != NULL) {
		for(int i = 1; i < EDGE_CHUNK_SIZE; i++) {
			if(cur_edge->to_points[i] == NULL_EPOINT) {
				cur_edge->to_points[i] = epnt;
				return;
			}
		}
	}
	
	edge_t *new_edge;
	*list = new_edge = (edge_t*)malloc(sizeof(edge_t));
	new_edge->to_points[0] = epnt;
	for(int i = 1; i < EDGE_CHUNK_SIZE; i++)
		new_edge->to_points[i] = NULL_EPOINT;
	new_edge->next = cur_edge;
}

/// Invokes \c arrive() on all points referenced by
/// the list \c edge.
static void arrive_edge(edge_t *edge, uint64_t sub_waits, uint64_t add_refs) {
	if(edge) {
		for(int i = 0; i < EDGE_CHUNK_SIZE; i++) {
			epoint_t epnt = edge->to_points[i];
			if(epnt == NULL_EPOINT)
				break;
			arrive(point_of_epoint(epnt), sub_waits, add_refs);
		}
		
		arrive_edge(edge->next, sub_waits, add_refs);
	}
}

/// Frees the list edge, optionally releasing references
/// on the points contained within.
static void free_edges(edge_t *edge, ///< List to free.
					   bool release) ///< If true, release the points.
{
	if(edge) {
		for(int i = 0; i < EDGE_CHUNK_SIZE; i++) {
			epoint_t epnt = edge->to_points[i];
			if(epnt == NULL_EPOINT)
				break;
			if(release)
				point_release(point_of_epoint(epnt));
		}
		
		free(edge);
		free_edges(edge->next, release);		
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
	info->unscheduled_starts = NULL;
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
				break;
			
			point_t *pnt = point_of_epoint(epnt);
			if(pnt == tar || pnt->bound == tar)
				return true;
		}
	}
	return false;
}

#pragma mark Manipulating Points

static point_t *point(point_t *bound, interval_task_t task, uint64_t wc, uint64_t rc)
{
	point_t *result = (point_t*)malloc(sizeof(point_t));
	result->bound = bound;
	result->depth = (bound ? bound->depth + 1 : 0);
	result->task = task;
	result->lock = 0;
	result->out_edges = NULL;
	result->wait_count = wc;
	result->ref_count = rc;
	debugf("%p = point(%p, wc=%llx, rc=%llx)", result, bound, wc, rc);
	
#   ifndef NDEBUG
	atomic_add(&live_points, 1);
#   endif
	
	return result;
}

static inline bool point_occurred(point_t *point) {
	return point->wait_count == WC_STARTED;
}

static inline void point_add_wait_count(point_t *point, uint64_t count) {
#   ifndef NDEBUG
	uint64_t new_count = 
#   endif
	atomic_add(&point->wait_count, count);
	debugf("%p point_add_wait_count(%llx) new_count=%llx", point, count, new_count);
}

static inline void point_lock(point_t *point) {
	OSSpinLockLock(&point->lock);
}

static inline void point_unlock(point_t *point) {
	OSSpinLockUnlock(&point->lock);
}

#pragma mark Tracking Wait Counts

static void interval_schedule_unchecked(current_interval_info_t *info);

void interval_execute(point_t *start) {
	point_t *end = start->bound;
	
	current_interval_info_t info;
	push_current_interval_info(&info, start, start->bound);
	
	task_execute(start, start->task, end);
	// Note: start may be freed by task_execute!
	
	interval_schedule_unchecked(&info);	
	arrive(end, 1, 1);
	pop_current_interval_info(&info);
}

// Decrements the count(s) of 'interval.side' by 'count'.  Note that
// count must not result in the point's ref. count becoming 0! This
// should never happen in any case, as one ref. belongs to the scheduler,
// and it is only released in this function once the wait count becomes 0.
static void arrive(point_t *point, uint64_t sub_waits, uint64_t add_refs) 
{
	assert(sub_waits);
	
	// Adjust Ref Count:
	uint64_t ref_count;
	if(add_refs) 
		ref_count = atomic_add(&point->ref_count, add_refs);
	else
		ref_count = point->ref_count;

	// Adjust Wait Count:
	uint64_t wait_count = atomic_sub(&point->wait_count, sub_waits);
	
	debugf("%p arrive(%llx) new_count=%llx", point, sub_waits, new_count);	
	if(wait_count == 0) {
		edge_t notify;
		notify.next = NULL;
		notify.to_points[0] = NULL_EPOINT;
		
		// We must add to counts[side] atomically in case of other, simultaneous
		// threads adjusting the ref count.  Note that we copy into notify by
		// value.  This is because, once we released the lock but before we
		// invoke arrive_edge(), other threads might add themselves into the
		// out_edges array.  They would then be over-notified of our termination.
		point_lock(point);
		point->wait_count = WC_STARTED;
		if(point->out_edges) notify = *point->out_edges;		
		point_unlock(point);
		
		// Notify those coming after us and dispatch task (if any)
		uint64_t add_refs;
		if(ref_count == 1) {
			// We hold the only ref on point.  Don't incr. RC of
			// point's successors, instead just remove them.  We don't
			// need a lock because no one else could be legally 
			// inspecting point->out_edges without a ref.
			add_refs = 0;
			free_edges(point->out_edges, false);
			point->out_edges = NULL;
		} else {
			// Someone else holds a ref on point.  
			add_refs = 1;
		}
		arrive_edge(&notify, 1, add_refs);
		if(point->bound) arrive(point->bound, 1, add_refs);
		task_dispatch(point, point->task);
	}
}

#pragma mark Safety Checks

static void interval_add_hb_unchecked(point_t *before, point_t *after, bool synthetic);

static interval_err_t check_no_cycle(current_interval_info_t *info, point_t *from_pnt, point_t *to_pnt) 
{
#   ifdef INTERVAL_SAFETY_CHECKS_ENABLED
	if(point_hb(to_pnt, from_pnt))
		return INTERVAL_CYCLE;
#   endif
	return INTERVAL_OK;
}

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

static interval_err_t check_can_add_hb(current_interval_info_t *info, point_t *from_pnt, point_t *to_pnt)
{
#   ifdef INTERVAL_SAFETY_CHECKS_ENABLED
	interval_err_t result;
	if((result = check_can_add_dep(info, to_pnt)) != INTERVAL_OK)
		return result;
	if((result = check_no_cycle(info, from_pnt, to_pnt)) != INTERVAL_OK)
		return result;
#   endif
	return INTERVAL_OK;
}


#pragma mark Creating Intervals 

void root_interval(interval_block_t blk)
{
	init_current_interval_key();
	assert(current_interval_info() == NULL);
	
	interval_pool_run(0, ^() {
		
		// Create root end point and configure it to signal when done:
		interval_pool_latch_t latch;
		interval_pool_init_latch(&latch);
		
		interval_task_t signalTask = task(&latch, TASK_LATCH_TAG);	
		point_t *root_end = point(NULL, signalTask, 1, 1); // Wait: Us. Ref: Scheduler.
		debugf("%p = root_end", root_end);
		
		// Start root block executing:
		current_interval_info_t root_info;	
		push_current_interval_info(&root_info, NULL, root_end);
		blk(root_end);	
		interval_schedule_unchecked(&root_info);
		arrive(root_end, 1, 0);	
		pop_current_interval_info(&root_info);
		
		// Wait until root_end occurs (it may already have done so):
		//    Note that the pool will shutdown when this block returns.
		interval_pool_wait_latch(&latch);
		
		assert(live_points == 0);		
	});
}

point_t *default_bound_unchecked(current_interval_info_t *info) 
{
	point_t *bound = info->end->bound;
	if(bound == NULL)
		return info->end;
	return bound;
}

point_t *default_bound()
{
	current_interval_info_t *info = current_interval_info();
	if(info) {
		return default_bound_unchecked(info);
	}
	return NULL;
}

interval_t interval(point_t *bound, interval_block_t blk)
{
	current_interval_info_t *info = current_interval_info();
	if(info != NULL) {
		if(bound == NULL)
			bound = default_bound_unchecked(info);
				
		point_t *currentStart = info->start;
		
		interval_err_t err;
		if(currentStart == NULL)
			err = check_can_add_dep(info, bound);
		else
			err = check_can_add_hb(info, currentStart, bound);

		if(err == INTERVAL_OK) {			
			interval_task_t startTask = task(Block_copy(blk), TASK_COPIED_BLOCK_TAG);

			// Refs on the start point: task, unscheduled list, and optionally currentStart
			int startRefs = 2;
			if(currentStart != NULL)
				startRefs++;
			
			point_add_wait_count(bound, 1);                        // from end point
			point_t *end = point(bound, EMPTY_TASK, 2, 1);         // Wait: start, task. Ref: scheduler.
			point_t *start = point(end, startTask, 1, startRefs);  // Wait: scheduler. Ref: (See above).
			
			if(currentStart) {				
				point_lock(currentStart);
				insert_edge(&currentStart->out_edges, epoint(start, false));
				point_unlock(currentStart);
			}
			
			debugf("%p-%p: interval(%p) from %p-%p", start, end, bound, currentStart, info->end);
			
			insert_edge(&info->unscheduled_starts, epoint(start, false));
			
			return (interval_t) { .start = start, .end = end };
		}
	}
	return (interval_t) { .start = NULL, .end = NULL };
}

interval_t interval_f(point_t *bound, task_func_t task, void *userdata)
{
	return interval(bound, ^(point_t *end) {
		task(end, userdata);
	});
}

interval_err_t subinterval(interval_block_t blk)
{
	current_interval_info_t *info = current_interval_info();
	if(info == NULL)
		return INTERVAL_NO_ROOT;
	
	// Create tasks:
	//    start will run blk (no need to copy, we're still on the stack)
	//    end will signal 'signal' when its occurred
	interval_pool_latch_t latch;
	interval_pool_init_latch(&latch);
	interval_task_t signalTask = task(&latch, TASK_LATCH_TAG);
	interval_task_t blkTask = task(blk, TASK_BLOCK_TAG);
	
	// Create points:
	//    Note that start occurs immediately.  Used in dyn. race det. to adjust bound, 
	//    but maybe we could get rid of it.
	point_add_wait_count(info->end, 1);                  // from end point
	point_t *end = point(info->end, signalTask, 1, 2);   // wait: task. refs: start, task.
	point_t *start = point(end, blkTask, WC_STARTED, 1); // refs: task.
	if(info->start)
		interval_add_hb_unchecked(info->start, start, false);
	
	debugf("%p-%p: subinterval of %p-%p", start, end, info->start, info->end);
	
	// Execute the user's code then wait until 'end' has occurred
	// (and, hence, executed notify).
	interval_execute(start);
	interval_pool_wait_latch(&latch);
	return INTERVAL_OK;
}

interval_err_t subinterval_f(task_func_t task, void *userdata)
{
	return subinterval(^(point_t *end) {
		task(end, userdata);
	});
}

#pragma mark Scheduling Intervals
static void interval_add_hb_unchecked(point_t *before, point_t *after, bool synthetic) {
	debugf("%p->%p", before, after);	
	
	// Note: we have to adjust 'after->counts' while holding 
	// before lock to ensure that 'before' doesn't occur
	// after we have added the edge but before we have
	// incremented 'after->counts' (that could cause 'after' to
	// occur when it shouldn't).  This means we hold the
	// lock a little longer than we otherwise might which
	// doesn't please me.  Alternatives:
	//
	// (0) Restrict API to require that either 'before' or 'after'
	//     is unscheduled (AND make changes in (3)).
	//
	// (1) Always pre-increment by ONE_REF_AND_WAIT_COUNT
	//     but later do a 'fix-up' if before already occurred.
	//
	// (2) If 'before' is unscheduled, we know it cannot
	//     yet have occurred.  Unfortunately, we don't know
	//     (and wouldn't normally need to know) whether 'before'
	//     is unscheduled or not.
	//
	// (3) If 'after' is unscheduled (common case), we could 
	//     (a) Initialize unscheduled intervals wait count to INT_MAX/2
	//     (b) Track desired wait count for unscheduled intervals in some side array
	//     (c) When we schedule them, adjust appropriately
	//     Since we already have to check whether 'after' is an unscheduled
	//     interval, it wouldn't add overhead to check though it would
	//     require some code restructuring.
	
	// Safety checks should ensure that after has not
	// yet occurred:
	assert(!point_occurred(after));
	
	point_lock(before);
	
	// If before has not yet started, after must wait for it.
	// Otherwise, after need not wait, but when before is freed
	// it will release a ref on after.  If before has 
	// occurred, it can only still be live if user holds a ref on it.
	if(!point_occurred(before))
		point_add_wait_count(after, 1);
	else 
		point_retain(after);
	
	insert_edge(&before->out_edges, epoint(after, synthetic));	
	
	point_unlock(before);
}

interval_err_t interval_add_hb(point_t *before, point_t *after) {
	if(before == NULL || after == NULL)
		return INTERVAL_OK;
	
	current_interval_info_t *info = current_interval_info();
	if(info == NULL)
		return INTERVAL_NO_ROOT;

	interval_err_t err;
	if((err = check_can_add_hb(info, before, after)) != INTERVAL_OK)
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
	
	point_t *pnt = atomic_xchg_ptr(&guard->last_lock, interval.end);
	if(pnt != NULL)
		interval_add_hb_unchecked(pnt, interval.start, true);
	return INTERVAL_OK;
}

static void interval_schedule_unchecked(current_interval_info_t *info)
{
	if(info->unscheduled_starts) {
		arrive_edge(info->unscheduled_starts, 1, 0);
		free_edges(info->unscheduled_starts, false);
		info->unscheduled_starts = NULL;	
	}	
}

interval_err_t interval_schedule() {
	current_interval_info_t *info = current_interval_info();
	if(info == NULL)
		return INTERVAL_NO_ROOT;

	interval_schedule_unchecked(info);
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
	pnt_walk->hash_mask = size - 1;
	
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
				break;
			
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

point_t *interval_bound(interval_t interval) {
	if(interval.end)
		return interval.end->bound;
	return NULL;
}

point_t *point_bound(point_t *point) {
	if(point)
		return point->bound;
	return NULL;
}

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
		atomic_add(&point->ref_count, 1);
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
		uint64_t rc = atomic_sub(&point->ref_count, 1);
		if(rc == 0) {
			assert(WAIT_COUNT(c) == WC_STARTED);
			debugf("%p: freed", point);
			free_edges(point->out_edges, true);
			point_release(point->bound);
			// Note: point->task is released by task_execute
			free(point);
			
#   ifndef NDEBUG
			atomic_sub(&live_points, 1);
#   endif
		} else {
			debugf("%p: point_release c=%llx", point, c);
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

// Define down here so as not to include dispatch
// and create accidental dependencies:
#if defined(__APPLE__) && !defined(NDEBUG)
#include <dispatch/dispatch.h>

static dispatch_once_t init_debug;
static dispatch_queue_t debug_queue;

void interval_debugf(const char *fmt, ...) {
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
	
	//dispatch_async(debug_queue, ^{
		fprintf(stderr, "%s\n", res);
		free(res);
	//});
}
#endif
