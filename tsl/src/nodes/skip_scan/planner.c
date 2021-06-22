/*
 * This file and its contents are licensed under the Timescale License.
 * Please see the included NOTICE for copyright information and
 * LICENSE-TIMESCALE for a copy of the license.
 */

#include <postgres.h>
#include <access/sysattr.h>
#include <nodes/extensible.h>
#include <nodes/nodeFuncs.h>
#include <nodes/makefuncs.h>
#include <nodes/pathnodes.h>
#include <optimizer/clauses.h>
#include <optimizer/cost.h>
#include <optimizer/optimizer.h>
#include <optimizer/paths.h>
#include <optimizer/pathnode.h>
#include <optimizer/planmain.h>
#include <optimizer/restrictinfo.h>
#include <optimizer/tlist.h>
#include <parser/parsetree.h>
#include <rewrite/rewriteManip.h>
#include <utils/syscache.h>
#include <utils/typcache.h>

#include <import/planner.h>
#include "guc.h"
#include "nodes/skip_scan/skip_scan.h"
#include "nodes/constraint_aware_append/constraint_aware_append.h"
#include "chunk_append/chunk_append.h"
#include "compat.h"

#include <math.h>

typedef struct SkipScanPath
{
	CustomPath cpath;
	IndexPath *index_path;

	/* Index clause which we'll use to skip past elements we've already seen */
	RestrictInfo *skip_clause;
	/* The column offset on the relation we are calling DISTINCT on */
	AttrNumber table_attno;
	/* The column offset on the index we are calling DISTINCT on */
	AttrNumber index_attno;
	int distinct_typ_len;
	bool distinct_by_val;
	/* Var referencing the distinct column on the relation */
	Var *distinct_var;
} SkipScanPath;

static int get_idx_key(IndexOptInfo *idxinfo, AttrNumber attno);
static List *sort_indexquals(IndexOptInfo *indexinfo, List *quals);
static OpExpr *fix_indexqual(IndexOptInfo *index, RestrictInfo *rinfo, AttrNumber table_attno);
static bool build_skip_qual(PlannerInfo *root, SkipScanPath *skip_scan_path, IndexPath *index_path,
							Var *var);
static List *build_subpath(PlannerInfo *root, List *subpaths, double ndistinct);
static ChunkAppendPath *copy_chunk_append_path(ChunkAppendPath *ca, List *subpaths);
static Var *get_distinct_var(PlannerInfo *root, IndexPath *index_path,
							 SkipScanPath *skip_scan_path);
static TargetEntry *tlist_member_match_var(Var *var, List *targetlist);

/**************************
 * SkipScan Plan Creation *
 **************************/

static CustomScanMethods skip_scan_plan_methods = {
	.CustomName = "SkipScan",
	.CreateCustomScanState = tsl_skip_scan_state_create,
};

void
_skip_scan_init(void)
{
	/*
	 * Because we reinitialize the tsl stuff when the license
	 * changes the init function may be called multiple times
	 * per session so we check if the SkipScan node has been
	 * registered already here to prevent registering it twice.
	 */
	if (GetCustomScanMethods(skip_scan_plan_methods.CustomName, true) == NULL)
		RegisterCustomScanMethods(&skip_scan_plan_methods);
}

