/*
 * The Yices SMT Solver. Copyright 2014 SRI International.
 *
 * This program may only be used subject to the noncommercial end user
 * license agreement which is downloadable along with this program.
 */

/*
 * SEARCH AND SOLVING PROCEDURES
 *
 * This module implements the check_context function (and variants).
 * It also implements model construction.
 */

#include <stdbool.h>
#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>

#include "context/context.h"
#include "solvers/simplex/simplex.h"
#include "solvers/funs/fun_solver.h"



/*
 * TRACE FUNCTIONS
 */

/*
 * Basic statistics
 */
static void trace_stats(smt_core_t *core, const char *when, uint32_t level) {
  tprintf(core->trace, level,
	  "(%-10s %8"PRIu64" %10"PRIu64" %8"PRIu64" %8"PRIu32" %8"PRIu32" %8"PRIu64" %8"PRIu32" %8"PRIu64" %7.1f)\n",
	  when, core->stats.conflicts, core->stats.decisions, core->stats.random_decisions,
	  num_binary_clauses(core), num_prob_clauses(core), num_prob_literals(core),
	  num_learned_clauses(core), num_learned_literals(core),
	  (double) num_learned_literals(core)/num_learned_clauses(core));
#if 0
  fprintf(stderr,
	  "(%-10s %8"PRIu64" %10"PRIu64" %8"PRIu64" %8"PRIu32" %8"PRIu32" %8"PRIu64" %8"PRIu32" %8"PRIu64" %7.1f)\n",
	  when, core->stats.conflicts, core->stats.decisions, core->stats.random_decisions,
	  num_binary_clauses(core), num_prob_clauses(core), num_prob_literals(core),
	  num_learned_clauses(core), num_learned_literals(core),
	  (double) num_learned_literals(core)/num_learned_clauses(core));
#endif
}

/*
 * On start_search
 */
static void trace_start(smt_core_t *core) {
  trace_stats(core, "start:", 1);
}


/*
 * On restart
 */
static void trace_restart(smt_core_t *core) {
  trace_stats(core, "restart:", 1);
}

static void trace_inner_restart(smt_core_t *core) {
  trace_stats(core, "inner restart:", 5);
}


/*
 * On reduce clause database
 */
static void trace_reduce(smt_core_t *core, uint64_t deleted) {
  trace_stats(core, "reduce:", 3);
  tprintf(core->trace, 4, "(%"PRIu64" clauses deleted)\n", deleted);
}



/*
 * End of search
 */
static void trace_done(smt_core_t *core) {
  trace_stats(core, "done:", 1);
  tnewline(core->trace, 1);
}


/*
 * MAIN SEARCH FUNCTIONS
 */

/*
 * Bounded search with the default branching heuristic (picosat-like)
 * - search until the conflict bound is reached or until the problem is solved.
 * - reduce_threshold: number of learned clauses above which reduce_clause_database is called
 * - r_factor = increment factor for reduce_threshold
 * - use the default branching heuristic implemented by the core
 */
static void search(smt_core_t *core, uint32_t conflict_bound, uint32_t *reduce_threshold, double r_factor) {
  uint64_t max_conflicts;
  uint64_t deletions;
  uint32_t r_threshold;
  literal_t l;

  assert(smt_status(core) == STATUS_SEARCHING || smt_status(core) == STATUS_INTERRUPTED);

  max_conflicts = num_conflicts(core) + conflict_bound;
  r_threshold = *reduce_threshold;

  smt_process(core);
  while (smt_status(core) == STATUS_SEARCHING && num_conflicts(core) <= max_conflicts) {
    // reduce heuristic
    if (num_learned_clauses(core) >= r_threshold) {
      deletions = core->stats.learned_clauses_deleted;
      reduce_clause_database(core);
      r_threshold = (uint32_t) (r_threshold * r_factor);
      trace_reduce(core, core->stats.learned_clauses_deleted - deletions);
    }

    // decision
    l = select_unassigned_literal(core);
    if (l == null_literal) {
      // all variables assigned: Call final_check
      smt_final_check(core);
    } else {
      decide_literal(core, l);
      smt_process(core);
    }
  }

  *reduce_threshold = r_threshold;
}


/*
 * Polarity selection (implements branching heuristics)
 * - filter is given a literal l + core and must return either l or not l
 */
typedef literal_t (*branching_fun_t)(smt_core_t *core, literal_t l);


