/*
 *  Intervals Library
 *
 *  Copyright 2009 Nicholas D. Matsakis.
 *
 *  This library is open source and distributed under the GPLv3 license.  
 *  Please see the file LICENSE for distribution and licensing details.
 */

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
