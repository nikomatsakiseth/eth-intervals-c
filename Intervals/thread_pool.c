/*
 *  Intervals Library
 *
 *  Copyright 2009 Nicholas D. Matsakis.
 *
 *  This library is open source and distributed under the GPLv3 license.  
 *  Please see the file LICENSE for distribution and licensing details.
 */

#ifndef INTERVALS_USE_LIB_DISPATCH

#include <pthread.h>
#include <stdlib.h>
#include <limits.h>
#include <string.h>
#include "thread_pool.h"
#include "internal.h"
#include "atomic.h"

//#define PROFILE

#pragma mark Miscellaneous Configuration

#include <unistd.h>
static long number_of_processors() {
	return sysconf(_SC_NPROCESSORS_CONF);
}

#pragma mark Simple Temporary Stacks

typedef struct llstack_t llstack_t;
struct llstack_t {
	void *value;
	llstack_t *next;
};

static void llstack_push(llstack_t **stack, void *value) {
	llstack_t *link = (llstack_t*)malloc(sizeof(llstack_t));
	link->value = value;
	link->next = *stack;
	*stack = link;
}

static void *llstack_pop(llstack_t **stack) {
	if(*stack == NULL)
		return NULL;
	llstack_t *link = *stack;
	void *value = link->value;
	*stack = link->next;
	free(link);
	return value;
}

static void llstack_free(llstack_t **stack) {
	while(*stack)
		llstack_pop(stack);
}

#pragma mark Data Types and Constants

#define DEQUE_PAD 0
#define DEQUE_OFFSET 0

typedef struct interval_pool_t interval_pool_t;
typedef struct interval_worker_t interval_worker_t;
typedef struct point_t interval_work_item_t;
typedef struct deque_t deque_t;

typedef unsigned deque_index_t;
#define DEQUE_INDEX_MAX UINT_MAX
struct deque_t {	
	interval_work_item_t **work_items; // Logical ength is always a power of two.
	deque_index_t work_items_mask;     // = logical length of array - 1.
	deque_index_t owner_head;          // May lag behind thief_head.
	deque_index_t owner_tail;          // Logical index; always correct.
	deque_index_t thief_head;
	OSSpinLock lock;                   // Should probably not be a spin-lock!
};

struct interval_worker_t {
	pthread_t thread;
	
	interval_pool_t *pool;
	
	// Per-worker lock.  Held under the following conditions:
	// (1) When stealing.
	// (2) When going idle (but not while idle, in that 
	//     case we are waiting on cond, see below).
	// This lock cannot be held while acquiring the pool lock.
	pthread_mutex_t lock;

	// We block on this condition when idle.  This allows
	// us to be re-awoken.
	pthread_cond_t cond;

	deque_t deque;
	
	// Links for the list of all workers.  It is
	// permitted to walk the "all workers" list in the
	// forward direction without locks, but editing
	// should occur under pool->lock.
	interval_worker_t *prev_all, *next_all;
	
	// Links for the list of idle workers.  Manipulated
	// and walked under pool->lock.
	interval_worker_t *prev_idle, *next_idle;
	
	// Links for the list of removed workers where worker
	// structures go before being freed.
	interval_worker_t *next_removed;
};

struct interval_pool_t {
	// lock for the pool.  Held while manipulating pool-level structures.
	pthread_mutex_t lock;
	
	// condition: the 'worker thread removal' thread blocks on this condition.
	// when enough removed workers accumulate, he walks and frees them.
	// before doing so he acquires the lock on all active workers.
	pthread_cond_t cond;
	
	// normally false until shutdown is initiated.
	bool shutdown;
	
	pthread_t cleanup_thread;
	
	interval_worker_t *first_worker;
	interval_worker_t *first_idle_worker;
	interval_worker_t *first_removed_worker;
};

#pragma mark Deque Operations

static inline size_t deque_index(deque_index_t mask, deque_index_t pos) 
{
	return (size_t)(((pos & mask) << DEQUE_PAD) + DEQUE_OFFSET);
}

static inline size_t deque_alloc_size(deque_index_t logical_size)
{
	return (size_t)((logical_size << DEQUE_PAD) + DEQUE_OFFSET);
}

static inline deque_index_t deque_mask(deque_index_t logical_len)
{
	return logical_len - 1;
}

static inline deque_index_t deque_logical_len(deque_index_t mask)
{
	return mask + 1;
}

static void deque_init(deque_t *deque)
{
	const deque_index_t llen = (1 << 10);
	size_t size = deque_alloc_size(llen);
	deque->work_items = (interval_work_item_t**)calloc(size, sizeof(interval_work_item_t*));
	memset(deque->work_items, 0, size * sizeof(interval_work_item_t*));
	deque->work_items_mask = deque_mask(llen);
	deque->owner_head = deque->owner_tail = deque->thief_head = 0;
	deque->lock = 0;
}

