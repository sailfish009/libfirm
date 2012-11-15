/*
 * Copyright (C) 1995-2008 University of Karlsruhe.  All right reserved.
 *
 * This file is part of libFirm.
 *
 * This file may be distributed and/or modified under the terms of the
 * GNU General Public License version 2 as published by the Free Software
 * Foundation and appearing in the file LICENSE.GPL included in the
 * packaging of this file.
 *
 * Licensees holding valid libFirm Professional Edition licenses may use
 * this file in accordance with the libFirm Commercial License.
 * Agreement provided with the Software.
 *
 * This file is provided AS IS with NO WARRANTY OF ANY KIND, INCLUDING THE
 * WARRANTY OF DESIGN, MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE.
 */

/**
 * @file
 * @brief       Performs SSA-Destruction.
 * @author      Daniel Grund
 * @date        25.05.2005
 */
#include "config.h"

#include "bessadestr.h"

#include "debug.h"
#include "set.h"
#include "pmap.h"
#include "irnode_t.h"
#include "ircons_t.h"
#include "iredges_t.h"
#include "irgwalk.h"
#include "irgmod.h"
#include "irdump.h"
#include "irprintf.h"

#include "be_t.h"
#include "beutil.h"
#include "bechordal_t.h"
#include "bearch.h"
#include "belive_t.h"
#include "benode.h"
#include "besched.h"
#include "bespillutil.h"
#include "bestatevent.h"
#include "beirg.h"
#include "beintlive_t.h"

DEBUG_ONLY(static firm_dbg_module_t *dbg = NULL;)
DEBUG_ONLY(static firm_dbg_module_t *dbg_icore = NULL;)

static be_chordal_env_t *the_env = NULL;

static const char *get_reg_name(unsigned index)
{
	return arch_register_get_name(arch_register_for_index(the_env->cls, index));
}

static void print_parcopy(unsigned *permutation_orig, unsigned *n_used_orig)
{
	const unsigned n_regs = the_env->cls->n_regs;
	unsigned permutation[n_regs];
	unsigned n_used[n_regs];
	memcpy(permutation, permutation_orig, sizeof(unsigned) * n_regs);
	memcpy(n_used, n_used_orig, sizeof(unsigned) * n_regs);

	for (unsigned i = 0; i < n_regs; ++i)
		if (n_used_orig[i] != 0)
			DB((dbg_icore, LEVEL_2, "#users[%s(%u)] = %u\n", get_reg_name(i), i, n_used_orig[i]));

	unsigned comp[n_regs];
	for (unsigned r = 0; r < n_regs; ) {
		if (permutation[r] == r || n_used[r] > 0) {
			++r;
			continue;
		}

		/* Perfect, end of a chain. */
		unsigned len = 0;
		comp[len++] = r;
		unsigned s = r;
		while (n_used[s] == 0 && permutation[s] != s) {
			unsigned src = permutation[s];
			permutation[s] = s;
			comp[len++] = src;
			assert(n_used[src] > 0);
			--n_used[src];
			s = src;
		}

		/* Reverse. */
		for (unsigned i = 0; i < len / 2; ++i) {
			unsigned t = comp[i];
			comp[i] = comp[len - i - 1];
			comp[len - i - 1] = t;
		}

		for (unsigned i = 0; i + 1 < len; ++i)
			DB((dbg_icore, LEVEL_2, "%s(%u) -> ", get_reg_name(comp[i]), comp[i]));
		DB((dbg_icore, LEVEL_2, "%s(%i)\n", get_reg_name(comp[len - 1]), comp[len - 1]));
	}

	/* Only cycles left. */
	for (unsigned r = 0; r < n_regs; ) {
		if (permutation[r] == r) {
			++r;
			continue;
		}

		assert(n_used[r] == 1);

		unsigned len = 0;
		unsigned s = r;
		while (permutation[s] != s) {
			unsigned src = permutation[s];
			comp[len++] = s;
			permutation[s] = s;
			s = src;
		}

		for (unsigned i = 0; i < len / 2; ++i) {
			unsigned t = comp[i];
			comp[i] = comp[len - i - 1];
			comp[len - i - 1] = t;
		}

		for (unsigned i = 0; i < len; ++i)
			DB((dbg_icore, LEVEL_2, "%s(%u) -> ", get_reg_name(comp[i]), comp[i]));
		DB((dbg_icore, LEVEL_2, "%s(%u)\n", get_reg_name(comp[0]), comp[0]));
	}
}