static Plan *
skip_scan_plan_create(PlannerInfo *root, RelOptInfo *relopt, CustomPath *best_path, List *tlist,
					  List *clauses, List *custom_plans)
{
	SkipScanPath *path = (SkipScanPath *) best_path;
	CustomScan *skip_plan = makeNode(CustomScan);
	IndexPath *index_path = path->index_path;

	OpExpr *op = fix_indexqual(index_path->indexinfo, path->skip_clause, path->table_attno);

	Plan *plan = linitial(custom_plans);
	if (IsA(plan, IndexScan))
	{
		IndexScan *idx_plan = castNode(IndexScan, plan);
		skip_plan->scan = idx_plan->scan;

		/* we prepend skip qual here so sort_indexquals will put it as first qual for that column */
		idx_plan->indexqual =
			sort_indexquals(index_path->indexinfo, lcons(op, idx_plan->indexqual));
	}
	else if (IsA(plan, IndexOnlyScan))
	{
		IndexOnlyScan *idx_plan = castNode(IndexOnlyScan, plan);
		skip_plan->scan = idx_plan->scan;
		/* we prepend skip qual here so sort_indexquals will put it as first qual for that column */
		idx_plan->indexqual =
			sort_indexquals(index_path->indexinfo, lcons(op, idx_plan->indexqual));
	}
	else
		elog(ERROR, "bad subplan type for SkipScan: %d", plan->type);

	skip_plan->scan.plan.targetlist = tlist;
	skip_plan->custom_scan_tlist = list_copy(tlist);
	skip_plan->scan.plan.qual = NIL;
	skip_plan->scan.plan.type = T_CustomScan;
	skip_plan->methods = &skip_scan_plan_methods;
	skip_plan->custom_plans = custom_plans;
	/* get position of skipped column in tuples produced by child scan */
	TargetEntry *tle = tlist_member_match_var(path->distinct_var, plan->targetlist);

	bool nulls_first = index_path->indexinfo->nulls_first[path->index_attno - 1];
	if (index_path->indexscandir == BackwardScanDirection)
		nulls_first = !nulls_first;

	skip_plan->custom_private = list_make5_int(tle->resno,
											   path->distinct_by_val,
											   path->distinct_typ_len,
											   nulls_first,
											   path->index_attno);
	return &skip_plan->scan.plan;
}

/*************************
 * SkipScanPath Creation *
 *************************/
static CustomPathMethods skip_scan_path_methods = {
	.CustomName = "SkipScanPath",
	.PlanCustomPath = skip_scan_plan_create,
};

static SkipScanPath *skip_scan_path_create(PlannerInfo *root, IndexPath *index_path,
										   double ndistinct);

/*
 * Create SkipScan paths based on existing Unique paths.
 * For a Unique path on a simple relation like the following
 *
 *  Unique
 *    ->  Index Scan using skip_scan_dev_name_idx on skip_scan
 *
 * a SkipScan path like this will be created:
 *
 *  Unique
 *    ->  Custom Scan (SkipScan) on skip_scan
 *          ->  Index Scan using skip_scan_dev_name_idx on skip_scan
 *
 * For a Unique path on a hypertable with multiple chunks like the following
 *
 *  Unique
 *    ->  Merge Append
 *          Sort Key: _hyper_2_1_chunk.dev_name
 *          ->  Index Scan using _hyper_2_1_chunk_idx on _hyper_2_1_chunk
 *          ->  Index Scan using _hyper_2_2_chunk_idx on _hyper_2_2_chunk
 *
 * a SkipScan path like this will be created:
 *
 *  Unique
 *    ->  Merge Append
 *          Sort Key: _hyper_2_1_chunk.dev_name
 *          ->  Custom Scan (SkipScan) on _hyper_2_1_chunk
 *                ->  Index Scan using _hyper_2_1_chunk_idx on _hyper_2_1_chunk
 *          ->  Custom Scan (SkipScan) on _hyper_2_2_chunk
 *                ->  Index Scan using _hyper_2_2_chunk_idx on _hyper_2_2_chunk
 */
