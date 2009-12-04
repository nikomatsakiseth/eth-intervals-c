/*
 *  Intervals Library
 *
 *  Copyright 2009 Nicholas D. Matsakis.
 *
 *  This library is open source and distributed under the GPLv3 license.  
 *  Please see the file LICENSE for distribution and licensing details.
 */

#ifndef INTERVAL_H
#define INTERVAL_H

#include <stdbool.h>

/**
 
 MEMORY MANAGEMENT CONVENTIONS
 
 All types in the interval library are ref counted, and each
 type (points, guards, etc) has an appropriate 
 retain()/release() function.  The fundamental rule for
 knowing when you need to invoke *_release() is:
 
 /------------------------------------------------\
 | Thou shalt balance every call to create_*() or |
 | *_retain() must with a call to *_release()     |
 \------------------------------------------------/ 
 
 If you get back a pointer from a functon that is not
 named create_*() or *_retain() and you would like to
 keep it around, you should retain it yourself!

 The detailed rules are as follows:
 
 POINTS: Note that no functions which create points or
 intervals begin with "create".  This is because points
 are retained by the system until they occur, and they
 cannot occur until (at minimum) they are scheduled.  
 Therefore, you do not need to retain/release a point
 unless you are storing it somewhere after it is scheduled.
  
 GUARDS: When created, guards have a ref count of 1.  You
 are responsible for releasing this initial reference at
 some point.
 
 **/

#define INTERVAL_SAFETY_CHECKS_ENABLED

#pragma mark Type Definitions

/// A point in the interval graph.
typedef struct point_t point_t;

/// A guard, which is similar to a lock.
typedef struct guard_t guard_t;

/// An interval is just a pair of points.
typedef struct interval_t {
	point_t *start, *end;
} interval_t;

/// An interval task function.  
/// \param end The end-point of the interval executing
///            this task function.
/// \param userdata The user-data provided when the
///                 interval was created.
typedef void (*task_func_t)(point_t *end, void *userdata);

/// An interval task block.  
/// \param end The end-point of the interval executing
///            this block.
typedef void (^interval_block_t)(point_t *end);

/// Error codes that can result from interval functions.
typedef enum interval_err_t {
	INTERVAL_OK,              ///< No error occurred.
	INTERVAL_EDGE_REQUIRED,   ///< A path is required from the end of the current interval to the target of the new dependency.
	INTERVAL_CYCLE,           ///< Executing this command would result in a cycle.
	INTERVAL_NO_ROOT          ///< Command executed from outside of a call to \c root_interval.
} interval_err_t;

#pragma mark Creating Intervals

/// Creates the root interval, executing \c task.  All uses of
/// other interval APIs must occur from within \c task (or within
/// intervals created by \c task).
void root_interval(interval_block_t task); 

/// Creates a parallel interval whose task is \c task.  The interval
/// is bounded by \c bound, which means that the end of the new
/// interval <em>happens before</em> \c bound.  
/// It must be legal to add an incoming HB relation to \c bound; 
/// the conditions for this are describing in \c interval_add_hb().  
///
/// The newly created interval may eventually execute in parallel with
/// the caller of this function.  However, it will not begin execution
/// until the function \c interval_schedule() is called.  Note that
/// \c interval_schedule() is called automatically when the current task
/// completes.  
///
/// Additional dependencies on the returned interval
/// object may be added by calling \c interval_add_hb() before invoking
/// \c interval_schedule().
///
/// The returned \c point_t objects are guaranteed not to be freed until
/// they are scheduled.  If you wish to keep a pointer to those objects
/// for longer than that, you must retain them using \c point_retain()
/// or \c interval_retain().
///
/// \param bound The bound for the new interval.  If NULL is 
///              passed, then the result of \c default_bound() is used.
/// \param task The block which the interval should execute when its
///             start point is scheduled.
///
/// \return The newly created interval.  Returns an interval with two
/// \c NULL points if \c bound violates any safety conditions.
///
/// \see interval_f()
/// \see interval_add_hb()
/// \see interval_schedule()
/// \see interval_retain()
/// \see point_retain()
interval_t interval(point_t *bound, interval_block_t task); 

/// Like \c interval(), \c interval_f() creates a new interval whose task
/// is the function \c task.
///
/// \see interval()
interval_t interval_f(point_t *bound, task_func_t task, void *userdata);

/// Creates a subinterval which executes \c task.  A subinterval differs
/// from a normal interval because it executes in a blocking fashion:
/// that is, a call to this function does not return until the created
/// interval has terminated.
///
/// \return \c INTERVAL_OK if the subinterval executed correctly.
/// \c INTERVAL_NO_ROOT if invoked from outside the root interval.
///
/// \see subinterval_f()
interval_err_t subinterval(interval_block_t task);

/// Like \c subinterval(), but the task is the function \c task.
///
/// \return \c INTERVAL_OK if the subinterval executed correctly.
/// \c INTERVAL_NO_ROOT if invoked from outside the root interval.
///
/// \see subinterval()
interval_err_t subinterval_f(task_func_t task, void *userdata);