static void mark_cycle_parts(bool *part_of_cycle, unsigned *permutation_orig,
                             unsigned *n_used_orig)
{
	const unsigned n_regs = the_env->cls->n_regs;
	unsigned permutation[n_regs];
	unsigned n_used[n_regs];
	memcpy(permutation, permutation_orig, sizeof(unsigned) * n_regs);
	memcpy(n_used, n_used_orig, sizeof(unsigned) * n_regs);
	memset(part_of_cycle, 0, sizeof(bool) * n_regs);

	for (unsigned r = 0; r < n_regs; ) {
		if (permutation[r] == r || n_used[r] > 0) {
			++r;
			continue;
		}

		/* Perfect, end of a chain. */
		unsigned s = r;
		while (n_used[s] == 0 && permutation[s] != s) {
			part_of_cycle[s] = false;

			unsigned src = permutation[s];
			permutation[s] = s;
			assert(n_used[src] > 0);
			--n_used[src];
			s = src;
		}
	}

	/* Only cycles left. */
	for (unsigned r = 0; r < n_regs; ) {
		if (permutation[r] == r) {
			if (n_used[r] > 0)
				part_of_cycle[r] = true;
			++r;
			continue;
		}

		assert(n_used[r] == 1);

		unsigned s = r;
		while (permutation[s] != s) {
			part_of_cycle[s] = true;
			unsigned src = permutation[s];
			permutation[s] = s;
			s = src;
		}
	}
}

static unsigned find_longest_chain(unsigned *parcopy, unsigned *n_used,
                                   unsigned fork_reg)
{
	/* fork_reg must be a fork. */
	assert(n_used[fork_reg] > 1);
	const unsigned n_regs = the_env->cls->n_regs;

	DB((dbg_icore, LEVEL_2, "  Searching for longest chain starting at %s\n", get_reg_name(fork_reg)));

	/* Search the longest chain starting from r. */
	unsigned max_len = 0;
	unsigned max_dst = (unsigned) -1;

	for (unsigned to_reg = 0; to_reg < n_regs; /* empty */) {
		unsigned from_reg = parcopy[to_reg];

		if (from_reg == to_reg || n_used[to_reg] > 0) {
			++to_reg;
			continue;
		}

		DB((dbg_icore, LEVEL_2, "  Found candidate ending in %s\n", get_reg_name(to_reg)));
		unsigned r   = to_reg;
		unsigned len = 0;
		while (r != parcopy[r]) {
			unsigned src = parcopy[r];
			++len;
			if (src == fork_reg && len > max_len) {
				DB((dbg_icore, LEVEL_2, "  Chain starts in %s, continues via %s, length %u\n", get_reg_name(fork_reg), get_reg_name(r), len));
				max_len = len;
				max_dst = r;
				break;
			}
			r = src;
		}

		++to_reg;
	}

	return max_dst;
}

