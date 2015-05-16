/*
 * This file is part of libFirm.
 * Copyright (C) 2012 University of Karlsruhe.
 */

/**
 * @file
 * @brief       Main Backend driver.
 * @author      Sebastian Hack
 * @date        25.11.2004
 */

#include <stdio.h>

#include "lc_opts.h"
#include "lc_opts_enum.h"

#include "ident_t.h"
#include "obst.h"
#include "statev.h"
#include "irprog.h"
#include "irgopt.h"
#include "irdump.h"
#include "irdom_t.h"
#include "iredges_t.h"
#include "irloop_t.h"
#include "irtools.h"
#include "irverify.h"
#include "iroptimize.h"
#include "firmstat.h"
#include "execfreq_t.h"
#include "irprofile.h"
#include "ircons.h"
#include "util.h"

#include "bearch.h"
#include "be_t.h"
#include "bediagnostic.h"
#include "begnuas.h"
#include "bemodule.h"
#include "beutil.h"
#include "benode.h"
#include "besched.h"
#include "belistsched.h"
#include "belive.h"
#include "bera.h"
#include "bechordal_t.h"
#include "beifg.h"
#include "becopystat.h"
#include "bessadestr.h"
#include "belower.h"
#include "bespillutil.h"
#include "bestat.h"
#include "beverify.h"
#include "beirg.h"
#include "bestack.h"
#include "beemitter.h"

/* options visible for anyone */
be_options_t be_options = {
	.dump_flags           = DUMP_NONE,
	.timing               = false,
	.opt_profile_generate = false,
	.opt_profile_use      = false,
	.omit_fp              = false,
	.pic                  = false,
	.do_verify            = true,
	.ilp_server           = "",
	.ilp_solver           = "",
	.verbose_asm          = true,
};

/* back end instruction set architecture to use */
arch_isa_if_t const *isa_if = NULL;

/* possible dumping options */
static const lc_opt_enum_mask_items_t dump_items[] = {
	{ "none",       DUMP_NONE },
	{ "initial",    DUMP_INITIAL },
	{ "sched",      DUMP_SCHED  },
	{ "prepared",   DUMP_PREPARED },
	{ "regalloc",   DUMP_RA },
	{ "final",      DUMP_FINAL },
	{ "be",         DUMP_BE },
	{ "all",        2 * DUMP_BE - 1 },
	{ NULL,         0 }
};

static lc_opt_enum_mask_var_t dump_var = {
	&be_options.dump_flags, dump_items
};

static const lc_opt_table_entry_t be_main_options[] = {
	LC_OPT_ENT_ENUM_MASK("dump",       "dump irg on several occasions",                       &dump_var),
	LC_OPT_ENT_BOOL     ("omitfp",     "omit frame pointer",                                  &be_options.omit_fp),
	LC_OPT_ENT_BOOL     ("pic",        "create PIC code",                                     &be_options.pic),
	LC_OPT_ENT_BOOL     ("verify",     "verify the backend irg",                              &be_options.do_verify),
	LC_OPT_ENT_BOOL     ("time",       "get backend timing statistics",                       &be_options.timing),
	LC_OPT_ENT_BOOL     ("profilegenerate", "instrument the code for execution count profiling", &be_options.opt_profile_generate),
	LC_OPT_ENT_BOOL     ("profileuse",      "use existing profile data",                         &be_options.opt_profile_use),
	LC_OPT_ENT_BOOL     ("verboseasm", "enable verbose assembler output",                        &be_options.verbose_asm),

	LC_OPT_ENT_STR("ilp.server", "the ilp server name", &be_options.ilp_server),
	LC_OPT_ENT_STR("ilp.solver", "the ilp solver name", &be_options.ilp_solver),
	LC_OPT_LAST
};

static be_module_list_entry_t *isa_ifs         = NULL;
static bool                    isa_initialized = false;

