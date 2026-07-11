/**
 * @file proc_threads_debug.h
 * @brief Thread Debugging Facility
 * 
 * Declares debugging infrastructure for kernel threading subsystem.
 * Provides configurable logging levels for thread operations:
 *  - State transitions
 *  - Scheduling decisions
 *  - Synchronization events
 *  - Priority changes
 * 
 * Supports both kernel-space and file-based logging targets with
 * severity-based filtering.
 * 
 * Author: Medour Mehdi
 * Date: June 2025
 * Version: 1.0
 */

#ifndef PROC_THREADS_DEBUG_H
#define PROC_THREADS_DEBUG_H

#include "debug.h"

/* Thread debugging levels */
#define THREAD_DEBUG_NONE     0  /* No thread debugging */
#define THREAD_DEBUG_NORMAL   1  /* Normal thread operations */
#define THREAD_DEBUG_VERBOSE  2  /* Verbose thread operations */

/* Current thread debug level - change this to adjust verbosity */
#define THREAD_DEBUG_LEVEL THREAD_DEBUG_NONE

#ifdef DEBUG_THREAD
/* Function declaration for thread logging */
extern void debug_to_file(const char *filename, const char *fmt, ...);

#define DEBUG_THREAD_MUTEX 1

/* Only log if current level is >= required level */
#define TRACE_THREAD_LEVEL(level, fmt, ...) \
    if (level <= THREAD_DEBUG_LEVEL) { \
        debug_to_file("c:\\thread.log", fmt, ##__VA_ARGS__); \
    }

/* Backward compatibility */
#define TRACE_THREAD(fmt, ...) TRACE_THREAD_LEVEL(THREAD_DEBUG_NORMAL, fmt, ##__VA_ARGS__)

#define TRACE_THREAD_VERBOSE(fmt, ...) TRACE_THREAD_LEVEL(THREAD_DEBUG_VERBOSE, fmt, ##__VA_ARGS__)

#else
/* No-op versions when debugging is disabled */
#define TRACE_THREAD_LEVEL(level, fmt, ...)
#define TRACE_THREAD(fmt, ...)
#define TRACE_THREAD_VERBOSE(fmt, ...)
#endif

#endif /* PROC_THREADS_DEBUG_H */