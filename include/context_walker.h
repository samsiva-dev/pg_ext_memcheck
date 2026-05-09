#ifndef CONTEXT_WALKER_H
#define CONTEXT_WALKER_H

#include "postgres.h"
#include "pg_config_manual.h"
#include "c.h"
#include "postgres_ext.h"

typedef struct CtxSnapshot {
    char   name[NAMEDATALEN];
    Size   totalAllocated;
    Size   totalFree;
    int    depth;           /* depth in tree */
    Oid    parentHash;      /* hash of parent name+depth for diff */
} CtxSnapshot;

typedef struct CtxTree {
    CtxSnapshot *entries;
    int          count;
    int          capacity;
} CtxTree;

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

#endif /* CONTEXT_WALKER_H */