void
tsl_skip_scan_paths_add(PlannerInfo *root, RelOptInfo *input_rel, RelOptInfo *output_rel)
{
	ListCell *lc;
	UpperUniquePath *unique = NULL;

	if (!ts_guc_enable_skip_scan)
		return;

	/*
	 * look for Unique Path so we dont have repeat some of
	 * the calculations done by postgres and can also assume
	 * that the DISTINCT clause is elegible for sort based
	 * DISTINCT
	 */
	foreach (lc, output_rel->pathlist)
	{
		if (IsA(lfirst(lc), UpperUniquePath))
		{
			unique = lfirst_node(UpperUniquePath, lc);

			/* currently we do not handle DISTINCT on more than one key. To do so,
			 * we would need to break down the SkipScan into subproblems: first
			 * find the minimal tuple then for each prefix find all unique suffix
			 * tuples. For instance, if we are searching over (int, int), we would
			 * first find (0, 0) then find (0, N) for all N in the domain, then
			 * find (1, N), then (2, N), etc
			 */
			if (unique->numkeys > 1)
				return;

			break;
		}
	}
	/* no UniquePath found so this query might not be
	 * elegible for sort-based DISTINCT and therefore
	 * not elegible for SkipScan either */
	if (!unique)
		return;

	foreach (lc, input_rel->pathlist)
	{
		bool project = false;
		bool has_caa = false;

		Path *subpath = lfirst(lc);

		if (!pathkeys_contained_in(unique->path.pathkeys, subpath->pathkeys))
			continue;

		/* If path is a ProjectionPath we strip it off for processing
		 * but also add a ProjectionPath on top of the SKipScanPaths
		 * later.
		 */
		if (IsA(subpath, ProjectionPath))
		{
			ProjectionPath *proj = castNode(ProjectionPath, subpath);
			subpath = proj->subpath;
			project = true;
		}

		/* Path might be wrapped in a ConstraintAwareAppendPath if this
		 * is a MergeAppend that could benefit from runtime exclusion.
		 * We treat this similar to ProjectionPath and add it back
		 * later
		 */
		if (ts_is_constraint_aware_append_path(subpath))
		{
			subpath = linitial(castNode(CustomPath, subpath)->custom_paths);
			Assert(IsA(subpath, MergeAppendPath));
			has_caa = true;
		}

		if (IsA(subpath, IndexPath))
		{
			IndexPath *index_path = castNode(IndexPath, subpath);

			subpath = (Path *) skip_scan_path_create(root, index_path, unique->path.rows);
			if (!subpath)
				continue;
		}
		else if (IsA(subpath, MergeAppendPath))
		{
			MergeAppendPath *merge_path = castNode(MergeAppendPath, subpath);
			List *new_paths = build_subpath(root, merge_path->subpaths, unique->path.rows);

			/* build_subpath returns NULL when no SkipScanPath was created */
			if (!new_paths)
				continue;

			subpath = (Path *) create_merge_append_path(root,
														merge_path->path.parent,
														new_paths,
														merge_path->path.pathkeys,
														NULL,
														merge_path->partitioned_rels);
			subpath->pathtarget = copy_pathtarget(merge_path->path.pathtarget);
		}
		else if (ts_is_chunk_append_path(subpath))
		{
			ChunkAppendPath *ca = (ChunkAppendPath *) subpath;
			List *new_paths = build_subpath(root, ca->cpath.custom_paths, unique->path.rows);
			/* ChunkAppend should never be wrapped in ConstraintAwareAppendPath */
			Assert(!has_caa);

			/* build_subpath returns NULL when no SkipScanPath was created */
			if (!new_paths)
				continue;

			/* We copy the existing ChunkAppendPath here because we don't have all the
			 * information used for creating the original one and we don't want to
			 * duplicate all the checks done when creating the original one.
			 */
			subpath = (Path *) copy_chunk_append_path(ca, new_paths);
		}
		else
		{
			continue;
		}

		/* add ConstraintAwareAppendPath if the original path had one */
		if (has_caa)
			subpath = ts_constraint_aware_append_path_create(root, subpath);

		Path *new_unique = (Path *)
			create_upper_unique_path(root, output_rel, subpath, unique->numkeys, unique->path.rows);
		new_unique->pathtarget = unique->path.pathtarget;

		if (project)
			new_unique = (Path *) create_projection_path(root,
														 output_rel,
														 new_unique,
														 copy_pathtarget(new_unique->pathtarget));

		add_path(output_rel, new_unique);
	}
}

static ChunkAppendPath *
copy_chunk_append_path(ChunkAppendPath *ca, List *subpaths)
{
	ListCell *lc;
	double total_cost = 0, rows = 0;
	ChunkAppendPath *new = palloc(sizeof(ChunkAppendPath));
	memcpy(new, ca, sizeof(ChunkAppendPath));
	new->cpath.custom_paths = subpaths;

	foreach (lc, subpaths)
	{
		Path *child = lfirst(lc);
		total_cost += child->total_cost;
		rows += child->rows;
	}
	new->cpath.path.total_cost = total_cost;
	new->cpath.path.rows = rows;

	return new;
}

