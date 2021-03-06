#include "aqo.h"

/*****************************************************************************
 *
 *	QUERY EXECUTION STATISTICS COLLECTING UNTILITIES
 *
 * The module which updates data in the feature space linked with executed query
 * type using obtained query execution statistics.
 * Works only if aqo_learn is on.
 *
 *****************************************************************************/

static double	cardinality_sum_errors;
static int		cardinality_num_objects;

/* Query execution statictics collecting utilities */
static void atomic_fss_learn_step(int fss_hash, int matrix_cols,
					  double **matrix, double *targets,
					  double *features, double target);
static void learn_sample(List *clauselist,
			 List *selectivities,
			 List *relidslist,
			 double true_cardinality,
			 double predicted_cardinality);
static List *restore_selectivities(List *clauselist,
					  List *relidslist,
					  JoinType join_type,
					  bool was_parametrized);
static void collect_planstat(PlanState *p, List **other_plans,
				 List **clauselist,
				 List **selectivities,
				 List **relidslist);
static void update_query_stat_row(double *et, int *et_size,
					  double *pt, int *pt_size,
					  double *ce, int *ce_size,
					  double planning_time,
					  double execution_time,
					  double cardinality_error,
					  long long *n_exec);


/*
 * This is the critical section: only one runner is allowed to be inside this
 * function for one feature subspace.
 * matrix and targets are just pre-allocated memory for computations.
 */
void
atomic_fss_learn_step(int fss_hash, int matrix_cols,
					  double **matrix, double *targets,
					  double *features, double target)
{
	int			matrix_rows;
	int			new_matrix_rows;
	List	   *changed_lines = NIL;
	ListCell   *l;

	if (!load_fss(fss_hash, matrix_cols, matrix, targets, &matrix_rows))
		matrix_rows = 0;

	changed_lines = OkNNr_learn(matrix_rows, matrix_cols,
								matrix, targets,
								features, target);

	new_matrix_rows = matrix_rows;
	foreach(l, changed_lines)
	{
		if (lfirst_int(l) >= new_matrix_rows)
			new_matrix_rows = lfirst_int(l) + 1;
	}
	update_fss(fss_hash, new_matrix_rows, matrix_cols, matrix, targets,
			   matrix_rows, changed_lines);
}

/*
 * For given object (i. e. clauselist, selectivities, relidslist, predicted and
 * true cardinalities) performs learning procedure.
 */
void
learn_sample(List *clauselist, List *selectivities, List *relidslist,
			 double true_cardinality, double predicted_cardinality)
{
	int			fss_hash;
	int			matrix_cols;
	double	  **matrix;
	double	   *targets;
	double	   *features;
	double		target;
	int			i;

	cardinality_sum_errors += fabs(log(predicted_cardinality) -
								   log(true_cardinality));
	cardinality_num_objects += 1;

	if (fabs(log(predicted_cardinality) - log(true_cardinality)) <
		object_selection_prediction_threshold)
		return;

	target = log(true_cardinality);

	get_fss_for_object(clauselist, selectivities, relidslist,
					   &matrix_cols, &fss_hash, &features);

	matrix = palloc(sizeof(*matrix) * aqo_K);
	for (i = 0; i < aqo_K; ++i)
		matrix[i] = palloc0(sizeof(**matrix) * matrix_cols);
	targets = palloc0(sizeof(*targets) * aqo_K);

	/* Here should be critical section */
	atomic_fss_learn_step(fss_hash, matrix_cols, matrix, targets,
						  features, target);
	/* Here should be the end of critical section */

	for (i = 0; i < aqo_K; ++i)
		pfree(matrix[i]);
	pfree(matrix);
	pfree(features);
	pfree(targets);
}

/*
 * For given node specified by clauselist, relidslist and join_type restores
 * the same selectivities of clauses as were used at query optimization stage.
 */
List *
restore_selectivities(List *clauselist,
					  List *relidslist,
					  JoinType join_type,
					  bool was_parametrized)
{
	List	   *lst = NIL;
	ListCell   *l;
	int			i = 0;
	bool		parametrized_sel;
	int         nargs;
	int        *args_hash;
	int        *eclass_hash;
	double	   *cur_sel;
	int			cur_hash;
	int			cur_relid;

	parametrized_sel = was_parametrized && (list_length(relidslist) == 1);
	if (parametrized_sel)
	{
		cur_relid = linitial_int(relidslist);
		get_eclasses(clauselist, &nargs, &args_hash, &eclass_hash);
	}

	foreach(l, clauselist)
	{
		RestrictInfo *rinfo = (RestrictInfo *) lfirst(l);

		cur_sel = NULL;
		if (parametrized_sel)
		{
			cur_hash = get_clause_hash(rinfo->clause, nargs,
									   args_hash, eclass_hash);
			cur_sel = selectivity_cache_find_global_relid(cur_hash, cur_relid);
			if (cur_sel == NULL)
			{
				if (join_type == JOIN_INNER)
					cur_sel = &rinfo->norm_selec;
				else
					cur_sel = &rinfo->outer_selec;
			}
		}
		else if (join_type == JOIN_INNER)
			cur_sel = &rinfo->norm_selec;
		else
			cur_sel = &rinfo->outer_selec;

		lst = lappend(lst, cur_sel);
		i++;
	}

	if (parametrized_sel)
	{
		pfree(args_hash);
		pfree(eclass_hash);
	}

	return lst;
}

