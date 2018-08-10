/* -*-mode:c; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */

/*
  Copyright (c) 2018 Rian Hunter

  Permission is hereby granted, free of charge, to any person obtaining a copy
  of this software and associated documentation files (the "Software"), to deal
  in the Software without restriction, including without limitation the rights
  to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
  copies of the Software, and to permit persons to whom the Software is
  furnished to do so, subject to the following conditions:

  The above copyright notice and this permission notice shall be included in all
  copies or substantial portions of the Software.

  THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
  IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
  FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
  AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
  LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
  OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
  SOFTWARE.
 */

#include <wasmjit/compile.h>

#include <wasmjit/ast.h>
#include <wasmjit/util.h>
#include <wasmjit/vector.h>
#include <wasmjit/runtime.h>

#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define FUNC_EXIT_CONT SIZE_MAX

static DEFINE_VECTOR_GROW(memrefs, struct MemoryReferences);

struct BranchPoints {
	size_t n_elts;
	struct BranchPointElt {
		size_t branch_offset;
		size_t continuation_idx;
	} *elts;
};

static DEFINE_VECTOR_GROW(bp, struct BranchPoints);

struct LabelContinuations {
	size_t n_elts;
	size_t *elts;
};

static DEFINE_VECTOR_GROW(labels, struct LabelContinuations);

struct StaticStack {
	size_t n_elts;
	struct StackElt {
		enum {
			STACK_I32 = VALTYPE_I32,
			STACK_I64 = VALTYPE_I64,
			STACK_F32 = VALTYPE_F32,
			STACK_F64 = VALTYPE_F64,
			STACK_LABEL,
		} type;
		union {
			struct {
				size_t arity;
				size_t continuation_idx;
			} label;
		} data;
	} *elts;
};

static DEFINE_VECTOR_GROW(stack, struct StaticStack);
static DEFINE_VECTOR_TRUNCATE(stack, struct StaticStack);

static int push_stack(struct StaticStack *sstack, unsigned type)
{
	assert(type == STACK_I32 ||
	       type == STACK_I64 || type == STACK_F32 || type == STACK_F64);
	if (!stack_grow(sstack, 1))
		return 0;
	sstack->elts[sstack->n_elts - 1].type = type;
	return 1;
}

static unsigned peek_stack(struct StaticStack *sstack)
{
	assert(sstack->n_elts);
	return sstack->elts[sstack->n_elts - 1].type;
}

static int pop_stack(struct StaticStack *sstack)
{
	assert(sstack->n_elts);
	return stack_truncate(sstack, sstack->n_elts - 1);
}

static void encode_le_uint32_t(uint32_t val, char *buf)
{
	uint32_t le_val = uint32_t_swap_bytes(val);
	memcpy(buf, &le_val, sizeof(le_val));
}

#define MMIN(x, y) (((x) < (y)) ? (x) : (y))

#define OUTS(str)					   \
	do {						   \
		if (!output_buf(output, str, strlen(str))) \
			goto error;			   \
	}						   \
	while (0)

#define OUTB(b)						   \
	do {						   \
		char __b;				   \
		assert((b) <= 127 && ((intmax_t)(b)) >= -128);  \
		__b = (b);				   \
		if (!output_buf(output, &__b, 1))	   \
			goto error;			   \
	}						   \
	while (0)

struct LocalsMD {
	wasmjit_valtype_t valtype;
	int32_t fp_offset;
};

static int wasmjit_compile_instructions(const struct FuncType *func_types,
					const struct ModuleTypes *module_types,
					const struct FuncType *type,
					const struct CodeSectionCode *code,
					const struct Instr *instructions,
					size_t n_instructions,
					struct SizedBuffer *output,
					struct LabelContinuations *labels,
					struct BranchPoints *branches,
					struct MemoryReferences *memrefs,
					struct LocalsMD *locals_md,
					size_t n_locals,
					size_t n_frame_locals,
					struct StaticStack *sstack);

static int emit_br_code(struct SizedBuffer *output,
			struct StaticStack *sstack,
			struct BranchPoints *branches,
			uint32_t labelidx)
{
	char buf[0x100];
	size_t arity;
	size_t je_offset_2, j;
	int32_t stack_shift;
	uint32_t olabelidx = labelidx;
	/* find out bottom of stack to L */
	j = sstack->n_elts;
	while (j) {
		j -= 1;
		if (sstack->elts[j].type == STACK_LABEL) {
			if (!labelidx) {
				break;
			}
			labelidx--;
		}
	}

	arity = sstack->elts[j].data.label.arity;
	assert(sstack->n_elts >= j + (olabelidx + 1) + arity);
	if (__builtin_mul_overflow(sstack->n_elts - j - (olabelidx + 1) - arity,
				   8, &stack_shift))
		goto error;

	if (arity) {
		int32_t off;
		if (__builtin_mul_overflow(arity - 1, 8, &off))
			goto error;

		/* move top <arity> values for Lth label to
		   bottom of stack where Lth label is */

		/* LOGIC: memmove(sp + stack_shift * 8, sp, arity * 8); */

		/* mov %rsp, %rsi */
		OUTS("\x48\x89\xe6");

		if (arity - 1) {
			/* add <(arity - 1) * 8>, %rsi */
			OUTS("\x48\x03\x34\x25");
			encode_le_uint32_t(off, buf);
			if (!output_buf
			    (output, buf,
			     sizeof(uint32_t)))
				goto error;
		}

		/* mov %rsp, %rdi */
		OUTS("\x48\x89\xe7");

		/* add <(arity - 1 + stack_shift) * 8>, %rdi */
		if (arity - 1 + stack_shift) {
			/* (arity - 1 +  stack_shift) * 8 */
			int32_t si;
			if (__builtin_add_overflow(off, stack_shift, &si))
				goto error;

			OUTS("\x48\x81\xc7");
			encode_le_uint32_t(si, buf);
			if (!output_buf
			    (output, buf,
			     sizeof(uint32_t)))
				goto error;
		}

		/* mov <arity>, %rcx */
		OUTS("\x48\xc7\xc1");
		if (arity > INT32_MAX)
			goto error;
		encode_le_uint32_t(arity, buf);
		if (!output_buf
		    (output, buf, sizeof(uint32_t)))
			goto error;

		/* std */
		OUTS("\xfd");

		/* rep movsq */
		OUTS("\x48\xa5");
	}

	/* increment esp to Lth label (simulating pop) */
	/* add <stack_shift * 8>, %rsp */
	if (stack_shift) {
		OUTS("\x48\x81\xc4");
		encode_le_uint32_t(stack_shift, buf);
		if (!output_buf
		    (output, buf, sizeof(uint32_t)))
			goto error;
	}

	/* place jmp to Lth label */

	/* jmp <BRANCH POINT> */
	je_offset_2 = output->n_elts;
	OUTS("\xe9\x90\x90\x90\x90");

	/* add jmp offset to branches list */
	{
		size_t branch_idx;

		branch_idx = branches->n_elts;
		if (!bp_grow(branches, 1))
			goto error;

		branches->
			elts[branch_idx].branch_offset =
			je_offset_2;
		branches->
			elts[branch_idx].continuation_idx =
			sstack->elts[j].data.
			label.continuation_idx;
	}

	return 1;

 error:
	return 0;
}

static int wasmjit_compile_instruction(const struct FuncType *func_types,
				       const struct ModuleTypes *module_types,
				       const struct FuncType *type,
				       const struct CodeSectionCode *code,
				       const struct Instr *instruction,
				       struct SizedBuffer *output,
				       struct LabelContinuations *labels,
				       struct BranchPoints *branches,
				       struct MemoryReferences *memrefs,
				       struct LocalsMD *locals_md,
				       size_t n_locals,
				       size_t n_frame_locals,
				       struct StaticStack *sstack)
{
	char buf[0x100];
	size_t *end_jumps = NULL;

#define BUFFMT(...)						\
	do {							\
		int ret;					\
		ret = snprintf(buf, sizeof(buf), __VA_ARGS__);	\
		if (ret < 0)					\
			goto error;				\
	}							\
	while (1)

#define INC_LABELS()				\
	do {					\
		int res;			\
		res = labels_grow(labels, 1);	\
		if (!res)			\
			goto error;		\
	}					\
	while (0)