asm_constraint_flags_t be_asm_constraint_flags[256];

void be_set_constraint_support(asm_constraint_flags_t const flags, char const *const constraints)
{
	for (char const *i = constraints; *i != '\0'; ++i) {
		be_asm_constraint_flags[(unsigned char)*i] = flags;
	}
}

static void be_init_default_asm_constraint_flags(void)
{
	for (size_t i = 0; i != ARRAY_SIZE(be_asm_constraint_flags); ++i) {
		be_asm_constraint_flags[i] = ASM_CONSTRAINT_FLAG_INVALID;
	}

	be_set_constraint_support(ASM_CONSTRAINT_FLAG_MODIFIER_EARLYCLOBBER, "&");

	/* List of constraints supported by gcc for any machine (or at least
	 * recognized). Mark them as NO_SUPPORT so we can differentiate them
	 * from INVALID. Backends should change the flags they support. */
	char const *const gcc_common_flags = "%,0123456789<>EFGHIJKLMNOPVXgimoprs";
	be_set_constraint_support(ASM_CONSTRAINT_FLAG_NO_SUPPORT, gcc_common_flags);
	/* Skip whitespace.
	 * TODO '*' actually penalizes the selection of the next constraint letter.
	 * We do not support this, yet.
	 * TODO '!' and '?' actually penalize an alternative of a multi alternative
	 * constraint.  We do not support this, yet. */
	be_set_constraint_support(ASM_CONSTRAINT_FLAG_NONE, "\t\n\r !*?");
}

static void initialize_isa(void)
{
	if (isa_initialized)
		return;
	be_init_default_asm_constraint_flags();
	isa_if->init();
	isa_initialized = true;
}

static void finish_isa(void)
{
	if (isa_initialized) {
		isa_if->finish();
		isa_initialized = false;
	}
}

asm_constraint_flags_t be_parse_asm_constraints(const char *constraint)
{
	initialize_isa();

	asm_constraint_flags_t flags;
	char            const *c = constraint;
	switch (*c) {
	case '=': ++c; flags = ASM_CONSTRAINT_FLAG_MODIFIER_WRITE; break;
	case '+': ++c; flags = ASM_CONSTRAINT_FLAG_MODIFIER_READ | ASM_CONSTRAINT_FLAG_MODIFIER_WRITE; break;
	default:       flags = ASM_CONSTRAINT_FLAG_MODIFIER_READ; break;
	}

	for (; *c != '\0'; ++c) {
		switch (*c) {
		case '#':
			/* text until comma is a comment */
			while (*c != 0 && *c != ',')
				++c;
			break;

		default:
			flags |= be_asm_constraint_flags[(unsigned char)*c];
			break;
		}
	}

	return flags;
}

int be_is_valid_clobber(const char *clobber)
{
	initialize_isa();

	/* memory is a valid clobber. (the frontend has to detect this case too,
	 * because it has to add memory edges to the asm) */
	if (strcmp(clobber, "memory") == 0)
		return 1;
	/* cc (condition code) is always valid */
	if (strcmp(clobber, "cc") == 0)
		return 1;

	return isa_if->is_valid_clobber(clobber);
}

void be_register_isa_if(const char *name, const arch_isa_if_t *isa)
{
	if (isa_if == NULL)
		isa_if = isa;

	be_add_module_to_list(&isa_ifs, name, (void*) isa);
}

static void be_opt_register(void)
{
	static bool run_once = false;
	if (run_once)
		return;
	run_once = true;

	lc_opt_entry_t *be_grp = lc_opt_get_grp(firm_opt_get_root(), "be");
	lc_opt_add_table(be_grp, be_main_options);

	be_add_module_list_opt(be_grp, "isa", "the instruction set architecture",
	                       &isa_ifs, (void**) &isa_if);

	be_init_modules();
}

