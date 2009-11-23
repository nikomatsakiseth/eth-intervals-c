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
	
	interval_release(zero);
	interval_release(one);
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
	
	interval_release(zero);
	interval_release(one);
}

void after2_check() {
	ASSERT(after2_stamps[0] == after2_stamps[1] + 1);
}

#pragma mark main()

int main() {
	root_interval(^(point_t *rootEnd) {
		subinterval_f(basic_test, NULL);
		
		subinterval_f(after_test, NULL);
		after_check();
		
		subinterval_f(after2_test, NULL);
		after2_check();
	});	
}