static void deque_free(deque_t *deque)
{
	free(deque->work_items);
}

static void deque_copy_array(deque_t *deque, deque_index_t new_llen)
{	
	interval_work_item_t **old_work_items = deque->work_items;
	deque_index_t old_mask = deque->work_items_mask;
	
	size_t new_size = deque_alloc_size(new_llen);
	deque_index_t new_mask = deque_mask(new_llen);
	interval_work_item_t **new_work_items = (interval_work_item_t**)calloc(new_size, sizeof(interval_work_item_t*));
	for(deque_index_t i = deque->owner_head, c = deque->owner_tail; i < c; i++) {
		interval_work_item_t *item = old_work_items[deque_index(old_mask, i)];
		new_work_items[deque_index(new_mask, i)] = item;
	}
	
	free(old_work_items);
	deque->work_items = new_work_items;
	deque->work_items_mask = new_mask;
	deque->owner_tail -= deque->owner_head;
	deque->owner_head = deque->thief_head = 0;
}

static void deque_expand(deque_t *deque) 
{
	OSSpinLockLock(&deque->lock);
	
	deque->owner_head = deque->thief_head;
	
	deque_index_t len = deque_logical_len(deque->work_items_mask);
	deque_index_t thold = len >> 4;
	deque_index_t size = deque->owner_tail - deque->owner_head;
	deque_index_t avail = len - size;
	if(avail <= thold) { 
		// Less than 1/16 is available
		//
		//   Expand the array.
		deque_copy_array(deque, len * 2);
	} else if(deque->owner_tail == DEQUE_INDEX_MAX) {
		// Would roll over
		//
		//   All we need to do here is compact the
		//   array, but it's non-trivial to do so
		//   without overwriting things, so we just
		//   reallocate it.
		deque_copy_array(deque, len);
	}
	
	OSSpinLockUnlock(&deque->lock);
}

static void deque_owner_put(deque_t *deque, interval_work_item_t *work_item)
{
	for(;;) {
		deque_index_t mask = deque->work_items_mask;
		deque_index_t len = mask + 1;
		deque_index_t head = deque->owner_head;
		deque_index_t tail = deque->owner_tail;
		
		if(tail - head >= len || tail == DEQUE_INDEX_MAX) {
			deque_expand(deque); // full or would roll-over
			continue; 
		}
		
		unsigned index = deque_index(mask, tail);
		
		// ensure that all writes are published before we make the pointer work_item available to others
		OSMemoryBarrier(); 
		
		deque->work_items[index] = work_item;
		deque->owner_tail = tail + 1;
		return;
	} 
}

static interval_work_item_t *deque_owner_take(deque_t *deque)
{
	deque_index_t mask = deque->work_items_mask;
	deque_index_t head = deque->owner_head;
	deque_index_t tail = deque->owner_tail;
	
	if(head == tail)
		return NULL; // empty
	
	deque_index_t last_tail = tail - 1;
	unsigned last_index = deque_index(mask, last_tail);
	
	interval_work_item_t *result = atomic_xchg(&deque->work_items[last_index], NULL);

	// If we got back NULL, then it was stolen.  Update our view
	// of the head.
	if(result == NULL) {
		deque->owner_head = deque->owner_tail;
		return NULL;
	}
			
	// Otherwise, just update the tail.
	deque->owner_tail = last_tail;
	return result;
}

static interval_work_item_t *deque_steal(deque_t *deque)
{
	OSSpinLockLock(&deque->lock);	
	deque_index_t mask = deque->work_items_mask;
	deque_index_t head = deque->thief_head;
	unsigned index = deque_index(mask, head);
	
	interval_work_item_t *result = atomic_xchg(&deque->work_items[index], NULL);
	
	if(result != NULL)
		deque->thief_head = head + 1;	
	OSSpinLockUnlock(&deque->lock);
	
	return result;
}

#pragma mark Tracking the Current Interval Worker

static pthread_once_t worker_key_init = PTHREAD_ONCE_INIT;
static pthread_key_t worker_key;

static void init_worker_key_helper() 
{
	pthread_key_create(&worker_key, NULL);
}

static void init_worker_key() 
{
	pthread_once(&worker_key_init, init_worker_key_helper);
}

static interval_worker_t *current_worker() 
{ // Must have invoked init_current_interval_key() first
	return (interval_worker_t*)pthread_getspecific(worker_key);
}

static void set_current_worker(interval_worker_t *worker) 
{ // Must have invoked init_current_interval_key() first
	pthread_setspecific(worker_key, worker);
}

#pragma mark Interval Worker