static SkipScanPath *
skip_scan_path_create(PlannerInfo *root, IndexPath *index_path, double ndistinct)
{
	double startup = index_path->path.startup_cost;
	double total = index_path->path.total_cost;
	double rows = index_path->path.rows;

	/* cannot use SkipScan with non-orderable index or IndexPath without pathkeys */
	if (!index_path->path.pathkeys || !index_path->indexinfo->sortopfamily)
		return NULL;

	/* orderbyops are not compatible with skipscan */
	if (index_path->indexorderbys != NIL)
		return NULL;

	SkipScanPath *skip_scan_path = (SkipScanPath *) newNode(sizeof(SkipScanPath), T_CustomPath);

	skip_scan_path->cpath.path.pathtype = T_CustomScan;
	skip_scan_path->cpath.path.pathkeys = index_path->path.pathkeys;
	skip_scan_path->cpath.path.pathtarget = index_path->path.pathtarget;
	skip_scan_path->cpath.path.param_info = index_path->path.param_info;
	skip_scan_path->cpath.path.parent = index_path->path.parent;
	skip_scan_path->cpath.path.rows = ndistinct;
	skip_scan_path->cpath.custom_paths = list_make1(index_path);
	skip_scan_path->cpath.methods = &skip_scan_path_methods;

	/* We calculate SkipScan cost as ndistinct * startup_cost + (ndistinct/rows) * total_cost
	 * ndistinct * startup_cost is to account for the rescans we have to do and since startup
	 * cost for indexes does not include page access cost we add a fraction of the total cost
	 * accounting for the number of rows we expect to fetch.
	 * If the row estimate for the scan is 1 we assume that the estimate got clamped to 1
	 * and no rows would be returned by this scan and this chunk will most likely be excluded
	 * by runtime exclusion. Otherwise the cost for this path would be highly inflated due
	 * to (ndistinct / rows) * total leading to SkipScan not being chosen for queries on
	 * hypertables with a lot of excluded chunks.
	 */
	skip_scan_path->cpath.path.startup_cost = startup;
	if (rows > 1)
		skip_scan_path->cpath.path.total_cost = ndistinct * startup + (ndistinct / rows) * total;
	else
		skip_scan_path->cpath.path.total_cost = startup;

	/* While add_path may pfree paths with higher costs
	 * it will never free IndexPaths and only ever do a shallow
	 * free so reusing the IndexPath here is safe. */
	skip_scan_path->index_path = index_path;

	Var *var = get_distinct_var(root, index_path, skip_scan_path);

	if (!var)
		return NULL;

	/* build skip qual this may fail if we cannot look up the operator */
	if (!build_skip_qual(root, skip_scan_path, index_path, var))
		return NULL;

	return skip_scan_path;
}

/* Extract the Var to use for the SkipScan and do the TupleConversion
 * if required.
 */