	switch (instruction->opcode) {
	case OPCODE_UNREACHABLE:
		/* ud2 */
		OUTS("\x0f\0b");
		break;
	case OPCODE_NOP:
		break;
	case OPCODE_BLOCK:
	case OPCODE_LOOP: {
		size_t arity;
		size_t label_idx, stack_idx, output_idx;
		struct StackElt *elt;

		arity =
			instruction->data.block.blocktype !=
			VALTYPE_NULL ? 1 : 0;

		label_idx = labels->n_elts;
		INC_LABELS();

		stack_idx = sstack->n_elts;
		if (!stack_grow(sstack, 1))
			goto error;
		elt = &sstack->elts[stack_idx];
		elt->type = STACK_LABEL;
		elt->data.label.arity = arity;
		elt->data.label.continuation_idx = label_idx;

		output_idx = output->n_elts;
		wasmjit_compile_instructions(func_types,
					     module_types,
					     type,
					     code,
					     instruction->data.
					     block.instructions,
					     instruction->data.
					     block.n_instructions,
					     output, labels,
					     branches, memrefs,
					     locals_md, n_locals,
					     n_frame_locals,
					     sstack);

		/* shift stack results over label */
		memmove(&sstack->elts[stack_idx],
			&sstack->elts[sstack->n_elts - arity],
			arity * sizeof(sstack->elts[0]));
		if (!stack_truncate(sstack, stack_idx + arity))
			goto error;

		switch (instruction->opcode) {
		case OPCODE_BLOCK:
			labels->elts[label_idx] =
				output->n_elts;
			break;
		case OPCODE_LOOP:
			labels->elts[label_idx] = output_idx;
			break;
		default:
			assert(0);
			break;
		}
		break;
	}
	case OPCODE_IF: {
		int arity;
		size_t jump_to_else_offset, jump_to_after_else_offset,
			stack_idx, label_idx;

		/* test top of stack */
		assert(peek_stack(sstack) == STACK_I32);
		pop_stack(sstack);
		/* pop %rax */
		OUTS("\x58");

		/* if not true jump to else case */
		/* test %eax, %eax */
		OUTS("\x85\xc0");

		jump_to_else_offset = output->n_elts + 2;
		/* je else_offset */
		OUTS("\x0f\x84\x90\x90\x90\x90");

		/* output then case */
		label_idx = labels->n_elts;
		INC_LABELS();

		arity =
			instruction->data.if_.blocktype !=
			VALTYPE_NULL ? 1 : 0;
		{
			struct StackElt *elt;
			stack_idx = sstack->n_elts;
			if (!stack_grow(sstack, 1))
				goto error;
			elt = &sstack->elts[stack_idx];
			elt->type = STACK_LABEL;
			elt->data.label.arity = arity;
			elt->data.label.continuation_idx = label_idx;
		}

		wasmjit_compile_instructions(func_types,
					     module_types,
					     type,
					     code,
					     instruction->data.
					     if_.instructions_then,
					     instruction->data.
					     if_.n_instructions_then,
					     output, labels,
					     branches, memrefs,
					     locals_md, n_locals,
					     n_frame_locals,
					     sstack);

		/* if (else_exist) {
		   jump after else
		   }
		*/
		if (instruction->data.if_.n_instructions_else) {
			jump_to_after_else_offset = output->n_elts + 1;
			/* jmp after_else_offset */
			OUTS("\xe9\x90\x90\x90\x90");
		}

		/* fix up jump_to_else_offset */
		jump_to_else_offset = output->n_elts - jump_to_else_offset;
		encode_le_uint32_t(jump_to_else_offset - 4,
				   &output->elts[output->n_elts - jump_to_else_offset]);

		/* if (else_exist) {
		   output else case
		   }
		*/
		if (instruction->data.if_.n_instructions_else) {
			wasmjit_compile_instructions(func_types,
						     module_types,
						     type,
						     code,
						     instruction->data.
						     if_.instructions_else,
						     instruction->data.
						     if_.n_instructions_else,
						     output, labels,
						     branches, memrefs,
						     locals_md, n_locals,
						     n_frame_locals,
						     sstack);

			/* fix up jump_to_after_else_offset */
			jump_to_after_else_offset = output->n_elts - jump_to_after_else_offset;
			encode_le_uint32_t(jump_to_after_else_offset - 4,
					   &output->elts[output->n_elts - jump_to_after_else_offset]);
		}

		/* fix up static stack */
		/* shift stack results over label */
		memmove(&sstack->elts[stack_idx],
			&sstack->elts[sstack->n_elts - arity],
			arity * sizeof(sstack->elts[0]));
		if (!stack_truncate(sstack, stack_idx + arity))
			goto error;

		/* set labels position */
		labels->elts[label_idx] = output->n_elts;;
		break;
	}
	case OPCODE_BR_IF:
	case OPCODE_BR: {
		size_t je_offset;
		const struct BrIfExtra *extra;

		if (instruction->opcode == OPCODE_BR_IF) {
			/* LOGIC: v = pop_stack() */

			/* pop %rsi */
			assert(peek_stack(sstack) == STACK_I32);
			if (!pop_stack(sstack))
				goto error;
			OUTS("\x5e");

			/* LOGIC: if (v) br(); */

			/* testl %esi, %esi */
			OUTS("\x85\xf6");

			/* je AFTER_BR */
			je_offset = output->n_elts;
			OUTS("\x74\x01");

			extra = &instruction->data.br_if;
		}
		else {
			extra = &instruction->data.br;;
		}

		if (!emit_br_code(output, sstack, branches, extra->labelidx))
			goto error;

		if (instruction->opcode == OPCODE_BR_IF) {
			/* update je operand in previous if block */
			int ret;
			size_t offset =
				output->n_elts - je_offset - 2;
			assert(offset < 128 && offset > 0);
			ret =
				snprintf(buf, sizeof(buf), "\x74%c",
					 (int)offset);
			if (ret < 0)
				goto error;
			assert(strlen(buf) == 2);
			memcpy(&output->elts[je_offset], buf,
			       2);
		}

		break;
	}
	case OPCODE_BR_TABLE: {
		size_t table_offset, i, default_branch_offset;

		end_jumps = wasmjit_alloc_vector(instruction->data.br_table.n_labelidxs, sizeof(size_t), NULL);
		if (!end_jumps)
			goto error;

		/* jump to the right code based on the input value */

		/* pop %rax */
		OUTS("\x58");
		if (!pop_stack(sstack))
			goto error;

		/* cmp $const, %eax */
		OUTS("\x48\x3d");
		/* const = instruction->data.br_table.n_labelidxs */
		encode_le_uint32_t(instruction->data.br_table.n_labelidxs,
				   buf);
		if (!output_buf(output, buf, sizeof(uint32_t)))
			goto error;

		/* jae default_branch */
		OUTS("\x0f\x83\x90\x90\x90\x90");
		default_branch_offset = output->n_elts;

		/* lea 9(%rip), %rdx */
		OUTS("\x48\x8d\x15\x09\x00\x00\x00");
		/* movsxl (%rdx, %rax, 4), %rax */
		OUTS("\x48\x63\x04\x82");
		/* add %rdx, %rax */
		OUTS("\x48\x01\xd0");
		/* jmp *%rax */
		OUTS("\xff\xe0");

		/* output nop for each branch */
		table_offset = output->n_elts;
		for (i = 0; i < instruction->data.br_table.n_labelidxs; ++i) {
			OUTS("\x90\x90\x90\x90");
		}

		for (i = 0; i < instruction->data.br_table.n_labelidxs; ++i) {
			/* store ip offset */
			uint32_t ip_offset = output->n_elts - table_offset;
			encode_le_uint32_t(ip_offset,
					   &output->elts[table_offset + i * sizeof(uint32_t)]);

			/* output branch */
			if (!emit_br_code(output, sstack, branches,
					  instruction->data.br_table.labelidxs[i]))
				goto error;

			/* output jmp to end */
			OUTS("\xe9\x90\x90\x90\x90");
			end_jumps[i] = output->n_elts;
		}

		/* store ip offset, output default branch */
		encode_le_uint32_t(output->n_elts - default_branch_offset,
				   &output->elts[default_branch_offset - 4]);
		if (!emit_br_code(output, sstack, branches,
				  instruction->data.br_table.labelidx))
			goto error;

		/* store ip offsets of end */
		for (i = 0; i < instruction->data.br_table.n_labelidxs; ++i) {
			encode_le_uint32_t(output->n_elts - end_jumps[i],
					   &output->elts[end_jumps[i] - 4]);
		}

		break;
	}
	case OPCODE_RETURN:
		/* shift $arity values from top of stock to below */

		if (FUNC_TYPE_N_OUTPUTS(type)) {
			int32_t out;

			/* lea (arity - 1)*8(%rsp), %rsi */
			OUTS("\x48\x8d\x74\x24");
			OUTB(((intmax_t) (FUNC_TYPE_N_OUTPUTS(type) - 1)) * 8);

			/* lea (-8 * n_frame_locals)(%rbp), %rdi */
			OUTS("\x48\x8d\xbd");
			if (n_frame_locals == SIZE_MAX)
				goto error;
			if (__builtin_mul_overflow(n_frame_locals + 1, -8, &out))
				goto error;
			encode_le_uint32_t(out, buf);
			if (!output_buf(output, buf, sizeof(uint32_t)))
				goto error;

			/* mov $arity, %rcx */
			OUTS("\x48\xc7\xc1");
			encode_le_uint32_t(FUNC_TYPE_N_OUTPUTS(type), buf);
			if (!output_buf(output, buf, sizeof(uint32_t)))
				goto error;

			/* std */
			OUTS("\xfd");

			/* rep movsq */
			OUTS("\x48\xa5");
		}

		/* adjust stack to top of arity */
		/* lea (arity + n_frame_locals)*-8(%rbp), %rsp */
		OUTS("\x48\x8d\xa5");
		if (n_frame_locals > SIZE_MAX - FUNC_TYPE_N_OUTPUTS(type))
			goto error;
		{
			int32_t out;
			if (__builtin_mul_overflow(n_frame_locals + FUNC_TYPE_N_OUTPUTS(type), -8, &out))
				goto error;
			encode_le_uint32_t(out, buf);
			if (!output_buf(output, buf, sizeof(uint32_t)))
				goto error;
		}

		/* jmp <EPILOGUE> */
		{
			size_t branch_idx;

			branch_idx = branches->n_elts;
			if (!bp_grow(branches, 1))
				goto error;

			branches->elts[branch_idx].branch_offset =
				output->n_elts;
			branches->elts[branch_idx].continuation_idx =
				FUNC_EXIT_CONT;

			OUTS("\xe9\x90\x90\x90\x90");
		}

		break;
	case OPCODE_CALL:
	case OPCODE_CALL_INDIRECT: {
		size_t i;
		size_t n_movs, n_xmm_movs, n_stack;
		int aligned = 0;
		const struct FuncType *ft;
		size_t cur_stack_depth = n_frame_locals;

		/* add current stack depth */
		for (i = sstack->n_elts; i;) {
			i -= 1;
			if (sstack->elts[i].type != STACK_LABEL) {
				cur_stack_depth += 1;
			}
		}

		if (instruction->opcode == OPCODE_CALL_INDIRECT) {
			ft = &func_types[instruction->data.call_indirect.typeidx];
			assert(peek_stack(sstack) == STACK_I32);
			if (!pop_stack(sstack))
				goto error;

			/* mov $const, %rdi */
			OUTS("\x48\xbf\x90\x90\x90\x90\x90\x90\x90\x90");
			{
				size_t memref_idx;
				memref_idx = memrefs->n_elts;
				if (!memrefs_grow(memrefs, 1))
					goto error;

				memrefs->elts[memref_idx].type =
					MEMREF_TABLE;
				memrefs->elts[memref_idx].code_offset =
					output->n_elts - 8;
				memrefs->elts[memref_idx].idx =
					0;
			}

			/* mov $const, %rsi */
			OUTS("\x48\xbe\x90\x90\x90\x90\x90\x90\x90\x90");
			{
				size_t memref_idx;
				memref_idx = memrefs->n_elts;
				if (!memrefs_grow(memrefs, 1))
					goto error;

				memrefs->elts[memref_idx].type =
					MEMREF_TYPE;
				memrefs->elts[memref_idx].code_offset =
					output->n_elts - 8;
				memrefs->elts[memref_idx].idx =
					instruction->data.call_indirect.typeidx;
			}

			/* pop %rdx */
			OUTS("\x5a");

			/* mov $const, %rax */
			OUTS("\x48\xb8\x90\x90\x90\x90\x90\x90\x90\x90");
			// address of _resolve_indirect_call
			{
				size_t memref_idx;
				memref_idx = memrefs->n_elts;
				if (!memrefs_grow(memrefs, 1))
					goto error;

				memrefs->elts[memref_idx].type =
					MEMREF_RESOLVE_INDIRECT_CALL;
				memrefs->elts[memref_idx].code_offset =
					output->n_elts - 8;
			}

			/* align to 16 bytes */
			if (cur_stack_depth % 2)
				/* sub $8, %rsp */
				OUTS("\x48\x83\xec\x08");

			/* call *%rax */
			OUTS("\xff\xd0");

			if (cur_stack_depth % 2)
				/* add $8, %rsp */
				OUTS("\x48\x83\xc4\x08");
		} else {
			uint32_t fidx =
				instruction->data.call.funcidx;
			ft = &module_types->functypes[fidx];

			/* movq $const, %rax */
			{
				size_t memref_idx;

				memref_idx = memrefs->n_elts;
				if (!memrefs_grow(memrefs, 1))
					goto error;

				memrefs->elts[memref_idx].type =
					MEMREF_FUNC;
				memrefs->elts[memref_idx].code_offset =
					output->n_elts + 2;
				memrefs->elts[memref_idx].idx = fidx;

				OUTS("\x48\xb8\x90\x90\x90\x90\x90\x90\x90\x90");
			}

			/* mov compiled_code_off(%rax), %rax */
			OUTS("\x48\x8b\x40");
			OUTB(offsetof(struct FuncInst, compiled_code));
		}

		static const char *const movs[] = {
			"\x48\x8b\xbc\x24",	/* mov N(%rsp), %rdi */
			"\x48\x8b\xb4\x24",	/* mov N(%rsp), %rsi */
			"\x48\x8b\x94\x24",	/* mov N(%rsp), %rdx */
			"\x48\x8b\x8c\x24",	/* mov N(%rsp), %rcx */
			"\x4c\x8b\x84\x24",	/* mov N(%rsp), %r8 */
			"\x4c\x8b\x8c\x24",	/* mov N(%rsp), %r9 */
		};

		static const char *const f32_movs[] = {
			"\xf3\x0f\x10\x84\x24",	/* movss N(%rsp), %xmm0 */
			"\xf3\x0f\x10\x8c\x24",	/* movss N(%rsp), %xmm1 */
			"\xf3\x0f\x10\x94\x24",	/* movss N(%rsp), %xmm2 */
			"\xf3\x0f\x10\x9c\x24",	/* movss N(%rsp), %xmm3 */
			"\xf3\x0f\x10\xa4\x24",	/* movss N(%rsp), %xmm4 */
			"\xf3\x0f\x10\xac\x24",	/* movss N(%rsp), %xmm5 */
			"\xf3\x0f\x10\xb4\x24",	/* movss N(%rsp), %xmm6 */
			"\xf3\x0f\x10\xbc\x24",	/* movss N(%rsp), %xmm7 */
		};

		static const char *const f64_movs[] = {
			"\xf2\x0f\x10\x84\x24",	/* movsd N(%rsp), %xmm0 */
			"\xf2\x0f\x10\x8c\x24",	/* movsd N(%rsp), %xmm1 */
			"\xf2\x0f\x10\x94\x24",	/* movsd N(%rsp), %xmm2 */
			"\xf2\x0f\x10\x9c\x24",	/* movsd N(%rsp), %xmm3 */
			"\xf2\x0f\x10\xa4\x24",	/* movsd N(%rsp), %xmm4 */
			"\xf2\x0f\x10\xac\x24",	/* movsd N(%rsp), %xmm5 */
			"\xf2\x0f\x10\xb4\x24",	/* movsd N(%rsp), %xmm6 */
			"\xf2\x0f\x10\xbc\x24",	/* movsd N(%rsp), %xmm7 */
		};

		/* align stack to 16-byte boundary */
		{
			/* add stack contribution from spilled arguments */
			n_movs = 0;
			n_xmm_movs = 0;
			for (i = 0; i < ft->n_inputs; ++i) {
				if ((ft->input_types[i] == VALTYPE_I32 ||
				     ft->input_types[i] == VALTYPE_I64)
				    && n_movs < 6) {
					n_movs += 1;
				} else if (ft->input_types[i] ==
					   VALTYPE_F32
					   && n_xmm_movs < 8) {
					n_xmm_movs += 1;
				} else if (ft->input_types[i] ==
					   VALTYPE_F64
					   && n_xmm_movs < 8) {
					n_xmm_movs += 1;
				} else {
					cur_stack_depth += 1;
				}
			}


			aligned = cur_stack_depth % 2;
			if (aligned)
				/* sub $8, %rsp */
				OUTS("\x48\x83\xec\x08");
		}

		n_movs = 0;
		n_xmm_movs = 0;
		n_stack = 0;
		for (i = 0; i < ft->n_inputs; ++i) {
			intmax_t stack_offset;
			assert(sstack->
			       elts[sstack->n_elts - ft->n_inputs +
				    i].type ==
			       ft->input_types[i]);

			stack_offset =
				(ft->n_inputs - i - 1 + n_stack + aligned) * 8;

			/* mov -n_inputs + i(%rsp), %rdi */
			if ((ft->input_types[i] == VALTYPE_I32 ||
			     ft->input_types[i] == VALTYPE_I64)
			    && n_movs < 6) {
				OUTS(movs[n_movs]);
				n_movs += 1;
			} else if (ft->input_types[i] ==
				   VALTYPE_F32
				   && n_xmm_movs < 8) {
				OUTS(f32_movs[n_xmm_movs]);
				n_xmm_movs += 1;
			} else if (ft->input_types[i] ==
				   VALTYPE_F64
				   && n_xmm_movs < 8) {
				OUTS(f64_movs[n_xmm_movs]);
				n_xmm_movs += 1;
			} else {
				stack_offset =
					(i - (ft->n_inputs - 1) + n_stack + aligned) * 8;
				OUTS("\xff\xb4\x24");	/* push N(%rsp) */
				n_stack += 1;
			}

			encode_le_uint32_t(stack_offset, buf);
			if (!output_buf(output, buf, sizeof(uint32_t)))
				goto error;
		}

		/* call *%rax */
		OUTS("\xff\xd0");

		/* clean up stack */
		/* add (n_stack + n_inputs + aligned) * 8, %rsp */
		OUTS("\x48\x81\xc4");
		encode_le_uint32_t((n_stack + ft->n_inputs + aligned) * 8,
				   buf);
		if (!output_buf(output, buf, sizeof(uint32_t)))
			goto error;

		if (!stack_truncate(sstack,
				    sstack->n_elts -
				    ft->n_inputs))
			goto error;

		if (FUNC_TYPE_N_OUTPUTS(ft)) {
			assert(FUNC_TYPE_N_OUTPUTS(ft) == 1);
			if (FUNC_TYPE_OUTPUT_TYPES(ft)[0] == VALTYPE_F32 ||
			    FUNC_TYPE_OUTPUT_TYPES(ft)[0] == VALTYPE_F64) {
				/* movq %xmm0, %rax */
				OUTS("\x66\x48\x0f\x7e\xc0");
			}
			/* push %rax */
			OUTS("\x50");

			if (!push_stack(sstack, FUNC_TYPE_OUTPUT_TYPES(ft)[0]))
				goto error;
		}
		break;
	}
	case OPCODE_DROP:
		/* add $8, %rsp */
		OUTS("\x48\x83\xc4\x08");
		if (!pop_stack(sstack))
			goto error;
		break;
	case OPCODE_GET_LOCAL:
		assert(instruction->data.get_local.localidx < n_locals);
		push_stack(sstack,
			   locals_md[instruction->data.
				     get_local.localidx].valtype);

		/* push fp_offset(%rbp) */
		OUTS("\xff\xb5");
		encode_le_uint32_t(locals_md
				   [instruction->data.get_local.localidx]
				   .fp_offset, buf);
		if (!output_buf(output, buf, sizeof(uint32_t)))
			goto error;
		break;
	case OPCODE_SET_LOCAL:
		assert(peek_stack(sstack) ==
		       locals_md[instruction->data.
				 set_local.localidx].valtype);

		/* pop fp_offset(%rbp) */
		OUTS("\x8f\x85");
		encode_le_uint32_t(locals_md
				   [instruction->data.set_local.localidx]
				   .fp_offset, buf);
		if (!output_buf(output, buf, sizeof(uint32_t)))
			goto error;
		pop_stack(sstack);
		break;
	case OPCODE_TEE_LOCAL:
		assert(peek_stack(sstack) ==
		       locals_md[instruction->data.
				 tee_local.localidx].valtype);

		/* movq (%rsp), %rax */
		OUTS("\x48\\x85");
		/* movq %rax, fp_offset(%rbp) */
		OUTS("\x48\x89\x45");
		encode_le_uint32_t(locals_md
				   [instruction->data.tee_local.localidx]
				   .fp_offset, buf);
		if (!output_buf(output, buf, sizeof(uint32_t)))
			goto error;
		break;
	case OPCODE_GET_GLOBAL: {
		uint32_t gidx = instruction->data.get_global.globalidx;
		unsigned type;

		/* movq $const, %rax */
		OUTS("\x48\xb8\x90\x90\x90\x90\x90\x90\x90\x90");
		{
			size_t memref_idx;

			memref_idx = memrefs->n_elts;
			if (!memrefs_grow(memrefs, 1))
				goto error;

			memrefs->elts[memref_idx].type =
				MEMREF_GLOBAL;
			memrefs->elts[memref_idx].code_offset =
				output->n_elts - 8;
			memrefs->elts[memref_idx].idx =
				gidx;
		}

		type = module_types->globaltypes[gidx].valtype;
		switch (type) {
		case VALTYPE_I32:
		case VALTYPE_F32:
			/* mov offset(%rax), %eax */
			OUTS("\x8b\x40");
			if (type == VALTYPE_I32) {
				OUTB(offsetof(struct GlobalInst, value) +
				     offsetof(struct Value, data) +
				     offsetof(union ValueUnion, i32));
			} else {
				OUTB(offsetof(struct GlobalInst, value) +
				     offsetof(struct Value, data) +
				     offsetof(union ValueUnion, f32));
			}
			break;
		case VALTYPE_I64:
		case VALTYPE_F64:
			/* mov offset(%rax), %rax */
			OUTS("\x48\x8b\x40");
			if (type == VALTYPE_I64) {
				OUTB(offsetof(struct GlobalInst, value) +
				     offsetof(struct Value, data) +
				     offsetof(union ValueUnion, i64));
			} else {
				OUTB(offsetof(struct GlobalInst, value) +
				     offsetof(struct Value, data) +
				     offsetof(union ValueUnion, f64));
			}
			break;
		default:
			assert(0);
			break;
		}


		/* push %rax*/
		OUTS("\x50");
		push_stack(sstack, type);

		break;
	}
	case OPCODE_SET_GLOBAL: {
		uint32_t gidx = instruction->data.get_global.globalidx;
		unsigned type = module_types->globaltypes[gidx].valtype;

		/* pop %rdx */
		OUTS("\x5a");

		assert(peek_stack(sstack) == type);
		if (!pop_stack(sstack))
			goto error;

		/* movq $const, %rax */
		OUTS("\x48\xb8\x90\x90\x90\x90\x90\x90\x90\x90");
		{
			size_t memref_idx;

			memref_idx = memrefs->n_elts;
			if (!memrefs_grow(memrefs, 1))
				goto error;

			memrefs->elts[memref_idx].type =
				MEMREF_GLOBAL;
			memrefs->elts[memref_idx].code_offset =
				output->n_elts - 8;
			memrefs->elts[memref_idx].idx =
				gidx;
		}

		type = module_types->globaltypes[gidx].valtype;
		switch (type) {
		case VALTYPE_I32:
		case VALTYPE_F32:
			/* mov %edx, offset(%rax) */
			OUTS("\x89\x50");
			if (type == VALTYPE_I32) {
				OUTB(offsetof(struct GlobalInst, value) +
				     offsetof(struct Value, data) +
				     offsetof(union ValueUnion, i32));
			} else {
				OUTB(offsetof(struct GlobalInst, value) +
				     offsetof(struct Value, data) +
				     offsetof(union ValueUnion, f32));
			}
			break;
		case VALTYPE_I64:
		case VALTYPE_F64:
			/* mov %rdx, offset(%rax) */
			OUTS("\x48\x89\x50");
			if (type == VALTYPE_I64) {
				OUTB(offsetof(struct GlobalInst, value) +
				     offsetof(struct Value, data) +
				     offsetof(union ValueUnion, i64));
			} else {
				OUTB(offsetof(struct GlobalInst, value) +
				     offsetof(struct Value, data) +
				     offsetof(union ValueUnion, f64));
			}
			break;
		default:
			assert(0);
			break;
		}

		break;
	}
	case OPCODE_I32_LOAD:
	case OPCODE_I64_LOAD:
	case OPCODE_F64_LOAD:
	case OPCODE_I32_LOAD8_S:
	case OPCODE_I32_STORE:
	case OPCODE_I64_STORE:
	case OPCODE_F64_STORE:
	case OPCODE_I32_STORE8:
	case OPCODE_I32_STORE16: {
		const struct LoadStoreExtra *extra;

		switch (instruction->opcode) {
		case OPCODE_I32_LOAD:
			extra = &instruction->data.i32_load;
			break;
		case OPCODE_I64_LOAD:
			extra = &instruction->data.i64_load;
			break;
		case OPCODE_F64_LOAD:
			extra = &instruction->data.f64_load;
			break;
		case OPCODE_I32_LOAD8_S:
			extra = &instruction->data.i32_load8_s;
			break;
		case OPCODE_I32_STORE:
			assert(peek_stack(sstack) == STACK_I32);
			extra = &instruction->data.i32_store;
			goto after;
		case OPCODE_I32_STORE8:
			assert(peek_stack(sstack) == STACK_I32);
			extra = &instruction->data.i32_store8;
			goto after;
		case OPCODE_I32_STORE16:
			assert(peek_stack(sstack) == STACK_I32);
			extra = &instruction->data.i32_store16;
			goto after;
		case OPCODE_I64_STORE:
			assert(peek_stack(sstack) == STACK_I64);
			extra = &instruction->data.i64_store;
			goto after;
		case OPCODE_F64_STORE:
			assert(peek_stack(sstack) == STACK_F64);
			extra = &instruction->data.f64_store;
		after:
			if (!pop_stack(sstack))
				goto error;

			/* pop rdi */
			OUTS("\x5f");
			break;
		default:
			assert(0);
			break;
		}

		/* LOGIC: ea = pop_stack() */

		/* pop %rsi */
		assert(peek_stack(sstack) == STACK_I32);
		if (!pop_stack(sstack))
			goto error;
		OUTS("\x5e");

		if (4 + extra->offset != 0) {
			/* LOGIC: ea += memarg.offset + 4 */

			/* add <VAL>, %rsi */
			OUTS("\x48\x81\xc6");
			encode_le_uint32_t(4 +
					   extra->offset, buf);
			if (!output_buf(output, buf, sizeof(uint32_t)))
				goto error;
		}

		{
			/* LOGIC: size = store->mems.elts[maddr].size */

			/* movq $const, %rax */
			OUTS("\x48\xb8\x90\x90\x90\x90\x90\x90\x90\x90");

			/* add reference to max */
			{
				size_t memref_idx;

				memref_idx = memrefs->n_elts;
				if (!memrefs_grow(memrefs, 1))
					goto error;

				memrefs->elts[memref_idx].type =
					MEMREF_MEM;
				memrefs->elts[memref_idx].code_offset =
					output->n_elts - 8;
				memrefs->elts[memref_idx].idx =
					0;
			}

			/* mov size_offset(%rax), %rax */
			OUTS("\x48\x8b\x40");
			OUTB(offsetof(struct MemInst, size));

			/* LOGIC: if ea > size then trap() */

			/* cmp %rax, %rsi */
			OUTS("\x48\x39\xc6");

			/* jle AFTER_TRAP: */
			/* int $4 */
			/* AFTER_TRAP1  */
			OUTS("\x7e\x02\xcd\x04");
		}

		/* LOGIC: data = store->mems.elts[maddr].data */
		{
			/* movq $const, %rax */
			OUTS("\x48\xb8\x90\x90\x90\x90\x90\x90\x90\x90");

			/* add reference to data */
			{
				size_t memref_idx;

				memref_idx = memrefs->n_elts;
				if (!memrefs_grow(memrefs, 1))
					goto error;

				memrefs->elts[memref_idx].type =
					MEMREF_MEM;
				memrefs->elts[memref_idx].code_offset =
					output->n_elts - 8;
				memrefs->elts[memref_idx].idx =
					0;
			}

			/* mov data_off(%rax), %rax */
			OUTS("\x48\x8b\x40");
			OUTB(offsetof(struct MemInst, data));
		}


		switch (instruction->opcode) {
		case OPCODE_I32_LOAD:
		case OPCODE_I32_LOAD8_S:
		case OPCODE_F64_LOAD:
		case OPCODE_I64_LOAD: {
			unsigned valtype;

			/* LOGIC: push_stack(data[ea - 4]) */
			switch (instruction->opcode) {
			case OPCODE_I32_LOAD8_S:
				/* movsbl -4(%rax, %rsi), %eax */
				OUTS("\x0f\xbe\x44\x30\xfc");
				valtype = STACK_I32;
				break;
			case OPCODE_I32_LOAD:
				/* movl -4(%rax, %rsi), %eax */
				OUTS("\x8b\x44\x30\xfc");
				valtype = STACK_I32;
				break;
			case OPCODE_I64_LOAD:
			case OPCODE_F64_LOAD:
				/* movq -4(%rax, %rsi), %rax */
				OUTS("\x48\x8b\x44\x30\xfc");
				switch (instruction->opcode) {
				case OPCODE_I64_LOAD: valtype = STACK_I64; break;
				case OPCODE_F64_LOAD: valtype = STACK_F64; break;
				default: assert(0); break;
				}
				break;
			default:
				assert(0);
				break;
			}

			/* push %rax */
			OUTS("\x50");
			if (!push_stack(sstack, valtype))
				goto error;

			break;
		}
		case OPCODE_I32_STORE:
			/* LOGIC: data[ea - 4] = pop_stack() */
			/* movl %edi, -4(%rax, %rsi) */
			OUTS("\x89\x7c\x30\xfc");
			break;
		case OPCODE_I32_STORE8:
			/* LOGIC: data[ea - 4] = pop_stack() */
			/* movb %dil, -4(%rax, %rsi) */
			OUTS("\x40\x88\x7c\x30\xfc");
			break;
		case OPCODE_I32_STORE16:
			/* movw %di, -4(%rax, %rsi) */
			OUTS("\x66\x89\x7c\x30\xfc");
			break;
		case OPCODE_I64_STORE:
		case OPCODE_F64_STORE:
			/* LOGIC: data[ea - 4] = pop_stack() */
			/* movq %rdi, -4(%rax, %rsi) */
			OUTS("\x48\x89\x7c\x30\xfc");
			break;
		default:
			assert(0);
			break;
		}

		break;
	}
	case OPCODE_I32_CONST:
		/* push $value */
		OUTS("\x68");
		encode_le_uint32_t(instruction->data.i32_const.value,
				   buf);
		if (!output_buf(output, buf, sizeof(uint32_t)))
			goto error;
		push_stack(sstack, STACK_I32);
		break;
	case OPCODE_I64_CONST:
		/* movq $value, %rax */
		OUTS("\x48\xb8");
		encode_le_uint64_t(instruction->data.i64_const.value,
				   buf);
		if (!output_buf(output, buf, sizeof(uint64_t)))
			goto error;

		/* push %rax */
		OUTS("\x50");

		push_stack(sstack, STACK_I64);
		break;
	case OPCODE_F64_CONST: {
		uint64_t bitrepr;
		/* movq $value, %rax */
		OUTS("\x48\xb8");
#ifndef		__STDC_IEC_559__
#error We dont support non-IEC 449 floats
#endif

		memcpy(&bitrepr, &instruction->data.f64_const.value,
		       sizeof(uint64_t));

		encode_le_uint64_t(bitrepr, buf);
		if (!output_buf(output, buf, sizeof(uint64_t)))
			goto error;

		/* push %rax */
		OUTS("\x50");

		push_stack(sstack, STACK_F64);
		break;
	}
	case OPCODE_I32_EQZ:
		assert(peek_stack(sstack) == STACK_I32);
		/* xor %eax, %eax */
		OUTS("\x31\xc0");
		/* cmpl $0, (%rsp) */
		OUTS("\x83\x3c\x24\x00");
		/* sete %al */
		OUTS("\x0f\x94\xc0");
		/* mov %eax, (%rsp) */
		OUTS("\x89\x04\x24");
		break;
	case OPCODE_I32_EQ:
	case OPCODE_I32_NE:
	case OPCODE_I32_LT_S:
	case OPCODE_I32_LT_U:
	case OPCODE_I32_GT_S:
	case OPCODE_I32_GT_U:
	case OPCODE_I32_LE_S:
	case OPCODE_I32_LE_U:
	case OPCODE_I32_GE_S:
	case OPCODE_I64_EQ:
	case OPCODE_I64_NE:
	case OPCODE_I64_LT_S:
	case OPCODE_I64_GT_U: {
		unsigned stack_type;

		switch (instruction->opcode) {
		case OPCODE_I64_EQ:
		case OPCODE_I64_NE:
		case OPCODE_I64_LT_S:
		case OPCODE_I64_GT_U:
			stack_type = STACK_I64;
			break;
		default:
			stack_type = STACK_I32;
			break;
		}

		assert(peek_stack(sstack) == stack_type);
		pop_stack(sstack);

		assert(peek_stack(sstack) == stack_type);
		pop_stack(sstack);

		/* popq %rdi */
		OUTS("\x5f");

		/* xor %(e|r)ax, %(e|r)ax */
		if (stack_type == STACK_I64)
			OUTS("\x48");
		OUTS("\x31\xc0");

		/* cmp %(r|e)di, (%rsp) */
		if (stack_type == STACK_I64)
			OUTS("\x48");
		OUTS("\x39\x3c\x24");

		switch (instruction->opcode) {
		case OPCODE_I32_EQ:
		case OPCODE_I64_EQ:
			/* sete %al */
			OUTS("\x0f\x94\xc0");
			break;
		case OPCODE_I32_NE:
		case OPCODE_I64_NE:
			OUTS("\x0f\x95\xc0");
			break;
		case OPCODE_I32_LT_S:
		case OPCODE_I64_LT_S:
			/* setl %al */
			OUTS("\x0f\x9c\xc0");
			break;
		case OPCODE_I32_LT_U:
			/* setb %al */
			OUTS("\x0f\x92\xc0");
			break;
		case OPCODE_I32_GT_S:
			/* setg %al */
			OUTS("\x0f\x9f\xc0");
			break;
		case OPCODE_I32_GT_U:
		case OPCODE_I64_GT_U:
			/* seta %al */
			OUTS("\x0f\x97\xc0");
			break;
		case OPCODE_I32_LE_S:
			/* setle  %al */
			OUTS("\x0f\x9e\xc0");
			break;
		case OPCODE_I32_LE_U:
			/* setbe %al */
			OUTS("\x0f\x96\xc0");
			break;
		case OPCODE_I32_GE_S:
			/* setge %al */
			OUTS("\x0f\x9d\xc0");
			break;
		default:
			assert(0);
			break;
		}

		/* mov %(r|e)ax, (%rsp) */
		if (stack_type == STACK_I64)
			OUTS("\x48");
		OUTS("\x89\x04\x24");

		push_stack(sstack, STACK_I32);
		break;
	}
	case OPCODE_F64_EQ:
	case OPCODE_F64_NE: {
		assert(peek_stack(sstack) == STACK_F64);
		pop_stack(sstack);

		assert(peek_stack(sstack) == STACK_F64);
		pop_stack(sstack);

		/* movsd (%rsp), %xmm0 */
		OUTS("\xf2\x0f\x10\x04\x24");
		/* add $8, %rsp */
		OUTS("\x48\x83\xc4\x08");
		/* xor %eax, %eax */
		OUTS("\x31\xc0");

		switch (instruction->opcode) {
		case OPCODE_F64_EQ:
			/* xor %edx, %edx */
			OUTS("\x31\xd2");
			break;
		case OPCODE_F64_NE:
			/* mov $1, %edx */
			OUTS("\xba\x01\x00\x00\x00");
			break;
		}

		/* ucomisd (%rsp), %xmm0 */
		OUTS("\x66\x0f\x2e\x04\x24");

		switch (instruction->opcode) {
		case OPCODE_F64_EQ:
			/* setnp %al */
			OUTS("\x0f\x9b\xc0");
			/* cmovne %edx, %eax */
			OUTS("\x0f\x45\xc2");
			break;
		case OPCODE_F64_NE:
			/* setp %al */
			OUTS("\x0f\x9a\xc0");
			/* cmovne %edx, %eax */
			OUTS("\x0f\x45\xc2");
			break;
		}

		/* mov %rax, (%rsp) */
		OUTS("\x48\x89\x04\x24");

		push_stack(sstack, STACK_I32);

		break;
	}
	case OPCODE_I32_SUB:
	case OPCODE_I32_ADD:
	case OPCODE_I32_MUL:
	case OPCODE_I32_AND:
	case OPCODE_I32_OR:
	case OPCODE_I32_XOR:
	case OPCODE_I64_ADD:
	case OPCODE_I64_SUB:
	case OPCODE_I64_MUL:
	case OPCODE_I64_AND:
	case OPCODE_I64_OR: {
		unsigned stack_type;

		switch (instruction->opcode) {
		case OPCODE_I64_ADD:
		case OPCODE_I64_SUB:
		case OPCODE_I64_MUL:
		case OPCODE_I64_AND:
		case OPCODE_I64_OR:
			stack_type = STACK_I64;
			break;
		default:
			stack_type = STACK_I32;
			break;
		}

		/* popq %rax */
		assert(peek_stack(sstack) == stack_type);
		pop_stack(sstack);
		OUTS("\x58");

		assert(peek_stack(sstack) == stack_type);

		if (stack_type == STACK_I64)
			OUTS("\x48");

		switch (instruction->opcode) {
		case OPCODE_I32_SUB:
		case OPCODE_I64_SUB:
			/* sub    %(r|e)ax,(%rsp) */
			OUTS("\x29\x04\x24");
			break;
		case OPCODE_I64_ADD:
		case OPCODE_I32_ADD:
			/* add    %eax,(%rsp) */
			OUTS("\x01\x04\x24");
			break;
		case OPCODE_I32_MUL:
		case OPCODE_I64_MUL:
			/* mul(q|l) (%rsp) */
			OUTS("\xf7\x24\x24");
			if (stack_type == STACK_I64)
				OUTS("\x48");
			/* mov    %(r|e)ax,(%rsp) */
			OUTS("\x89\x04\x24");
			break;
		case OPCODE_I32_AND:
		case OPCODE_I64_AND:
			/* and    %eax,(%rsp) */
			OUTS("\x21\x04\x24");
			break;
		case OPCODE_I32_OR:
		case OPCODE_I64_OR:
			/* or    %eax,(%rsp) */
			OUTS("\x09\x04\x24");
			break;
		case OPCODE_I32_XOR:
			/* xor    %eax,(%rsp) */
			OUTS("\x31\x04\x24");
			break;
		default:
			assert(0);
			break;
		}

		break;
	}
	case OPCODE_I32_DIV_S:
	case OPCODE_I32_DIV_U:
	case OPCODE_I32_REM_S:
	case OPCODE_I32_REM_U:
	case OPCODE_I64_DIV_S:
	case OPCODE_I64_DIV_U:
	case OPCODE_I64_REM_S:
	case OPCODE_I64_REM_U: {
		unsigned stack_type;
		switch (instruction->opcode) {
		case OPCODE_I32_DIV_S:
		case OPCODE_I32_DIV_U:
		case OPCODE_I32_REM_S:
		case OPCODE_I32_REM_U:
			stack_type = STACK_I32;
			break;
		case OPCODE_I64_DIV_S:
		case OPCODE_I64_DIV_U:
		case OPCODE_I64_REM_S:
		case OPCODE_I64_REM_U:
			stack_type = STACK_I64;
			break;
		}

		assert(peek_stack(sstack) == stack_type);
		pop_stack(sstack);

		assert(peek_stack(sstack) == stack_type);

		/* pop %rdi */
		OUTS("\x5f");

		/* mov (%rsp), %(r|e)ax */
		if (stack_type == STACK_I64)
			OUTS("\x48");
		OUTS("\x8b\x04\x24");

		if (stack_type == STACK_I64)
			OUTS("\x48");

		switch (instruction->opcode) {
		case OPCODE_I32_DIV_S:
		case OPCODE_I32_REM_S:
		case OPCODE_I64_DIV_S:
		case OPCODE_I64_REM_S:
			/* cld|cqto */
			OUTS("\x99");
			/* idiv %(r|e)di */
			if (stack_type == STACK_I64)
				OUTS("\x48");
			OUTS("\xf7\xff");
			break;
		case OPCODE_I32_DIV_U:
		case OPCODE_I32_REM_U:
		case OPCODE_I64_DIV_U:
		case OPCODE_I64_REM_U:
			/* xor %(r|e)dx, %(r|e)dx */
			OUTS("\x31\xd2");
			/* div %(r|e)di */
			if (stack_type == STACK_I64)
				OUTS("\x48");
			OUTS("\xf7\xf7");
			break;
		}

		if (stack_type == STACK_I64)
			OUTS("\x48");

		switch (instruction->opcode) {
		case OPCODE_I32_REM_S:
		case OPCODE_I32_REM_U:
		case OPCODE_I64_REM_S:
		case OPCODE_I64_REM_U:
			/* mov %(r|e)dx, (%rsp) */
			OUTS("\x89\x14\x24");
			break;
		default:
			/* mov %(e|r)ax, (%rsp) */
			OUTS("\x89\x04\x24");
			break;
		}

		break;
	}
	case OPCODE_I32_SHL:
	case OPCODE_I32_SHR_S:
	case OPCODE_I32_SHR_U:
	case OPCODE_I64_SHL:
	case OPCODE_I64_SHR_S:
	case OPCODE_I64_SHR_U: {
		unsigned stack_type;

		switch (instruction->opcode) {
		case OPCODE_I64_SHL:
		case OPCODE_I64_SHR_S:
		case OPCODE_I64_SHR_U:
			stack_type = STACK_I64;
			break;
		default:
			stack_type = STACK_I32;
			break;
		}

		/* pop %rcx */
		OUTS("\x59");
		assert(peek_stack(sstack) == stack_type);
		pop_stack(sstack);

		assert(peek_stack(sstack) == stack_type);

		if (stack_type == STACK_I64)
			OUTS("\x48");

		switch (instruction->opcode) {
		case OPCODE_I32_SHL:
		case OPCODE_I64_SHL:
			/* shl(l|q)   %cl,(%rsp) */
			OUTS("\xd3x24\x24");
			break;
		case OPCODE_I32_SHR_S:
		case OPCODE_I64_SHR_S:
			/* sar(l|q) %cl, (%rsp) */
			OUTS("\xd3\x3c\x24");
			break;
		case OPCODE_I32_SHR_U:
		case OPCODE_I64_SHR_U:
			/* shr(l|q) %cl, (%rsp) */
			OUTS("\xd3\x2c\x24");
			break;
		}

		break;
	}
	case OPCODE_F64_NEG:
		assert(peek_stack(sstack) == STACK_F64);
		/* btcq   $0x3f,(%rsp)  */
		OUTS("\x48\x0f\xba\x3c\x24\x3f");
		break;
	case OPCODE_F64_ADD:
	case OPCODE_F64_SUB:
	case OPCODE_F64_MUL:
		assert(peek_stack(sstack) == STACK_F64);
		pop_stack(sstack);

 		assert(peek_stack(sstack) == STACK_F64);

		/* movsd (%rsp), %xmm0 */
		OUTS("\xf2\x0f\x10\x07");
		/* add $8, %rsp */
		OUTS("\x48\x8d\x47\x08");

		switch (instruction->opcode) {
		case OPCODE_F64_ADD:
			/* addsd (%rsp), %xmm0 */
			OUTS("\xf2\x0f\x58\x04\x24");
			break;
		case OPCODE_F64_SUB:
			/* subsd (%rsp), %xmm0 */
			OUTS("\xf2\x0f\x5c\x04\x24");
			break;
		case OPCODE_F64_MUL:
			/* mulsd (%rsp), %xmm0 */
			OUTS("\xf2\x0f\x59\x47\x08");
			break;
		default:
			assert(0);
			break;
		}
		/* movsd %xmm0,(%rsp) */
		OUTS("\xf2\x0f\x11\x04\x24");
		break;
	case OPCODE_I32_WRAP_I64:
		assert(peek_stack(sstack) == STACK_I64);
		pop_stack(sstack);

		/* mov $0xffffffff,%eax */
		OUTS("\xb8\xff\xff\xff\xff");
		/* and %rax,(%rdi) */
		OUTS("\x48\x21\x07");

		if (!push_stack(sstack, STACK_I32))
			goto error;

		break;
	case OPCODE_I32_TRUNC_U_F64:
	case OPCODE_I32_TRUNC_S_F64:
		assert(peek_stack(sstack) == STACK_F64);
		pop_stack(sstack);

		/* cvttsd2si (%rsp), %eax */
		OUTS("\xf2\x0f\x2c\x04\x24");

		/* mov %rax, (%rsp) */
		OUTS("\x48\x89\x04\x24");

		if (!push_stack(sstack, STACK_I32))
			goto error;
		break;
	case OPCODE_I64_EXTEND_S_I32:
		assert(peek_stack(sstack) == STACK_I32);
		pop_stack(sstack);

		/* movsxl (%rsp), %rax */
		OUTS("\x48\x63\x04\x24");
		/* mov %rax, (%rsp) */
		OUTS("\x48\x89\x04\x24");

		if (!push_stack(sstack, STACK_I64))
			goto error;

		break;
	case OPCODE_I64_EXTEND_U_I32:
		assert(peek_stack(sstack) == STACK_I32);
		pop_stack(sstack);

		/* NB: don't need to do anything,
		   we store 32-bits as zero-extended 64-bits
		 */

		if (!push_stack(sstack, STACK_I64))
			goto error;
		break;
	case OPCODE_F64_CONVERT_S_I32:
	case OPCODE_F64_CONVERT_U_I32:
		assert(peek_stack(sstack) == STACK_I32);
		pop_stack(sstack);

		switch (instruction->opcode) {
		case OPCODE_F64_CONVERT_S_I32:
			/* cvtsi2sdl (%rsp),%xmm0 */
			OUTS("\xf2\x0f\x2a\x04\x24");
			break;
		case OPCODE_F64_CONVERT_U_I32:
			/* mov (%rsp), %eax */
			OUTS("\x8b\x04\x24");
			/* cvtsi2sd %rax,%xmm0 */
			OUTS("\xf2\x48\x0f\x2a\xc0");
			break;
		}

		/* movsd %xmm0,(%rsp) */
		OUTS("\xf2\x0f\x11\x04\x24");

		if (!push_stack(sstack, STACK_F64))
			goto error;
		break;
	case OPCODE_I64_REINTERPRET_F64:
		assert(peek_stack(sstack) == STACK_F64);
		pop_stack(sstack);

		/* no need to do anything */

		if (!push_stack(sstack, STACK_I64))
			goto error;
		break;
	case OPCODE_F64_REINTERPRET_I64:
		assert(peek_stack(sstack) == STACK_I64);
		pop_stack(sstack);

		/* no need to do anything */

		if (!push_stack(sstack, STACK_F64))
			goto error;
		break;
	default:
		fprintf(stderr, "Unhandled Opcode: 0x%" PRIx8 "\n", instruction->opcode);
		assert(0);
		break;
	}

#undef INC_LABELS
#undef BUFFMT

