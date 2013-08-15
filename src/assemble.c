/*
 * Copyright 2013 Ciaran Anscomb
 *
 * This file is part of asm6809.
 *
 * asm6809 is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation, either version 2 of the License,
 * or (at your option) any later version.
 *
 * asm6809 is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with asm6809.  If not, see <http://www.gnu.org/licenses/>.
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <glib.h>

#include "asm6809.h"
#include "assemble.h"
#include "error.h"
#include "eval.h"
#include "instr.h"
#include "interp.h"
#include "listing.h"
#include "node.h"
#include "opcode.h"
#include "program.h"
#include "register.h"
#include "section.h"
#include "symbol.h"

static struct prog_ctx *defining_macro_ctx = NULL;
static int defining_macro_level = 0;

static unsigned asm_pass;
static unsigned prog_depth = 0;

static void set_label(struct node *label, struct node *value);
static void args_float_to_int(struct node *args);

/* Pseudo-operations */

static void pseudo_macro(struct prog_line *);
static void pseudo_endm(struct prog_line *);

static void pseudo_equ(struct prog_line *);
static void pseudo_org(struct prog_line *);
static void pseudo_section(struct prog_line *);

static void pseudo_put(struct prog_line *);
static void pseudo_setdp(struct prog_line *);
static void pseudo_export(struct prog_line *);
static void pseudo_fcc(struct prog_line *);
static void pseudo_fdb(struct prog_line *);
static void pseudo_rzb(struct prog_line *);
static void pseudo_rmb(struct prog_line *);
static void pseudo_include(struct prog_line *);
static void pseudo_includebin(struct prog_line *);

struct pseudo_op {
	const char *name;
	void (*handler)(struct prog_line *);
};

/* Pseudo-ops that override any label meaning */

static struct pseudo_op label_ops[] = {
	{ .name = "equ", .handler = &pseudo_equ },
	{ .name = "org", .handler = &pseudo_org },
	{ .name = "section", .handler = &pseudo_section },
};

/* Pseudo-ops that emit data */

static struct pseudo_op pseudo_data_ops[] = {
	{ .name = "fcc", .handler = &pseudo_fcc },
	{ .name = "fcb", .handler = &pseudo_fcc },
	{ .name = "fdb", .handler = &pseudo_fdb },
	{ .name = "rzb", .handler = &pseudo_rzb },
	{ .name = "rmb", .handler = &pseudo_rmb },
};

/* Other pseudo-ops */

static struct pseudo_op pseudo_ops[] = {
	{ .name = "put", .handler = &pseudo_put },
	{ .name = "setdp", .handler = &pseudo_setdp },
	{ .name = "include", .handler = &pseudo_include },
	{ .name = "includebin", .handler = &pseudo_includebin },
};

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

static enum node_attr arg_attr(struct node *args, int index) {
	if (!args)
		return node_attr_undef;
	if (index >= args->data.as_array.nargs)
		return node_attr_undef;
	return node_attr_of(args->data.as_array.args[index]);
}

static void args_float_to_int(struct node *args) {
	if (!args)
		return;
	if (node_type_of(args) != node_type_array)
		return;
	int nargs = args->data.as_array.nargs;
	struct node **arga = args->data.as_array.args;
	for (int i = 0; i < nargs; i++) {
		if (node_type_of(arga[i]) == node_type_float)
			arga[i] = eval_int_free(arga[i]);
	}
}