static void impl_parallel_copy(ir_node *before, unsigned *parcopy, unsigned *n_used)
{
	ir_node *block = get_nodes_block(before);
	const unsigned n_regs = the_env->cls->n_regs;

	unsigned restore_srcs[n_regs];
	unsigned restore_dsts[n_regs];
	unsigned num_restores = 0;

	DB((dbg_icore, LEVEL_2, "Searching for out-of-cycle propagations.\n"));
	bool is_part_of_cycle[n_regs];
	mark_cycle_parts(is_part_of_cycle, parcopy, n_used);
	for (unsigned to_reg = 0; to_reg < n_regs; /* empty */) {
		unsigned from_reg = parcopy[to_reg];

		if (from_reg == to_reg) {
			++to_reg;
			continue;
		}

		if (is_part_of_cycle[from_reg] && !is_part_of_cycle[to_reg]) {
			DB((dbg_icore, LEVEL_2, "  Found out-of-cycle propagation %s -> %s\n", get_reg_name(from_reg), get_reg_name(to_reg)));
			unsigned new_src = (unsigned) -1;
			for (unsigned src = 0; src < n_regs; ++src) {
				if (parcopy[src] == from_reg && is_part_of_cycle[src]) {
					/* new_src must be unambiguous. */
					new_src = src;
					break;
				}
			}
			assert((new_src != ((unsigned) -1)) && "Could not find new source for out-of-cycle propagation");

			restore_srcs[num_restores] = new_src;
			restore_dsts[num_restores] = to_reg;
			++num_restores;
			DB((dbg_icore, LEVEL_2, "  Added restore %s -> %s\n", get_reg_name(new_src), get_reg_name(to_reg)));
			--n_used[from_reg];
			parcopy[to_reg] = to_reg;
		}

		++to_reg;
	}
	DB((dbg_icore, LEVEL_2, "Finished search for out-of-cycle propagation.\n"));

	DB((dbg_icore, LEVEL_2, "Searching for forks.\n"));
	for (unsigned to_reg = 0; to_reg < n_regs; /* empty */) {
		unsigned from_reg = parcopy[to_reg];

		if (from_reg == to_reg || n_used[to_reg] > 0) {
			++to_reg;
			continue;
		}

		/* Found the end of a chain, follow it. */
		unsigned r = to_reg;
		while (r != parcopy[r]) {
			r = parcopy[r];
			if (n_used[r] > 1) {
				/* Found a fork. */
				DB((dbg_icore, LEVEL_2, "  Found a fork at %s\n", get_reg_name(r)));
				unsigned longest_next = find_longest_chain(parcopy, n_used, r);
				DB((dbg_icore, LEVEL_2, "  Longest chain from %s via %s\n", get_reg_name(r), get_reg_name(longest_next)));

				/* Reroute all others. */
				for (unsigned dst = 0; dst < n_regs; ++dst) {
					if (dst != longest_next && dst != r && parcopy[dst] == r) {
						restore_srcs[num_restores] = longest_next;
						restore_dsts[num_restores] = dst;
						++num_restores;
						DB((dbg_icore, LEVEL_2, "  Added restore %s -> %s\n", get_reg_name(longest_next), get_reg_name(dst)));
						--n_used[r];
						parcopy[dst] = dst;
					}
				}
			}
		}

		++to_reg;
	}
	DB((dbg_icore, LEVEL_2, "Finished searching for forks.\n"));

	DB((dbg_icore, LEVEL_2, "Current parallel copy:\n"));
	print_parcopy(parcopy, n_used);

#if 0
	/* Step 3: The remaining parallel copy must be suitable for a Perm. */
	unsigned perm_size = 0;
	ir_node *ins[n_regs];
	for (unsigned r = 0; r < n_regs; ++r) {
		unsigned src = parcopy[r];
		if (src != r)
			ins[perm_size++] = assignments[src];
	}

	if (perm_size > 0) {
		DB((dbg_icore, LEVEL_2, "Creating Perm using permutation.\n"));

		ir_node *perm = be_new_Perm(cls, block, perm_size, ins);
		sched_add_before(before, perm);
		unsigned input = 0;
		for (unsigned r = 0; r < n_regs; ++r) {
			unsigned src = parcopy[r];
			if (src != r) {
				ir_node *proj = new_r_Proj(perm, get_irn_mode(ins[input]), input);
				mark_as_copy_of(proj, ins[input]);

				const arch_register_t *reg = arch_register_for_index(cls, r);
				use_reg(proj, reg, /* width = */ 1);

				assert(n_used[src] == 1);
				// TODO: When to do?
				if (parcopy[src] == src) {
					DB((dbg_icore, LEVEL_2, "Perm: Freeing register %s of value %+F\n", get_reg_name(src), ins[input]));
					free_reg_of_value(assignments[src]);
				}

				if (live_nodes != NULL) {
					ir_nodeset_remove(live_nodes, ins[input]);
					ir_nodeset_insert(live_nodes, proj);
				}
				++input;
			}
		}

		for (unsigned r = 0; r < n_regs; ++r)
			parcopy[r] = r;
	}

#ifndef NDEBUG
	/* now we should only have fixpoints left */
	for (unsigned r = 0; r < n_regs; ++r) {
		assert(parcopy[r] == r);
	}
#endif

	if (num_restores > 0) {
		/* Step 4: Place restore movs. */
		DB((dbg_icore, LEVEL_2, "Placing restore movs.\n"));
		for (unsigned i = 0; i < num_restores; ++i) {
			unsigned src_reg = restore_srcs[i];
			unsigned dst_reg = restore_dsts[i];
			ir_node *src     = assignments[src_reg];
			ir_node *copy    = be_new_Copy(block, src);
			sched_add_before(before, copy);

			DB((dbg_icore, LEVEL_2, "Inserted restore copy %+F %s -> %s\n", copy, get_reg_name(src_reg), get_reg_name(dst_reg)));
			mark_as_copy_of(copy, src);
			const arch_register_t *reg = arch_register_for_index(cls, dst_reg);
			use_reg(copy, reg, /* width = */ 1);

			if (live_nodes != NULL) {
				ir_nodeset_remove(live_nodes, src);
				ir_nodeset_insert(live_nodes, copy);
			}
		}
		DB((dbg_icore, LEVEL_2, "Finished placing restore movs.\n"));
	}
#endif
}

