/*
 *  Intervals Library
 *
 *  Copyright 2009 Nicholas D. Matsakis.
 *
 *  This library is open source and distributed under the GPLv3 license.  
 *  Please see the file LICENSE for distribution and licensing details.
 */

#include <assert.h>

// Stub which executes the point's task and then releases the
// point.  It also signals the end point that it has arrived.
// This is invoked both by the thread pool and (for subinterval's)
// directly by the interval code.  It should only be invoked 
// when start_point's task is a block task.
void interval_execute(point_t *start_point);

#ifndef NDEBUG
#  ifdef APPLE
void interval_debugf(const char *fmt, ...);
#    define debugf(...) interval_debugf(__VA_ARGS__)
#  else
#    define debugf(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while(0)
#  endif
#else
#  define debugf(...)
#endif

#pragma mark Tagged Pointers

typedef intptr_t tagged_ptr_t;
#define ALL_TAG_BITS 0x3

static inline tagged_ptr_t tagged_ptr(void *ptr, int tag) {
	assert((tag & ALL_TAG_BITS) == tag);
	tagged_ptr_t tagptr = (tagged_ptr_t)ptr;
	tagptr |= tag;
	return tagptr;
}

static inline int extract_tag(tagged_ptr_t tagptr) {
	return (tagptr & ALL_TAG_BITS);
}

static inline void *extract_ptr(tagged_ptr_t tagptr) {
	return (void*)(tagptr & ~ALL_TAG_BITS);
}

#pragma mark Simple Stacks 

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