/*
 * Bounded search with a non-default branching heuristics
 * - search until the conflict bound is reached or until the problem is solved.
 * - reduce_threshold: number of learned clauses above which reduce_clause_database is called
 * - r_factor = increment factor for reduce_threshold
 * - use the branching heuristic implemented by branch
 */
static void special_search(smt_core_t *core, uint32_t conflict_bound, uint32_t *reduce_threshold,
                           double r_factor, branching_fun_t branch) {
  uint64_t max_conflicts;
  uint64_t deletions;
  uint32_t r_threshold;
  literal_t l;

  assert(smt_status(core) == STATUS_SEARCHING || smt_status(core) == STATUS_INTERRUPTED);

  max_conflicts = num_conflicts(core) + conflict_bound;
  r_threshold = *reduce_threshold;

  smt_process(core);
  while (smt_status(core) == STATUS_SEARCHING && num_conflicts(core) <= max_conflicts) {
    // reduce heuristic
    if (num_learned_clauses(core) >= r_threshold) {
      deletions = core->stats.learned_clauses_deleted;
      reduce_clause_database(core);
      r_threshold = (uint32_t) (r_threshold * r_factor);
      trace_reduce(core, core->stats.learned_clauses_deleted - deletions);
    }

    // decision
    l = select_unassigned_literal(core);
    if (l == null_literal) {
      // all variables assigned: call final check
      smt_final_check(core);
    } else {
      // apply the branching heuristic
      l = branch(core, l);
      // propagation
      decide_literal(core, l);
      smt_process(core);
    }
  }

  *reduce_threshold = r_threshold;
}





/*
 * SUPPORTED BRANCHING
 */

/*
 * Simple branching heuristics:
 * - branch to the negative polarity
 * - branch to the positive polarity
 */
static literal_t negative_branch(smt_core_t *core, literal_t l) {
  return l | 1; // force the sign bit to 1
}

static literal_t positive_branch(smt_core_t *core, literal_t l) {
  return l & ~1; // force the sign bit to 0
}


/*
 * For literals with no atom, use the default, otherwise let the theory solver decide
 */
static literal_t theory_branch(smt_core_t *core, literal_t l) {
  if (bvar_has_atom(core, var_of(l))) {
    l =  core->th_smt.select_polarity(core->th_solver, get_bvar_atom(core, var_of(l)), l);
  }
  return l;
}

// variants
static literal_t theory_or_neg_branch(smt_core_t *core, literal_t l) {
  if (bvar_has_atom(core, var_of(l))) {
    return core->th_smt.select_polarity(core->th_solver, get_bvar_atom(core, var_of(l)), l);
  } else {
    return l | 1;
  }
}

static literal_t theory_or_pos_branch(smt_core_t *core, literal_t l) {
  if (bvar_has_atom(core, var_of(l))) {
    return core->th_smt.select_polarity(core->th_solver, get_bvar_atom(core, var_of(l)), l);
  } else {
    return l & ~1;
  }
}





/*
 * CORE SOLVER
 */


/*
 * Full solver:
 * - params: heuristic parameters.
 *   If params is NULL, the default settings are used.
 */
static void solve(smt_core_t *core, const param_t *params) {
  uint32_t c_threshold, d_threshold; // Picosat-style
  uint32_t reduce_threshold;

  assert(smt_status(core) == STATUS_IDLE);

  c_threshold = params->c_threshold;
  d_threshold = c_threshold; // required by trace_start in slow_restart mode
  if (params->fast_restart) {
    d_threshold = params->d_threshold;
  }

  reduce_threshold = (uint32_t) (num_prob_clauses(core) * params->r_fraction);
  if (reduce_threshold < params->r_threshold) {
    reduce_threshold = params->r_threshold;
  }

  // initialize then do a propagation + simplification step.
  start_search(core);
  trace_start(core);

  if (smt_status(core) == STATUS_SEARCHING) {
    // loop
    for (;;) {
      switch (params->branching) {
      case BRANCHING_DEFAULT:
        search(core, c_threshold, &reduce_threshold, params->r_factor);
        break;
      case BRANCHING_NEGATIVE:
        special_search(core, c_threshold, &reduce_threshold, params->r_factor, negative_branch);
        break;
      case BRANCHING_POSITIVE:
        special_search(core, c_threshold, &reduce_threshold, params->r_factor, positive_branch);
        break;
      case BRANCHING_THEORY:
        special_search(core, c_threshold, &reduce_threshold, params->r_factor, theory_branch);
        break;
      case BRANCHING_TH_NEG:
        special_search(core, c_threshold, &reduce_threshold, params->r_factor, theory_or_neg_branch);
        break;
      case BRANCHING_TH_POS:
        special_search(core, c_threshold, &reduce_threshold, params->r_factor, theory_or_pos_branch);
        break;
      }

      if (smt_status(core) != STATUS_SEARCHING) break;

      smt_restart(core);
      //      smt_partial_restart_var(core);

      // inner restart: increase c_threshold
      c_threshold = (uint32_t) (c_threshold * params->c_factor);

      if (c_threshold >= d_threshold) {
        d_threshold = c_threshold; // Minisat-style
        if (params->fast_restart) {
          // outer restart: reset c_threshold and increase d_threshold
          c_threshold = params->c_threshold;
          d_threshold = (uint32_t) (d_threshold * params->d_factor);
        }

	trace_restart(core);
      } else {
	trace_inner_restart(core);
      }
    }
  }

  trace_done(core);
}





