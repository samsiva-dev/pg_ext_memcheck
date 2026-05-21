/*-------------------------------------------------------------------------
 * pg_ext_memcheck
 *
 * Copyright (c) 2026, Samba Siva Reddy
 *
 * This software is released under the MIT License.
 * See LICENSE for details.
 *-------------------------------------------------------------------------
*/
#ifndef GUCS_H
#define GUCS_H

#include "enums.h"

/* 
    GUCs (Grand Unified Configuration) definitions,
    including custom configuration variables for the extension.
*/

extern void DefineCustomGUCs(void);

// internal GUC variables


// external GUC variables
// integer GUCs
extern int memcheck_min_leak_bytes; // minimum growth (bytes) to log as INFO
extern int memcheck_bloat_min_bytes; // minimum growth (bytes) to log as WARNING

// GUC variable for memory checking mode
extern MemCheckMode memcheck_mode; 


// boolean GUCs

// string GUCs

#endif /* GUCS_H */