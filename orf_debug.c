/*
 * orf_debug.c
 *
 * Debug configuration implementation for optimized_row_format extension
 */

#include "postgres.h"
#include "utils/guc.h"
#include "orf_debug.h"

/* Global debug configuration variables */
int orf_debug_level = ORF_DEBUG_OFF;
bool orf_debug_scan = false;
bool orf_debug_dml = false;
bool orf_debug_utils = false;

/*
 * Initialize GUC variables for debug configuration
 */
void
orf_debug_init_guc(void)
{
    /* Debug level configuration */
    DefineCustomIntVariable("optimized_row_format.debug_level",
                           "Sets the debug level for optimized_row_format extension (0=off, 1=error, 2=info, 3=verbose)",
                           NULL,
                           &orf_debug_level,
                           ORF_DEBUG_OFF,
                           ORF_DEBUG_OFF,
                           ORF_DEBUG_VERBOSE,
                           PGC_USERSET,
                           0,
                           NULL,
                           NULL,
                           NULL);

    /* Category-specific debug flags */
    DefineCustomBoolVariable("optimized_row_format.debug_scan",
                            "Enable debug logging for scan operations",
                            NULL,
                            &orf_debug_scan,
                            false,
                            PGC_USERSET,
                            0,
                            NULL,
                            NULL,
                            NULL);

    DefineCustomBoolVariable("optimized_row_format.debug_dml",
                            "Enable debug logging for DML operations (INSERT/UPDATE/DELETE)",
                            NULL,
                            &orf_debug_dml,
                            false,
                            PGC_USERSET,
                            0,
                            NULL,
                            NULL,
                            NULL);

    DefineCustomBoolVariable("optimized_row_format.debug_utils",
                            "Enable debug logging for utility functions (attribute extraction, etc.)",
                            NULL,
                            &orf_debug_utils,
                            false,
                            PGC_USERSET,
                            0,
                            NULL,
                            NULL,
                            NULL);
}