/*
 * Initialize search parameters then call solve
 * - if ctx->status is not IDLE, return the status.
 */
smt_status_t check_context(context_t *ctx, const param_t *params) {
  smt_status_t stat;
  smt_core_t *core;
  egraph_t *egraph;
  simplex_solver_t *simplex;
  fun_solver_t *fsolver;
  uint32_t quota;

  core = ctx->core;
  egraph = ctx->egraph;

  stat = smt_status(core);
  if (stat == STATUS_IDLE) {
    /*
     * Clean state: search can proceed
     */
    if (params == NULL) {
      params = get_default_params();
    }

    /*
     * Set core parameters
     */
    set_randomness(core, params->randomness);
    set_random_seed(core, params->random_seed);
    set_var_decay_factor(core, params->var_decay);
    set_clause_decay_factor(core, params->clause_decay);
    if (params->cache_tclauses) {
      enable_theory_cache(core, params->tclause_size);
    } else {
      disable_theory_cache(core);
    }

    /*
     * Set egraph parameters
     */
    if (egraph != NULL) {
      if (params->use_optimistic_fcheck) {
	egraph_enable_optimistic_final_check(egraph);
      } else {
	egraph_disable_optimistic_final_check(egraph);
      }
      if (params->use_dyn_ack) {
        egraph_enable_dyn_ackermann(egraph, params->max_ackermann);
        egraph_set_ackermann_threshold(egraph, params->dyn_ack_threshold);
      } else {
        egraph_disable_dyn_ackermann(egraph);
      }
      if (params->use_bool_dyn_ack) {
        egraph_enable_dyn_boolackermann(egraph, params->max_boolackermann);
        egraph_set_boolack_threshold(egraph, params->dyn_bool_ack_threshold);
      } else {
        egraph_disable_dyn_boolackermann(egraph);
      }
      quota = egraph_num_terms(egraph) * params->aux_eq_ratio;
      if (quota < params->aux_eq_quota) {
        quota = params->aux_eq_quota;
      }
      egraph_set_aux_eq_quota(egraph, quota);
      egraph_set_max_interface_eqs(egraph, params->max_interface_eqs);
    }

    /*
     * Set simplex parameters
     */
    if (context_has_simplex_solver(ctx)) {
      simplex = ctx->arith_solver;
      if (params->use_simplex_prop) {
        simplex_enable_propagation(simplex);
        simplex_set_prop_threshold(simplex, params->max_prop_row_size);
      }
      if (params->adjust_simplex_model) {
        simplex_enable_adjust_model(simplex);
      }
      simplex_set_bland_threshold(simplex, params->bland_threshold);
      if (params->integer_check) {
        simplex_enable_periodic_icheck(simplex);
        simplex_set_integer_check_period(simplex, params->integer_check_period);
      }
    }


    /*
     * Set array solver parameters
     */
    if (context_has_fun_solver(ctx)) {
      fsolver = ctx->fun_solver;
      fun_solver_set_max_update_conflicts(fsolver, params->max_update_conflicts);
      fun_solver_set_max_extensionality(fsolver, params->max_extensionality);
    }

    solve(core, params);
    stat = smt_status(core);
  }

  return stat;
}



/*
 * Precheck: force generation of clauses and other stuff that's
 * constructed lazily by the solvers. For example, this
 * can be used to convert all the constraints asserted in the
 * bitvector to clauses so that we can export the result to DIMACS.
 *
 * If ctx status is IDLE:
 * - the function calls 'start_search' and does one round of propagation.
 * - if this results in UNSAT, the function returns UNSAT
 * - if the precheck is interrupted, the function returns INTERRRUPTED
 * - otherwise the function returns UNKNOWN and sets the status to
 *   UNKWOWN.
 *
 * IMPORTANT: call smt_clear or smt_cleanup to restore the context to
 * IDLE before doing anything else with this context.
 *
 * If ctx status is not IDLE, the function returns it and does nothing
 * else.
 */
