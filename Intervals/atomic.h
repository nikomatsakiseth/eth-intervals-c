/*
 *  Intervals Library
 *
 *  Copyright 2009 Nicholas D. Matsakis.
 *
 *  This library is open source and distributed under the GPLv3 license.  
 *  Please see the file LICENSE for distribution and licensing details.
 */

//
// Internal header file defining atomic operations in a 
// platform portable way.
//

#define ALIGNED(n) __attribute__((aligned(n)))
#define atomic_xchg_int __sync_lock_test_and_set
#define atomic_cmpxchg __sync_bool_compare_and_swap
#define atomic_add(v, a) __sync_add_and_fetch(v, a)
#define atomic_sub(v, a) __sync_sub_and_fetch(v, a)
#define atomic_rel_lock(v) __sync_lock_release(v)
#define memory_barrier() __sync_synchronize()

#ifdef APPLE
#  define thread_yield pthread_yield_np
#else
#  define thread_yield sched_yield
#endif

#ifdef __APPLE__
#  include <libkern/OSAtomic.h>
#  define atomic_xchg_ptr __sync_lock_test_and_set
#else

typedef int OSSpinLock;

static inline void OSSpinLockLock(OSSpinLock *lock)
{ // quick hack
  do {
    while(*lock == 1)
      ;
  } while(__sync_lock_test_and_set(lock, 1));
}

static inline void OSSpinLockUnlock(OSSpinLock *lock)
{
  __sync_lock_release(lock);
}

// Like atomic_xchg_int, writes 'src' into 'dest' and returns
// old value of 'dest'.
static inline void *atomic_xchg_ptr(void *dest, void *src)
{
  intptr_t isrc = (intptr_t)src;
  return (void*)atomic_xchg_int((void**)dest, isrc);
}

#endif

