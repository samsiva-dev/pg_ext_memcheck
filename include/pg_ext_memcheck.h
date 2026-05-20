/*-------------------------------------------------------------------------
 * pg_ext_memcheck
 *
 * Copyright (c) 2026, Samba Siva Reddy
 *
 * This software is released under the MIT License.
 * See LICENSE for details.
 *-------------------------------------------------------------------------
*/
#ifndef PG_EXT_MEMCHECK_H
#define PG_EXT_MEMCHECK_H 

#include "violation_log.h"

// Shared Ring Buffer to store violation logs
extern ViolationLog *violation_log;

/*
 * Set to true when executing a internal query (e.g., SPI queries from within the extension) 
 * to prevent recursive self-monitoring of internal queries.
 */
extern bool memcheck_in_internal_query;

#endif /* PG_EXT_MEMCHECK_H */