smt_status_t precheck_context(context_t *ctx) {
  smt_status_t stat;
  smt_core_t *core;

  core = ctx->core;

  stat = smt_status(core);
  if (stat == STATUS_IDLE) {
    start_search(core);
    smt_process(core);
    stat = smt_status(core);

    assert(stat == STATUS_UNSAT || stat == STATUS_SEARCHING ||
	   stat == STATUS_INTERRUPTED);

    if (stat == STATUS_SEARCHING) {
      end_search_unknown(core);
      stat = STATUS_UNKNOWN;
    }
  }

  return stat;
}





/*
 * MODEL CONSTRUCTION
 */

/*
 * Value of literal l in ctx->core
 */
static value_t bool_value(context_t *ctx, value_table_t *vtbl, literal_t l) {
  value_t v;

  v = null_value; // prevent GCC warning
  switch (literal_value(ctx->core, l)) {
  case VAL_FALSE:
    v = vtbl_mk_false(vtbl);
    break;
  case VAL_UNDEF_FALSE:
  case VAL_UNDEF_TRUE:
    v = vtbl_mk_unknown(vtbl);
    break;
  case VAL_TRUE:
    v = vtbl_mk_true(vtbl);
    break;
  }
  return v;
}


/*
 * Value of arithmetic variable x in ctx->arith_solver
 */
static value_t arith_value(context_t *ctx, value_table_t *vtbl, thvar_t x) {
  rational_t *a;
  value_t v;

  assert(context_has_arith_solver(ctx));

  a = &ctx->aux;
  if (ctx->arith.value_in_model(ctx->arith_solver, x, a)) {
    v = vtbl_mk_rational(vtbl, a);
  } else {
    v = vtbl_mk_unknown(vtbl);
  }

  return v;
}



/*
 * Value of bitvector variable x in ctx->bv_solver
 */
static value_t bv_value(context_t *ctx, value_table_t *vtbl, thvar_t x) {
  bvconstant_t *b;
  value_t v;

  assert(context_has_bv_solver(ctx));

  b = &ctx->bv_buffer;
  if (ctx->bv.value_in_model(ctx->bv_solver, x, b)) {
    v = vtbl_mk_bv_from_constant(vtbl, b);
  } else {
    v = vtbl_mk_unknown(vtbl);
  }

  return v;
}


/*
 * Get a value for term t in the solvers or egraph
 * - attach the mapping from t to that value in model
 * - if we don't have a concrete object for t but t is
 *   mapped to a term u and the model->has_alias is true,
 *   then we store the mapping [t --> u] in the model's
 *   alias map.
 */
static void build_term_value(context_t *ctx, model_t *model, term_t t) {
  value_table_t *vtbl;
  term_t r;
  uint32_t polarity;
  int32_t x;
  type_t tau;
  value_t v;

  /*
   * Get the root of t in the substitution table
   */
  r = intern_tbl_get_root(&ctx->intern, t);
  if (intern_tbl_root_is_mapped(&ctx->intern, r)) {
    /*
     * r is mapped to some object x in egraph/core/or theory solvers
     * - keep track of polarity then force r to positive polarity
     */
    vtbl = model_get_vtbl(model);
    polarity = polarity_of(r);
    r = unsigned_term(r);

    /*
     * Convert x to a concrete value
     */
    x = intern_tbl_map_of_root(&ctx->intern, r);
    if (code_is_eterm(x)) {
      // x refers to an egraph object or true_occ/false_occ
      if (x == bool2code(true)) {
        v = vtbl_mk_true(vtbl);
      } else if (x == bool2code(false)) {
        v = vtbl_mk_false(vtbl);
      } else {
        assert(context_has_egraph(ctx));
        v = egraph_get_value(ctx->egraph, vtbl, code2occ(x));
      }

    } else {
      // x refers to a literal or a theory variable
      tau = term_type(ctx->terms, r);
      switch (type_kind(ctx->types, tau)) {
      case BOOL_TYPE:
        v = bool_value(ctx, vtbl, code2literal(x));
        break;

      case INT_TYPE:
      case REAL_TYPE:
        v = arith_value(ctx, vtbl, code2thvar(x));
        break;

      case BITVECTOR_TYPE:
        v = bv_value(ctx, vtbl, code2thvar(x));
        break;

      default:
        /*
         * This should never happen:
         * scalar, uninterpreted, tuple, function terms
         * are mapped to egraph terms.
         */
        assert(false);
        v = vtbl_mk_unknown(vtbl); // prevent GCC warning
        break;
      }
    }

    /*
     * Restore polarity then add mapping [t --> v] in the model
     */
    if (! object_is_unknown(vtbl, v)) {
      if (object_is_boolean(vtbl, v)) {
        if (polarity) {
          // negate the value
          v = vtbl_mk_not(vtbl, v);
        }
      }
      model_map_term(model, t, v);
    }

  } else {
    /*
     * r is not mapped to anything:
     *
     * 1) If t == r and t is present in the internalization table
     *    then t is relevant. So we should display its value
     *    when we print the model. To do this, we assign an
     *    arbitrary value v to t and store [t := v] in the map.
     *
     * 2) If t != r then we keep the mapping [t --> r] in
     *    the alias table (provided the model supports aliases).
     */
    if (t == r) {
      if (intern_tbl_term_present(&ctx->intern, t)) {
	tau = term_type(ctx->terms, t);
	vtbl = model_get_vtbl(model);
	v = vtbl_make_object(vtbl, tau);
	model_map_term(model, t, v);
      }
    } else if (model->has_alias) {
      // t != r: keep the substitution [t --> r] in the model
      model_add_substitution(model, t, r);
    }
  }
}