static void worker_add_to_all_worker_list(interval_worker_t *worker)
{
	interval_pool_t *pool = worker->pool;

	pthread_mutex_lock(&pool->lock);
	interval_worker_t *first = pool->first_worker;
	
	// Note
	//
	// Write barriers are needed to handle the case where
	// people are simultaneously walking the 'all worker'
	// list without locking the pool.  We guarantee that
	// if you start walking *forward* at some worker X,
	// and can guarantee that X will not be removed,
	// you will always encouter worker X again.
	//
	// In the case of the empty list, someone who reads 
	// first_worker and finds worker will see that worker
	// is properly initialized.
	//
	// In the caes of a non-empty list, someone who reads
	// first_worker and gets 'first' can start iterating.
	// If they make it all around the list without seeing
	// any of our writes, that's fine.  If they see 'worker'
	// when reading last->next_all, they will also see
	// that worker->next_all == first.  Similarly, if they
	// read pool->first_worker and get 'worker', they will 
	// see that last->next_all also points to worker.
	//
	// If x86 requires read barriers, we would insert them
	// before reading next_all.
	
	if(first == NULL) {
		// List is currently empty.  Insert.
		worker->next_all = worker->prev_all = worker;
	} else {
		// It is possible that first == last.
		interval_worker_t *last = first->prev_all;
		worker->next_all = first;
		worker->prev_all = last;
		OSMemoryBarrier();
		last->next_all = worker;
		first->prev_all = worker;
	}
	
	OSMemoryBarrier();
	pool->first_worker = worker;
	
	pthread_mutex_unlock(&pool->lock);	
}

static void worker_remove_from_all_worker_list(interval_worker_t *worker)
{
	interval_pool_t *pool = worker->pool;
	
	// Some guarantees:
	//
	// (1) No one is simultaneously walking the list starting from 'worker'.
	
	pthread_mutex_lock(&pool->lock);
	
	interval_worker_t *next = worker->next_all;
	interval_worker_t *prev = worker->prev_all;
	
	if(next == worker) { // list is of size 1.
		pool->first_worker = NULL;		
	} else {             // list of size > 1.
		prev->next_all = next;
		next->prev_all = prev;		
	}
	
	// Problem: there could still be simultaneous readers currently traversing
	// the list and looking at worker.  We cannot modify worker's pointers 
	// not free it until they have all finished.  For now, we toss the pointer onto
	// a list of removed workers.
	worker->next_removed = pool->first_removed_worker;
	pool->first_removed_worker = worker;
	
	pthread_mutex_unlock(&pool->lock);
	
	// Signal the cleanup thread to come along and delete the worker.
	// Note: as deleting workers is kinda' expensive, we could do this
	// less often if we wanted.
	pthread_cond_signal(&pool->cond);
	
}

static interval_worker_t *worker_create(interval_pool_t *pool) 
{
	interval_worker_t *worker = (interval_worker_t*)malloc(sizeof(interval_worker_t));	
	memset(worker, 0, sizeof(interval_worker_t));	
	worker->pool = pool;
	pthread_mutex_init(&worker->lock, NULL);
	pthread_cond_init(&worker->cond, NULL);
	deque_init(&worker->deque);
	
	worker_add_to_all_worker_list(worker);
	
	return worker;
}

static void worker_free(interval_worker_t *worker)
{
	deque_free(&worker->deque);
	pthread_cond_destroy(&worker->cond);
	pthread_mutex_destroy(&worker->lock);
	free(worker);
}

static void worker_main(interval_worker_t *worker)
{
	
}

static interval_work_item_t *worker_steak_work(interval_worker_t *worker)
{
	interval_work_item_t *stolen_item = NULL;
	
	pthread_mutex_lock(&worker->lock);
	
	for(interval_worker_t *victim = worker->next_all; 
		victim != worker; 
		victim = victim->next_all)
	{
		if((stolen_item = deque_steal(&victim->deque)) != NULL) {
			debugf("stole %p from %p", stolen_item, victim->thread);
			break;
		}
	}	
	
	pthread_mutex_unlock(&worker->lock);
	
	return stolen_item;
}

static inline void worker_enqueue(interval_worker_t *worker, interval_work_item_t *work_item)
{
	debugf("enqueued %p", work_item);
	deque_owner_put(&worker->deque, work_item);
}

static bool worker_do_work(interval_worker_t *worker, int steal_attempts)
{
	interval_work_item_t *work_item;
	
	if((work_item = deque_owner_take(&worker->deque)) != NULL) {
		debugf("took %p", work_item);
		goto found_work;
	}

	while(steal_attempts--) {
		if((work_item = worker_steak_work(worker)) != NULL)
			goto found_work;
		pthread_yield_np();
	}
	
	return false;
	
found_work:
	interval_execute(work_item);
	return true;
}

// Pthread start point for a new worker thread:
static void *worker_thread(void *_worker)
{
	interval_worker_t *worker = (interval_worker_t*)_worker;
	interval_pool_t *pool = worker->pool;
	set_current_worker(worker);
	
	while(!pool->shutdown) {
		worker_do_work(worker, 22);
	}
	
	return NULL;
}

