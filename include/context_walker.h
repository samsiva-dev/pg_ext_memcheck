/*-------------------------------------------------------------------------
 * pg_ext_memcheck
 *
 * Copyright (c) 2026, Samba Siva Reddy
 *
 * This software is released under the MIT License.
 * See LICENSE for details.
 *-------------------------------------------------------------------------
*/
#ifndef CONTEXT_WALKER_H
#define CONTEXT_WALKER_H

#include "postgres.h"
#include "pg_config_manual.h"
#include "c.h"
#include "postgres_ext.h"

// Context Snapshot Structure   
typedef struct CtxSnapshot {
    char   name[NAMEDATALEN];
    Size   totalAllocated;
    Size   totalFree;
    int      depth;         /* depth in tree */
    uint32   parentHash;    /* hash of parent name+depth for diff */
} CtxSnapshot;

// Context Tree Structure
typedef struct CtxTree {
    CtxSnapshot *entries;
    int          count;
    int          capacity;
} CtxTree;

// Context Diff Structure
typedef struct CtxDiff {
    char   name[NAMEDATALEN];
    Size   beforeAllocated;
    Size   afterAllocated;
    Size   beforeFree;
    Size   afterFree;
    int    depth;           /* depth in tree */
} CtxDiff;

extern CtxTree* snapshot_context_tree(MemoryContext root);
extern CtxDiff* diff_context_trees(CtxTree *before, CtxTree *after, int *diff_count);
extern void free_context_tree(CtxTree *tree);
extern void free_context_diff(CtxDiff *diff);
extern uint32 ctx_compute_hash(const char *name, int depth);

#endif /* CONTEXT_WALKER_H */