#pragma mark Scheduling Intervals

/// Adds a <em>happens before</em> relation between \c before and
/// \c after.  If either \c before or \c after is \c NULL, then
/// this method has no effect.  For this method to be legal, \em one
/// of the following safety conditions must be met:
///   - \c after must be a point on an unscheduled interval.
///   - \c after must be the end of the current interval.
///   - the end of the current interval must <em>happen before</em> \c after.
/// If at least one of these conditions is not met, then the function
/// will return \c INTERVAL_EDGE_REQUIRED.
///
/// In addition, \c after must not <em>happen before</em> \c before,
/// because that would create a dependency cycle.  In this case, the
/// function will return \c INTERVAL_CYCLE.
///
/// \param before The point which is constrained to happen first.
/// \param after The point which is constrained to happen second.
///
/// \return \c INTERVAL_OK if the point was successfully added.  Otherwise,
/// an appropriate error code describing why it could not be added.
interval_err_t interval_add_hb(point_t *before, point_t *after);

/// Indicates that the point \c start should acquire a lock on
/// \c guard before executing.  The lock will be released after
/// the bound of \c start occurs.  Generally, \c start is the
/// start point of an interval, in which case its bound is the
/// interval's end point, and so the lock is held for the entire
/// interval.
/// 
/// If \c guard is \c NULL, this function has no effect.  
///
/// It must be legal to add a dependency to \c start.  These
/// conditions are the same as when adding a new edge with
/// \c start as the target, see \c interval_add_hb() for details.
///
/// Note that the use of locks can cause deadlocks.  Currently
/// deadlocks resulting from the use of locks are not detected.
///
/// \return \c INTERVAL_OK if the lock requirement was correctly
/// specified.
/// \return \c INTERVAL_EDGE_REQUIRED if it is not legal to add
/// a dependency to \c start.
/// \return \c INTERVAL_NO_ROOT if invoked from outside the root interval.
interval_err_t interval_add_lock(point_t *start, guard_t *guard);

/// Indicates that all intervals which have been created by this task
/// using \c interval() or \c interval_f() but not yet scheduled should
/// be made eligible for execution.  If there are no such intervals
/// (i.e., neither \c interval() nor \c interval_f() have been invoked
/// since the previous call to \c interval_schedule()), then this call
/// has no effect.  Intervals which are scheduled as a result of this
/// call may execute in parallel with the caller, and may begin execution
/// at any time (even before the call returns).
///
/// Note that such intervals may
/// still not execute immediately, either because there is no CPU available
/// to run them, or because the incoming dependencies on their start points
/// are not satisfied.  
///
/// Any points returned by \c interval() or \c interval_f()
/// may no longer be used once \c interval_schedule() is invoked unless they
/// have been retained using \c point_retain(), as they may have been freed
/// by the scheduler.
///
/// This function is invoked automatically once the task function or block
/// for an interval returns.
/// 
/// \return \c INTERVAL_OK if no errors occurred.  \c INTERVAL_NO_ROOT
/// if invoked from outside the root interval.
interval_err_t interval_schedule();

#pragma mark Querying Intervals and Points

/// Returns the bound of the end point of \c interval.
point_t *interval_bound(interval_t interval);

/// Returns the bound of the given point.  If \c point is an end point,
/// its bound was specified by the user when it was created.  If \c point
/// is a start point, the bound is the corresponding end point.  If \c point
/// is \c NULL, returns \c NULL.
point_t *point_bound(point_t *point);

/// Returns the default bound for newly created intervals.  This is normally the
/// bound of the current interval, unless the current interval is the root interval,
/// in which case it is the end of the root interval.
point_t *default_bound(); 

/// Returns \c true if \c before <em>happens before</em> \c after.
bool point_hb(point_t *before, point_t *after);

/// Returns true if \c point is bounded by \c bound, either directly or
/// transitively.
bool point_bounded_by(point_t *point, point_t *bound);

#pragma mark Creating Guards

/// Creates and returns a new guard object.  You are
/// responsible for releasing this guard eventually.
guard_t *create_guard();

#pragma mark Memory Management

/// Increases the reference count on \c point and returns \c point.
/// No effect if \c point is \c NULL.  This must be called at least
/// once before newly created points are scheduled if you wish to
/// use them later.
point_t *point_retain(point_t *point);

/// Increases the reference count on \c guard and returns \c guard.
/// No effect if \c guard is \c NULL.
guard_t *guard_retain(guard_t *guard);

/// Increases the reference count on both the start and end point of
/// \c interval.  Returns \c interval.
interval_t interval_retain(interval_t interval);

/// Releases a reference on \c point.  If this was the last reference,
/// then \c point will be freed.
void point_release(point_t *point);

/// Releases a reference on \c guard.  If this was the last reference,
/// then \c guard will be freed.
void guard_release(guard_t *guard);

/// Releases a reference on both the start and end point of \c interval.
void interval_release(interval_t interval);

#endif