/*
 * Walks over obtained PlanState tree, collects relation objects with their
 * clauses, selectivivties and relids and passes each object to learn_sample.
 *
 * Returns clauselist, selectivities and relids.
 * Store observed subPlans into other_plans list.
 */
void
collect_planstat(PlanState *p, List **other_plans,
				 List **clauselist, List **selectivities, List **relidslist)
{
	double		learn_rows;
	List	   *cur_clauselist = NIL;
	List	   *cur_relidslist = NIL;
	List	   *cur_selectivities = NIL;
	ListCell   *l;

	foreach(l, p->subPlan)
		*other_plans = lappend(*other_plans, lfirst(l));

	if (p->lefttree == NULL && p->righttree != NULL)
	{
		elog(WARNING, "failed to parse planstat");
		return;
	}
	if (p->lefttree != NULL && p->righttree == NULL)
		collect_planstat(p->lefttree, other_plans,
						 clauselist, selectivities, relidslist);
	if (p->lefttree != NULL && p->righttree != NULL)
	{
		collect_planstat(p->lefttree, other_plans,
						 clauselist, selectivities, relidslist);
		collect_planstat(p->righttree, other_plans,
					   &cur_clauselist, &cur_selectivities, &cur_relidslist);
		(*clauselist) = list_concat(cur_clauselist, (*clauselist));
		(*relidslist) = list_concat(cur_relidslist, (*relidslist));
		(*selectivities) = list_concat(cur_selectivities, (*selectivities));
	}

	if (p->plan->had_path)
	{
		cur_selectivities = restore_selectivities(p->plan->path_clauses,
												  p->plan->path_relids,
												  p->plan->path_jointype,
												  p->plan->was_parametrized);

		(*clauselist) = list_concat(p->plan->path_clauses, (*clauselist));
		if (p->plan->path_relids != NIL)
			(*relidslist) = p->plan->path_relids;
		(*selectivities) = list_concat(cur_selectivities, (*selectivities));
		if (p->instrument && (p->righttree != NULL ||
							  p->lefttree == NULL ||
							  p->plan->path_clauses != NIL))
		{
			InstrEndLoop(p->instrument);
			if (p->instrument->nloops >= 0.5)
			{
				learn_rows = p->instrument->ntuples / p->instrument->nloops;
				if (p->plan->path_parallel_workers > 0 && p->lefttree == NULL && p->righttree == NULL)
					learn_rows *= (p->plan->path_parallel_workers + 1);
				if (learn_rows < 1)
					learn_rows = 1;
			}
			else
				learn_rows = 1;

			if (!(p->instrument->ntuples == 0 && p->instrument->nloops == 0))
				learn_sample(*clauselist, *selectivities, *relidslist,
							 learn_rows, p->plan->plan_rows);
		}
	}
}

/*
 * Updating given row of query statistics.
 */
void
update_query_stat_row(double *et, int *et_size,
					  double *pt, int *pt_size,
					  double *ce, int *ce_size,
					  double planning_time,
					  double execution_time,
					  double cardinality_error,
					  long long *n_exec)
{
	int			i;

	if (cardinality_error >= 0)
	{
		if (*ce_size >= aqo_stat_size)
			for (i = 1; i < aqo_stat_size; ++i)
				ce[i - 1] = ce[i];
		*ce_size = (*ce_size >= aqo_stat_size) ? aqo_stat_size : (*ce_size + 1);
		ce[*ce_size - 1] = cardinality_error;
	}
	if (*et_size >= aqo_stat_size)
		for (i = 1; i < aqo_stat_size; ++i)
			et[i - 1] = et[i];
	*et_size = (*et_size >= aqo_stat_size) ? aqo_stat_size : (*et_size + 1);
	et[*et_size - 1] = execution_time;
	if (*pt_size >= aqo_stat_size)
		for (i = 1; i < aqo_stat_size; ++i)
			pt[i - 1] = pt[i];
	*pt_size = (*pt_size >= aqo_stat_size) ? aqo_stat_size : (*pt_size + 1);
	pt[*pt_size - 1] = planning_time;
	(*n_exec)++;
}

/*****************************************************************************
 *
 *	QUERY EXECUTION STATISTICS COLLECTING HOOKS
 *
 *****************************************************************************/

/*
 * Set up flags to store cardinality statistics.
 */