	return 1;

 error:
	assert(0);
	return 0;

}


static int wasmjit_compile_instructions(const struct FuncType *func_types,
					const struct ModuleTypes *module_types,
					const struct FuncType *type,
					const struct CodeSectionCode *code,
					const struct Instr *instructions,
					size_t n_instructions,
					struct SizedBuffer *output,
					struct LabelContinuations *labels,
					struct BranchPoints *branches,
					struct MemoryReferences *memrefs,
					struct LocalsMD *locals_md,
					size_t n_locals,
					size_t n_frame_locals,
					struct StaticStack *sstack)
{
	size_t i;

	for (i = 0; i < n_instructions; ++i) {
		if (!wasmjit_compile_instruction(func_types,
						 module_types,
						 type,
						 code,
						 &instructions[i],
						 output,
						 labels,
						 branches,
						 memrefs,
						 locals_md,
						 n_locals,
						 n_frame_locals,
						 sstack))
			goto error;
	}

	return 1;

 error:
	return 0;
}

char *wasmjit_compile_function(const struct FuncType *func_types,
			       const struct ModuleTypes *module_types,
			       const struct FuncType *type,
			       const struct CodeSectionCode *code,
			       struct MemoryReferences *memrefs, size_t *out_size)
{
	char buf[0x100];
	struct SizedBuffer outputv = { 0, NULL };
	struct SizedBuffer *output = &outputv;
	struct BranchPoints branches = { 0, NULL };
	struct StaticStack sstack = { 0, NULL };
	struct LabelContinuations labels = { 0, NULL };
	struct LocalsMD *locals_md;
	size_t n_frame_locals;
	size_t n_locals;

