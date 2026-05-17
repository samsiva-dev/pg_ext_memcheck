// Postgres Includes
#include "postgres.h"
#include "fmgr.h"
#include "nodes/memnodes.h"

// Local Includes
#include "include/pg_ext_memcheck.h"
#include "include/gucs.h"
#include "include/context_walker.h"

/*
 * Stable identity hash for a context node: djb2 over the name XOR'd with
 * depth.  Used as parentHash so the diff can distinguish same-named contexts
 * that live at different levels of the tree.
 */
Oid
ctx_compute_hash(const char *name, int depth)
{
    uint32      h = 5381;
    const char *p;

    for (p = name; *p; p++)
        h = ((h << 5) + h) ^ (unsigned char) *p;
    h = h * 31 ^ (uint32) depth;
    return (Oid) h;
}

/* Grow tree->entries by doubling, handling the initial NULL case. */
static void
tree_ensure_capacity(CtxTree *tree)
{
    if (tree->count < tree->capacity)
        return;

    tree->capacity = (tree->capacity == 0) ? 16 : tree->capacity * 2;
    if (tree->entries == NULL) {
        // if entries is NULL, allocate new memory
        tree->entries = (CtxSnapshot *) palloc(sizeof(CtxSnapshot) * tree->capacity);
    }
    else {
        // if entries already has allocated memory, reallocate to grow
        tree->entries = (CtxSnapshot *) repalloc(tree->entries, sizeof(CtxSnapshot) * tree->capacity);
    }
}

/*
 * Get CtxSnapshot for the passed MemoryContext.
 *
 * Use the stats() method with MemoryContextCounters to obtain totalspace and
 * freespace for the whole context.  
 */
static CtxSnapshot
get_context_snapshot(MemoryContext context, int depth, Oid parentHash)
{
    CtxSnapshot             snapshot;
    MemoryContextCounters   counters;

    snprintf(snapshot.name, NAMEDATALEN, "%s", context->name);

    memset(&counters, 0, sizeof(counters));
    (*context->methods->stats)(context, NULL, NULL, &counters, false);

    snapshot.totalAllocated = counters.totalspace;
    snapshot.totalFree      = counters.freespace;
    snapshot.depth          = depth;
    snapshot.parentHash     = parentHash;
    return snapshot;
}

/*
 * Recursive helper: record ctx at the given depth, then descend into every
 * child.  Using a dedicated helper (rather than a while loop that recurses
 * into firstchild) ensures depth and parentHash are computed correctly at
 * every level of the tree.
 */
static void
walk_context_tree(MemoryContext ctx, int depth, Oid parentHash, CtxTree *tree)
{
    CtxSnapshot  snapshot = get_context_snapshot(ctx, depth, parentHash);
    Oid          myHash   = ctx_compute_hash(snapshot.name, depth);
    MemoryContext child;

    tree_ensure_capacity(tree);
    tree->entries[tree->count++] = snapshot;

    for (child = ctx->firstchild; child != NULL; child = child->nextchild)
        walk_context_tree(child, depth + 1, myHash, tree);
}

/*
    Function: snapshot_context_tree
    Description: Takes a snapshot of the memory context tree starting from the given root context.
    Parameters:
        - MemoryContext root: The root memory context from which to start the snapshot.
    Returns:
        - CtxTree*: A pointer to a CtxTree structure containing the snapshot of the memory context tree.
*/
CtxTree *
snapshot_context_tree(MemoryContext root)
{
    CtxTree *tree = (CtxTree *) palloc0(sizeof(CtxTree));

    if (root != NULL)
        walk_context_tree(root, 0, 0, tree);

    return tree;
}

/*
    Function: diff_context_trees
    Description: Compares two CtxTree snapshots and returns an array of CtxDiff structures
                 representing contexts present in both snapshots.  Only matched contexts are
                 included; *diff_count is set to the number of valid entries returned.
                 Returns NULL (with *diff_count = 0) when nothing matched.
    Parameters:
        - CtxTree *before:    snapshot before the operation.
        - CtxTree *after:     snapshot after the operation.
        - int     *diff_count: out-parameter; set to the number of entries in the returned array.
    Returns:
        - CtxDiff*: palloc'd array of diff entries, or NULL if there were no matches.
*/
CtxDiff *
diff_context_trees(CtxTree *before, CtxTree *after, int *diff_count)
{
    CtxDiff    *diffs    = NULL;
    int         count    = 0;
    int         capacity = 0;
    bool       *before_matched = NULL;

    Assert(before != NULL);
    Assert(after  != NULL);
    Assert(diff_count != NULL);

    /* Track which before-entries have already been paired so that duplicate
     * (name, depth, parentHash) siblings — e.g. the many 'index info' or
     * 'dynahash' contexts — are each consumed at most once. */
    if (before->count > 0)
    {
        before_matched = (bool *) palloc0(sizeof(bool) * before->count);
    }

    for (int i = 0; i < after->count; i++)
    {
        CtxSnapshot *after_snapshot = &after->entries[i];
        CtxSnapshot *before_snapshot = NULL;
        int          matched_j = -1;

        /* Match by name + depth + parentHash for stable identity. */
        for (int j = 0; j < before->count; j++)
        {
            CtxSnapshot *b = &before->entries[j];

            if (!before_matched[j]                             &&
                b->depth       == after_snapshot->depth        &&
                b->parentHash  == after_snapshot->parentHash   &&
                strcmp(b->name, after_snapshot->name) == 0)
            {
                before_snapshot = b;
                matched_j = j;
                break;
            }
        }

        /* New context (not in before): treat baseline as 0 so its full
         * allocation is visible to analyze_and_log_diff.  This matters for
         * MEMCHECK_ALL where before_snapshot is taken pre-planning and
         * planner-born contexts that grow during execution would otherwise
         * be silently dropped. */
        if (before_snapshot != NULL && before_matched != NULL)
            before_matched[matched_j] = true;

        if (count >= capacity)
        {
            capacity = (capacity == 0) ? 16 : capacity * 2;
            if (diffs == NULL)
                diffs = (CtxDiff *) palloc(sizeof(CtxDiff) * capacity);
            else
                diffs = (CtxDiff *) repalloc(diffs, sizeof(CtxDiff) * capacity);
        }

        CtxDiff *diff = &diffs[count++];
        snprintf(diff->name, NAMEDATALEN, "%s", after_snapshot->name);
        diff->beforeAllocated = (before_snapshot != NULL) ? before_snapshot->totalAllocated : 0;
        diff->afterAllocated  = after_snapshot->totalAllocated;
        diff->beforeFree      = (before_snapshot != NULL) ? before_snapshot->totalFree : 0;
        diff->afterFree       = after_snapshot->totalFree;
        diff->depth           = after_snapshot->depth;
    }

    if (before_matched != NULL)
        pfree(before_matched);

    *diff_count = count;
    return diffs;
}

void
free_context_tree(CtxTree *tree)
{
    if (tree != NULL)
    {
        if (tree->entries != NULL)
            pfree(tree->entries);
        pfree(tree);
    }
}

void
free_context_diff(CtxDiff *diff)
{
    if (diff != NULL)
        pfree(diff);
}