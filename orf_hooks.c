/*
 * orf_hooks.c
 * 
 * ExecutorStart/End hooks for smart attribute extraction.
 * Analyzes query plans to determine which attributes are actually needed,
 * stores this information in a global registry for use during execution.
 */
#include "postgres.h"

#include "executor/executor.h"
#include "nodes/plannodes.h"
#include "nodes/execnodes.h"
#include "nodes/nodeFuncs.h"
#include "nodes/bitmapset.h"
#include "utils/rel.h"
#include "utils/hsearch.h"
#include "utils/memutils.h"
#include "commands/defrem.h"

#include "orf_scan.h"

/* Forward declarations */
static bool orf_plan_has_aggregate(PlanState *planstate);
static bool orf_scan_feeds_aggregate(PlanState *scan_planstate, PlanState *root_planstate);

/*
 * Global bitmap registry: Maps relation OID to attribute bitmaps.
 * Populated during ExecutorStart, accessed during scan, cleared at ExecutorEnd.
 */
typedef struct OrfBitmapEntry
{
    Oid relid;              /* Hash key: relation OID */
    Bitmapset *attrs_used;  /* Bitmap of required attributes */
} OrfBitmapEntry;

static HTAB *bitmap_registry = NULL;
static MemoryContext registry_context = NULL;

/* Save previous hooks for chaining */
static ExecutorStart_hook_type prev_ExecutorStart_hook = NULL;
static ExecutorEnd_hook_type prev_ExecutorEnd_hook = NULL;

/*
 * Context for walking expression trees to find Var nodes.
 */
typedef struct OrfWalkerContext
{
    Bitmapset *attrs_used;
    Index scan_relid;
    bool is_aggregate_context;  /* True if this scan feeds an aggregate operation */
} OrfWalkerContext;

/*
 * Context for walking plan trees to find scan nodes.
 */
typedef struct OrfPlanWalkerContext
{
    PlanState *root_planstate;  /* Root of the entire plan tree */
} OrfPlanWalkerContext;

/*
 * Initialize the bitmap registry.
 */
