/*-------------------------------------------------------------------------
 * pg_ext_memcheck
 *
 * Copyright (c) 2026, Samba Siva Reddy
 *
 * This software is released under the MIT License.
 * See LICENSE for details.
 *-------------------------------------------------------------------------
*/

// Postgres Includes
#include "postgres.h"
#include "fmgr.h"
#include "nodes/memnodes.h"

// Local Includes
#include "include/pg_ext_memcheck.h"
#include "include/gucs.h"
#include "include/context_walker.h"

// Get CtxSnapshot for the Passed MemoryContext
static CtxSnapshot get_context_snapshot(MemoryContext context, int depth, Oid parentHash) {
    CtxSnapshot snapshot;
    snprintf(snapshot.name, NAMEDATALEN, "%s", context->name);
    snapshot.totalAllocated = context->mem_allocated;
    snapshot.totalFree = context->mem_allocated - context->methods->get_chunk_space(context);
    snapshot.depth = depth;
    snapshot.parentHash = parentHash;
    return snapshot;
}

/*
    Function: snapshot_context_tree
    Description: Takes a snapshot of the memory context tree starting from the given root context.
    Parameters:
        - MemoryContext root: The root memory context from which to start the snapshot.
    Returns:
        - CtxTree*: A pointer to a CtxTree structure containing the snapshot of the memory context tree.
*/
CtxTree* snapshot_context_tree(MemoryContext root) {
    CtxTree *tree = (CtxTree *) palloc(sizeof(CtxTree));
    tree->entries = NULL;
    tree->count = 0;
    tree->capacity = 0;

    while (root != NULL) {
        Oid parentHash = 0; // For simplicity, we can compute a hash based on the context name and depth
        CtxSnapshot snapshot = get_context_snapshot(root, 0, parentHash);

        if (root->firstchild != NULL) {
            // Recursively snapshot child contexts
            CtxTree *childTree = snapshot_context_tree(root->firstchild);
            for (int i = 0; i < childTree->count; i++) {
                CtxSnapshot childSnapshot = childTree->entries[i];
                childSnapshot.depth += 1; // Increment depth for child contexts
                childSnapshot.parentHash = parentHash; // Set parent hash for diffing
                // Add child snapshot to the main tree
                if (tree->count >= tree->capacity) {
                    tree->capacity = (tree->capacity == 0) ? 4 : tree->capacity * 2;
                    tree->entries = (CtxSnapshot *) repalloc(tree->entries, sizeof(CtxSnapshot) * tree->capacity);
                }
                tree->entries[tree->count++] = childSnapshot;
            }
            pfree(childTree);
        }

        // Add the current context snapshot to the tree
        if (tree->count >= tree->capacity) {
            tree->capacity = (tree->capacity == 0) ? 4 : tree->capacity * 2;
            tree->entries = (CtxSnapshot *) repalloc(tree->entries, sizeof(CtxSnapshot) * tree->capacity);
        }
        tree->entries[tree->count++] = snapshot;
    }
    
    return tree;
}

/*
    Function: diff_context_trees
    Description: Compares two CtxTree snapshots and returns an array of CtxDiff structures representing the differences.
    Parameters:
        - CtxTree *before: The snapshot of the memory context tree before the operation.
        - CtxTree *after: The snapshot of the memory context tree after the operation.
    Returns:
        - CtxDiff*: A pointer to an array of CtxDiff structures representing the differences between the two snapshots.
*/
CtxDiff* diff_context_trees(CtxTree *before, CtxTree *after) {
    // For simplicity, we can use a hash map to store the before snapshot for quick lookup
    // In a real implementation, we would need to handle hash collisions and ensure unique identification of contexts
    CtxDiff *diffs = NULL;
    int diffCount = 0;
    int diffCapacity = 0;

    for (int i = 0; i < after->count; i++) {
        CtxSnapshot afterSnapshot = after->entries[i];
        CtxSnapshot *beforeSnapshot = NULL;

        // Find the corresponding snapshot in the before tree using parentHash and name
        for (int j = 0; j < before->count; j++) {
            if (strcmp(before->entries[j].name, afterSnapshot.name) == 0 &&
                before->entries[j].depth == afterSnapshot.depth &&
                before->entries[j].parentHash == afterSnapshot.parentHash) {
                beforeSnapshot = &before->entries[j];
                break;
            }
        }

        if (beforeSnapshot != NULL) {
            // Create a diff entry
            CtxDiff diff;
            snprintf(diff.name, NAMEDATALEN, "%s", afterSnapshot.name);
            diff.beforeAllocated = beforeSnapshot->totalAllocated;
            diff.afterAllocated = afterSnapshot.totalAllocated;
            diff.beforeFree = beforeSnapshot->totalFree;
            diff.afterFree = afterSnapshot.totalFree;
            diff.depth = afterSnapshot.depth;

            // Add the diff entry to the diffs array
            if (diffCount >= diffCapacity) {
                diffCapacity = (diffCapacity == 0) ? 4 : diffCapacity * 2;
                diffs = (CtxDiff *) repalloc(diffs, sizeof(CtxDiff) * diffCapacity);
            }
            diffs[diffCount++] = diff;
        }
    }

    return diffs;
}

void free_context_tree(CtxTree *tree) {
    if (tree != NULL) {
        if (tree->entries != NULL) {
            pfree(tree->entries);
        }
        pfree(tree);
    }
}

void free_context_diff(CtxDiff *diff) {
    if (diff != NULL) {
        pfree(diff);
    }
}