void
aqo_ExecutorStart(QueryDesc *queryDesc, int eflags)
{
	instr_time	current_time;

	INSTR_TIME_SET_CURRENT(current_time);
	INSTR_TIME_SUBTRACT(current_time, query_starttime);
	query_planning_time = INSTR_TIME_GET_DOUBLE(current_time);

	explain_only = ((eflags & EXEC_FLAG_EXPLAIN_ONLY) != 0);

	if (learn_aqo && !explain_only)
		queryDesc->instrument_options |= INSTRUMENT_ROWS;

	if (prev_ExecutorStart_hook)
		(*prev_ExecutorStart_hook) (queryDesc, eflags);
	else
		standard_ExecutorStart(queryDesc, eflags);
}

/*
 * Retranslates path info to plan node for collecting it after query execution.
 */
void
aqo_copy_generic_path_info(PlannerInfo *root, Plan *dest, Path *src)
{
	bool		is_join_path;

	is_join_path = (src->type == T_NestPath || src->type == T_MergePath ||
					src->type == T_HashPath);

	dest->had_path = true;
	if (is_join_path)
	{
		dest->path_clauses = ((JoinPath *) src)->joinrestrictinfo;
		dest->path_jointype = ((JoinPath *) src)->jointype;
	}
	else
	{
		dest->path_clauses = list_concat(
									list_copy(src->parent->baserestrictinfo),
						 src->param_info ? src->param_info->ppi_clauses : NIL
			);
		dest->path_jointype = JOIN_INNER;
	}
	dest->path_relids = get_list_of_relids(root, src->parent->relids);
	dest->path_parallel_workers = src->parallel_workers;
	dest->was_parametrized = (src->param_info != NULL);

	if (prev_copy_generic_path_info_hook)
		(*prev_copy_generic_path_info_hook) (root, dest, src);
}

/*
 * General hook which runs before ExecutorEnd and collects query execution
 * cardinality statistics.
 * Also it updates query execution statistics in aqo_query_stat.
 */
void
learn_query_stat(QueryDesc *queryDesc)
{
	List	   *other_plans = NIL;
	PlanState  *to_walk;
	List	   *tmp_clauselist = NIL;
	List	   *tmp_relidslist = NIL;
	List	   *tmp_selectivities = NIL;
	double		totaltime;
	double		cardinality_error;
	QueryStat  *stat;
	instr_time	endtime;

	if (explain_only)
	{
		learn_aqo = false;
		collect_stat = false;
	}

	if (learn_aqo)
	{
		cardinality_sum_errors = 0;
		cardinality_num_objects = 0;

		other_plans = lappend(other_plans, queryDesc->planstate);
		while (list_length(other_plans) != 0)
		{
			to_walk = lfirst(list_head(other_plans));
			if (to_walk->type == T_SubPlanState)
				to_walk = ((SubPlanState *) to_walk)->planstate;
			collect_planstat(to_walk, &other_plans, &tmp_clauselist,
							 &tmp_selectivities, &tmp_relidslist);
			other_plans = list_delete_first(other_plans);
		}
	}

	if (collect_stat)
	{
		INSTR_TIME_SET_CURRENT(endtime);
		INSTR_TIME_SUBTRACT(endtime, query_starttime);
		totaltime = INSTR_TIME_GET_DOUBLE(endtime);
		if (learn_aqo && cardinality_num_objects)
			cardinality_error = cardinality_sum_errors /
				cardinality_num_objects;
		else
			cardinality_error = -1;

		stat = get_aqo_stat(fspace_hash);
		if (stat != NULL)
		{
			if (use_aqo)
				update_query_stat_row(stat->execution_time_with_aqo,
									  &stat->execution_time_with_aqo_size,
									  stat->planning_time_with_aqo,
									  &stat->planning_time_with_aqo_size,
									  stat->cardinality_error_with_aqo,
									  &stat->cardinality_error_with_aqo_size,
									  query_planning_time,
									  totaltime - query_planning_time,
									  cardinality_error,
									  &stat->executions_with_aqo);
			else
				update_query_stat_row(stat->execution_time_without_aqo,
									  &stat->execution_time_without_aqo_size,
									  stat->planning_time_without_aqo,
									  &stat->planning_time_without_aqo_size,
									  stat->cardinality_error_without_aqo,
									  &stat->cardinality_error_without_aqo_size,
									  query_planning_time,
									  totaltime - query_planning_time,
									  cardinality_error,
									  &stat->executions_without_aqo);
			if (!adding_query && auto_tuning)
				automatical_query_tuning(query_hash, stat);
			update_aqo_stat(fspace_hash, stat);
			pfree_query_stat(stat);
		}
	}
	selectivity_cache_clear();

	disable_aqo_for_query();

	if (prev_ExecutorEnd_hook)
		(*prev_ExecutorEnd_hook) (queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}