/*
 * Build a model for the current context
 * - the context status must be SAT (or UNKNOWN)
 * - if model->has_alias is true, we store the term substitution
 *   defined by ctx->intern_tbl into the model
 */
void context_build_model(model_t *model, context_t *ctx) {
  term_table_t *terms;
  uint32_t i, n;
  term_t t;

  assert(smt_status(ctx->core) == STATUS_SAT || smt_status(ctx->core) == STATUS_UNKNOWN);

  /*
   * First build assignments in the satellite solvers
   * and get the val_in_model functions for the egraph
   */
  if (context_has_arith_solver(ctx)) {
    ctx->arith.build_model(ctx->arith_solver);
  }
  if (context_has_bv_solver(ctx)) {
    ctx->bv.build_model(ctx->bv_solver);
  }

  /*
   * Construct the egraph model
   */
  if (context_has_egraph(ctx)) {
    egraph_build_model(ctx->egraph, model_get_vtbl(model));
  }

  // scan the internalization table
  terms = ctx->terms;
  n = intern_tbl_num_terms(&ctx->intern);
  for (i=1; i<n; i++) { // first real term has index 1 (i.e. true_term)
    t = pos_occ(i);
    if (term_kind(terms, t) == UNINTERPRETED_TERM) {
      build_term_value(ctx, model, t);
    }
  }

  /*
   * Cleanup
   */
  if (context_has_arith_solver(ctx)) {
    ctx->arith.free_model(ctx->arith_solver);
  }
  if (context_has_bv_solver(ctx)) {
    ctx->bv.free_model(ctx->bv_solver);
  }
  if (context_has_egraph(ctx)) {
    egraph_free_model(ctx->egraph);
  }

}




/*
 * Read the value of a Boolean term t
 * - return VAL_TRUE/VAL_FALSE or VAL_UNDEF_FALSE/VAL_UNDEF_TRUE if t's value is not known
 */
bval_t context_bool_term_value(context_t *ctx, term_t t) {
  term_t r;
  uint32_t polarity;
  int32_t x;
  bval_t v;

  assert(is_boolean_term(ctx->terms, t));

  // default value if t is not in the internalization table
  v = VAL_UNDEF_FALSE;
  r = intern_tbl_get_root(&ctx->intern, t);
  if (intern_tbl_root_is_mapped(&ctx->intern, r)) {
    // r is mapped to some object x
    polarity = polarity_of(r);
    r = unsigned_term(r);

    x  = intern_tbl_map_of_root(&ctx->intern, r);
    if (code_is_eterm(x)) {
      // x must be either true_occ or false_occ
      if (x == bool2code(true)) {
	v = VAL_TRUE;
      } else {
	assert(x == bool2code(false));
	v = VAL_FALSE;
      }
    } else {
      // x refers to a literal in the smt core
      v = literal_value(ctx->core, code2literal(x));
    }

    // negate v if polarity is 1 (cf. smt_core_base_types.h)
    v ^= polarity;
  }

  return v;
}