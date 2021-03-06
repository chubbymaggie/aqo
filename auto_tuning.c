#include "aqo.h"

/*****************************************************************************
 *
 *	AUTOMATICAL QUERY TUNING
 *
 * This module automatically implements basic strategies of tuning aqo for best
 * PostgreSQL performance.
 *
 *****************************************************************************/

static double get_estimation(double *elems, int nelems);
static bool converged_cq(double *elems, int nelems);


/*
 * Having a time series it tries to predict its next value.
 * Now it do simple window averaging.
 */
double
get_estimation(double *elems, int nelems)
{
	double		sum = 0;
	int			i;

	for (i = 0; i < auto_tuning_window_size; ++i)
		sum += elems[nelems - 1 - i];
	return sum / auto_tuning_window_size;
}

/*
 * Tests whether cardinality qualities series is converged, i. e. learning
 * process may be considered as finished.
 * Now it checks whether the cardinality quality stopped decreasing with
 * absolute or relative error 0.1.
 */
bool
converged_cq(double *elems, int nelems)
{
	double		est;

	if (nelems < auto_tuning_window_size + 2)
		return false;

	est = get_estimation(elems, nelems - 1);
	return (est * 1.1 > elems[nelems - 1] || est + 0.1 > elems[nelems - 1]) &&
			(est * 0.9 < elems[nelems - 1] || est - 0.1 < elems[nelems - 1]);
}

/*
 * Here we use execution statistics for the given query tuning. Note that now
 * we cannot execute queries on our own wish, so the tuning now is in setting
 * use_aqo and learn_aqo parameters for the query type.
 *
 * Now the workflow is quite simlple:
 *
 * Firstly, we run a new query type auto_tuning_window_size times without our
 * method to have an execution time statistics for such type of quieries.
 * Secondly, we run the query type with both aqo usage and aqo learning enabled
 * until convergence.
 *
 * If aqo provides better execution time for the query type according to
 * collected statistics, we prefer to enable it, otherwise we prefer to disable
 * it.
 * In the stable workload case we perform an exploration. That means that with
 * some probability which depends on execution time with and without using aqo
 * we run the slower method to check whether it remains slower.
 * Cardinality statistics collection is enabled by default in this mode.
 * If we find out that cardinality quality diverged during the exploration, we
 * return to step 2 and run the query type with both aqo usage and aqo learning
 * enabled until convergence.
 */
void
automatical_query_tuning(int query_hash, QueryStat * stat)
{
	double		unstability = auto_tuning_exploration;
	double		t_aqo,
				t_not_aqo;
	double		p_use;

	learn_aqo = true;
	if (stat->executions_without_aqo < auto_tuning_window_size)
		use_aqo = false;
	else if (!converged_cq(stat->cardinality_error_with_aqo,
						   stat->cardinality_error_with_aqo_size))
		use_aqo = true;
	else
	{
		t_aqo = get_estimation(stat->execution_time_with_aqo,
							   stat->execution_time_with_aqo_size) +
				get_estimation(stat->planning_time_with_aqo,
							   stat->planning_time_with_aqo_size);
		t_not_aqo = get_estimation(stat->execution_time_without_aqo,
								   stat->execution_time_without_aqo_size) +
					get_estimation(stat->planning_time_without_aqo,
								   stat->planning_time_without_aqo_size);
		p_use = t_not_aqo / (t_not_aqo + t_aqo);
		p_use = 1 / (1 + exp((p_use - 0.5) / unstability));
		p_use -= 1 / (1 + exp(-0.5 / unstability));
		p_use /= 1 - 2 / (1 + exp(-0.5 / unstability));

		use_aqo = ((double) rand() / RAND_MAX < p_use);
		learn_aqo = use_aqo;
	}

	update_query(query_hash, learn_aqo, use_aqo, fspace_hash, true);
}