	{
		size_t i;
		n_locals = type->n_inputs;
		for (i = 0; i < code->n_locals; ++i) {
			n_locals += code->locals[i].count;
		}
	}

	{
		size_t n_movs = 0, n_xmm_movs = 0, n_stack = 0, i;

		locals_md = calloc(n_locals, sizeof(locals_md[0]));
		if (!locals_md)
			goto error;

		for (i = 0; i < type->n_inputs; ++i) {
			if ((type->input_types[i] == VALTYPE_I32 ||
			     type->input_types[i] == VALTYPE_I64) &&
			    n_movs < 6) {
				locals_md[i].fp_offset =
				    -(1 + n_movs + n_xmm_movs) * 8;
				n_movs += 1;
			} else if ((type->input_types[i] == VALTYPE_F32 ||
				    type->input_types[i] == VALTYPE_F64) &&
				   n_xmm_movs < 8) {
				locals_md[i].fp_offset =
				    -(1 + n_movs + n_xmm_movs) * 8;
				n_xmm_movs += 1;
			} else {
				int32_t off = 2 * 8;
				int32_t si; /* (n_stack + 2) * 8) */
				if (__builtin_mul_overflow(n_stack, 8, &si))
					goto error;
				if (__builtin_add_overflow(si, off, &si))
					goto error;
				locals_md[i].fp_offset = si;
				n_stack += 1;
			}
			locals_md[i].valtype = type->input_types[i];
		}

		for (i = 0; i < n_locals - type->n_inputs; ++i) {
			int32_t off = -(1 + n_movs + n_xmm_movs) * 8;
			int32_t si; /* -(1 + n_movs + n_xmm_movs + i) * 8; */
			if (__builtin_mul_overflow(-8, i, &si))
				goto error;
			if (__builtin_add_overflow(si, off, &si))
				goto error;
			locals_md[i + type->n_inputs].fp_offset = si;
		}

		{
			size_t off = type->n_inputs;
			for (i = 0; i < code->n_locals; ++i) {
				size_t j;
				for (j = 0; j < code->locals[i].count; j++) {
					locals_md[off].valtype =  code->locals[i].valtype;
					off += 1;
				}
			}
		}

		if (n_locals - type->n_inputs > SIZE_MAX - (n_movs + n_xmm_movs))
			goto error;
		n_frame_locals = n_movs + n_xmm_movs + (n_locals - type->n_inputs);
	}

