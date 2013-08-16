/*
 * asm6809 - a 6809 assembler
 * Copyright 2013 Ciaran Anscomb
 *
 * See COPYING.GPL for redistribution conditions.
 */

#ifndef ASM6809_SECTION_H_
#define ASM6809_SECTION_H_

#include <stdint.h>

#include <glib.h>

/* Types of data that assembly instructions and pseudo-ops can pass to
 * section_emit() */

enum section_emit_type {
	section_emit_type_pad,
	section_emit_type_op_immediate,
	section_emit_type_op_direct,
	section_emit_type_op_indexed,
	section_emit_type_op_extended,
	section_emit_type_imm8,
	section_emit_type_imm16,
	section_emit_type_rel8,
	section_emit_type_rel16
};

/*
 * A section span is one region of consecutive data.  Reference counted so that
 * meta-sections can be created combining other sections, and coelesced.
 *
 * - sequence: Used to ensure later code overwrites earlier code when coelesced
 *   sections overlap.
 *
 * - org: Base address of instructions within span.
 *
 * - put: Address in memory to locate span.
 *
 * - size: Amount of consecutive bytes in span.  Increased by section_emit().
 */

struct section_span {
	unsigned ref;
	unsigned sequence;  // used for resolving overlaps when coalescing
	int org;
	unsigned put;
	unsigned size;
	unsigned allocated;
	uint8_t *data;
};

/*
 * A section comprises multiple spans.  While assembling, named sections are
 * created by section_set().  Later, unnamed sections are created in order to
 * coalesce span data for output.  Other important data tracked per section:
 *
 * - local_labels: A hash passed to symbol_local_*() to manipulate local
 *   labels.
 *
 * - pass: When a section is selected during a new pass, old data is destroyed.
 *
 * - line_number: A dummy line number associated with local labels, used in
 *   searching forward or back for the nearest one that matches.  Incremented
 *   by assemble_prog().  Must be consistent across passes otherwise the search
 *   results will not be correct, hence the requirement that all included
 *   source files and macro expansions occur within the first pass.
 *
 * - pc: Current Program Counter.  May be modified without emitting data (e.g.,
 *   by ORG or RMB), so if found not to match the current span's org + size, a
 *   new span needs to be created.
 *
 * - dp: Direct Page.  Each section gets its own idea of what the direct page
 *   is, as specified by SETDP.
 *
 * - last_pc: Maintained across passes, when switching sections the new one
 *   will default to coming after the last address in the previous.  Obviously
 *   this can be overridden with ORG.
 */

struct section {
	GSList *spans;
	struct section_span *span;
	GHashTable *local_labels;
	unsigned pass;
	unsigned line_number;
	int pc;
	unsigned put;
	unsigned dp;
	int last_pc;
};

/* Current section made available */
extern struct section *cur_section;

// - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - - -

/* Free one section */

void section_free(struct section *sect);

/* Free all *named* sections (tidy up) */

void section_free_all(void);

/* Select a named section or creates a new one if it does not already exist */

void section_set(const char *name, unsigned pass);

/* Check consistency of the end address of named sections. */

void section_finish_pass(unsigned pass);

/* Add data to the current section.  Additional arguments depend on the value
 * of type. */

void section_emit(enum section_emit_type type, ...);

/* Coalesce all the spans in a section.  Adjacent sequential spans are joined
 * together into one.  If sort is 1, spans are sorted first.  If pad is 1, all
 * spans are coalesced into one large span with zero padding between them. */

void section_coalesce(struct section *, _Bool sort, _Bool pad);

/* Coalesce all spans from all sections, returning a new unnamed section.  If
 * pad is 1, this will result in one large zero-padded span.  If more than one
 * section is involved, all spans will be sorted before coalescing. */

struct section *section_coalesce_all(_Bool pad);

#endif  /* ASM6809_SECTION_H_ */