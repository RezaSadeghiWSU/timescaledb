#include <postgres.h>
#include <nodes/parsenodes.h>
#include <nodes/nodeFuncs.h>
#include <nodes/makefuncs.h>
#include <parser/parsetree.h>
#include <parser/parse_coerce.h>
#include <parser/parse_func.h>
#include <parser/parse_collate.h>
#include <parser/parse_oper.h>
#include <optimizer/clauses.h>
#include <catalog/pg_type.h>
#include <optimizer/paths.h>

#include "cache.h"
#include "hypertable.h"
#include "partitioning.h"
#include "compat.h"

#include "space_partition_exclude.h"
#include "chunk.h"
#include "hypercube.h"
#include "hypertable.h"

typedef struct SpaceExcludeCtx
{
	Query	   *parse;
	Hypertable *hentry;
} SpaceExcludeCtx;



static inline Dimension *
get_dimension_for_partition_column_var(Var *var_expr, SpaceExcludeCtx *context)
{
	Hypertable *ht = context->hentry;
	RangeTblEntry *rte = rt_fetch(var_expr->varno, context->parse->rtable);
	char	   *varname;

	if (rte->relid != ht->main_table_relid)
		return NULL;

	varname = get_rte_attribute_name(rte, var_expr->varattno);

	return hyperspace_get_dimension_by_name(ht->space, DIMENSION_TYPE_CLOSED, varname);
}

SpacePartitionExclude *
space_partition_exclude_get(RestrictInfo *base_restrict_info, Query *query, Hypertable *hentry)
{
	Expr	   *node = base_restrict_info->clause;

	if (IsA(node, OpExpr))
	{
		OpExpr	   *exp = (OpExpr *) node;

		if (list_length(exp->args) == 2)
		{
			/* only look at var op const or const op var; */
			Node	   *left = (Node *) linitial(exp->args);
			Node	   *right = (Node *) lsecond(exp->args);
			Var		   *var_expr = NULL;
			Node	   *other_expr = NULL;

			if (IsA(left, Var))
			{
				var_expr = (Var *) left;
				other_expr = right;
			}
			else if (IsA(right, Var))
			{
				var_expr = (Var *) right;
				other_expr = left;
			}

			if (var_expr != NULL)
			{
				if (!IsA(other_expr, Const))
				{
					/* try to simplify the non-var expression */
					other_expr = eval_const_expressions(NULL, other_expr);
				}
				if (IsA(other_expr, Const))
				{
					/* have a var and const, make sure the op is = */
					Const	   *const_expr = (Const *) other_expr;
					Oid			eq_oid = OpernameGetOprid(list_make2(makeString("pg_catalog"), makeString("=")), exprType(left), exprType(right));

					if (eq_oid == exp->opno)
					{
						/*
						 * I now have a var = const. Make sure var is a
						 * partitioning column
						 */
						SpaceExcludeCtx context = (SpaceExcludeCtx) {
							.parse = query,
							.hentry = hentry,
						};

						Dimension  *dim =
						get_dimension_for_partition_column_var(var_expr,
															   &context);

						if (dim != NULL)
						{
							int32		hash_value =
							partitioning_func_apply(dim->partitioning, const_expr->constvalue);
							SpacePartitionExclude *spe = palloc(sizeof(SpacePartitionExclude));

							*spe = (SpacePartitionExclude)
							{
								.dim = dim,
									.value = hash_value,
							};
							return spe;
						}
					}
				}
			}
		}
	}
	return NULL;
}

void
space_partition_exclude_apply(SpacePartitionExclude *spe,
							  PlannerInfo *root,
							  Hypertable *ht,
							  Index main_table_child_index)
{
	ListCell   *lsib;

	/*
	 * Go through all the append rel list and find all children that are
	 * chunks. Then check the chunk for exclusion and make dummy if necessary
	 */
	foreach(lsib, root->append_rel_list)
	{
		AppendRelInfo *appinfo = (AppendRelInfo *) lfirst(lsib);
		RelOptInfo *siblingrel;
		RangeTblEntry *siblingrte;
		Chunk	   *chunk;
		DimensionSlice *ds;

		/* find all chunks: children that are not the main table */
		if (appinfo->parent_reloid != ht->main_table_relid || appinfo->child_relid == main_table_child_index)
			continue;

		siblingrel = root->simple_rel_array[appinfo->child_relid];
		siblingrte = root->simple_rte_array[appinfo->child_relid];
		chunk = chunk_get_by_relid(siblingrte->relid, ht->space->num_dimensions, true);
		ds = hypercube_get_slice_by_dimension_id(chunk->cube, spe->dim->fd.id);

		/* If the hash value is not inside the ds, exclude the chunk */
		if (dimension_slice_cmp_coordinate(ds, (int64) spe->value) != 0)
			set_dummy_rel_pathlist(siblingrel);
	}
}
