/*
 * orf_provider.c
 * 
 * Custom scan provider for the optimized row format.
 */
#include "postgres.h"

#include "commands/explain.h"
#include "nodes/execnodes.h"
#include "optimizer/pathnode.h"
#include "optimizer/planmain.h"
#include "optimizer/restrictinfo.h"
#include "optimizer/tlist.h"

#include "orf_scan.h"

/*
 * Context for walking the expression trees.
 */
typedef struct OrfWalkerContext
{
	Bitmapset *attrs_used;
	Index scan_relid;
} OrfWalkerContext;

static bool orf_expression_walker(Node *node, OrfWalkerContext *context);

/*
 * Expression walker for finding used attributes.
 */
static bool
orf_expression_walker(Node *node, OrfWalkerContext *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, Var))
	{
		Var *var = (Var *) node;

		/* We're only interested in Vars that belong to the relation being scanned */
		if (var->varno == context->scan_relid && var->varlevelsup == 0)
		{
			/* Add the attribute number to our bitmap */
			if (var->varattno > 0)
				context->attrs_used = bms_add_member(context->attrs_used, var->varattno);
		}
		return false; /* stop recursing */
	}

	/* For all other node types, recurse. */
	return expression_tree_walker(node, orf_expression_walker, (void *) context);
}

/*
 * Create a CustomScanState for our optimized scan.
 */
static Node *
orf_create_scan_state(CustomScan *custom_scan)
{
	OptimizedScanState *oss = palloc0(sizeof(OptimizedScanState));

	oss->css.ss.ps.type = T_CustomScanState;

	return (Node *) oss;
}

/*
 * Explain the custom scan.
 */
static void
orf_explain_scan(CustomScanState *node, List *ancestors, ExplainState *es)
{
	/* Nothing to do here for now */
}

/*
 * Begin the custom scan.
 */
static void
orf_begin_scan(CustomScanState *node, EState *estate, int eflags)
{
	OptimizedScanState *oss = (OptimizedScanState *) node;
	CustomScan *cscan = (CustomScan *) oss->css.ss.ps.plan;
	OrfWalkerContext context;

	/* Initialize the walker context */
	context.scan_relid = cscan->scan.scanrelid;
	context.attrs_used = NULL;

	/* Walk the target list and qualifier expressions */
	orf_expression_walker((Node *) cscan->scan.plan.targetlist, &context);
	orf_expression_walker((Node *) cscan->scan.plan.qual, &context);

	/* Store the bitmap in our scan state */
	oss->attrs_used = context.attrs_used;

	if (oss->attrs_used != NULL)
	{
		char *bitmap_str = bms_to_string(oss->attrs_used);
		elog(LOG, "Optimized Scan Begin: Attributes used: %s", bitmap_str);
		pfree(bitmap_str);
	}
}

/*
 * Custom scan methods.
 */

static CustomExecMethods orf_exec_methods = {
	.CustomName = "OptimizedScan",
	.BeginCustomScan = orf_begin_scan,
	.ExecCustomScan = NULL, /* We don't have a custom execution loop */
	.EndCustomScan = NULL, /* We'll use the standard end scan */
	.ReScanCustomScan = NULL,
	.MarkPosCustomScan = NULL,
	.RestrPosCustomScan = NULL,
	.ExplainCustomScan = orf_explain_scan,
};

/*
 * Custom scan provider.
 */
static CustomScanMethods orf_scan_methods = {
	.CustomName = "OptimizedScan",
	.CreateCustomScanState = orf_create_scan_state
};

/* OID of our access method */
static Oid OptimizedAmOid = InvalidOid;

/* Save the previous hook */
static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

/*
 * Hook for creating custom paths.
 */
static void
orf_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti, RangeTblEntry *rte)
{
	/* Chain to previous hook */
	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	/* If it's not our table, we're not interested */
	if (rel->rd_rel->relam != OptimizedAmOid)
		return;

	/* Create a custom path for our scan */
	Path *custom_path = (Path *) create_custom_path(rel, &orf_scan_methods, NULL);

	/* Add the path to the relation's pathlist */
	add_path(rel, custom_path);
}

/*
 * _PG_init
 * 
 * Entry point for the extension. Registers our custom scan provider.
 */
void
_PG_init_provider(void)
{
	/* Get our access method's OID */
	OptimizedAmOid = get_table_am_oid("optimized_row_format", false);

	/* Register our custom scan provider */
	RegisterCustomScanMethods(&orf_scan_methods);

	/* Install our planner hook */
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = orf_set_rel_pathlist;
}

/*
 * _PG_fini_provider
 * 
 * Cleanup function for the provider.
 */
void
_PG_fini_provider(void)
{
	/* Uninstall our planner hook */
	set_rel_pathlist_hook = prev_set_rel_pathlist_hook;
}