/* Parse one argument. */
int be_parse_arg(const char *arg)
{
	lc_opt_entry_t *be_grp = lc_opt_get_grp(firm_opt_get_root(), "be");
	if (strcmp(arg, "help") == 0 || (arg[0] == '?' && arg[1] == '\0')) {
		lc_opt_print_help_for_entry(be_grp, '-', stdout);
		return -1;
	}
	return lc_opt_from_single_arg(be_grp, arg);
}

void be_check_verify_result(bool fine, ir_graph *irg)
{
	if (!fine) {
		be_errorf(NULL, "verifier failed; trying to write assert graph and abort");
		dump_ir_graph(irg, "assert");
		abort();
	}
}

/* Perform schedule verification if requested. */
static void be_sched_verify(ir_graph *irg)
{
	if (be_options.do_verify) {
		be_timer_push(T_VERIFY);
		bool fine = be_verify_schedule(irg);
		be_check_verify_result(fine, irg);
		be_timer_pop(T_VERIFY);
	}
}

static void be_regalloc_verify(ir_graph *const irg, bool const ignore_sp_problems)
{
	if (be_options.do_verify) {
		be_timer_push(T_VERIFY);
		bool const fine = be_verify_register_allocation(irg, ignore_sp_problems);
		be_check_verify_result(fine, irg);
		be_timer_pop(T_VERIFY);
	}
}

/* Initialize the Firm backend. Must be run first in init_firm()! */
void firm_be_init(void)
{
	be_opt_register();
	be_init_modules();
}

/* Finalize the Firm backend. */
void firm_be_finish(void)
{
	finish_isa();
	be_quit_modules();
}

/* Returns the backend parameter */
const backend_params *be_get_backend_param(void)
{
	initialize_isa();
	return isa_if->get_params();
}

int be_is_big_endian(void)
{
	return be_get_backend_param()->byte_order_big_endian;
}

unsigned be_get_machine_size(void)
{
	return be_get_backend_param()->machine_size;
}

ir_mode *be_get_mode_float_arithmetic(void)
{
	return be_get_backend_param()->mode_float_arithmetic;
}

ir_type *be_get_type_long_long(void)
{
	return be_get_backend_param()->type_long_long;
}

ir_type *be_get_type_unsigned_long_long(void)
{
	return be_get_backend_param()->type_unsigned_long_long;
}

ir_type *be_get_type_long_double(void)
{
	return be_get_backend_param()->type_long_double;
}

float_int_conversion_overflow_style_t be_get_float_int_overflow(void)
{
	return be_get_backend_param()->float_int_overflow;
}

/**
 * Initializes the main environment for the backend.
 *
 * @param env          an empty environment
 * @param file_handle  the file handle where the output will be written to
 */
static be_main_env_t *be_init_env(be_main_env_t *const env,
                                  char const *const compilation_unit_name)
{
	memset(env, 0, sizeof(*env));
	env->ent_trampoline_map   = pmap_create();
	env->pic_trampolines_type = new_type_segment(NEW_IDENT("$PIC_TRAMPOLINE_TYPE"), tf_none);
	env->ent_pic_symbol_map   = pmap_create();
	env->pic_symbols_type     = new_type_segment(NEW_IDENT("$PIC_SYMBOLS_TYPE"), tf_none);
	env->cup_name             = compilation_unit_name;

	isa_if->begin_codegeneration();

	memset(be_asm_constraint_flags, 0, sizeof(be_asm_constraint_flags));

	return env;
}

/**
 * Called when the be_main_env_t can be destroyed.
 */
static void be_done_env(be_main_env_t *env)
{
	isa_if->end_codegeneration();
	pmap_destroy(env->ent_trampoline_map);
	pmap_destroy(env->ent_pic_symbol_map);
	free_type(env->pic_trampolines_type);
	free_type(env->pic_symbols_type);
}

/**
 * Prepare a backend graph for code generation and initialize its irg
 */