static Var *
get_distinct_var(PlannerInfo *root, IndexPath *index_path, SkipScanPath *skip_scan_path)
{
	ListCell *lc;
	int num_vars = 0;
	RelOptInfo *rel = index_path->path.parent;
	Expr *tlexpr = NULL;

	foreach (lc, root->parse->distinctClause)
	{
		SortGroupClause *clause = lfirst_node(SortGroupClause, lc);
		Node *expr = get_sortgroupclause_expr(clause, root->parse->targetList);

		/* we ignore any columns that can be constified to allow for cases like DISTINCT 'abc',
		 * column */
		if (IsA(estimate_expression_value(root, expr), Const))
			continue;

		num_vars++;

		/* We ignore binary-compatible relabeling on both ends */
		tlexpr = (Expr *) expr;
		while (tlexpr && IsA(tlexpr, RelabelType))
			tlexpr = ((RelabelType *) tlexpr)->arg;
	}

	if (num_vars != 1)
		return NULL;

	/* SkipScan on expressions not supported */
	if (!tlexpr || !IsA(tlexpr, Var))
		return NULL;

	Var *var = castNode(Var, tlexpr);

	/* If we are dealing with a hypertable Var extracted from distinctClause will point to
	 * the parent hypertable while the IndexPath will be on a Chunk */
	if (var->varno != rel->relid)
	{
		RangeTblEntry *ht_rte = planner_rt_fetch(var->varno, root);
		RangeTblEntry *chunk_rte = planner_rt_fetch(rel->relid, root);

		/* Check for hypertable */
		if (!ts_is_hypertable(ht_rte->relid) || !bms_is_member(var->varno, rel->top_parent_relids))
			return NULL;

		Relation ht_rel = table_open(ht_rte->relid, AccessShareLock);
		Relation chunk_rel = table_open(chunk_rte->relid, AccessShareLock);
		bool found_wholerow;
		TupleConversionMap *map =
			convert_tuples_by_name_compat(RelationGetDescr(chunk_rel),
										  RelationGetDescr(ht_rel),
										  gettext_noop("could not convert row type"));

		/* tuple conversion necessary */
		if (map)
		{
			var = (Var *) map_variable_attnos_compat((Node *) var,
													 var->varno,
													 0,
													 map->attrMap,
													 map->outdesc->natts,
													 InvalidOid,
													 &found_wholerow);

			/* If we found whole row here skipscan wouldnt be applicable
			 * but this should have been caught already in previous checks */
			Assert(!found_wholerow);
			if (found_wholerow)
				return NULL;
		}
		else
		{
			var = copyObject(var);
		}

		var->varno = rel->relid;

		table_close(ht_rel, NoLock);
		table_close(chunk_rel, NoLock);
	}

	skip_scan_path->distinct_var = var;

	return var;
}

/*
 * Creates SkipScanPath for each path of subpaths that is an IndexPath
 * If no subpath can be changed to SkipScanPath returns NULL
 * otherwise returns list of new paths
 */
static List *
build_subpath(PlannerInfo *root, List *subpaths, double ndistinct)
{
	bool has_skip_path = false;
	List *new_paths = NIL;
	ListCell *lc;

	foreach (lc, subpaths)
	{
		Path *child = lfirst(lc);
		if (IsA(child, IndexPath))
		{
			SkipScanPath *skip_path =
				skip_scan_path_create(root, castNode(IndexPath, child), ndistinct);

			if (skip_path)
			{
				child = (Path *) skip_path;
				has_skip_path = true;
			}
		}

		new_paths = lappend(new_paths, child);
	}

	if (!has_skip_path && new_paths)
	{
		pfree(new_paths);
		return NIL;
	}

	return new_paths;
}

static bool
build_skip_qual(PlannerInfo *root, SkipScanPath *skip_scan_path, IndexPath *index_path, Var *var)
{
	IndexOptInfo *info = index_path->indexinfo;
	Oid column_type = exprType((Node *) var);
	Oid column_collation = get_typcollation(column_type);
	TypeCacheEntry *tce = lookup_type_cache(column_type, 0);
	int idx_key = get_idx_key(info, var->varattno);

	skip_scan_path->table_attno = var->varattno;
	skip_scan_path->distinct_by_val = tce->typbyval;
	skip_scan_path->distinct_typ_len = tce->typlen;
	/* sk_attno of the skip qual */
	skip_scan_path->index_attno = idx_key + 1;

	int16 strategy = info->reverse_sort[idx_key] ? BTLessStrategyNumber : BTGreaterStrategyNumber;
	if (index_path->indexscandir == BackwardScanDirection)
	{
		strategy =
			(strategy == BTLessStrategyNumber) ? BTGreaterStrategyNumber : BTLessStrategyNumber;
	}

	Oid comparator =
		get_opfamily_member(info->sortopfamily[idx_key], column_type, column_type, strategy);
	if (!OidIsValid(comparator))
		return false; /* cannot use this index */

	Const *prev_val = makeNullConst(column_type, -1, column_collation);
	Var *current_val = makeVar(info->rel->relid /*varno*/,
							   var->varattno /*varattno*/,
							   column_type /*vartype*/,
							   -1 /*vartypmod*/,
							   column_collation /*varcollid*/,
							   0 /*varlevelsup*/);

	Expr *comparison_expr = make_opclause(comparator,
										  BOOLOID /*opresulttype*/,
										  false /*opretset*/,
										  &current_val->xpr /*leftop*/,
										  &prev_val->xpr /*rightop*/,
										  InvalidOid /*opcollid*/,
										  info->indexcollations[idx_key] /*inputcollid*/);
	set_opfuncid(castNode(OpExpr, comparison_expr));

	skip_scan_path->skip_clause = make_simple_restrictinfo_compat(root, comparison_expr);

	return true;
}