	/* output prologue, i.e. create stack frame */
	{
		size_t n_movs = 0, n_xmm_movs = 0, i;

		static char *const movs[] = {
			"\x48\x89\x7d",	/* mov %rdi, N(%rbp) */
			"\x48\x89\x75",	/* mov %rsi, N(%rbp) */
			"\x48\x89\x55",	/* mov %rdx, N(%rbp) */
			"\x48\x89\x4d",	/* mov %rcx, N(%rbp) */
			"\x4c\x89\x45",	/* mov %r8, N(%rbp) */
			"\x4c\x89\x4d",	/* mov %r9, N(%rbp) */
		};

		static const char *const f32_movs[] = {
			"\xf3\x0f\x11\x45",	/* movss %xmm0, N(%rbp) */
			"\xf3\x0f\x11\x4d",	/* movss %xmm1, N(%rbp) */
			"\xf3\x0f\x11\x55",	/* movss %xmm2, N(%rbp) */
			"\xf3\x0f\x11\x5d",	/* movss %xmm3, N(%rbp) */
			"\xf3\x0f\x11\x65",	/* movss %xmm4, N(%rbp) */
			"\xf3\x0f\x11\x6d",	/* movss %xmm5, N(%rbp) */
			"\xf3\x0f\x11\x75",	/* movss %xmm6, N(%rbp) */
			"\xf3\x0f\x11\x7d",	/* movss %xmm7, N(%rbp) */
		};

		static const char *const f64_movs[] = {
			"\xf2\x0f\x11\x45",	/* movsd %xmm0, N(%rbp) */
			"\xf2\x0f\x11\x4d",	/* movsd %xmm1, N(%rbp) */
			"\xf2\x0f\x11\x55",	/* movsd %xmm2, N(%rbp) */
			"\xf2\x0f\x11\x5d",	/* movsd %xmm3, N(%rbp) */
			"\xf2\x0f\x11\x65",	/* movsd %xmm4, N(%rbp) */
			"\xf2\x0f\x11\x6d",	/* movsd %xmm5, N(%rbp) */
			"\xf2\x0f\x11\x75",	/* movsd %xmm6, N(%rbp) */
			"\xf2\x0f\x11\x7d",	/* movsd %xmm7, N(%rbp) */
		};

		/* push %rbp */
		OUTS("\x55");

		/* mov %rsp, %rbp */
		OUTS("\x48\x89\xe5");

		/* generate breakpoint on function entrance for now */
		OUTS("\xcc");

		/* sub $(8 * (n_frame_locals)), %rsp */
		if (n_frame_locals) {
			int32_t out;
			OUTS("\x48\x81\xec");
			if (__builtin_mul_overflow(n_frame_locals, 8, &out))
				goto error;
			encode_le_uint32_t(out, buf);
			if (!output_buf(output, buf, sizeof(uint32_t)))
				goto error;
		}

		/* push args to stack */
		for (i = 0; i < type->n_inputs; ++i) {
			if (locals_md[i].fp_offset > 0)
				continue;

			if (type->input_types[i] == VALTYPE_I32 ||
			    type->input_types[i] == VALTYPE_I64) {
				OUTS(movs[n_movs]);
				n_movs += 1;
			} else {
				if (type->input_types[i] == VALTYPE_F32) {
					OUTS(f32_movs[n_xmm_movs]);
				} else {
					assert(type->input_types[i] ==
					       VALTYPE_F64);
					OUTS(f64_movs[n_xmm_movs]);
				}
				n_xmm_movs += 1;
			}
			OUTB(locals_md[i].fp_offset);
		}

		/* initialize and push locals to stack */
		if (n_locals - type->n_inputs) {
			if (n_locals - type->n_inputs == 1) {
				/* movq $0, (%rsp) */
				if (!output_buf
				    (output, "\x48\xc7\x04\x24\x00\x00\x00\x00",
				     8))
					goto error;
			} else {
				char buf[sizeof(uint32_t)];
				/* mov %rsp, %rdi */
				OUTS("\x48\x89\xe7");
				/* xor %rax, %rax */
				OUTS("\x48\x31\xc0");
				/* mov $n_locals, %rcx */
				OUTS("\x48\xc7\xc1");
				if (n_locals - type->n_inputs > INT32_MAX)
					goto error;
				encode_le_uint32_t(n_locals - type->n_inputs, buf);
				if (!output_buf(output, buf, sizeof(uint32_t)))
					goto error;
				/* cld */
				OUTS("\xfc");
				/* rep stosq */
				OUTS("\xf3\x48\xab");
			}
		}
	}