static void
orf_registry_init(void)
{
    HASHCTL hash_ctl;

    if (bitmap_registry != NULL)
        return; /* Already initialized */

    /* Create memory context for registry (lives until extension unload) */
    registry_context = AllocSetContextCreate(TopMemoryContext,
                                             "ORF Bitmap Registry",
                                             ALLOCSET_DEFAULT_SIZES);

    /* Create hash table */
    MemSet(&hash_ctl, 0, sizeof(hash_ctl));
    hash_ctl.keysize = sizeof(Oid);
    hash_ctl.entrysize = sizeof(OrfBitmapEntry);
    hash_ctl.hcxt = registry_context;

    bitmap_registry = hash_create("ORF Bitmap Registry",
                                  32, /* initial size */
                                  &hash_ctl,
                                  HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
}

/*
 * Store a bitmap for a relation in the registry.
 */
static void
orf_registry_store(Oid relid, Bitmapset *attrs)
{
    OrfBitmapEntry *entry;
    bool found;
    MemoryContext oldcontext;

    if (bitmap_registry == NULL)
        orf_registry_init();

    /* 
     * CRITICAL: Switch to registry context before allocating bitmaps.
     * This ensures bitmaps persist for the entire query execution.
     */
    oldcontext = MemoryContextSwitchTo(registry_context);

    entry = (OrfBitmapEntry *) hash_search(bitmap_registry,
                                           &relid,
                                           HASH_ENTER,
                                           &found);

    if (found)
    {
        /* Union with existing bitmap (handles multiple scans on same relation) */
        Bitmapset *old_bitmap = entry->attrs_used;
        entry->attrs_used = bms_union(old_bitmap, attrs);
        /* Free the old bitmap to avoid memory leak */
        bms_free(old_bitmap);
    }
    else
    {
        /* New entry */
        entry->attrs_used = bms_copy(attrs);
    }
    
    MemoryContextSwitchTo(oldcontext);
}

/*
 * Look up a bitmap for a relation in the registry.
 * Returns NULL if not found.
 */
Bitmapset *
orf_registry_lookup(Oid relid)
{
    OrfBitmapEntry *entry;

    if (bitmap_registry == NULL)
        return NULL;

    entry = (OrfBitmapEntry *) hash_search(bitmap_registry,
                                           &relid,
                                           HASH_FIND,
                                           NULL);

    return entry ? entry->attrs_used : NULL;
}

/*
 * Clear all bitmaps from the registry.
 */
static void
orf_registry_clear(void)
{
    HASH_SEQ_STATUS status;
    OrfBitmapEntry *entry;

    if (bitmap_registry == NULL)
        return;

    /* Free all bitmaps */
    hash_seq_init(&status, bitmap_registry);
    while ((entry = (OrfBitmapEntry *) hash_seq_search(&status)) != NULL)
    {
        if (entry->attrs_used)
            bms_free(entry->attrs_used);
    }

    /* Destroy hash table */
    hash_destroy(bitmap_registry);
    bitmap_registry = NULL;
}

/*
 * Expression walker: Find all Var nodes and collect their attribute numbers.
 */
static bool
orf_expression_walker(Node *node, OrfWalkerContext *context)
{
    if (node == NULL)
        return false;

    if (IsA(node, Var))
    {
        Var *var = (Var *) node;

#if ORF_DEBUG_ENABLED
        elog(DEBUG1, "ORF DEBUG: Found Var node: varno=%d, varattno=%d, varlevelsup=%d (scan_relid=%d)",
             var->varno, var->varattno, var->varlevelsup, context->scan_relid);
#endif

        /* Only interested in Vars from the scanned relation */
        if (var->varno == context->scan_relid && var->varlevelsup == 0)
        {
            if (var->varattno == 0)
            {
                /* Whole-row reference: need all attributes */
#if ORF_DEBUG_ENABLED
                elog(DEBUG1, "ORF DEBUG: Whole-row reference detected! Setting attrs_used = NULL");
#endif
                context->attrs_used = NULL; /* Signal to use fallback */
                return true; /* Stop walking */
            }
            else if (var->varattno > 0)
            {
                /* Regular attribute */
#if ORF_DEBUG_ENABLED
                elog(DEBUG1, "ORF DEBUG: Adding attribute %d to bitmap", var->varattno);
#endif
                context->attrs_used = bms_add_member(context->attrs_used, var->varattno);
            }
            else
            {
#if ORF_DEBUG_ENABLED
                elog(DEBUG1, "ORF DEBUG: System column (varattno=%d), ignoring", var->varattno);
#endif
            }
        }
        else
        {
#if ORF_DEBUG_ENABLED
            elog(DEBUG1, "ORF DEBUG: Var from different relation or level, ignoring");
#endif
        }
        return false;
    }

    /* Log other node types for debugging */
#if ORF_DEBUG_ENABLED
    if (IsA(node, TargetEntry))
    {
        elog(DEBUG1, "ORF DEBUG: Found TargetEntry node");
    }
    else if (IsA(node, OpExpr))
    {
        elog(DEBUG1, "ORF DEBUG: Found OpExpr node");
    }
    else if (IsA(node, Const))
    {
        elog(DEBUG1, "ORF DEBUG: Found Const node");
    }
    else
    {
        elog(DEBUG1, "ORF DEBUG: Found other node type: %d", (int)nodeTag(node));
    }
#endif

    /* Recurse for all other node types */
    return expression_tree_walker(node, orf_expression_walker, (void *) context);
}

/*
 * Detect if a given scan node is feeding an aggregate operation.
 * This checks if there's an Agg node anywhere in the plan tree.
 */
static bool
orf_scan_feeds_aggregate(PlanState *scan_planstate, PlanState *root_planstate)
{
    /* Check if root plan contains Agg node */
    if (IsA(root_planstate, AggState))
    {
#if ORF_DEBUG_ENABLED
        elog(DEBUG1, "ORF DEBUG: Root plan is aggregate operation");
#endif
        return true;
    }

    /* Recursively check if any node in the plan tree is Agg */
    return orf_plan_has_aggregate(root_planstate);
}

/*
 * Recursively check if plan tree contains any aggregate operations.
 */
static bool
orf_plan_has_aggregate(PlanState *planstate)
{
    ListCell *lc;

    if (planstate == NULL)
        return false;

    /* Check current node */
    if (IsA(planstate, AggState))
    {
#if ORF_DEBUG_ENABLED
        elog(DEBUG1, "ORF DEBUG: Found aggregate node in plan tree");
#endif
        return true;
    }

    /* Check left subtree */
    if (planstate->lefttree && orf_plan_has_aggregate(planstate->lefttree))
        return true;

    /* Check right subtree */
    if (planstate->righttree && orf_plan_has_aggregate(planstate->righttree))
        return true;

    /* Check subplans */
    foreach(lc, planstate->subPlan)
    {
        SubPlanState *sps = (SubPlanState *) lfirst(lc);
        if (orf_plan_has_aggregate(sps->planstate))
            return true;
    }

    return false;
}

/*
 * Plan walker: Find all Scan nodes on optimized tables and analyze their expressions.
 */
static bool
orf_plan_walker(PlanState *planstate, void *context)
{
    OrfPlanWalkerContext *plan_context = (OrfPlanWalkerContext *) context;
    Plan *plan;
    Scan *scan;
    ScanState *ss;
    Relation rel;
    OrfWalkerContext expr_context;
    Oid relid;
#if ORF_DEBUG_ENABLED
    char *bitmap_str;
#endif
    bool is_aggregate_context;

    if (planstate == NULL)
        return false;

    plan = planstate->plan;

    /* Check if this is a scan node */
    if (IsA(plan, SeqScan) || IsA(plan, IndexScan) || IsA(plan, IndexOnlyScan) ||
        IsA(plan, BitmapHeapScan) || IsA(plan, TidScan) || IsA(plan, SampleScan))
    {
        scan = (Scan *) plan;
        ss = (ScanState *) planstate;
        rel = ss->ss_currentRelation;

        /* Check if this relation uses optimized_row_format */
        /* We check by comparing the table AM OID with our registered OID */
        if (rel && rel->rd_rel && rel->rd_rel->relam == get_table_am_oid("optimized_row_format", true))
        {
            relid = RelationGetRelid(rel);

            expr_context.scan_relid = scan->scanrelid;
            expr_context.attrs_used = NULL;

            /* AGGREGATE DETECTION: Check if this scan feeds an aggregate operation */
            is_aggregate_context = false;
            if (plan_context && plan_context->root_planstate)
            {
                is_aggregate_context = orf_scan_feeds_aggregate(planstate, plan_context->root_planstate);
            }
            expr_context.is_aggregate_context = is_aggregate_context;

            /* DEBUG: Log what we're about to analyze */
#if ORF_DEBUG_ENABLED
            elog(DEBUG1, "ORF DEBUG: Analyzing scan on relation '%s' (scanrelid=%d, aggregate_context=%s)",
                 RelationGetRelationName(rel), scan->scanrelid,
                 is_aggregate_context ? "true" : "false");
#endif

            /* Walk the target list (SKIP for aggregate context) */
            if (is_aggregate_context)
            {
#if ORF_DEBUG_ENABLED
                elog(DEBUG1, "ORF DEBUG: SKIPPING targetlist walk (aggregate context detected)");
#endif
            }
            else
            {
#if ORF_DEBUG_ENABLED
                elog(DEBUG1, "ORF DEBUG: Walking targetlist...");
#endif
                orf_expression_walker((Node *) plan->targetlist, &expr_context);

#if ORF_DEBUG_ENABLED
                if (expr_context.attrs_used != NULL) {
                    char *bitmap_str = bmsToString(expr_context.attrs_used);
                    elog(DEBUG1, "ORF DEBUG: After targetlist: attrs_used = %s", bitmap_str);
                    pfree(bitmap_str);
                } else {
                    elog(DEBUG1, "ORF DEBUG: After targetlist: attrs_used = NULL (whole-row or empty)");
                }
#endif
            }

            /* Walk the qual (ALWAYS process quals for filtering) */
#if ORF_DEBUG_ENABLED
            elog(DEBUG1, "ORF DEBUG: Walking qual...");
#endif
            orf_expression_walker((Node *) plan->qual, &expr_context);

#if ORF_DEBUG_ENABLED
            if (expr_context.attrs_used != NULL) {
                char *bitmap_str = bmsToString(expr_context.attrs_used);
                elog(DEBUG1, "ORF DEBUG: After qual: attrs_used = %s", bitmap_str);
                pfree(bitmap_str);
            } else {
                elog(DEBUG1, "ORF DEBUG: After qual: attrs_used = NULL (no qual attributes or whole-row)");
            }
#endif

            /* Store in registry */
            if (expr_context.attrs_used != NULL)
            {
                /* 
                 * CRITICAL: Copy the bitmap to registry FIRST, then use the registry's copy
                 * for logging. The expr_context.attrs_used might be in a temporary memory
                 * context that could be freed.
                 */
                orf_registry_store(relid, expr_context.attrs_used);
                
#if ORF_DEBUG_ENABLED
                /* Get the stored bitmap from registry for logging */
                Bitmapset *stored_bitmap = orf_registry_lookup(relid);
                if (stored_bitmap)
                {
                    bitmap_str = bmsToString(stored_bitmap);
                    elog(LOG, "ORF: Scan on '%s' (OID %u) needs attributes: %s",
                         RelationGetRelationName(rel), relid, bitmap_str);
                    pfree(bitmap_str);
                }
#endif
            }
            else
            {
#if ORF_DEBUG_ENABLED
                elog(LOG, "ORF: Scan on '%s' (OID %u) needs all attributes (whole-row reference)",
                     RelationGetRelationName(rel), relid);
#endif
            }
        }
    }

    /* Recurse to child nodes */
    return planstate_tree_walker(planstate, orf_plan_walker, context);
}

/*
 * ExecutorStart hook: Analyze the plan and populate the bitmap registry.
 */
static void
orf_executor_start(QueryDesc *queryDesc, int eflags)
{
    /* Chain to previous hook first */
    if (prev_ExecutorStart_hook)
        prev_ExecutorStart_hook(queryDesc, eflags);
    else
        standard_ExecutorStart(queryDesc, eflags);

    /* Walk the plan tree to find optimized scans and build bitmaps */
    if (queryDesc->planstate)
    {
        OrfPlanWalkerContext plan_context;
        plan_context.root_planstate = queryDesc->planstate;
        orf_plan_walker(queryDesc->planstate, &plan_context);
    }
}

/*
 * ExecutorEnd hook: Clear the bitmap registry.
 */
static void
orf_executor_end(QueryDesc *queryDesc)
{
    /* Clear the registry */
    orf_registry_clear();

    /* Chain to previous hook */
    if (prev_ExecutorEnd_hook)
        prev_ExecutorEnd_hook(queryDesc);
    else
        standard_ExecutorEnd(queryDesc);
}

/*
 * _PG_init: Install hooks.
 */
void
_PG_init(void)
{
    /* Initialize registry */
    orf_registry_init();

    /* Install hooks */
    prev_ExecutorStart_hook = ExecutorStart_hook;
    ExecutorStart_hook = orf_executor_start;

    prev_ExecutorEnd_hook = ExecutorEnd_hook;
    ExecutorEnd_hook = orf_executor_end;

    elog(LOG, "ORF: Smart extraction hooks installed");
}

/*
 * _PG_fini: Uninstall hooks.
 */
void
_PG_fini(void)
{
    /* Uninstall hooks */
    ExecutorStart_hook = prev_ExecutorStart_hook;
    ExecutorEnd_hook = prev_ExecutorEnd_hook;

    /* Clean up registry */
    orf_registry_clear();

    if (registry_context)
    {
        MemoryContextDelete(registry_context);
        registry_context = NULL;
    }
}
