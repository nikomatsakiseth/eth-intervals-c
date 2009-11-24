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

#include <libkern/OSAtomic.h>
#define ALIGNED(n) __attribute__((aligned(n)))
#define atomic_xchg __sync_lock_test_and_set
#define atomic_cmpxchg __sync_bool_compare_and_swap
#define atomic_add(v, a) __sync_add_and_fetch(v, a)
#define atomic_sub(v, a) __sync_sub_and_fetch(v, a)
#define atomic_rel_lock(v) __sync_lock_release(v)