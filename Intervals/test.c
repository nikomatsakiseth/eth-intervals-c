/*
 *  test.c
 *  Intervals
 *
 *  Created by Niko Matsakis on 9/21/09.
 *  Copyright 2009 __MyCompanyName__. All rights reserved.
 *
 */

#include "test.h"
#include <stdlib.h>
#include <limits.h>
#include "interval.h"
#include <CoreServices/CoreServices.h>

void ASSERT(int cond) {
	if(!cond)
		Debugger();
}

int stamp() {
	static int value = 1;
	return __sync_fetch_and_add(&value, 1);
}

void stamp_task(interval_t *parent, void *result) {
	*(int*)result = stamp();
}

#pragma mark basic_test()

void basic_test(interval_t *parent, void *_) {
}

#pragma mark after_test()

static int after_stamps[2];

void after_test(interval_t *parent, void *_) {
	after_stamps[0] = after_stamps[1] = INT_MAX;
	interval_t *zero = create_async_interval(parent, stamp_task, after_stamps + 0, INTERVAL_NO_DEPS);
	create_async_interval(parent, stamp_task, after_stamps + 1, INTERVAL_DEPS(
		DEP_START_AFTER_END_OF(zero)
	));
}

void after_check() {
	ASSERT(after_stamps[1] == after_stamps[0] + 1);
}

#pragma mark after_test2()

static int after2_stamps[2];

void after_2_test_task(interval_t *after, void *_parent) {
	interval_t *parent = _parent;
	
	interval_t *one = create_async_interval(parent, stamp_task, after_stamps + 1, INTERVAL_DEPS(
		DEP_START_AFTER_END_OF(after)
	));
									  
	create_async_interval(parent, stamp_task, after_stamps + 0, INTERVAL_DEPS(
		DEP_END_BEFORE_START_OF(one)
	));
}

void after2_test(interval_t *parent, void *_) {
	create_async_interval(parent, after_2_test_task, parent, INTERVAL_NO_DEPS);
}

void after2_check() {
	ASSERT(after2_stamps[1] == after2_stamps[0] + 1);
}

#pragma mark main()

int main() {
	sync_interval(basic_test, NULL, INTERVAL_NO_DEPS);
	
	sync_interval(after_test, NULL, INTERVAL_NO_DEPS);
	after_check();
}