static void analyze_parallel_copies_walker(ir_node *block, void *data)
{
	be_chordal_env_t *chordal_env = (be_chordal_env_t*) data;
	be_lv_t *lv = be_get_irg_liveness(chordal_env->irg);

	assert(is_Block(block));

	if (!get_irn_link(block))
		return;

	the_env = chordal_env;

	const unsigned n_regs = chordal_env->cls->n_regs;

	for (int i = 0; i < get_irn_arity(block); ++i) {
		unsigned parcopy[n_regs];
		unsigned n_used[n_regs];
		for (unsigned i = 0; i < n_regs; ++i) {
			parcopy[i] = i;
			n_used[i] = 0;
		}

		for (ir_node *phi = (ir_node *) get_irn_link(block); phi != NULL;
		     phi = (ir_node *) get_irn_link(phi)) {

			const arch_register_t *phi_reg = arch_get_irn_register(phi);
			ir_node *arg = get_irn_n(phi, i);
			const arch_register_t *arg_reg = arch_get_irn_register(arg);

			if (phi_reg == arg_reg
				|| (arg_reg->type & arch_register_type_joker)
				|| (arg_reg->type & arch_register_type_virtual)) {
				continue;
			}

			assert(parcopy[phi_reg->index] == phi_reg->index);
			parcopy[phi_reg->index] = arg_reg->index;
			DB((dbg_icore, LEVEL_2, "copy %s -> %s\n", arg_reg->name, phi_reg->name));
			++n_used[arg_reg->index];

			if (be_is_live_in(lv, block, arg))
				++n_used[arg_reg->index];
		}

		ir_node *pred   = get_Block_cfgpred_block(block, i);
		ir_node *before = be_get_end_of_block_insertion_point(pred);
		DB((dbg_icore, LEVEL_2, "copies for %+F:\n", pred));
		print_parcopy(parcopy, n_used);
		impl_parallel_copy(before, parcopy, n_used);
	}
}

static void clear_link(ir_node *irn, void *data)
{
	(void) data;
	set_irn_link(irn, NULL);
}

/**
 * For each block build a linked list of phis that
 *  - are in that block
 *  - have the current register class
 * The list is rooted at get_irn_link(BB).
 */
static void collect_phis_walker(ir_node *irn, void *data)
{
	be_chordal_env_t *env = (be_chordal_env_t*)data;
	if (is_Phi(irn) && chordal_has_class(env, irn)) {
		ir_node *bl = get_nodes_block(irn);
		set_irn_link(irn, get_irn_link(bl));
		set_irn_link(bl, irn);
	}
}

/**
 * This struct represents a Proj for a Perm.
 * It records the argument in the Perm and the corresponding Proj of the
 * Perm.
 */
typedef struct {
	ir_node *arg;  /**< The phi argument to make the Proj for. */
	int pos;       /**< The proj number the Proj will get.
									 This also denotes the position of @p arg
									 in the in array of the Perm. */
	ir_node *proj; /**< The proj created for @p arg. */
} perm_proj_t;