static void initialize_birg(be_irg_t *birg, ir_graph *irg, be_main_env_t *env)
{
	/* don't duplicate locals in backend when dumping... */
	ir_remove_dump_flags(ir_dump_flag_consts_local);

	be_dump(DUMP_INITIAL, irg, "begin");

	assure_irg_properties(irg,
		IR_GRAPH_PROPERTY_NO_BADS
		| IR_GRAPH_PROPERTY_NO_UNREACHABLE_CODE
		| IR_GRAPH_PROPERTY_NO_CRITICAL_EDGES
		| IR_GRAPH_PROPERTY_MANY_RETURNS);

	memset(birg, 0, sizeof(*birg));
	birg->main_env = env;
	obstack_init(&birg->obst);
	irg->be_data = birg;

	be_info_init_irg(irg);
	birg->lv = be_liveness_new(irg);
}

int be_timing;

static const char *get_timer_name(be_timer_id_t id)
{
	switch (id) {
	case T_ABI:            return "abi";
	case T_CODEGEN:        return "codegen";
	case T_RA_PREPARATION: return "ra_preparation";
	case T_SCHED:          return "sched";
	case T_CONSTR:         return "constr";
	case T_FINISH:         return "finish";
	case T_EMIT:           return "emit";
	case T_VERIFY:         return "verify";
	case T_OTHER:          return "other";
	case T_HEIGHTS:        return "heights";
	case T_LIVE:           return "live";
	case T_EXECFREQ:       return "execfreq";
	case T_SSA_CONSTR:     return "ssa_constr";
	case T_RA_EPILOG:      return "ra_epilog";
	case T_RA_CONSTR:      return "ra_constr";
	case T_RA_SPILL:       return "ra_spill";
	case T_RA_SPILL_APPLY: return "ra_spill_apply";
	case T_RA_COLOR:       return "ra_color";
	case T_RA_IFG:         return "ra_ifg";
	case T_RA_COPYMIN:     return "ra_copymin";
	case T_RA_SSA:         return "ra_ssa";
	case T_RA_OTHER:       return "ra_other";
	}
	return "unknown";
}
ir_timer_t *be_timers[T_LAST+1];

static void dummy_after_transform(ir_graph *irg, const char *name)
{
	(void)irg;
	(void)name;
}

after_transform_func be_after_transform = dummy_after_transform;

void be_set_after_transform_func(after_transform_func after_transform)
{
	be_after_transform = after_transform;
}

void be_after_irp_transform(const char *name)
{
	if (be_after_transform == NULL)
		return;

	foreach_irp_irg_r(i, irg) {
		be_after_transform(irg, name);
	}
}

void be_lower_for_target(void)
{
	initialize_isa();

	isa_if->lower_for_target();
	/* set the phase to low */
	foreach_irp_irg_r(i, irg) {
		assert(!irg_is_constrained(irg, IR_GRAPH_CONSTRAINT_TARGET_LOWERED));
		add_irg_constraints(irg, IR_GRAPH_CONSTRAINT_TARGET_LOWERED);
	}
}

/**
 * The Firm backend main loop.
 * Do architecture specific lowering for all graphs
 * and call the architecture specific code generator.
 *
 * @param file_handle   the file handle the output will be written to
 * @param cup_name      name of the compilation unit
 */
