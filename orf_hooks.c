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
} OrfWalkerContext;

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

    if (bitmap_registry == NULL)
        orf_registry_init();

    entry = (OrfBitmapEntry *) hash_search(bitmap_registry,
                                           &relid,
                                           HASH_ENTER,
                                           &found);

    if (found)
    {
        /* Union with existing bitmap (handles multiple scans on same relation) */
        entry->attrs_used = bms_union(entry->attrs_used, attrs);
    }
    else
    {
        /* New entry */
        entry->attrs_used = bms_copy(attrs);
    }
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

        /* Only interested in Vars from the scanned relation */
        if (var->varno == context->scan_relid && var->varlevelsup == 0)
        {
            if (var->varattno == 0)
            {
                /* Whole-row reference: need all attributes */
                context->attrs_used = NULL; /* Signal to use fallback */
                return true; /* Stop walking */
            }
            else if (var->varattno > 0)
            {
                /* Regular attribute */
                context->attrs_used = bms_add_member(context->attrs_used, var->varattno);
            }
            /* Negative varattno = system column, handled separately */
        }
        return false;
    }

    /* Recurse for all other node types */
    return expression_tree_walker(node, orf_expression_walker, (void *) context);
}

/*
 * Plan walker: Find all Scan nodes on optimized tables and analyze their expressions.
 */
static bool
orf_plan_walker(PlanState *planstate, void *context)
{
    Plan *plan;
    Scan *scan;
    ScanState *ss;
    Relation rel;
    OrfWalkerContext expr_context;
    Oid relid;
    char *bitmap_str;

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

            /* Walk the target list */
            orf_expression_walker((Node *) plan->targetlist, &expr_context);

            /* Walk the qual */
            if (expr_context.attrs_used != NULL) /* Not whole-row */
                orf_expression_walker((Node *) plan->qual, &expr_context);

            /* Store in registry */
            if (expr_context.attrs_used != NULL)
            {
                orf_registry_store(relid, expr_context.attrs_used);

                /* Debug logging */
                bitmap_str = bmsToString(expr_context.attrs_used);
                elog(LOG, "ORF: Scan on '%s' (OID %u) needs attributes: %s",
                     RelationGetRelationName(rel), relid, bitmap_str);
                pfree(bitmap_str);
            }
            else
            {
                elog(LOG, "ORF: Scan on '%s' (OID %u) needs all attributes (whole-row reference)",
                     RelationGetRelationName(rel), relid);
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
        orf_plan_walker(queryDesc->planstate, NULL);
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