static int cmp_perm_proj(const void *a, const void *b, size_t n)
{
	const perm_proj_t *p = (const perm_proj_t*)a;
	const perm_proj_t *q = (const perm_proj_t*)b;
	(void) n;

	return !(p->arg == q->arg);
}

/**
 * Insert Perms in all predecessors of a block containing a phi
 */
static void insert_all_perms_walker(ir_node *bl, void *data)
{
	be_chordal_env_t *const chordal_env = (be_chordal_env_t*)data;
	be_lv_t *lv = be_get_irg_liveness(chordal_env->irg);
	int i, n;

	assert(is_Block(bl));

	/* If the link flag is NULL, this block has no phis. */
	if (!get_irn_link(bl))
		return;

	/* Look at all predecessors of the phi block */
	for (i = 0, n = get_irn_arity(bl); i < n; ++i) {
		ir_node *phi, *perm, *insert_after, **in;
		set *arg_set     = new_set(cmp_perm_proj, chordal_env->cls->n_regs);
		ir_node *pred_bl = get_Block_cfgpred_block(bl, i);
		int n_projs      = 0;

		/*
		 * Note that all phis in the list are in the same
		 * register class by construction.
		 */
		for (phi = (ir_node*)get_irn_link(bl); phi != NULL;
		     phi = (ir_node*)get_irn_link(phi)) {
			ir_node                   *arg = get_irn_n(phi, i);
			unsigned                   hash;
			perm_proj_t                templ;

			hash = hash_irn(arg);
			templ.arg  = arg;
			perm_proj_t *const pp = set_find(perm_proj_t, arg_set, &templ, sizeof(templ), hash);

			/*
			 * If a proj_perm_t entry has not been made in the argument set,
			 * create one. The only restriction is, that the phi argument
			 * may not be live in at the current block, since this argument
			 * interferes with the phi and must thus not be member of a
			 * Perm. A copy will be inserted for this argument later on.
			 */
			if (!pp && !be_is_live_in(lv, bl, arg)) {
				templ.pos = n_projs++;
				(void)set_insert(perm_proj_t, arg_set, &templ, sizeof(templ), hash);
			}
		}


		if (n_projs) {
			/*
			 * Create a new Perm with the arguments just collected
			 * above in the arg_set and insert it into the schedule.
			 */
			in = XMALLOCN(ir_node*, n_projs);
			foreach_set(arg_set, perm_proj_t, pp) {
				in[pp->pos] = pp->arg;
			}

			perm = be_new_Perm(chordal_env->cls, pred_bl, n_projs, in);
			be_stat_ev("phi_perm", n_projs);

			insert_after = pred_bl;
			do {
				insert_after = sched_prev(insert_after);
			} while (is_cfop(insert_after));
			sched_add_after(insert_after, perm);

			/*
			 * Make the Projs for the Perm and insert into schedule.
			 * Register allocation is copied from the former phi
			 * arguments to the projs (new phi arguments).
			 */
			insert_after = perm;
			foreach_set(arg_set, perm_proj_t, pp) {
				ir_node *proj = new_r_Proj(perm, get_irn_mode(pp->arg), pp->pos);
				pp->proj = proj;
				assert(arch_get_irn_register(pp->arg));
				arch_set_irn_register(proj, arch_get_irn_register(pp->arg));
				insert_after = proj;
				DBG((dbg, LEVEL_2, "Copy register assignment %s from %+F to %+F\n", arch_get_irn_register(pp->arg)->name, pp->arg, pp->proj));
			}

			/*
			 * Set the phi nodes to their new arguments: The Projs of the Perm
			 */
			for (phi = (ir_node*)get_irn_link(bl); phi != NULL;
			     phi = (ir_node*)get_irn_link(phi)) {
				perm_proj_t templ;

				templ.arg = get_irn_n(phi, i);
				perm_proj_t *const pp = set_find(perm_proj_t, arg_set, &templ, sizeof(templ), hash_irn(templ.arg));

				/* If not found, it was an interfering argument */
				if (pp) {
					set_irn_n(phi, i, pp->proj);
					be_liveness_introduce(lv, pp->proj);
				}
			}

			/* update the liveness of the Perm's operands. It might be changed. */
			{
				int i;
				for (i = 0; i < n_projs; ++i)
					be_liveness_update(lv, in[i]);
			}
			free(in);
		}

		del_set(arg_set);
	}
}