	if (!wasmjit_compile_instructions(func_types, module_types, type, code,
					  code->instructions, code->n_instructions,
					  output, &labels, &branches, memrefs,
					  locals_md, n_locals, n_frame_locals, &sstack))
		goto error;

	/* fix branch points */
	{
		size_t i;
		for (i = 0; i < branches.n_elts; ++i) {
			char buf[1 + sizeof(uint32_t)] = { 0xe9 };
			struct BranchPointElt *branch = &branches.elts[i];
			size_t continuation_offset = (branch->continuation_idx == FUNC_EXIT_CONT)
				? output->n_elts
				: labels.elts[branch->continuation_idx];
			uint32_t rel =
			    continuation_offset - branch->branch_offset -
			    sizeof(buf);
			encode_le_uint32_t(rel, &buf[1]);
			memcpy(&output->elts[branch->branch_offset], buf,
			       sizeof(buf));
		}
	}

	/* output epilogue */

	if (FUNC_TYPE_N_OUTPUTS(type)) {
		assert(FUNC_TYPE_N_OUTPUTS(type) == 1);
		assert(sstack.n_elts == 1);
		assert(peek_stack(&sstack) == FUNC_TYPE_OUTPUT_TYPES(type)[0]);
		pop_stack(&sstack);
		/* pop %rax */
		OUTS("\x58");
	}

	/* add $(8 * (n_frame_locals)), %rsp */
	if (n_frame_locals) {
		int32_t out;
		OUTS("\x48\x81\xc4");
		if (__builtin_mul_overflow(n_frame_locals, 8, &out))
			goto error;
		encode_le_uint32_t(out, buf);
		if (!output_buf(output, buf, sizeof(uint32_t)))
			goto error;
	}

	/* pop %rbp */
	OUTS("\x5d");

	/* retq */
	OUTS("\xc3");

	*out_size = output->n_elts;

	return output->elts;

 error:
	assert(0);
	return NULL;
}

#undef OUTB
#undef OUTS
