/*
 *  test.c
 *  Intervals
 *
 *  Created by Niko Matsakis on 9/21/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#include <stdlib.h>
#include <limits.h>
#include "interval.h"
#include <CoreServices/CoreServices.h>
#include <unistd.h>

void ASSERT(int cond) {
	if(!cond)
		Debugger();
}

int stamp() {
	static int value = 1;
	return __sync_fetch_and_add(&value, 1);
}

void stamp_task(point_t *_, void *result) {
	*(int*)result = stamp();
}

#pragma mark basic_test()

void basic_test(point_t *end, void *_) {
}

#pragma mark after_test()

static int after_stamps[2];

void after_test(point_t *currentEnd, void *_) {
	after_stamps[0] = after_stamps[1] = INT_MAX;	
	interval_t zero = interval_f(currentEnd, stamp_task, after_stamps + 0);	
	interval_t one = interval_f(currentEnd, stamp_task, after_stamps + 1);
	interval_add_hb(zero.end, one.start);	
}

void after_check() {
	ASSERT(after_stamps[1] == after_stamps[0] + 1);
}

#pragma mark after_test2()

static int after2_stamps[2];

void after2_test(point_t *parentEnd, void *_) {
	interval_t zero = interval_f(parentEnd, stamp_task, after2_stamps + 0);
	interval_t one = interval_f(parentEnd, stamp_task, after2_stamps + 1);
	interval_add_hb(one.end, zero.start);
}

void after2_check() {
	ASSERT(after2_stamps[0] == after2_stamps[1] + 1);
}

#pragma mark ordered locks

// Creates two ordered intervals a -> b.  Then
// asks them both to acquire a lock on guard g,
// but asks b first.  If b greedily acquired g,
// the result would be a cycle.

static int ordered_lock_stamps[2];

void ordered_lock_test(point_t *parentEnd, void *_) {
	interval_t a = interval_f(parentEnd, stamp_task, ordered_lock_stamps + 0);
	interval_t b = interval_f(parentEnd, stamp_task, ordered_lock_stamps + 1);
	guard_t *g = guard();
	interval_add_hb(a.end, b.start);
	interval_add_lock(b.start, g);
	interval_add_lock(a.start, g);
	guard_release(g);
}

void ordered_lock_check() {
	ASSERT(ordered_lock_stamps[0] + 1 == ordered_lock_stamps[1]);
}

#pragma mark check locks

// Creates two pairs of intervals.  The first pair
// is totally unordered and the second pair acquire
// a lock on the same guard g.  Checks that the first
// pair overlaps and the second runs during
// disjoint times.  To make it certain that they
// would overlap, each task sleeps for 1 second.

static int check_lock_times[8];

void check_lock_task(point_t *end, void *_times) {
	int *times = (int*)_times;
	times[0] = stamp();
	sleep(1);
	times[1] = stamp();
}

void check_lock_test(point_t *_1, void *_2) {
	subinterval(^(point_t *end) {
		interval_f(end, check_lock_task, check_lock_times + 0);
		interval_f(end, check_lock_task, check_lock_times + 2);
	});
	
	subinterval(^(point_t *end) {
		interval_t a = interval_f(end, check_lock_task, check_lock_times + 4);
		interval_t b = interval_f(end, check_lock_task, check_lock_times + 6);
		guard_t *g = guard();
		interval_add_lock(a.start, g);
		interval_add_lock(b.start, g);
		guard_release(g);
	});
}

void check_lock_check() {
	ASSERT(check_lock_times[0] < check_lock_times[1]);
	ASSERT(check_lock_times[2] < check_lock_times[3]);
	ASSERT(check_lock_times[4] < check_lock_times[5]);
	ASSERT(check_lock_times[6] < check_lock_times[7]);	

	// First pair overlapped:
	ASSERT(check_lock_times[2] < check_lock_times[1]
		   || check_lock_times[1] < check_lock_times[3]);
	
	// Second pair did not:
	ASSERT(check_lock_times[5] < check_lock_times[6] // first ended before second started
		   || check_lock_times[7] < check_lock_times[4]); // second ended before first started
}

#pragma mark main()

int main() {
	root_interval(^(point_t *rootEnd) {
		subinterval_f(basic_test, NULL);
		
		subinterval_f(after_test, NULL);
		after_check();
		
		subinterval_f(after2_test, NULL);
		after2_check();
		
		subinterval_f(ordered_lock_test, NULL);
		ordered_lock_check();
		
		subinterval_f(check_lock_test, NULL);
		check_lock_check();
	});	
}
