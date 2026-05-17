/*
    * gucs.c
    *
    * This file contains the implementation of the GUCs (Grand Unified Configuration)
    * for the pg_ext_memcheck extension. GUCs are used to define configuration parameters
    * that can be set by the user to control the behavior of the extension.
    *
*/

// PostgreSQL Includes
#include "postgres.h"
#include "fmgr.h"
#include "utils/guc.h"
#include <limits.h>

// Local Includes
#include "include/gucs.h"

// GUC variable definitions
MemCheckMode memcheck_mode      = MEMCHECK_ALL;
int          memcheck_min_leak_bytes = 8192; /* 8 KiB default */

static const struct config_enum_entry memcheck_mode_options[] = {
    {"all",      MEMCHECK_ALL,      false},
    {"executor", MEMCHECK_EXECUTOR, false},
    {"none",     MEMCHECK_NONE,     false},
    {NULL, 0, false}
};

// Function to define custom GUCs for the extension
void 
DefineCustomGUCs(void)
{
    // Define GUC for memory checking mode
    DefineCustomEnumVariable("pg_ext_memcheck.memcheck_mode",
                             "Sets the memory checking mode for the pg_ext_memcheck extension.",
                             "Available modes: MEMCHECK_ALL, MEMCHECK_EXECUTOR, MEMCHECK_NONE.",
                             (int *) &memcheck_mode,
                             MEMCHECK_ALL,           // default value
                             memcheck_mode_options,  // enum options
                             PGC_USERSET,            // context
                             0,            // flags
                             NULL,         // check hook
                             NULL,         // assign hook
                             NULL);        // show hook

    // Minimum allocation growth (in bytes) required to log an INFO violation.
    // Contexts that grow by less than this amount are silently ignored.
    DefineCustomIntVariable("pg_ext_memcheck.min_leak_bytes",
                            "Minimum allocation growth in bytes to log as an INFO violation.",
                            "Contexts growing by less than this value are silently skipped. Default is 8192 (8 KiB).",
                            &memcheck_min_leak_bytes,
                            8192,       /* default: 8 KiB */
                            0,          /* min */
                            INT_MAX,    /* max */
                            PGC_USERSET,
                            GUC_UNIT_BYTE,
                            NULL, NULL, NULL);
}