/*
 * orf_debug.h
 *
 * Simple debug configuration system for optimized_row_format extension
 * Provides compile-time configurable debug logging
 */

#ifndef ORF_DEBUG_H
#define ORF_DEBUG_H

#include "postgres.h"
#include "utils/elog.h"

/* 
 * Debug configuration flags - set to 1 to enable, 0 to disable
 * These can be easily toggled without code changes
 */
#define ORF_DEBUG_ENABLED 0     /* Master debug switch */
#define ORF_DEBUG_SCAN 0        /* Scan operation debugging */
#define ORF_DEBUG_DML 0         /* DML operation debugging */
#define ORF_DEBUG_UTILS 0       /* Utility function debugging */

/* Debug logging macros */
#if ORF_DEBUG_ENABLED

#define ORF_DEBUG_ERROR(category, fmt, ...) \
    do { \
        if (ORF_DEBUG_##category) { \
            elog(NOTICE, "ORF_DEBUG[" #category "]: " fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define ORF_DEBUG_INFO(category, fmt, ...) \
    do { \
        if (ORF_DEBUG_##category) { \
            elog(NOTICE, "ORF_DEBUG[" #category "]: " fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#define ORF_DEBUG_VERBOSE(category, fmt, ...) \
    do { \
        if (ORF_DEBUG_##category) { \
            elog(NOTICE, "ORF_DEBUG[" #category "]: " fmt, ##__VA_ARGS__); \
        } \
    } while (0)

#else

/* Debug disabled - all macros become no-ops */
#define ORF_DEBUG_ERROR(category, fmt, ...) do { } while (0)
#define ORF_DEBUG_INFO(category, fmt, ...) do { } while (0)
#define ORF_DEBUG_VERBOSE(category, fmt, ...) do { } while (0)

#endif /* ORF_DEBUG_ENABLED */

#endif /* ORF_DEBUG_H */
