/*-------------------------------------------------------------------------
 * pg_ext_memcheck
 *
 * Copyright (c) 2026, Samba Siva Reddy
 *
 * This software is released under the MIT License.
 * See LICENSE for details.
 *-------------------------------------------------------------------------
*/

#include "postgres.h"
#include "fmgr.h"

void _PG_init(void);
void _PG_fini(void);

PG_MODULE_MAGIC;

// Extension load callback
void
_PG_init(void)
{
    elog(INFO, "pg_ext_memcheck loaded");
}

// Extension unload callback
void
_PG_fini(void)
{
    elog(INFO, "pg_ext_memcheck unloaded");
}