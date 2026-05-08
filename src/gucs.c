/*-------------------------------------------------------------------------
 * pg_ext_memcheck
 *
 * Copyright (c) 2026, Samba Siva Reddy
 *
 * This software is released under the MIT License.
 * See LICENSE for details.
 *-------------------------------------------------------------------------
*/

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

// Local Includes
#include "include/gucs.h"

// GUC variable definition
MemCheckMode memcheck_mode = MEMCHECK_ALL;

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
                             &memcheck_mode,
                             MEMCHECK_ALL,           // default value
                             memcheck_mode_options,  // enum options
                             PGC_USERSET,            // context
                             0,            // flags
                             NULL,         // check hook
                             NULL,         // assign hook
                             NULL);        // show hook

    // Define other GUCs as needed (e.g., boolean and string GUCs)
}