static int
get_idx_key(IndexOptInfo *idxinfo, AttrNumber attno)
{
	for (int i = 0; i < idxinfo->nkeycolumns; i++)
	{
		if (attno == idxinfo->indexkeys[i])
			return i;
	}

	elog(ERROR, "column not present in index: %d", attno);
	pg_unreachable();
}

/* Sort quals according to index column order.
 * ScanKeys need to be sorted by the position of the index column
 * they are referencing but since we don't want to adjust actual
 * ScanKey array we presort qual list when creating plan.
 */
static List *
sort_indexquals(IndexOptInfo *indexinfo, List *quals)
{
	List *indexclauses[INDEX_MAX_KEYS] = { 0 };
	List *ordered_list = NIL;
	ListCell *lc;
	int i;

	foreach (lc, quals)
	{
		Bitmapset *bms = NULL;
		pull_varattnos(lfirst(lc), INDEX_VAR, &bms);
		Assert(bms_num_members(bms) >= 1);

		i = bms_next_member(bms, -1) + FirstLowInvalidHeapAttributeNumber - 1;
		indexclauses[i] = lappend(indexclauses[i], lfirst(lc));
	}

	for (i = 0; i < INDEX_MAX_KEYS; i++)
	{
		if (indexclauses[i] != NIL)
			ordered_list = list_concat(ordered_list, indexclauses[i]);
	}

	return ordered_list;
}

static OpExpr *
fix_indexqual(IndexOptInfo *index, RestrictInfo *rinfo, AttrNumber table_attno)
{
	/* technically our placeholder col > NULL is unsatisfiable, and in some instances
	 * the planner will realize this and use is as an excuse to remove other quals.
	 * in order to prevent this, we prepare this qual ourselves.
	 */

	/* fix_indexqual_references */
	int indexcol = get_idx_key(index, table_attno);
	OpExpr *op = copyObject(castNode(OpExpr, rinfo->clause));
	Assert(list_length(op->args) == 2);
	Assert(bms_equal(rinfo->left_relids, index->rel->relids));

	/* fix_indexqual_operand */
	Assert(index->indexkeys[indexcol] != 0);
	Var *node = linitial_node(Var, op->args);
	Assert(((Var *) node)->varno == index->rel->relid &&
		   ((Var *) node)->varattno == index->indexkeys[indexcol]);

	Var *result = (Var *) copyObject(node);
	result->varno = INDEX_VAR;
	result->varattno = indexcol + 1;

	linitial(op->args) = result;

	return op;
}

/*
 * tlist_member_match_var
 *    Same as tlist_member, except that we match the provided Var on the basis
 *    of varno/varattno/varlevelsup/vartype only, rather than full equal().
 *
 * This is needed in some cases where we can't be sure of an exact typmod
 * match.  For safety, though, we insist on vartype match.
 *
 * static function copied from src/backend/optimizer/util/tlist.c
 */
static TargetEntry *
tlist_member_match_var(Var *var, List *targetlist)
{
	ListCell *temp;

	foreach (temp, targetlist)
	{
		TargetEntry *tlentry = (TargetEntry *) lfirst(temp);
		Var *tlvar = (Var *) tlentry->expr;

		if (!tlvar || !IsA(tlvar, Var))
			continue;
		if (var->varno == tlvar->varno && var->varattno == tlvar->varattno &&
			var->varlevelsup == tlvar->varlevelsup && var->vartype == tlvar->vartype)
			return tlentry;
	}
	return NULL;
}