#define is_pinned(irn) (get_irn_link(irn))
#define get_pinning_block(irn) ((ir_node *)get_irn_link(irn))
#define pin_irn(irn, lock) (set_irn_link(irn, lock))

/**
 * Adjusts the register allocation for the (new) phi-operands
 * and insert duplicates iff necessary.
 */
static void set_regs_or_place_dupls_walker(ir_node *bl, void *data)
{
	be_chordal_env_t *chordal_env = (be_chordal_env_t*)data;
	be_lv_t *lv = be_get_irg_liveness(chordal_env->irg);
	ir_node *phi;

	/* Consider all phis of this block */
	for (phi = (ir_node*)get_irn_link(bl); phi != NULL;
	     phi = (ir_node*)get_irn_link(phi)) {
		ir_node                     *phi_block = get_nodes_block(phi);
		const arch_register_t       *phi_reg   = arch_get_irn_register(phi);
		int                          max;
		int                          i;

		assert(is_Phi(phi) && "Can only handle phi-destruction :)");

		/* process all arguments of the phi */
		for (i = 0, max = get_irn_arity(phi); i < max; ++i) {
			ir_node                   *arg = get_irn_n(phi, i);
			const arch_register_t     *arg_reg;
			ir_node                   *arg_block;

			arg_block = get_Block_cfgpred_block(phi_block, i);
			arg_reg   = arch_get_irn_register(arg);

			assert(arg_reg && "Register must be set while placing perms");

			DBG((dbg, LEVEL_1, "  for %+F(%s) -- %+F(%s)\n", phi, phi_reg->name, arg, arg_reg->name));

			if (phi_reg == arg_reg
					|| (arg_reg->type & arch_register_type_joker)
					|| (arg_reg->type & arch_register_type_virtual)) {
				/* Phi and arg have the same register, so pin and continue */
				pin_irn(arg, phi_block);
				DBG((dbg, LEVEL_1, "      arg has same reg: pin %+F(%s)\n", arg, arch_get_irn_register(arg)->name));
				continue;
			}

			if (be_values_interfere(lv, phi, arg)) {
				ir_node *schedpoint;

				/*
					Insert a duplicate in arguments block,
					make it the new phi arg,
					set its register,
					insert it into schedule,
					pin it
				*/
				ir_node *dupl = be_new_Copy(arg_block, arg);

				set_irn_n(phi, i, dupl);
				arch_set_irn_register(dupl, phi_reg);
				schedpoint = arg_block;
				do {
					schedpoint = sched_prev(schedpoint);
				} while (is_cfop(schedpoint));
				sched_add_after(schedpoint, dupl);
				pin_irn(dupl, phi_block);
				be_liveness_introduce(lv, dupl);
				be_liveness_update(lv, arg);
				DBG((dbg, LEVEL_1, "    they do interfere: insert %+F(%s)\n", dupl, arch_get_irn_register(dupl)->name));
				continue; /* with next argument */
			}

			DBG((dbg, LEVEL_1, "    they do not interfere\n"));
			assert(is_Proj(arg));
			/*
				First check if there is an other phi
				- in the same block
				- having arg at the current pos in its arg-list
				- having the same color as arg

				If found, then pin the arg (for that phi)
			*/
			if (! is_pinned(arg)) {
				ir_node *other_phi;

				DBG((dbg, LEVEL_1, "      searching for phi with same arg having args register\n"));

				for (other_phi = (ir_node*)get_irn_link(phi_block);
				     other_phi != NULL;
				     other_phi = (ir_node*)get_irn_link(other_phi)) {

					assert(is_Phi(other_phi)                               &&
						get_nodes_block(phi) == get_nodes_block(other_phi) &&
						"link fields are screwed up");

					if (get_irn_n(other_phi, i) == arg && arch_get_irn_register(other_phi) == arg_reg) {
						DBG((dbg, LEVEL_1, "        found %+F(%s)\n", other_phi, arch_get_irn_register(other_phi)->name));
						pin_irn(arg, phi_block);
						break;
					}
				}
			}

			if (is_pinned(arg)) {
				/*
					Insert a duplicate of the original value in arguments block,
					make it the new phi arg,
					set its register,
					insert it into schedule,
					pin it
				*/
				ir_node *perm = get_Proj_pred(arg);
				ir_node *dupl = be_new_Copy(arg_block, arg);
				ir_node *ins;

				set_irn_n(phi, i, dupl);
				arch_set_irn_register(dupl, phi_reg);
				/* skip the Perm's Projs and insert the copies behind. */
				for (ins = sched_next(perm); is_Proj(ins); ins = sched_next(ins)) {
				}
				sched_add_before(ins, dupl);
				pin_irn(dupl, phi_block);
				be_liveness_introduce(lv, dupl);
				be_liveness_update(lv, arg);
				DBG((dbg, LEVEL_1, "      arg is pinned: insert %+F(%s)\n", dupl, arch_get_irn_register(dupl)->name));
			} else {
				/*
					No other phi has the same color (else arg would have been pinned),
					so just set the register and pin
				*/
				arch_set_irn_register(arg, phi_reg);
				pin_irn(arg, phi_block);
				DBG((dbg, LEVEL_1, "      arg is not pinned: so pin %+F(%s)\n", arg, arch_get_irn_register(arg)->name));
			}
		}
	}
}