static void be_main_loop(FILE *file_handle, const char *cup_name)
{
	be_timing = be_options.timing;

	/* perform target lowering if it didn't happen yet */
	if (get_irp_n_irgs() > 0 && !irg_is_constrained(get_irp_irg(0), IR_GRAPH_CONSTRAINT_TARGET_LOWERED))
		be_lower_for_target();

	if (be_timing) {
		for (be_timer_id_t t = T_FIRST; t < T_LAST+1; ++t) {
			be_timers[t] = ir_timer_new();
			ir_timer_init_parent(be_timers[t]);
		}
	}

	be_emit_init(file_handle);

	be_main_env_t env;
	be_init_env(&env, cup_name);
	be_info_init();

	be_gas_begin_compilation_unit(&env);

	/* First: initialize all birgs */
	size_t          num_birgs = 0;
	/* we might need 1 birg more for instrumentation constructor */
	be_irg_t *const birgs     = ALLOCAN(be_irg_t, get_irp_n_irgs() + 1);
	foreach_irp_irg(i, irg) {
		ir_entity *entity = get_irg_entity(irg);
		if (get_entity_linkage(entity) & IR_LINKAGE_NO_CODEGEN)
			continue;
		initialize_birg(&birgs[num_birgs++], irg, &env);
		if (isa_if->handle_intrinsics)
			isa_if->handle_intrinsics(irg);
		be_dump(DUMP_INITIAL, irg, "prepared");
	}

	/*
		Get the filename for the profiling data.
		Beware: '\0' is already included in sizeof(suffix)
	*/
	static const char suffix[] = ".prof";
	char prof_filename[256];
	sprintf(prof_filename, "%.*s%s",
	        (int)(sizeof(prof_filename) - sizeof(suffix)), cup_name, suffix);

	bool have_profile = false;
	if (be_options.opt_profile_use) {
		bool res = ir_profile_read(prof_filename);
		if (!res) {
			be_warningf(NULL, "could not read profile data '%s'", prof_filename);
		} else {
			ir_create_execfreqs_from_profile();
			ir_profile_free();
			have_profile = true;
		}
	}

	if (num_birgs > 0 && be_options.opt_profile_generate) {
		ir_graph *const prof_init_irg = ir_profile_instrument(prof_filename);
		assert(prof_init_irg->be_data == NULL);
		initialize_birg(&birgs[num_birgs++], prof_init_irg, &env);
	}

	if (!have_profile) {
		be_timer_push(T_EXECFREQ);
		foreach_irp_irg(i, irg) {
			ir_estimate_execfreq(irg);
		}
		be_timer_pop(T_EXECFREQ);
	}

	/* For all graphs */
	foreach_irp_irg(i, irg) {
		ir_entity *const entity = get_irg_entity(irg);
		if (get_entity_linkage(entity) & IR_LINKAGE_NO_CODEGEN)
			continue;

		be_timer_push(T_OTHER);
		if (stat_ev_enabled) {
			stat_ev_ctx_push_fmt("bemain_irg", "%+F", irg);
			stat_ev_ull("bemain_insns_start", be_count_insns(irg));
			stat_ev_ull("bemain_blocks_start", be_count_blocks(irg));
		}

		/* Verify the initial graph */
		if (be_options.do_verify) {
			be_timer_push(T_VERIFY);
			bool fine = irg_verify(irg);
			be_check_verify_result(fine, irg);
			be_timer_pop(T_VERIFY);
		}

		/* prepare and perform codeselection */
		isa_if->prepare_graph(irg);

		/* schedule the irg */
		be_timer_push(T_SCHED);
		be_schedule_graph(irg);
		be_timer_pop(T_SCHED);

		be_dump(DUMP_SCHED, irg, "sched");

		/* check schedule */
		be_sched_verify(irg);

		/* we switch off optimizations here, because they might cause trouble */
		optimization_state_t state;
		save_optimization_state(&state);
		set_optimize(0);
		set_opt_cse(0);

		/* stuff needs to be done after scheduling but before register allocation */
		be_timer_push(T_RA_PREPARATION);
		isa_if->before_ra(irg);
		be_timer_pop(T_RA_PREPARATION);

		if (stat_ev_enabled) {
			stat_ev_dbl("bemain_costs_before_ra", be_estimate_irg_costs(irg));
			stat_ev_ull("bemain_insns_before_ra", be_count_insns(irg));
			stat_ev_ull("bemain_blocks_before_ra", be_count_blocks(irg));
		}

		be_timer_push(T_RA_CONSTR);
		/* add CopyKeeps for should_be_different constrained nodes  */
		/* beware: needs schedule due to usage of be_ssa_constr */
		be_spill_prepare_for_constraints(irg);
		be_timer_pop(T_RA_CONSTR);
		be_dump(DUMP_RA, irg, "spillprepare");

		if (stat_ev_enabled) {
			be_stat_values(irg);
		}

		/* Do register allocation */
		be_allocate_registers(irg);
		be_regalloc_verify(irg, true);

		if (stat_ev_enabled) {
			stat_ev_dbl("bemain_costs_after_ra", be_estimate_irg_costs(irg));
			stat_ev_ull("bemain_insns_after_ra", be_count_insns(irg));
			stat_ev_ull("bemain_blocks_after_ra", be_count_blocks(irg));
		}

		be_dump(DUMP_RA, irg, "ra");

		/* emit assembler code */
		be_timer_push(T_EMIT);
		isa_if->emit(irg);
		be_timer_pop(T_EMIT);

		if (stat_ev_enabled) {
			stat_ev_ull("bemain_insns_finish", be_count_insns(irg));
			stat_ev_ull("bemain_blocks_finish", be_count_blocks(irg));
		}

		be_dump(DUMP_FINAL, irg, "final");
		be_regalloc_verify(irg, false);

		restore_optimization_state(&state);

		be_timer_pop(T_OTHER);

		if (be_timing) {
			if (stat_ev_enabled) {
				for (be_timer_id_t t = T_FIRST; t < T_LAST+1; ++t) {
					char buf[128];
					snprintf(buf, sizeof(buf), "bemain_time_%s",
					         get_timer_name(t));
					stat_ev_dbl(buf, ir_timer_elapsed_usec(be_timers[t]));
				}
			} else {
				printf("==>> IRG %s <<==\n",
				       get_entity_name(get_irg_entity(irg)));
				for (be_timer_id_t t = T_FIRST; t < T_LAST+1; ++t) {
					double val = ir_timer_elapsed_usec(be_timers[t]) / 1000.0;
					printf("%-20s: %10.3f msec\n", get_timer_name(t), val);
				}
			}
			for (be_timer_id_t t = T_FIRST; t < T_LAST+1; ++t) {
				ir_timer_reset(be_timers[t]);
			}
		}

		be_free_birg(irg);
		stat_ev_ctx_pop("bemain_irg");
	}

	be_gas_end_compilation_unit(&env);
	be_emit_exit();

	be_done_env(&env);

	be_info_free();
}

