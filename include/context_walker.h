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
    char   name[NAMEDATALEN];   /* Name of the context */
    Size   totalAllocated;      /* Total allocated bytes in this context */
    Size   totalFree;           /* Total free bytes in this context */  
    int      depth;             /* Depth of this context in tree */
    uint32   parentHash;        /* Hash of parent name+depth for diff */
} CtxSnapshot;

// Context Tree Structure
typedef struct CtxTree {
    CtxSnapshot *entries;        /* Array of context snapshots, grows dynamically based on capacity */
    int          count;           /* Number of context snapshots in the array */
    int          capacity;        /* Capacity of the entries array */
} CtxTree;

// Context Diff Structure
typedef struct CtxDiff {
    char   name[NAMEDATALEN];    /* Name of the context */
    Size   beforeAllocated;      /* Allocated bytes in before_snapshot i.e., before the test */
    Size   afterAllocated;       /* Allocated bytes in after_snapshot i.e., after the test */
    Size   beforeFree;           /* Free bytes in before_snapshot i.e., before the test */
    Size   afterFree;            /* Free bytes in after_snapshot i.e., after the test */
    int    depth;                /* Depth of this context in tree w.r.t Parent */
} CtxDiff;

// Helper function declarations

extern CtxTree* snapshot_context_tree(MemoryContext root);
extern CtxDiff* diff_context_trees(CtxTree *before, CtxTree *after, int *diff_count);
extern void free_context_tree(CtxTree *tree);
extern void free_context_diff(CtxDiff *diff);
extern uint32 ctx_compute_hash(const char *name, int depth);

#endif /* CONTEXT_WALKER_H */