void assemble_prog(struct prog *prog, unsigned pass) {
	if (prog_depth >= asm6809_options.max_program_depth) {
		error(error_type_fatal, "maximum program depth exceeded");
		return;
	}
	asm_pass = pass;
	prog_depth++;
	struct prog_ctx *ctx = prog_ctx_new(prog);

	while (!prog_ctx_end(ctx)) {
		/* Dummy line to be populated with values evaluated or not, as
		 * appropriate. */
		struct prog_line n_line;

		struct prog_line *l = prog_ctx_next_line(ctx);

		assert(l != NULL);

		/* Incremented for every line encountered.  Doesn't correspond
		 * to any file or macro line number.  Must be consistent across
		 * passes - see section.h for details. */
		cur_section->line_number++;

		if (!l->label && !l->opcode && !l->args) {
			listing_add_line(-1, 0, NULL, l->text);
			continue;
		}

		n_line.label = NULL;
		n_line.opcode = eval_string(l->opcode);
		n_line.args = NULL;
		n_line.text = l->text;

		/* Macro handling */

		if (n_line.opcode && 0 == g_ascii_strcasecmp("macro", n_line.opcode->data.as_string)) {
			defining_macro_level++;
			if (defining_macro_level == 1) {
				n_line.label = eval_string(l->label);
				n_line.args = eval_node(l->args);
				pseudo_macro(&n_line);
				listing_add_line(-1, 0, NULL, l->text);
				goto next_line;
			}
		}

		if (n_line.opcode && 0 == g_ascii_strcasecmp("endm", n_line.opcode->data.as_string)) {
			if (defining_macro_level == 0) {
				error(error_type_syntax, "ENDM without beginning MACRO");
				goto next_line;
			}
			defining_macro_level--;
			if (defining_macro_level == 0) {
				n_line.args = eval_node(l->args);
				pseudo_endm(&n_line);
				listing_add_line(-1, 0, NULL, l->text);
				goto next_line;
			}
		}

		if (defining_macro_level > 0) {
			if (defining_macro_ctx)
				prog_ctx_add_line(defining_macro_ctx, prog_line_ref(l));
			listing_add_line(-1, 0, NULL, l->text);
			goto next_line;
		}

		/* Normal processing */

		n_line.label = eval_int(l->label);
		if (!n_line.label)
			n_line.label = eval_string(l->label);

		/* EXPORT only needs symbol names, not their values */
		if (n_line.opcode && 0 == g_ascii_strcasecmp("export", n_line.opcode->data.as_string)) {
			n_line.args = node_ref(l->args);
			pseudo_export(&n_line);
			listing_add_line(-1, 0, NULL, l->text);
			goto next_line;
		}

		/* Anything else needs a fully evaluated list of arguments */
		n_line.args = eval_node(l->args);

		/* Pseudo-ops which determine a label's value */
		if (n_line.opcode) {
			for (unsigned i = 0; i < G_N_ELEMENTS(label_ops); i++) {
				if (0 == g_ascii_strcasecmp(label_ops[i].name, n_line.opcode->data.as_string)) {
					label_ops[i].handler(&n_line);
					goto next_line;
				}
			}
		}

		/* Otherwise, any label on the line gets PC as its value */
		if (n_line.label) {
			set_label(n_line.label, node_new_int(cur_section->pc));
		}

		/* No opcode?  Next line. */
		if (!n_line.opcode) {
			if (n_line.label)
				listing_add_line(cur_section->pc & 0xffff, 0, NULL, l->text);
			goto next_line;
		}

		/* Pseudo-ops that emit or reserve data */
		for (unsigned i = 0; i < G_N_ELEMENTS(pseudo_data_ops); i++) {
			if (0 == g_ascii_strcasecmp(pseudo_data_ops[i].name, n_line.opcode->data.as_string)) {
				int old_pc = cur_section->pc;
				pseudo_data_ops[i].handler(&n_line);
				int nbytes = cur_section->pc - old_pc;
				if (cur_section->span && cur_section->pc == (int)(cur_section->span->put + cur_section->span->size))
					listing_add_line(old_pc & 0xffff, nbytes, cur_section->span, l->text);
				else
					listing_add_line(old_pc & 0xffff, nbytes, NULL, l->text);
				goto next_line;
			}
		}

		/* Other pseudo-ops */
		for (unsigned i = 0; i < G_N_ELEMENTS(pseudo_ops); i++) {
			if (0 == g_ascii_strcasecmp(pseudo_ops[i].name, n_line.opcode->data.as_string)) {
				listing_add_line(-1, 0, NULL, l->text);
				pseudo_ops[i].handler(&n_line);
				goto next_line;
			}
		}

		/* Real instructions */
		struct opcode *op = opcode_by_name(n_line.opcode->data.as_string);
		if (op) {
			int old_pc = cur_section->pc;
			int op_imm = op->type & OPCODE_IMM;
			/* No instruction accepts floats, convert them all to
			 * integer here as a convenience: */
			args_float_to_int(n_line.args);
			if (op->type == OPCODE_INHERENT) {
				instr_inherent(op, n_line.args);
			} else if ((op_imm == OPCODE_IMM8 ||
				    op_imm == OPCODE_IMM16) &&
				   (arg_attr(l->args, 0) == node_attr_immediate)) {
				instr_immediate(op, n_line.args);
			} else if (op->type & OPCODE_MEM) {
				instr_address(op, n_line.args);
			} else if (op_imm == OPCODE_REL8 ||
				   op_imm == OPCODE_REL16) {
				instr_rel(op, n_line.args);
			} else if (op_imm == OPCODE_STACKU) {
				instr_stack(op, n_line.args, REG_U);
			} else if (op_imm == OPCODE_STACKS) {
				instr_stack(op, n_line.args, REG_S);
			} else if (op_imm == OPCODE_PAIR) {
				instr_pair(op, n_line.args);
			} else {
				error(error_type_syntax, "invalid addressing mode");
			}
			int nbytes = cur_section->pc - old_pc;
			listing_add_line(old_pc & 0xffff, nbytes, cur_section->span, l->text);
			goto next_line;
		}

		/* Macro expansion */
		struct prog *macro = prog_macro_by_name(n_line.opcode->data.as_string);
		if (macro) {
			listing_add_line(cur_section->pc & 0xffff, 0, NULL, l->text);
			interp_push(n_line.args);
			assemble_prog(macro, pass);
			interp_pop();
			goto next_line;
		}

		error(error_type_syntax, "unknown instruction '%s'", n_line.opcode->data.as_string);

next_line:
		node_free(n_line.label);
		node_free(n_line.opcode);
		node_free(n_line.args);
	}

	assert(prog_depth > 0);
	prog_depth--;
	prog_ctx_free(ctx);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/* A disposable node must be passed in as value.  symbol_set() performs an eval
 * and stores the result, not the original node. */

static void set_label(struct node *label, struct node *value) {
	switch (node_type_of(label)) {
	default:
		error(error_type_syntax, "invalid label type");
		break;
	case node_type_undef:
		break;
	case node_type_int:
		symbol_local_set(cur_section->local_labels, label->data.as_int, cur_section->line_number, value, asm_pass);
		break;
	case node_type_string:
		symbol_set(label->data.as_string, value, asm_pass);
		break;
	}
	node_free(value);
}

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/* EQU.  A symbol with the name of this line's label is assigned a value. */

/* TODO: would be nice to support EQU arrays */

static void pseudo_equ(struct prog_line *line) {
	int nargs = node_array_count(line->args);
	if (nargs != 1) {
		error(error_type_syntax, "EQU requires exactly one argument");
		return;
	}
	struct node **arga = node_array_of(line->args);
	set_label(line->label, node_ref(arga[0]));
	struct node *n = eval_int(arga[0]);
	if (n) {
		listing_add_line(n->data.as_int & 0xffff, 0, NULL, line->text);
		node_free(n);
	} else {
		listing_add_line(-1, 0, NULL, line->text);
	}
}

/* ORG.  Following instructions will be assembled to this address. */

static void pseudo_org(struct prog_line *line) {
	int nargs = node_array_count(line->args);
	if (nargs != 1) {
		error(error_type_syntax, "ORG requires exactly one argument");
		return;
	}
	struct node **arga = node_array_of(line->args);
	args_float_to_int(line->args);
	switch (node_type_of(arga[0])) {
	default:
		error(error_type_syntax, "invalid argument to ORG");
		break;
	case node_type_undef:
		break;
	case node_type_int:
		cur_section->pc = arga[0]->data.as_int;  // & 0xffff;
		set_label(line->label, node_ref(arga[0]));
		listing_add_line(cur_section->pc & 0xffff, 0, NULL, line->text);
		break;
	}
}

/* SECTION.  Switch sections. */

static void pseudo_section(struct prog_line *line) {
	int nargs = node_array_count(line->args);
	if (nargs != 1) {
		error(error_type_syntax, "SECTION requires exactly one argument");
		return;
	}
	struct node **arga = node_array_of(line->args);
	if (node_type_of(arga[0]) == node_type_undef)
		return;
	struct node *n = eval_string(arga[0]);
	if (!n) {
		error(error_type_syntax, "invalid argument to SECTION");
		return;
	}
	section_set(n->data.as_string, asm_pass);
	node_free(n);
}

/* PUT.  Following instructions will be located at this address.  Allows
 * assembling as if at one address while locating them elsewhere. */

/* TODO: not written at all yet! */

// NOTE: if the current span has no data, it is safe to simply alter its put
// address, otherwise a new span must be created.

static void pseudo_put(struct prog_line *line) {
	int nargs = node_array_count(line->args);
	if (nargs != 1) {
		error(error_type_syntax, "PUT requires exactly one argument");
		return;
	}
	struct node **arga = node_array_of(line->args);
	args_float_to_int(line->args);
	switch (node_type_of(arga[0])) {
	default:
		error(error_type_syntax, "invalid argument to PUT");
		break;
	case node_type_undef:
		break;
	case node_type_int:
		error(error_type_fatal, "TODO: PUT");
		break;
	}
}

/* SETDP.  Set the assumed Direct Page value (8-bit).  Addresses evaluated to
 * exist within this page will be assembled to use direct addressing, if
 * possible. */

static void pseudo_setdp(struct prog_line *line) {
	int nargs = node_array_count(line->args);
	if (nargs != 1) {
		error(error_type_syntax, "SETDP requires exactly one argument");
		return;
	}
	args_float_to_int(line->args);
	struct node **arga = node_array_of(line->args);
	switch (node_type_of(arga[0])) {
	default:
		error(error_type_syntax, "invalid argument to SETDP");
		break;
	case node_type_undef:
		cur_section->dp = -1;
		break;
	case node_type_int:
		cur_section->dp = arga[0]->data.as_int;
		// negative numbers imply no valid DP
		if (arga[0]->data.as_int >= 0)
			cur_section->dp &= 0xff;
		break;
	}
}

/* EXPORT.  Flag a symbol or macro for exporting in the symbols file. */

static void pseudo_export(struct prog_line *line) {
	int nargs = node_array_count(line->args);
	if (nargs < 1) {
		error(error_type_syntax, "EXPORT requires one or more arguments");
		return;
	}
	struct node **arga = node_array_of(line->args);
	for (int i = 0; i < nargs; i++) {
		struct node *n = eval_string(arga[i]);
		if (n) {
			prog_export(n->data.as_string);
			node_free(n);
		}
	}
}

/* FCC, FCB.  Embed string and byte constants. */

static void pseudo_fcc(struct prog_line *line) {
	int nargs = node_array_count(line->args);
	if (nargs < 1)
		return;
	args_float_to_int(line->args);
	struct node **arga = node_array_of(line->args);
	for (int i = 0; i < nargs; i++) {
		switch (node_type_of(arga[i])) {
		default:
			error(error_type_syntax, "invalid argument to FCB/FCC");
			break;
		case node_type_undef:
			sect_emit(sect_emit_type_pad, 1);
			break;
		case node_type_empty:
			sect_emit(sect_emit_type_imm8, 0);
			break;
		case node_type_int:
			sect_emit(sect_emit_type_imm8, arga[i]->data.as_int);
			break;
		case node_type_string:
			for (int j = 0; arga[i]->data.as_string[j]; j++) {
				sect_emit(sect_emit_type_imm8, arga[i]->data.as_string[j]);
			}
			break;
		}
	}
}

/* FDB.  Embed 16-bit constants. */

static void pseudo_fdb(struct prog_line *line) {
	int nargs = node_array_count(line->args);
	if (nargs < 1)
		return;
	args_float_to_int(line->args);
	struct node **arga = node_array_of(line->args);
	for (int i = 0; i < nargs; i++) {
		switch (node_type_of(arga[i])) {
		default:
			error(error_type_syntax, "invalid argument to FDB");
			break;
		case node_type_undef:
			sect_emit(sect_emit_type_pad, 2);
			break;
		case node_type_empty:
			sect_emit(sect_emit_type_imm16, 0);
			break;
		case node_type_int:
			sect_emit(sect_emit_type_imm16, arga[i]->data.as_int);
			break;
		}
	}
}

/* RZB.  Reserve zero bytes. */

static void pseudo_rzb(struct prog_line *line) {
	int nargs = node_array_count(line->args);
	if (nargs != 1) {
		// TODO: support a fill value
		error(error_type_syntax, "RZB requires exactly one argument");
		return;
	}
	args_float_to_int(line->args);
	struct node **arga = node_array_of(line->args);
	switch (node_type_of(arga[0])) {
	default:
		error(error_type_syntax, "invalid argument to RZB");
		break;
	case node_type_undef:
		break;
	case node_type_int:
		if (arga[0]->data.as_int < 0) {
			error(error_type_out_of_range, "negative argument to RZB");
		} else {
			for (int i = 0; i < arga[0]->data.as_int; i++)
				sect_emit(sect_emit_type_imm8, 0);
		}
		break;
	}
}

/* RMB.  Reserve memory. */

static void pseudo_rmb(struct prog_line *line) {
	int nargs = node_array_count(line->args);
	if (nargs != 1) {
		error(error_type_syntax, "RMB requires exactly one argument");
		return;
	}
	args_float_to_int(line->args);
	struct node **arga = node_array_of(line->args);
	switch (node_type_of(arga[0])) {
	default:
		error(error_type_syntax, "invalid argument to RMB");
		break;
	case node_type_undef:
		break;
	case node_type_int:
		if (arga[0]->data.as_int < 0) {
			error(error_type_out_of_range, "negative argument to RMB");
		} else {
			cur_section->pc += arga[0]->data.as_int;
		}
		break;
	}
}

/* INCLUDE.  Nested inclusion of source files. */

/* TODO: extra arguments should become available as positional variables. */

static void pseudo_include(struct prog_line *line) {
	int nargs = node_array_count(line->args);
	if (nargs < 1) {
		error(error_type_syntax, "INCLUDE requires a filename");
		return;
	}
	struct node **arga = node_array_of(line->args);
	if (node_type_of(arga[0]) != node_type_string) {
		error(error_type_syntax, "invalid argument to INCLUDE");
		return;
	}
	struct prog *file = prog_new_file(arga[0]->data.as_string);
	if (!file)
		return;
	assemble_prog(file, asm_pass);
}

/* INCLUDEBIN.  Include a binary object in-place.  Unlike INCLUDE, the filename
 * may be a forward reference, as binary objects cannot introduce new local
 * labels. */

static void pseudo_includebin(struct prog_line *line) {
	int nargs = node_array_count(line->args);
	if (nargs < 1) {
		error(error_type_syntax, "INCLUDEBIN requires a filename");
		return;
	}
	struct node **arga = node_array_of(line->args);
	if (node_type_of(arga[0]) != node_type_string) {
		error(error_type_syntax, "invalid argument to INCLUDEBIN");
		return;
	}
	FILE *f = fopen(arga[0]->data.as_string, "rb");
	if (!f) {
		error(error_type_fatal, "file not found: %s", arga[0]->data.as_string);
		return;
	}
	int c;
	while ((c = fgetc(f)) != EOF) {
		sect_emit(sect_emit_type_imm8, c);
	}
	fclose(f);
}

/* MACRO.  Start defining a named macro.  The macro name can either be
 * specified as an argument or as the label for the line the directive appears
 * on. */

static void pseudo_macro(struct prog_line *line) {
	int nargs = node_array_count(line->args);
	struct node **arga = node_array_of(line->args);
	const char *name;
	if (nargs == 1 && !line->label) {
		name = arga[0]->data.as_string;
	} else if (nargs == 0 && node_type_of(line->label) == node_type_string) {
		name = line->label->data.as_string;
	} else {
		error(error_type_syntax, "macro name must either be label OR argument");
		return;
	}
	struct prog *macro = prog_macro_by_name(name);
	if (macro) {
		if (macro->pass == asm_pass)
			error(error_type_syntax, "macro '%s' redefined", name);
		return;
	}
	macro = prog_new_macro(name);
	macro->pass = asm_pass;
	defining_macro_ctx = prog_ctx_new(macro);
}

/* ENDM.  Finish a macro definition.  If an argument appears, it must match the
 * name of the macro being defined. */

/* TODO: argument not actually checked yet */

static void pseudo_endm(struct prog_line *line) {
	int nargs = node_array_count(line->args);
	struct node **arga = node_array_of(line->args);
	if (nargs > 1) {
		error(error_type_syntax, "invalid number of arguments to ENDM");
		return;
	}
	if (nargs > 0 && node_type_of(arga[0]) != node_type_string) {
		error(error_type_syntax, "invalid argument to ENDM");
		return;
	}
	if (!defining_macro_ctx)
		return;
	prog_ctx_free(defining_macro_ctx);
	defining_macro_ctx = NULL;
}