/* Main interface to the frontend. */
void be_main(FILE *file_handle, const char *cup_name)
{
	ir_timer_t *t = NULL;

	if (be_options.timing) {
		t = ir_timer_new();

		if (ir_timer_enter_high_priority())
			be_warningf(NULL, "could not enter high priority mode");

		ir_timer_reset_and_start(t);
	}

	if (stat_ev_enabled) {
		const char *dot = strrchr(cup_name, '.');
		const char *pos = dot ? dot : cup_name + strlen(cup_name);
		char       *buf = ALLOCAN(char, pos - cup_name + 1);
		strncpy(buf, cup_name, pos - cup_name);
		buf[pos - cup_name] = '\0';

		stat_ev_ctx_push_str("bemain_compilation_unit", cup_name);
	}

	be_main_loop(file_handle, cup_name);

	if (be_options.timing) {
		ir_timer_stop(t);
		ir_timer_leave_high_priority();
		if (stat_ev_enabled) {
			stat_ev_dbl("bemain_backend_time", ir_timer_elapsed_msec(t));
		} else {
			double val = ir_timer_elapsed_usec(t) / 1000.0;
			printf("%-20s: %10.3f msec\n", "BEMAINLOOP", val);
		}
	}

	if (stat_ev_enabled) {
		stat_ev_ctx_pop("bemain_compilation_unit");
	}
}