void be_ssa_destruction(be_chordal_env_t *chordal_env)
{
	ir_graph *irg = chordal_env->irg;

	FIRM_DBG_REGISTER(dbg, "ir.be.ssadestr");
	FIRM_DBG_REGISTER(dbg_icore, "ir.be.ssadestr.icore");

	be_invalidate_live_sets(irg);

	/* create a map for fast lookup of perms: block --> perm */
	irg_walk_graph(irg, clear_link, collect_phis_walker, chordal_env);

	bool use_paper_method = true;

	if (use_paper_method) {
		DBG((dbg, LEVEL_1, "Analyzing parallel copies...\n"));
		irg_block_walk_graph(irg, analyze_parallel_copies_walker, NULL, chordal_env);
	}

	{
		DBG((dbg, LEVEL_1, "Placing perms...\n"));
		irg_block_walk_graph(irg, insert_all_perms_walker, NULL, chordal_env);

		if (chordal_env->opts->dump_flags & BE_CH_DUMP_SSADESTR)
			dump_ir_graph(irg, "ssa_destr_perms_placed");

		be_assure_live_chk(irg);

		DBG((dbg, LEVEL_1, "Setting regs and placing dupls...\n"));
		irg_block_walk_graph(irg, set_regs_or_place_dupls_walker, NULL, chordal_env);

		/* unfortunately updating doesn't work yet. */
		be_invalidate_live_chk(irg);

		if (chordal_env->opts->dump_flags & BE_CH_DUMP_SSADESTR)
			dump_ir_graph(irg, "ssa_destr_regs_set");
	}
}

static void ssa_destruction_check_walker(ir_node *bl, void *data)
{
	ir_node *phi;
	int i, max;
	(void)data;

	for (phi = (ir_node*)get_irn_link(bl); phi != NULL;
	     phi = (ir_node*)get_irn_link(phi)) {
		const arch_register_t *phi_reg, *arg_reg;

		phi_reg = arch_get_irn_register(phi);
		/* iterate over all args of phi */
		for (i = 0, max = get_irn_arity(phi); i < max; ++i) {
			ir_node                   *arg = get_irn_n(phi, i);
			const arch_register_req_t *req = arch_get_irn_register_req(arg);

			if (req->type & arch_register_req_type_ignore)
				continue;

			arg_reg = arch_get_irn_register(arg);

			if (phi_reg != arg_reg) {
				DBG((dbg, 0, "Error: Registers of %+F and %+F differ: %s %s\n", phi, arg, phi_reg->name, arg_reg->name));
				assert(0);
			}

			if (! is_pinned(arg)) {
				DBG((dbg, 0, "Warning: Phi argument %+F is not pinned.\n", arg));
				assert(0);
			}
		}
	}
}

void be_ssa_destruction_check(be_chordal_env_t *chordal_env)
{
	irg_block_walk_graph(chordal_env->irg, ssa_destruction_check_walker, NULL, NULL);
}