static inline void worker_start(interval_worker_t *worker)
{
	pthread_create(&worker->thread, NULL, worker_thread, worker);
}

static inline void worker_join(interval_worker_t *worker)
{
	pthread_join(worker->thread, NULL);
}

// Called when this worker is already doing something,
// but he's idling for a signal and wants to do some work.
// Returns true if it managed to do a little work.
static bool worker_recurse(interval_worker_t *worker)
{
	debugf("worker_recurse: %p", worker);
	return worker_do_work(worker, 3);
}

#pragma mark Interval Pool

void interval_pool_iter_all_workers(interval_pool_t *pool, void (^blk)(interval_worker_t *worker)) {
	if(pool->first_worker) {
		blk(pool->first_worker);
		for(interval_worker_t *worker = pool->first_worker->next_all;
			worker != pool->first_worker;
			worker = worker->next_all)
		{
			blk(worker);
		}
	}
}

static void interval_pool_free_removed_workers(interval_worker_t *removed) 
{
	if(removed) {
		interval_pool_free_removed_workers(removed->next_removed);
		worker_free(removed);
	}
}

static void *interval_pool_clean_thread(void *_pool)
{
	interval_pool_t *pool = (interval_pool_t*)_pool;

	while(!pool->shutdown) {
		pthread_mutex_lock(&pool->lock);
		
		interval_worker_t *removed_workers = pool->first_removed_worker;
		if(removed_workers != NULL) {
			
			// Acquire locks on all worker threads to prevent
			// them from simultaneously stealing!
			interval_pool_iter_all_workers(pool, ^(interval_worker_t *worker) {
				pthread_mutex_lock(&worker->lock);
			});

			interval_pool_free_removed_workers(removed_workers);
			
			interval_pool_iter_all_workers(pool, ^(interval_worker_t *worker) {
				pthread_mutex_unlock(&worker->lock);
			});
			
		} else {
			pthread_cond_wait(&pool->cond, &pool->lock);
		}
		
		pthread_mutex_unlock(&pool->lock);
	}
	
	return NULL;
}


static interval_pool_t *interval_pool_create()
{
	interval_pool_t *pool = (interval_pool_t*)malloc(sizeof(interval_pool_t));
	memset(pool, 0, sizeof(interval_pool_t));
	pthread_mutex_init(&pool->lock, NULL);
	pthread_cond_init(&pool->cond, NULL);
	
	pthread_create(&pool->cleanup_thread, NULL, interval_pool_clean_thread, pool);
	return pool;
}

void interval_pool_destroy(interval_pool_t *pool)
{
	// First bring all workers (and cleanup thread) into
	// an inactive state.
	pthread_mutex_lock(&pool->lock);
	pool->shutdown = true;	
	pthread_mutex_unlock(&pool->lock);
	pthread_cond_broadcast(&pool->cond);

	pthread_t self = pthread_self();
	interval_pool_iter_all_workers(pool, ^(interval_worker_t *worker) {
		if(!pthread_equal(self, worker->thread))		
			worker_join(worker);
	});
	
	interval_pool_iter_all_workers(pool, ^(interval_worker_t *worker) {
		worker_free(worker);
	});

	pthread_join(pool->cleanup_thread, NULL);
	pthread_cond_destroy(&pool->cond);
	pthread_mutex_destroy(&pool->lock);	
	free(pool);
}

void interval_pool_run(int workers, void (^blk)(interval_pool_t *pool))
{
	init_worker_key();
	interval_pool_t *pool = interval_pool_create();
	
	// Fire-up workers.  This is non-ideal, as we
	// currently have a fixed number of workers, etc.,
	// but it will do.
	if(!workers)
		workers = number_of_processors();	
	
	// worker 0 is the current process:
	interval_worker_t *worker = worker_create(pool);
	worker->thread = pthread_self();
	set_current_worker(worker);
	
	for(int i = 1; i < workers; i++) {
		interval_worker_t *worker = worker_create(pool);
		worker_start(worker);
	}
	
	blk(pool);
	
	interval_pool_destroy(pool);
}

void interval_pool_enqueue(point_t *start_point)
{
	interval_worker_t *worker = current_worker();
	worker_enqueue(worker, start_point);
}

void interval_pool_init_latch(interval_pool_latch_t *latch)
{
	*latch = 0;
}

void interval_pool_signal_latch(interval_pool_latch_t *latch)
{
	OSMemoryBarrier();
	*latch = 1;
}

void interval_pool_wait_latch(interval_pool_latch_t *latch)
{
	interval_worker_t *worker = current_worker();
	
	/* Read barrier would go here, if needed on x86. */
	while(!*latch)
		worker_recurse(worker);
}

#endif