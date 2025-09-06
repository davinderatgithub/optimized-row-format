#include "postgres.h"

/* Required for most Postgres development */
#include "postgres.h"
#include "fmgr.h"
#include "access/tableam.h"
#include "commands/defrem.h"
#include "nodes/makefuncs.h"
#include "utils/rel.h"
#include "catalog/namespace.h"

#include "optimized_row_format.h"
#include "orf_debug.h"
#include "orf_functions.h"


PG_MODULE_MAGIC;

/*
 * Custom logging for optimized row format extension
 * This allows us to control logging independently of PostgreSQL's general debug level
 * DISABLED for testing - uncomment to enable debugging
 */
// #define OPTIMIZED_LOG(fmt, ...) \
//     elog(NOTICE, "OPTIMIZED_DEBUG: " fmt, ##__VA_ARGS__)
#define OPTIMIZED_LOG(fmt, ...) do { } while (0)


/* Forward declarations */
static TableAmRoutine optimized_tableam;

/* Table access method handler */
static bool optimized_tableam_initialized = false;

/* Table AM handler function */
PG_FUNCTION_INFO_V1(optimized_row_format_tableam_handler);
Datum
optimized_row_format_tableam_handler(PG_FUNCTION_ARGS)
{
	if (!optimized_tableam_initialized)
	{
		const TableAmRoutine *heap_am = GetHeapamTableAmRoutine();

		/*
		 * Copy the entire heap AM routine and then override the functions
		 * we want to implement. This is more robust than initializing
		 * everything manually.
		 */
		memcpy(&optimized_tableam, heap_am, sizeof(TableAmRoutine));

		/* Override with our custom functions */
		optimized_tableam.type = T_TableAmRoutine;
		optimized_tableam.scan_begin = optimized_scan_begin;
		optimized_tableam.scan_end = optimized_scan_end;
		optimized_tableam.scan_rescan = optimized_scan_rescan;
		optimized_tableam.scan_getnextslot = optimized_scan_getnextslot;
		optimized_tableam.parallelscan_estimate = optimized_parallelscan_estimate;
		optimized_tableam.tuple_insert = optimized_tuple_insert;
        optimized_tableam.tuple_delete = optimized_tuple_delete;
        optimized_tableam.tuple_update = optimized_tuple_update;
		optimized_tableam.relation_needs_toast_table = optimized_relation_needs_toast_table;
		// optimized_tableam.slot_callbacks = optimized_slot_callbacks; // Temporarily disabled for debugging

		/* Index scan functions */
		optimized_tableam.index_fetch_begin = optimized_index_fetch_begin;
		optimized_tableam.index_fetch_reset = optimized_index_fetch_reset;
		optimized_tableam.index_fetch_end = optimized_index_fetch_end;
		optimized_tableam.index_fetch_tuple = optimized_index_fetch_tuple;

		optimized_tableam_initialized = true;
	}

    PG_RETURN_POINTER(&optimized_tableam);
}