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
 * @brief       This file implements the IR transformation from firm into ia32-Firm.
 * @author      Christian Wuerdig, Matthias Braun
 * @version     $Id$
 */
#ifndef FIRM_BE_IA32_IA32_TRANSFORM_H
#define FIRM_BE_IA32_IA32_TRANSFORM_H

#include "bearch_ia32_t.h"

/**
 * Transform firm nodes to x86 assembler nodes
 */
void ia32_transform_graph(ia32_code_gen_t *cg);

/**
 * Some constants needed for code generation.
 * Generated on demand.
 */
typedef enum {
	ia32_SSIGN,          /**< SSE2 single precision sign */
	ia32_DSIGN,          /**< SSE2 double precision sign */
	ia32_SABS,           /**< SSE2 single precision ABS mask */
	ia32_DABS,           /**< SSE2 double precision ABS mask */
	ia32_INTMAX,         /**< x87 single precision INTMAX */
	ia32_known_const_max /**< last constant */
} ia32_known_const_t;

/**
 * Generate a known floating point constant
 */
ir_entity *ia32_gen_fp_known_const(ia32_known_const_t kct);

void ia32_add_missing_keeps(ia32_code_gen_t *cg);

/**
 * Skip all Down-Conv's on a given node and return the resulting node.
 */
ir_node *ia32_skip_downconv(ir_node *node);

#endif /* FIRM_BE_IA32_IA32_TRANSFORM_H */
