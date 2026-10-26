/**
 * Code compiler for JIT compiling
 *
 * These functions are handling the collection of the macroblocks to the buffer,
 * the high level optimization of the register flow and the compiling of
 * the macroblocks into PowerPC executable instructions.
 */

#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "events.h"
#include "uae_malloc.h"
#include "include/memory.h"
#include "custom.h"
#include "newcpu.h"
#include "compemu.h"
#include "compemu_compiler.h"
#include "compemu_macroblock_structs.h"
#include "debug.h"

/* Number of maximum macroblocks we handle
 * Note: we assume that each instruction consists of 20 macroblocks in average,
 * this might be a wrong assumtion...
 * TODO: is there a better way to handle buffering of macroblocks?
 */
#define MAXMACROBLOCKS (MAXRUN*20)

/* Collection of pre-compiled macroblocks
 */
union comp_compiler_mb_union* macroblocks;

/**
 * Macro for initializing the basic macroblock structure
 * Parameters:
 *   n - name of the new macroblock structure
 *   h - handler function
 *   ir - input registers
 *   or - output registers
 */
#define comp_mb_init(n, h, ir, or) union comp_compiler_mb_union* n = comp_compiler_get_next_macroblock(); if(n == NULL) { comp_compile_error(); return; } n->base.handler=(h); n->base.input_registers=comp_mapped_reg_deps(ir); n->base.output_registers=comp_mapped_reg_deps(or); n->base.name = __func__; n->base.start = NULL; n->base.m68k_ptr = comp_current_m68k_location(); n->base.remove = FALSE;

//Pointer to the end of the macroblock buffer
int macroblock_ptr;

/**
 * Prototypes of the internal macroblock implementation functions
 */
void comp_macroblock_impl_load_register_long(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_load_register_word_extended(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_load_memory_spec(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_load_memory_spec_save_temps(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_load_memory_long(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_load_memory_word(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_load_memory_word_extended(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_load_memory_byte(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_map_physical_mem(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_load_memory_checked(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_save_memory_checked(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_stack_frame_alloc(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_stack_frame_free(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_shift_count(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_mull(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_bcd(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_bitfield_reg(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_bitfield_mem(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_bitfield_mark_low_byte(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_bitfield_skip_low_byte(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_bitfield_skip_target(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_save_memory_spec(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_save_memory_long(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_save_memory_word(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_save_memory_word_update(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_save_memory_byte(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_add_with_flags(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_add_register_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_add_extended_with_flags(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_add(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_sub(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_sub_with_flags(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_sub_extended_with_flags(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_sub_register_from_immediate(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_add_register_imm(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_add_high_register_imm(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_negate_with_overflow(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_copy_register_long_with_flags(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_copy_register_long(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_opcode_unsupported(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_fixregs_store(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_fpu(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_fixregs_load(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_and_low_register_imm(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_and_high_register_imm(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_and_register_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_and_register_complement_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_or_low_register_imm(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_or_high_register_imm(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_or_register_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_not_or_register_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_xor_register_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_xor_low_register_imm(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_xor_high_register_imm(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_and_registers(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_multiply_registers(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_multiply_registers_unsigned_high(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_multiply_registers_signed_high(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_copy_nzcv_flags_to_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_copy_nz_flags_to_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_copy_cv_flags_to_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_copy_register_to_cv_flags(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_check_byte_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_check_word_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_copy_register_word_extended(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_copy_register_byte_extended(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_rotate_and_copy_bits(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_rotate_and_mask_bits(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_rotate_and_mask_bits_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_arithmetic_shift_right_register_imm(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_logic_shift_left_register_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_logic_shift_right_register_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_arithmetic_shift_right_register_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_count_leading_zeroes_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_arithmetic_left_shift_extract_v_flag(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_shift_extract_c_flag(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_shift_extract_cx_flag(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_save_register_to_context(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_restore_register_from_context(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_set_byte_from_z_flag(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_or_negative_mask_if_n_flag_set(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_convert_ccr_to_internal(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_convert_internal_to_ccr(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_division_32_16bit(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_division_32_32bit(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_division_32_32bit_no_remainder(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_division_64_32bit(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_add_decimal(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_sub_decimal(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_negate_decimal(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_stop(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_nop(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_null_operation(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_load_pc_from_immediate_conditional(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_load_pc_from_immediate_conditional_decrement_register(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_trace_exit(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_trace_dbcc(union comp_compiler_mb_union* mb);
void comp_macroblock_impl_check_word_in_memory(union comp_compiler_mb_union* mb);

/**
 * Prototypes for local helper functions
 */
STATIC_INLINE void helper_access_memory_spec(union comp_compiler_mb_union* mb, int iswrite);
STATIC_INLINE void helper_map_physical_mem(comp_ppc_reg inreg, comp_ppc_reg outreg, comp_ppc_reg tmpreg);
STATIC_INLINE void helper_load_baseaddr_checked(comp_ppc_reg inreg, comp_ppc_reg tmpreg);
STATIC_INLINE void helper_call_memfb_stub(uae_u32 desc);

/* v18: CR field for the bit 0 test of baseaddr[] in checked memory
 * accesses. CR7 is volatile and not used otherwise by the JIT (CR0 and XER
 * hold the internal flags, CR1/CR2 are temporaries of other macroblocks). */
#define PPCR_CR_MEMCHECK	7
#define PPC_B_CR7_NE		0x009E	//0b0010011110: BO=00100 (branch if false), BI=30 (CR7 EQ)
STATIC_INLINE void helper_division_by_zero_check(BOOL signed_division, comp_ppc_reg dividend_reg, comp_exception_data* exception_data);
STATIC_INLINE void helper_divide_32_bit(BOOL signed_division, comp_ppc_reg quotient_reg, comp_ppc_reg dividend_reg, comp_ppc_reg divisor_reg, BOOL check_overflow);

/**
 * Allocate macroblock buffer
 *
 * Returns:
 *   TRUE if the allocation was successful, FALSE when failed.
 */
BOOL comp_alloc_macroblock_buffer()
{
	if (!macroblocks)
	{
		macroblocks = xcalloc(MAXMACROBLOCKS, sizeof(union comp_compiler_mb_union));

		write_log("JIT: allocated %d KB macroblock buffer.\n", (sizeof(union comp_compiler_mb_union) * MAXMACROBLOCKS) / 1024);
	}

	return (macroblocks != NULL);
}

/**
 * Free macroblock buffer
 */
void comp_free_macroblock_buffer()
{
	if (macroblocks)
	{
		xfree(macroblocks);
		macroblocks = NULL;

		write_log("JIT: Deallocated macroblock buffer.\n");
	}
}

/**
 * Initialization of the code compiler
 */
void comp_compiler_init()
{
	//Reset actual macroblock pointer
	macroblock_ptr = 0;
}

/**
 * Clean-up for the code compiler
 */
void comp_compiler_done()
{
	//Nothing to do for cleanup right now
}

/**
 * Push a macroblock to the next position of the buffer and increase the buffer top index
 * Parameters:
 *    macroblock - pointer to the macroblock that has to be pushed to buffer
 * Returns:
 *    pointer to the next available macroblock address in the buffer or
 *    null if no free macroblock left.
 * Note:
 *    This function warns the user in the console output when the macroblock array runs out.
 *    The warning is shown only once then it is suppressed.
 */
BOOL macroblock_overflow_warning = TRUE;
union comp_compiler_mb_union* comp_compiler_get_next_macroblock()
{
	if (macroblock_ptr == MAXMACROBLOCKS)
	{
		//Oops, we ran out of the array
		if (macroblock_overflow_warning)
		{
			write_log("Warning: JIT macroblock array ran out of space, your program might be doing something wrong\n");
			macroblock_overflow_warning = FALSE;
		}

		write_jit_log("Warning: JIT macroblock array ran out of space\n");

		return NULL;
	}

	union comp_compiler_mb_union* mb = &macroblocks[macroblock_ptr];
	macroblock_ptr++;

	return mb;
}

/**
 * Optimize the macroblocks in the buffer, the output goes back to the same buffer
 */
void comp_compiler_optimize_macroblocks()
{
	int i;

	//Optimize the compiled code only if it was enabled in the config
	if (!currprefs.compoptim) return;

	//Run thru the collected macroblocks in reverse order and calculate
	//registration usage flags
	union comp_compiler_mb_union* mb = macroblocks + (macroblock_ptr - 1);

	//At the end of the block we depend on all registers and flags, except internal flags
	uae_u64 carry = COMP_COMPILER_MACROBLOCK_REG_ALL;
	uae_u64 flagsin, flagsout;
	char remove;

	for(i = macroblock_ptr; i > 0; i-- , mb--)
	{
		flagsin = (mb->base.input_registers & (~COMP_COMPILER_MACROBLOCK_CONTROL_FLAGS));
		flagsout = (mb->base.output_registers & (~COMP_COMPILER_MACROBLOCK_CONTROL_FLAGS));

		//Remove the registers from the output that is not needed by the following instructions
		flagsout &= carry;

		//If no registers left in the output then this instruction is useless,
		//unless the "no optim" control flag was specified
		remove = mb->base.remove = (flagsout == 0) &&
					(((mb->base.output_registers & COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM) == 0) &&
						((mb->base.input_registers & COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM) == 0));

		//Which registers are not overwritten by this instruction must remain in the carry
		carry &= ~flagsout;

		//Should we keep this macroblock?
		if (!remove)
		{
			//Then add the registers which are required by this instruction to the carry
			carry |= flagsin;
		}

		//This step is only for the debug logging, it can be removed later on
		mb->base.carry_registers = carry;
	}
}

/**
 * Generate the PowerPC native code for the macroblocks in the buffer
 */
void comp_compiler_generate_code()
{
	int i;

	if (macroblock_ptr == 0)
	{
		//There are no macroblocks in the buffer
		write_jit_log("Warning: JIT macroblock array is empty at code generation\n");
		return;
	}

	//Run thru the collected macroblocks and call the code generator handler for each
	union comp_compiler_mb_union* mb = macroblocks;
	for(i = 0; i < macroblock_ptr; i++ , mb++)
	{
		comp_compiler_macroblock_func* handler = mb->base.handler;

		//If there is a handler then call it
		if ((handler) && (!mb->base.remove))
		{
			//Store the start of the compiled code
			mb->base.start = comp_ppc_buffer_top();

			//Call handler
			handler(mb);

			//Store the end of the compiled code
			mb->base.end = comp_ppc_buffer_top();
		}
	}
}

#ifdef JIT_DEBUG
/**
 * Dump the compiled code with the macroblocks to the console
 */
void comp_compiler_debug_dump_compiled()
{
	int i;
	uaecptr nextpc;
	char str[200];
	char inputregs_str[200];
	char outputregs_str[200];
	char carry_str[200];
	uae_u16* prev_68k_inst = NULL;

	//Dump the compiled only if it was enabled in the config
	if (!currprefs.complogcompiled || !currprefs.complog) return;

	//Run thru the collected macroblocks and dump the compiled native code for the
	//console using disassembler routine
	union comp_compiler_mb_union* mb = macroblocks;
	for(i = 0; i < macroblock_ptr; i++ , mb++)
	{
		//Macroblocks won't be listed if there was no corresponding M68k instruction to it
		if (mb->base.m68k_ptr)
		{
			//Is this the same M68k instruction?
			if (prev_68k_inst != mb->base.m68k_ptr)
			{
				//No: dump it
				m68k_disasm_str(str, (uaecptr) mb->base.m68k_ptr, &nextpc, 1);
				write_jit_log("M68k: %s", str);
				prev_68k_inst = mb->base.m68k_ptr;
			}

			//Dump the name of the macroblock (bit of a hack: skip the function suffix)
			write_jit_log("Mblk: %s%s\n", mb->base.name + 21, (mb->base.remove ? " *rem*": ""));
			comp_dump_reg_usage(mb->base.input_registers, inputregs_str, TRUE);
			comp_dump_reg_usage(mb->base.output_registers, outputregs_str, TRUE);
			comp_dump_reg_usage(mb->base.carry_registers, carry_str, FALSE);
			write_jit_log("Flg: %s:%s:%s\n", carry_str, inputregs_str, outputregs_str);

			//Dump disassembled code only if there was a start address for the compiled code
			if (mb->base.start)
			{
				disassemble_compiled(mb->base.start, mb->base.end);
			}
		}
	}
}
#endif

/***********************************************************************
 * Macroblock compiling handlers
 */

/**
 * Macroblock: Unsuppordted opcode interpretive handler callback compiling function
 * Note: this function purges all temporary registers, because it calls
 * an external function.
 * Parameters:
 *    opcode - unsupported opcode number
 */
/**
 * Macroblocks: v18 (TODO 7.4): write the fixed 68k registers back to
 * regs.regs[] / load them again, around code that uses regs.regs[]
 * (interpreter, C helpers, MOVEM through the bank handlers).
 * Nothing when the fixed registers are not active.
 */
void comp_macroblock_impl_fixregs_store(union comp_compiler_mb_union* mb)
{
	comp_fixregs_emit_store();
}

void comp_macroblock_impl_fixregs_load(union comp_compiler_mb_union* mb)
{
	comp_fixregs_emit_load();
}

void comp_macroblock_push_fixregs_store(void)
{
	if (!comp_fixregs_active()) return;
	{
		comp_mb_init(mb,
					comp_macroblock_impl_fixregs_store,
					comp_fixregs_deps(),
					COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM);
	}
}

void comp_macroblock_push_fixregs_load(void)
{
	if (!comp_fixregs_active()) return;
	{
		comp_mb_init(mb,
					comp_macroblock_impl_fixregs_load,
					COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM,
					comp_fixregs_deps() | COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM);
	}
}

void comp_macroblock_push_opcode_unsupported(uae_u16 opcode)
{
	//Before the call write back the M68k registers and clear the temp register mapping
	comp_flush_temp_registers(FALSE);
	comp_macroblock_push_fixregs_store();

	{
		comp_mb_init(mb,
					comp_macroblock_impl_opcode_unsupported,
					COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM,
					COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM);
		mb->unsupported.opcode = opcode;
	}

	//The interpreter may have changed any register
	comp_macroblock_push_fixregs_load();
}

void comp_macroblock_impl_opcode_unsupported(union comp_compiler_mb_union* mb)
{
	uae_u16 opcode = mb->unsupported.opcode;

	//Compile call to the interpretive emulator
	// ## liw	r3,opcode
	// ## liw	r4,&reg
	// ## liw	r0,inst_func
	// ## mtlr	r0
	// ## blrl
	comp_ppc_liw(PPCR_PARAM1_MAPPED, opcode);
	comp_ppc_liw(PPCR_PARAM2_MAPPED, (uae_u32) &regs);
	comp_ppc_call(PPCR_SPECTMP_MAPPED, (uae_uintptr) cpufunctbl[opcode]);
}


/**
 * Macroblock: Load flags
 * Read the M68k flags from the interpretive emulator's structure into a register
 * We assume the GCC PPC target, there are two fields in the structure: cznv and x
 * These two has to be merged into one register.
 * Have a look on md-ppc-gcc/m68k.h for the details.
 */
void comp_macroblock_push_load_flags()
{
	//TODO: this macroblock is more like a helper function, must be moved out
	comp_tmp_reg* tempreg = comp_allocate_temp_register(NULL, PPC_TMP_REG_NOTUSED_MAPPED);

	//Load flag_struct.cznv to the flags register
	comp_macroblock_push_load_memory_long(
			COMP_COMPILER_MACROBLOCK_REG_NONE,
			COMP_COMPILER_MACROBLOCK_REG_FLAGN | COMP_COMPILER_MACROBLOCK_REG_FLAGZ | COMP_COMPILER_MACROBLOCK_REG_FLAGC | COMP_COMPILER_MACROBLOCK_REG_FLAGV,
			PPCR_FLAGS_MAPPED,
			PPCR_REGS_BASE_MAPPED,
			COMP_GET_OFFSET_IN_REGS(ccrflags.cznv));

	//Load flag_struct.x to a temp register
	comp_macroblock_push_load_memory_long(
			COMP_COMPILER_MACROBLOCK_REG_NONE,
			tempreg->reg_usage_mapping,
			tempreg->mapped_reg_num,
			PPCR_REGS_BASE_MAPPED,
			COMP_GET_OFFSET_IN_REGS(ccrflags.x));

	//Rotate X flag to the position and insert it into the flag register
	comp_macroblock_push_rotate_and_copy_bits(
			tempreg->reg_usage_mapping,
			COMP_COMPILER_MACROBLOCK_REG_FLAGX,
			PPCR_FLAGS_MAPPED,
			tempreg->mapped_reg_num,
			16, 26, 26, FALSE);

	comp_free_temp_register(tempreg);
}

/**
 * Macroblock: Save flags
 * Write the M68k flags from register to the interpretive emulator's structure
 * We assume the GCC PPC target, there are two fields in the structure: cznv and x
 * These two fields both have to be saved, but no other operation is needed to separate the flags.
 * Have a look on md-ppc-gcc/m68k.h for the details.
 * Note: this is the same implementation as comp_ppc_save_flags() function, but
 *       prepares macroblocks instead of direct code emitting.
 */
void comp_macroblock_push_save_flags()
{
	//Save flags register to flag_struct.cznv and avoid optimize away this block
	comp_macroblock_push_save_memory_long(
			COMP_COMPILER_MACROBLOCK_REG_FLAGN | COMP_COMPILER_MACROBLOCK_REG_FLAGZ | COMP_COMPILER_MACROBLOCK_REG_FLAGC | COMP_COMPILER_MACROBLOCK_REG_FLAGV,
			COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM,
			PPCR_FLAGS_MAPPED,
			PPCR_REGS_BASE_MAPPED,
			COMP_GET_OFFSET_IN_REGS(ccrflags.cznv));

	//Save flags register to flag_struct.x and avoid optimize away this block
	//There is a little trick in this: to let us skip the shifting
	//operation, we store the lower half word of the flag register,
	//because X flag should go to the same bit as C flag in the other
	//field (flag_struct.cznv).
	comp_macroblock_push_save_memory_word(
			COMP_COMPILER_MACROBLOCK_REG_FLAGX,
			COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM,
			PPCR_FLAGS_MAPPED,
			PPCR_REGS_BASE_MAPPED,
			COMP_GET_OFFSET_IN_REGS(ccrflags.x));
}

/**
 * Macroblock: Loads the M68k PC
 * Note: this macroblock won't be optimized away.
 * 		 Same implementation as comp_ppc_load_pc() function, but it pushes macroblocks
 * 		 instead of emitting code directly.
 */
void comp_macroblock_push_load_pc(const cpu_history * inst_history)
{
	//TODO: this macroblock is more like a helper function, must be moved out
	comp_tmp_reg* temp_reg = comp_allocate_temp_register(NULL, PPC_TMP_REG_NOTUSED_MAPPED);

	//Load real memory pointer for the executed instruction
	comp_macroblock_push_load_register_long(
			temp_reg->reg_usage_mapping | COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM,
			temp_reg->mapped_reg_num,
			(uae_u32)inst_history->location);

	comp_macroblock_push_save_memory_long(
			temp_reg->reg_usage_mapping,
			COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM,
			temp_reg->mapped_reg_num,
			PPCR_REGS_BASE_MAPPED,
			COMP_GET_OFFSET_IN_REGS(pc_p));

	//Synchronize the executed instruction pointer from the block start to the actual
	comp_macroblock_push_save_memory_long(
			temp_reg->reg_usage_mapping,
			COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM,
			temp_reg->mapped_reg_num,
			PPCR_REGS_BASE_MAPPED,
			COMP_GET_OFFSET_IN_REGS(pc_oldp));

	//Load emulated instruction pointer (PC register)
	comp_macroblock_push_load_register_long(
			temp_reg->reg_usage_mapping | COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM,
			temp_reg->mapped_reg_num,
			(uae_u32)inst_history->pc);

	comp_macroblock_push_save_memory_long(
			temp_reg->reg_usage_mapping,
			COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM,
			temp_reg->mapped_reg_num,
			PPCR_REGS_BASE_MAPPED,
			COMP_GET_OFFSET_IN_REGS(pc));

	comp_free_temp_register(temp_reg);
}

/**
 * Macroblock: compare content of a word-sized data in physical memory to
 * a reference constant word and set consistency error flag if did not match.
 * Parameters:
 *    location - physical memory address
 *    content - reference word for the comparison
 */
void comp_macroblock_push_check_word_in_memory(uae_u16* location, uae_u16 content)
{
	//TODO: this macroblock is more like a helper function, must be moved out
	comp_tmp_reg* tmpreg = comp_allocate_temp_register(NULL, PPC_TMP_REG_NOTUSED_MAPPED);
	comp_tmp_reg* locreg = comp_allocate_temp_register(NULL, PPC_TMP_REG_NOTUSED_MAPPED);

	comp_mb_init(mb,
				comp_macroblock_impl_check_word_in_memory,
				COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM,
				COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM);
	mb->check_word_in_memory.location = (uae_u32) location;
	mb->check_word_in_memory.content = content;
	mb->check_word_in_memory.locreg = locreg->mapped_reg_num;
	mb->check_word_in_memory.tmpreg = tmpreg->mapped_reg_num;

	comp_free_temp_register(tmpreg);
	comp_free_temp_register(locreg);
}

void comp_macroblock_impl_check_word_in_memory(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg locreg = mb->check_word_in_memory.locreg;
	comp_ppc_reg tmpreg = mb->check_word_in_memory.tmpreg;

	//Load address into location register
	comp_ppc_liw(locreg, mb->check_word_in_memory.location);

	//Read word into temp register
	comp_ppc_lhz(tmpreg, 0, locreg);

	//Compare to reference word
	comp_ppc_cmplwi(PPCR_CR_TMP0, tmpreg, mb->check_word_in_memory.content);

	//if it matches then skip to the end
	comp_ppc_bc(PPC_B_CR_TMP0_EQ, 0);

	//Set cache incoherent flag
	comp_ppc_li(tmpreg, -1);
	comp_ppc_stw(tmpreg, COMP_GET_OFFSET_IN_REGS(jit_cache_inconsistent), PPCR_REGS_BASE_MAPPED);

	//Branch target 0 is reached
	comp_ppc_branch_target(0);
}

/**
 * Macroblock: Adds two registers then copies the result into a third and updates all
 * the flags in PPC flag registers (NZCV)
 */
void comp_macroblock_push_add_with_flags(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg1, comp_ppc_reg input_reg2)
{
	comp_mb_init(mb,
				comp_macroblock_impl_add_with_flags,
				regsin, regsout);
	mb->three_regs_opcode.output_reg = output_reg;
	mb->three_regs_opcode.input_reg1 = input_reg1;
	mb->three_regs_opcode.input_reg2 = input_reg2;
}

void comp_macroblock_impl_add_with_flags(union comp_compiler_mb_union* mb)
{
	comp_ppc_addco(
			mb->three_regs_opcode.output_reg,
			mb->three_regs_opcode.input_reg1,
			mb->three_regs_opcode.input_reg2,
			TRUE);
}

/**
 * Macroblock: Adds two registers then copies the result into a third, updates only
 * the flags in PPC flag register for negative and zero (NZ)
 */
void comp_macroblock_push_add_register_register(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg1, comp_ppc_reg input_reg2, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_add_register_register,
				regsin, regsout);
	mb->three_regs_opcode_flags.output_reg = output_reg;
	mb->three_regs_opcode_flags.input_reg1 = input_reg1;
	mb->three_regs_opcode_flags.input_reg2 = input_reg2;
	mb->three_regs_opcode_flags.updateflags = updateflags;
}

void comp_macroblock_impl_add_register_register(union comp_compiler_mb_union* mb)
{
	comp_ppc_add(
			mb->three_regs_opcode_flags.output_reg,
			mb->three_regs_opcode_flags.input_reg1,
			mb->three_regs_opcode_flags.input_reg2,
			mb->three_regs_opcode_flags.updateflags);
}

/**
 * Macroblock: Adds two registers plus the C flag then copies the result into a third and updates all
 * the flags in PPC flag registers (NZCV)
 */
void comp_macroblock_push_add_extended_with_flags(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg1, comp_ppc_reg input_reg2)
{
	comp_mb_init(mb,
				comp_macroblock_impl_add_extended_with_flags,
				regsin, regsout);
	mb->three_regs_opcode.output_reg = output_reg;
	mb->three_regs_opcode.input_reg1 = input_reg1;
	mb->three_regs_opcode.input_reg2 = input_reg2;
}

void comp_macroblock_impl_add_extended_with_flags(union comp_compiler_mb_union* mb)
{
	comp_ppc_addeo(
			mb->three_regs_opcode.output_reg,
			mb->three_regs_opcode.input_reg1,
			mb->three_regs_opcode.input_reg2,
			TRUE);
}

/**
 * Macroblock: Adds two registers then copies the result into a third without flag update.
 */
void comp_macroblock_push_add(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg1, comp_ppc_reg input_reg2)
{
	comp_mb_init(mb,
				comp_macroblock_impl_add,
				regsin, regsout);
	mb->three_regs_opcode.output_reg = output_reg;
	mb->three_regs_opcode.input_reg1 = input_reg1;
	mb->three_regs_opcode.input_reg2 = input_reg2;
}

void comp_macroblock_impl_add(union comp_compiler_mb_union* mb)
{
	comp_ppc_add(
			mb->three_regs_opcode.output_reg,
			mb->three_regs_opcode.input_reg1,
			mb->three_regs_opcode.input_reg2,
			FALSE);
}

/**
 * Macroblock: Subtracts a register from another register then copies the result into a third without flag update.
 */
void comp_macroblock_push_sub(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg subtrahend_input_reg1, comp_ppc_reg minuend_input_reg2)
{
	comp_mb_init(mb,
				comp_macroblock_impl_sub,
				regsin, regsout);
	mb->three_regs_opcode.output_reg = output_reg;
	mb->three_regs_opcode.input_reg1 = subtrahend_input_reg1;
	mb->three_regs_opcode.input_reg2 = minuend_input_reg2;
}

void comp_macroblock_impl_sub(union comp_compiler_mb_union* mb)
{
	comp_ppc_subf(
			mb->three_regs_opcode.output_reg,
			mb->three_regs_opcode.input_reg1,
			mb->three_regs_opcode.input_reg2,
			FALSE);
}

/**
 * Macroblock: Subtracts a register from another register then copies
 * the result into a third and updates all the flags in PPC flag registers (NZCVX)
 */
void comp_macroblock_push_sub_with_flags(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg subtrahend_input_reg1, comp_ppc_reg minuend_input_reg2)
{
	comp_mb_init(mb,
				comp_macroblock_impl_sub_with_flags,
				regsin, regsout);
	mb->three_regs_opcode.output_reg = output_reg;
	mb->three_regs_opcode.input_reg1 = subtrahend_input_reg1;
	mb->three_regs_opcode.input_reg2 = minuend_input_reg2;
}

void comp_macroblock_impl_sub_with_flags(union comp_compiler_mb_union* mb)
{
	comp_ppc_subfco(
			mb->three_regs_opcode.output_reg,
			mb->three_regs_opcode.input_reg1,
			mb->three_regs_opcode.input_reg2,
			TRUE);
}

/**
 * Macroblock: Subtracts a register plus the C flag from another register then copies
 * the result into a third and updates all the flags in PPC flag registers (NZCVX)
 */
void comp_macroblock_push_sub_extended_with_flags(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg subtrahend_input_reg1, comp_ppc_reg minuend_input_reg2)
{
	comp_mb_init(mb,
				comp_macroblock_impl_sub_extended_with_flags,
				regsin, regsout);
	mb->three_regs_opcode.output_reg = output_reg;
	mb->three_regs_opcode.input_reg1 = subtrahend_input_reg1;
	mb->three_regs_opcode.input_reg2 = minuend_input_reg2;
}

void comp_macroblock_impl_sub_extended_with_flags(union comp_compiler_mb_union* mb)
{
	comp_ppc_subfeo(
			mb->three_regs_opcode.output_reg,
			mb->three_regs_opcode.input_reg1,
			mb->three_regs_opcode.input_reg2,
			TRUE);
}

/**
 * Macroblock: Subtracts a register from an immediate then copies the result into a second register.
 */
void comp_macroblock_push_sub_register_from_immediate(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg subtrahend_input_reg, uae_u8 minuend_input_imm)
{
	comp_mb_init(mb,
				comp_macroblock_impl_sub_register_from_immediate,
				regsin, regsout);
	mb->two_regs_imm_opcode.output_reg = output_reg;
	mb->two_regs_imm_opcode.input_reg = subtrahend_input_reg;
	mb->two_regs_imm_opcode.immediate = minuend_input_imm;
}

void comp_macroblock_impl_sub_register_from_immediate(union comp_compiler_mb_union* mb)
{
	comp_ppc_subfic(
			mb->two_regs_imm_opcode.output_reg,
			mb->two_regs_imm_opcode.input_reg,
			mb->two_regs_imm_opcode.immediate);
}

/**
 * Macroblock: Copies a long date from a register into another register and updates NZ flags in PPC flag registers
 */
void comp_macroblock_push_copy_register_long_with_flags(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg)
{
	comp_mb_init(mb,
				comp_macroblock_impl_copy_register_long_with_flags,
				regsin,
				regsout | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ);
	mb->two_regs_opcode.output_reg = output_reg;
	mb->two_regs_opcode.input_reg = input_reg;
}

void comp_macroblock_impl_copy_register_long_with_flags(union comp_compiler_mb_union* mb)
{
	comp_ppc_mr(mb->two_regs_opcode.output_reg, mb->two_regs_opcode.input_reg, TRUE);
}

/**
 * Macroblock: Copies a register into another register without flag update
 * Note: this function is guaranteed not to allocate a temp register,
 * it is safe to use it for copying a previously not mapped register into a
 * mapped temporary register.
 */
void comp_macroblock_push_copy_register_long(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg)
{
	comp_mb_init(mb,
				comp_macroblock_impl_copy_register_long,
				regsin, regsout);
	mb->two_regs_opcode.output_reg = output_reg;
	mb->two_regs_opcode.input_reg = input_reg;
}

void comp_macroblock_impl_copy_register_long(union comp_compiler_mb_union* mb)
{
	comp_ppc_mr(mb->two_regs_opcode.output_reg, mb->two_regs_opcode.input_reg, FALSE);
}

/**
 * Macroblock: Copies a word data from a register into another register
 */
void comp_macroblock_push_copy_register_word(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg)
{
	comp_macroblock_push_rotate_and_copy_bits(
			regsin,
			regsout,
			output_reg,
			input_reg,
			0, 16, 31, FALSE);
}

/**
 * Macroblock: Copies a byte data from a register into another register
 */
void comp_macroblock_push_copy_register_byte(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg)
{
	comp_macroblock_push_rotate_and_copy_bits(
			regsin,
			regsout,
			output_reg,
			input_reg,
			0, 24, 31, FALSE);
}

/**
 * Macroblock: Loads an immediate longword value into a temporary register
 */
void comp_macroblock_push_load_register_long(uae_u64 regsout, comp_ppc_reg output_reg, uae_u32 imm)
{
	comp_mb_init(mb,
				comp_macroblock_impl_load_register_long,
				COMP_COMPILER_MACROBLOCK_REG_NONE,
				regsout);
	mb->load_register.immediate = imm;
	mb->load_register.output_reg = output_reg;
}

void comp_macroblock_impl_load_register_long(union comp_compiler_mb_union* mb)
{
	comp_ppc_liw(mb->load_register.output_reg, mb->load_register.immediate);
}

/**
 * Macroblock: Loads an immediate word value into a temporary register
 * sign extended to longword
 */
void comp_macroblock_push_load_register_word_extended(uae_u64 regsout, comp_ppc_reg output_reg, uae_u16 imm)
{
	comp_mb_init(mb,
				comp_macroblock_impl_load_register_word_extended,
				COMP_COMPILER_MACROBLOCK_REG_NONE,
				regsout);
	mb->load_register.immediate = imm;
	mb->load_register.output_reg = output_reg;
}

void comp_macroblock_impl_load_register_word_extended(union comp_compiler_mb_union* mb)
{
	comp_ppc_li(mb->load_register.output_reg, (uae_u16)mb->load_register.immediate);
}

/**
 * Macroblock: Loads a longword data from memory into a temporary register
 */
void comp_macroblock_push_load_memory_long(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg base_reg, uae_u16 offset)
{
	comp_mb_init(mb,
				comp_macroblock_impl_load_memory_long,
				regsin, regsout);
	mb->access_memory.offset = offset;
	mb->access_memory.output_reg = output_reg;
	mb->access_memory.base_reg = base_reg;
}

void comp_macroblock_impl_load_memory_long(union comp_compiler_mb_union* mb)
{
	comp_ppc_lwz(mb->access_memory.output_reg, mb->access_memory.offset, mb->access_memory.base_reg);
}

/**
 * Macroblock: Loads a word data from memory into a temporary register
 */
void comp_macroblock_push_load_memory_word(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg base_reg, uae_u16 offset)
{
	comp_mb_init(mb,
				comp_macroblock_impl_load_memory_word,
				regsin, regsout);
	mb->access_memory.offset = offset;
	mb->access_memory.output_reg = output_reg;
	mb->access_memory.base_reg = base_reg;
}

void comp_macroblock_impl_load_memory_word(union comp_compiler_mb_union* mb)
{
	comp_ppc_lhz(mb->access_memory.output_reg, mb->access_memory.offset, mb->access_memory.base_reg);
}

/**
 * Macroblock: Loads a word data from memory into a temporary register,
 * data is sign-extended to longword
 */
void comp_macroblock_push_load_memory_word_extended(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg base_reg, uae_u16 offset)
{
	comp_mb_init(mb,
				comp_macroblock_impl_load_memory_word_extended,
				regsin, regsout);
	mb->access_memory.offset = offset;
	mb->access_memory.output_reg = output_reg;
	mb->access_memory.base_reg = base_reg;
}

void comp_macroblock_impl_load_memory_word_extended(union comp_compiler_mb_union* mb)
{
	comp_ppc_lha(mb->access_memory.output_reg, mb->access_memory.offset, mb->access_memory.base_reg);
}

/**
 * Macroblock: Loads a byte data from memory into a temporary register
 */
void comp_macroblock_push_load_memory_byte(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg base_reg, uae_u16 offset)
{
	comp_mb_init(mb,
				comp_macroblock_impl_load_memory_byte,
				regsin, regsout);
	mb->access_memory.offset = offset;
	mb->access_memory.output_reg = output_reg;
	mb->access_memory.base_reg = base_reg;
}

void comp_macroblock_impl_load_memory_byte(union comp_compiler_mb_union* mb)
{
	comp_ppc_lbz(mb->access_memory.output_reg, mb->access_memory.offset, mb->access_memory.base_reg);
}

/**
 * Macroblock: Saves a longword data from a temporary register into memory
 */
void comp_macroblock_push_save_memory_long(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg source_reg, comp_ppc_reg base_reg, uae_u16 offset)
{
	comp_mb_init(mb,
				comp_macroblock_impl_save_memory_long,
				regsin, regsout);
	mb->access_memory.offset = offset;
	mb->access_memory.output_reg = source_reg;
	mb->access_memory.base_reg = base_reg;
}

void comp_macroblock_impl_save_memory_long(union comp_compiler_mb_union* mb)
{
	comp_ppc_stw(mb->access_memory.output_reg, mb->access_memory.offset, mb->access_memory.base_reg);
}

/**
 * Macroblock: Saves a word data from a temporary register into memory
 */
void comp_macroblock_push_save_memory_word(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg source_reg, comp_ppc_reg base_reg, uae_u16 offset)
{
	comp_mb_init(mb,
				comp_macroblock_impl_save_memory_word,
				regsin, regsout);
	mb->access_memory.offset = offset;
	mb->access_memory.output_reg = source_reg;
	mb->access_memory.base_reg = base_reg;
}

void comp_macroblock_impl_save_memory_word(union comp_compiler_mb_union* mb)
{
	comp_ppc_sth(mb->access_memory.output_reg, mb->access_memory.offset, mb->access_memory.base_reg);
}

/**
 * Macroblock: Saves a word data from a temporary register into memory with updating the register before saving
 */
void comp_macroblock_push_save_memory_word_update(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg source_reg, comp_ppc_reg base_reg, uae_u16 offset)
{
	comp_mb_init(mb,
				comp_macroblock_impl_save_memory_word_update,
				regsin, regsout);
	mb->access_memory.offset = offset;
	mb->access_memory.output_reg = source_reg;
	mb->access_memory.base_reg = base_reg;
}

void comp_macroblock_impl_save_memory_word_update(union comp_compiler_mb_union* mb)
{
	comp_ppc_sthu(mb->access_memory.output_reg, mb->access_memory.offset, mb->access_memory.base_reg);
}

/**
 * Macroblock: Saves a byte data from a temporary register into memory
 */
void comp_macroblock_push_save_memory_byte(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg source_reg, comp_ppc_reg base_reg, uae_u32 offset)
{
	comp_mb_init(mb,
				comp_macroblock_impl_save_memory_byte,
				regsin, regsout);
	mb->access_memory.offset = offset;
	mb->access_memory.output_reg = source_reg;
	mb->access_memory.base_reg = base_reg;
}

void comp_macroblock_impl_save_memory_byte(union comp_compiler_mb_union* mb)
{
	comp_ppc_stb(mb->access_memory.output_reg, mb->access_memory.offset, mb->access_memory.base_reg);
}

/**
 * Macroblock: Loads the physical address of a mapped memory address into a register
 */
void comp_macroblock_push_map_physical_mem(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg dest_mem_reg, comp_ppc_reg source_reg)
{
	//TODO: this temp registration allocation must be moved out from the macroblock
	comp_tmp_reg* tmpreg = comp_allocate_temp_register(NULL, PPC_TMP_REG_NOTUSED_MAPPED);

	comp_mb_init(mb,
				comp_macroblock_impl_map_physical_mem,
				regsin, regsout);
	mb->map_physical_mem.output_reg = dest_mem_reg;
	mb->map_physical_mem.input_reg = source_reg;
	mb->map_physical_mem.temp_reg = tmpreg->mapped_reg_num;

	comp_free_temp_register(tmpreg);
}

void comp_macroblock_impl_map_physical_mem(union comp_compiler_mb_union* mb)
{
	helper_map_physical_mem(
			mb->map_physical_mem.input_reg,
			mb->map_physical_mem.output_reg,
			mb->map_physical_mem.temp_reg);
}

/** Macroblock helper: implementation of the physical address mapping
 *  Parameters:
 *     inreg - source 68k address
 *     outreg - output register for the result (physical address)
 *     tmpreg - temporary register for the calculation
 *  Note: inreg and outreg can be the same, but the tmpreg must be an independent
 *  register which is trashed.
 *  PPCR_SPECTMP (R0) is used by this function, it cannot be specified as any of the parameter registers.
 */
STATIC_INLINE void helper_map_physical_mem(comp_ppc_reg inreg, comp_ppc_reg outreg, comp_ppc_reg tmpreg)
{
	//Get the baseaddr[] entry of the bank into tmpreg (and its bit 0 into CR7)
	helper_load_baseaddr_checked(inreg, tmpreg);

	//v18: bit 0 set = special bank, no direct host address. Only MOVEM
	//and MOVE16 still map an address without the checked access macroblocks
	//(several accesses relative to one mapped address): point them to a
	//scratch area instead of into the addrbank structure. No crash, but the
	//data of such an access is lost (write) or undefined (read).
	comp_ppc_bc(PPC_B_CR7_NE, 0);

	//TODO: base and offset could be sent separately and used in instruction code
	//Sum address and offset to outreg: outreg = inreg + rtmp
	comp_ppc_add(outreg, inreg, tmpreg, FALSE);
	comp_ppc_b(0, 1);

	comp_ppc_branch_target(0);
	comp_ppc_liw(outreg, (uae_u32) comp_memfb_scratch());

	comp_ppc_branch_target(1);
}

/**
 * v18: loads baseaddr[] of the bank of the address in inreg into tmpreg
 * and sets CR7 to "not equal" if bit 0 of it is set (special bank).
 * Uses r0 (PPCR_SPECTMP). CR0 and XER (the internal flags) are not touched.
 */
STATIC_INLINE void helper_load_baseaddr_checked(comp_ppc_reg inreg, comp_ppc_reg tmpreg)
{
	//Get 64k page index into R0: R0 = (inreg >> 14) & 0x3fffc;
	comp_ppc_rlwinm(PPCR_SPECTMP_MAPPED, inreg, 18, 14, 29, FALSE);

#ifdef PPCR_BASEADDR
	//v18: the start of baseaddr[] is kept in a fixed register (TODO 7.2)
	comp_ppc_lwzx(tmpreg, PPCR_BASEADDR_MAPPED, PPCR_SPECTMP_MAPPED);
#else
	//Load base address array address to tmp reg, then rtmp = rtmp[r0]
	comp_ppc_liw(tmpreg, (uae_u32)baseaddr);
	comp_ppc_lwzx(tmpreg, tmpreg, PPCR_SPECTMP_MAPPED);
#endif

	//Bit 0 into CR7: r0 = rtmp & 1, compare with 0
	comp_ppc_rlwinm(PPCR_SPECTMP_MAPPED, tmpreg, 0, 31, 31, FALSE);
	comp_ppc_cmplwi(PPCR_CR_MEMCHECK, PPCR_SPECTMP_MAPPED, 0);
}

/**
 * v18: emits the call of the fallback stub for a checked direct memory
 * access (see comp_memfb_fallback() in compemu_support.c)
 */
STATIC_INLINE void helper_call_memfb_stub(uae_u32 desc)
{
	comp_ppc_liw(PPCR_SPECTMP_MAPPED, (uae_u32) comp_memfb_stub());
	comp_ppc_mtctr(PPCR_SPECTMP_MAPPED);
	comp_ppc_li(PPCR_SPECTMP_MAPPED, (uae_u16) desc);
	comp_ppc_bctrl();
}



/**
 * Macroblock: v18: reads memory through a direct host address, but checks
 * bit 0 of baseaddr[] first: for a special bank the access goes through the
 * bank handlers (fallback stub). Replaces map_physical_mem + load_memory_*.
 * Parameters:
 *    output_reg - register for the result
 *    addr_reg - 68k address
 *    temp_reg - temporary register, trashed (must differ from addr_reg)
 *    size - 1, 2, 4, or -2 for a sign extended word
 */
void comp_macroblock_push_load_memory_checked(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg addr_reg, comp_ppc_reg temp_reg, int size)
{
	comp_mb_init(mb,
				comp_macroblock_impl_load_memory_checked,
				regsin, regsout);
	mb->access_memory_checked.output_reg = output_reg;
	mb->access_memory_checked.base_reg = addr_reg;
	mb->access_memory_checked.temp_reg = temp_reg;
	mb->access_memory_checked.size = size;
}

void comp_macroblock_impl_load_memory_checked(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg dest = mb->access_memory_checked.output_reg;
	comp_ppc_reg addr = mb->access_memory_checked.base_reg;
	comp_ppc_reg tmp = mb->access_memory_checked.temp_reg;
	uae_u32 desc;

	if (tmp.r == addr.r || tmp.r == 0 || addr.r == 0 || dest.r == 0
		|| !comp_memfb_reg_ok(dest.r) || !comp_memfb_reg_ok(addr.r))
	{
		write_log("JIT: Compiling error: checked memory read with registers %d, %d, %d\n", dest.r, addr.r, tmp.r);
		comp_compile_error();
		return;
	}

	helper_load_baseaddr_checked(addr, tmp);
#ifdef PPCR_MEMFB
	comp_memfb_branch();
#else
	comp_ppc_bc(PPC_B_CR7_NE, 0);
#endif

	//Direct: tmp = host address, load
	comp_ppc_add(tmp, addr, tmp, FALSE);
	switch (mb->access_memory_checked.size)
	{
	case 4:  comp_ppc_lwz(dest, 0, tmp); desc = COMP_MEMFB_SIZE_LONG << 10; break;
	case 2:  comp_ppc_lhz(dest, 0, tmp); desc = COMP_MEMFB_SIZE_WORD << 10; break;
	case -2: comp_ppc_lha(dest, 0, tmp); desc = (COMP_MEMFB_SIZE_WORD << 10) | COMP_MEMFB_SIGNEXT; break;
	default: comp_ppc_lbz(dest, 0, tmp); desc = COMP_MEMFB_SIZE_BYTE << 10; break;
	}
#ifdef PPCR_MEMFB
	//Special bank: through the bank handler (code behind the block)
	comp_memfb_back(desc | (addr.r << 5) | dest.r);
#else
	comp_ppc_b(0, 1);

	//Special bank: through the bank handler
	comp_ppc_branch_target(0);
	helper_call_memfb_stub(desc | (addr.r << 5) | dest.r);

	comp_ppc_branch_target(1);
#endif
}

/**
 * Macroblock: v18: writes memory through a direct host address with the
 * bit 0 check of comp_macroblock_push_load_memory_checked().
 * Parameters:
 *    source_reg - value to write
 *    addr_reg - 68k address
 *    temp_reg - temporary register, trashed (must differ from addr_reg and source_reg)
 *    size - 1, 2 or 4
 */
void comp_macroblock_push_save_memory_checked(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg source_reg, comp_ppc_reg addr_reg, comp_ppc_reg temp_reg, int size)
{
	comp_mb_init(mb,
				comp_macroblock_impl_save_memory_checked,
				regsin, regsout);
	mb->access_memory_checked.output_reg = source_reg;
	mb->access_memory_checked.base_reg = addr_reg;
	mb->access_memory_checked.temp_reg = temp_reg;
	mb->access_memory_checked.size = size;
}

void comp_macroblock_impl_save_memory_checked(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg src = mb->access_memory_checked.output_reg;
	comp_ppc_reg addr = mb->access_memory_checked.base_reg;
	comp_ppc_reg tmp = mb->access_memory_checked.temp_reg;
	uae_u32 desc;

	if (tmp.r == addr.r || tmp.r == src.r || tmp.r == 0 || addr.r == 0 || src.r == 0
		|| !comp_memfb_reg_ok(src.r) || !comp_memfb_reg_ok(addr.r))
	{
		write_log("JIT: Compiling error: checked memory write with registers %d, %d, %d\n", src.r, addr.r, tmp.r);
		comp_compile_error();
		return;
	}

	helper_load_baseaddr_checked(addr, tmp);
#ifdef PPCR_MEMFB
	comp_memfb_branch();
#else
	comp_ppc_bc(PPC_B_CR7_NE, 0);
#endif

	//Direct: tmp = host address, store
	comp_ppc_add(tmp, addr, tmp, FALSE);
	switch (mb->access_memory_checked.size)
	{
	case 4:  comp_ppc_stw(src, 0, tmp); desc = COMP_MEMFB_SIZE_LONG << 10; break;
	case 2:  comp_ppc_sth(src, 0, tmp); desc = COMP_MEMFB_SIZE_WORD << 10; break;
	default: comp_ppc_stb(src, 0, tmp); desc = COMP_MEMFB_SIZE_BYTE << 10; break;
	}
#ifdef PPCR_MEMFB
	comp_memfb_back(desc | COMP_MEMFB_WRITE | (addr.r << 5) | src.r);
#else
	comp_ppc_b(0, 1);

	//Special bank: through the bank handler
	comp_ppc_branch_target(0);
	helper_call_memfb_stub(desc | COMP_MEMFB_WRITE | (addr.r << 5) | src.r);

	comp_ppc_branch_target(1);
#endif
}

/**
 * Macroblock: v18: allocates a SysV stack frame (stwu r1,-size(r1))
 * for saving temporary registers around a function call
 */
void comp_macroblock_push_stack_frame_alloc(uae_u16 size)
{
	comp_mb_init(mb,
				comp_macroblock_impl_stack_frame_alloc,
				COMP_COMPILER_MACROBLOCK_REG_NONE,
				COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM);
	mb->stack_frame.size = size;
}

void comp_macroblock_impl_stack_frame_alloc(union comp_compiler_mb_union* mb)
{
	comp_ppc_stwu(PPCR_SP_MAPPED, (uae_u16) -mb->stack_frame.size, PPCR_SP_MAPPED);
}

/**
 * Macroblock: v18: frees a stack frame of comp_macroblock_push_stack_frame_alloc()
 */
void comp_macroblock_push_stack_frame_free(uae_u16 size)
{
	comp_mb_init(mb,
				comp_macroblock_impl_stack_frame_free,
				COMP_COMPILER_MACROBLOCK_REG_NONE,
				COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM);
	mb->stack_frame.size = size;
}

void comp_macroblock_impl_stack_frame_free(union comp_compiler_mb_union* mb)
{
	comp_ppc_addi(PPCR_SP_MAPPED, PPCR_SP_MAPPED, mb->stack_frame.size);
}

/**
 * Macroblock: Writes a value into emulated memory from a specified register
 * using the special memory bank handlers.
 * Note: this function purges all temporary registers, because it calls
 * an external function.
 */
void comp_macroblock_push_save_memory_spec(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg source_reg, comp_ppc_reg dest_mem_reg, uae_u8 size)
{
	//Before the call write back the M68k registers and clear the temp register mapping
	//Warning is suppressed, we are in the middle of some instruction compiling.
	//probably there are temporary registers, but we cannot do anything about it
	comp_flush_temp_registers(TRUE);

	comp_mb_init(mb,
				comp_macroblock_impl_save_memory_spec,
				regsin, regsout);
	mb->access_memory_size.base_reg = dest_mem_reg;
	mb->access_memory_size.output_reg = source_reg;
	mb->access_memory_size.size = size;
}

void comp_macroblock_impl_save_memory_spec(union comp_compiler_mb_union* mb)
{
	helper_access_memory_spec(mb, TRUE);
}

/**
 * Macroblock: Reads a value into a specified register from emulated memory
 * using the special memory bank handlers.
 * Note: this function purges all temporary registers, because it calls
 * an external function.
 */
void comp_macroblock_push_load_memory_spec(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg src_mem_reg, comp_ppc_reg dest_reg, uae_u8 size)
{
	//Before the call write back the M68k registers and clear the temp register mapping
	//Warning is suppressed, we are in the middle of some instruction compiling.
	//probably there are temporary registers, but we cannot do anything about it
	comp_flush_temp_registers(TRUE);

	comp_mb_init(mb,
				comp_macroblock_impl_load_memory_spec,
				regsin, regsout);
	mb->access_memory_size.base_reg = src_mem_reg;
	mb->access_memory_size.output_reg = dest_reg;	//Copy the result into this register
	mb->access_memory_size.size = size;
}

void comp_macroblock_impl_load_memory_spec(union comp_compiler_mb_union* mb)
{
	helper_access_memory_spec(mb, FALSE);
}

/**
 * Macroblock: Reads a value into a specified register from emulated memory
 * using the special memory bank handlers.
 * Note: this function preserves all allocated temporary registers,
 * because it calls an external function.
 * If possible then use comp_macroblock_push_load_memory_spec() instead.
 */
void comp_macroblock_push_load_memory_spec_save_temps(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg src_mem_reg, comp_ppc_reg dest_reg, uae_u8 size)
{
	//Save the allocated temporary register, except the destination register
	uae_u32 saved_regs = comp_ppc_save_temp_regs(1 << dest_reg.r);

	comp_mb_init(mb,
				comp_macroblock_impl_load_memory_spec_save_temps,
				regsin, regsout);
	mb->access_memory_size.base_reg = src_mem_reg;
	mb->access_memory_size.output_reg = dest_reg;	//Copy the result into this register
	mb->access_memory_size.size = size;

	//Restore the saved temporary registers
	comp_ppc_restore_temp_regs(saved_regs);
}

void comp_macroblock_impl_load_memory_spec_save_temps(union comp_compiler_mb_union* mb)
{
	helper_access_memory_spec(mb, FALSE);
}

/**
 * Helper function for compiling special memory access
 * Parameters:
 *    mb - pointer to the actual macroblock descriptor
 *    iswrite - if TRUE then it is a write operation otherwise it is read
 */
STATIC_INLINE void helper_access_memory_spec(union comp_compiler_mb_union* mb, int iswrite)
{
	comp_ppc_reg srcreg = mb->access_memory_size.output_reg;
	comp_ppc_reg basereg = mb->access_memory_size.base_reg;
	addrbank refaddrstruct;		//We don't really use this, only for calculating the offsets in the structure
	uae_u16 handleroffset;

	//At this stage all temporary registers are free'd,
	//we need to operate on raw registers without mapping.
	//But we have to be careful not to overwrite the other register
	//while moving registers.

	//Move the output value to R4, but avoid
	//any conflicts with the target address register (only if it is write access)
	if ((srcreg.r != PPCR_PARAM2) && (iswrite))
	{
		//Source register is not R4, we need to move the value
		if (basereg.r != PPCR_PARAM2)
		{
			//We are lucky: the address register is not R4, safe to overwrite
			comp_ppc_mr(PPCR_PARAM2_MAPPED, srcreg, FALSE);
		} else {
			//Oops, the target register is the same as the address register,
			//move the amount thru R0 and move the address to R3
			comp_ppc_mr(PPCR_SPECTMP_MAPPED, srcreg, FALSE);		//R0 = srcreg
			comp_ppc_mr(PPCR_PARAM1_MAPPED, basereg, FALSE);		//R3 = basereg (R4)
			comp_ppc_mr(PPCR_PARAM2_MAPPED, PPCR_SPECTMP_MAPPED, FALSE);	//R4 = R0
			basereg = PPCR_PARAM1_MAPPED;
		}
	}

	//Move the address to R3, if it is not already in that register
	if (basereg.r != PPCR_PARAM1)
	{
		comp_ppc_mr(PPCR_PARAM1_MAPPED, basereg, FALSE);
	}

	//At this point the registers are:
	//R3 = address
	//R4 = value (if it is write access)

	//Get 64k page index into R0: R0 = (inreg >> 14) & 0x3fffc;
	comp_ppc_rlwinm(PPCR_SPECTMP_MAPPED, PPCR_PARAM1_MAPPED, 18, 14, 29, FALSE);

	//Load the mem_banks array address into PPCR_TMP2
	//This is a bit of a hack: we assume that PPCR_TMP2 is neither PPCR_PARAM1 nor PPCR_PARAM2
	//TODO: we could load this from the regs structure
	comp_ppc_liw(PPCR_TMP2_MAPPED, (uae_u32)mem_banks);

	//Load address structure pointer for the memory block to PPCR_TMP2: r5 = r5[r0]
	comp_ppc_lwzx(PPCR_TMP2_MAPPED, PPCR_TMP2_MAPPED, PPCR_SPECTMP_MAPPED);

	//Select the handler according to the size
	switch(mb->access_memory_size.size)
	{
	case 1:
		//Byte
		if (iswrite)
		{
			handleroffset = ((void*)&refaddrstruct.bput) - ((void*)&refaddrstruct);
		} else {
			handleroffset = ((void*)&refaddrstruct.bget) - ((void*)&refaddrstruct);
		}
		break;
	case 2:
		//Word
		if (iswrite)
		{
			handleroffset = ((void*)&refaddrstruct.wput) - ((void*)&refaddrstruct);
		} else {
			handleroffset = ((void*)&refaddrstruct.wget) - ((void*)&refaddrstruct);
		}
		break;
	case 4:
		//Long
		if (iswrite)
		{
			handleroffset = ((void*)&refaddrstruct.lput) - ((void*)&refaddrstruct);
		} else {
			handleroffset = ((void*)&refaddrstruct.lget) - ((void*)&refaddrstruct);
		}
		break;
	default:
		//v18: was abort()
		write_log("JIT: Compiling error: unknown memory access size: %d\n", mb->access_memory_size.size);
		comp_compile_error();
		return;
	}

	//Load the address of the handler to PPCR_TMP2
	comp_ppc_lwz(PPCR_TMP2_MAPPED, handleroffset, PPCR_TMP2_MAPPED);

	//Call handler
	comp_ppc_call_reg(PPCR_TMP2_MAPPED);

	//If it was read access then the result is returned in R3,
	//copy to the specified register if it was not there already
	if ((!iswrite) && (srcreg.r != PPCR_PARAM1))
	{
		comp_ppc_mr(srcreg, PPCR_PARAM1_MAPPED, FALSE);
	}
}

/**
 * Macroblock: OR a 16 bit immediate to the lower half word of a register and put it into a new register
 */
void comp_macroblock_push_or_low_register_imm(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, uae_u16 imm)
{
	comp_mb_init(mb,
				comp_macroblock_impl_or_low_register_imm,
				regsin, regsout);
	mb->two_regs_imm_opcode.output_reg = output_reg;
	mb->two_regs_imm_opcode.input_reg = input_reg;
	mb->two_regs_imm_opcode.immediate = imm;
}

void comp_macroblock_impl_or_low_register_imm(union comp_compiler_mb_union* mb)
{
	comp_ppc_ori(
			mb->two_regs_imm_opcode.output_reg,
			mb->two_regs_imm_opcode.input_reg,
			mb->two_regs_imm_opcode.immediate);
}

/**
 * Macroblock: OR a 16 bit immediate to the higher half word of a register and put it into a new register
 */
void comp_macroblock_push_or_high_register_imm(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, uae_u16 imm)
{
	comp_mb_init(mb,
				comp_macroblock_impl_or_high_register_imm,
				regsin, regsout);
	mb->two_regs_imm_opcode.output_reg = output_reg;
	mb->two_regs_imm_opcode.input_reg = input_reg;
	mb->two_regs_imm_opcode.immediate = imm;
}

void comp_macroblock_impl_or_high_register_imm(union comp_compiler_mb_union* mb)
{
	comp_ppc_oris(
			mb->two_regs_imm_opcode.output_reg,
			mb->two_regs_imm_opcode.input_reg,
			mb->two_regs_imm_opcode.immediate);
}

/**
 * Macroblock: OR a register to another register
 */
void comp_macroblock_push_or_register_register(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg1, comp_ppc_reg input_reg2, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_or_register_register,
				regsin, regsout);
	mb->three_regs_opcode_flags.output_reg = output_reg;
	mb->three_regs_opcode_flags.input_reg1 = input_reg1;
	mb->three_regs_opcode_flags.input_reg2 = input_reg2;
	mb->three_regs_opcode_flags.updateflags = updateflags;
}

void comp_macroblock_impl_or_register_register(union comp_compiler_mb_union* mb)
{
	comp_ppc_or(
			mb->three_regs_opcode_flags.output_reg,
			mb->three_regs_opcode_flags.input_reg1,
			mb->three_regs_opcode_flags.input_reg2,
			mb->three_regs_opcode_flags.updateflags);
}

/**
 * Macroblock: OR a register to another register
 */
void comp_macroblock_push_not_or_register_register(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg1, comp_ppc_reg input_reg2, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_not_or_register_register,
				regsin, regsout);
	mb->three_regs_opcode_flags.output_reg = output_reg;
	mb->three_regs_opcode_flags.input_reg1 = input_reg1;
	mb->three_regs_opcode_flags.input_reg2 = input_reg2;
	mb->three_regs_opcode_flags.updateflags = updateflags;
}

void comp_macroblock_impl_not_or_register_register(union comp_compiler_mb_union* mb)
{
	comp_ppc_nor(
			mb->three_regs_opcode_flags.output_reg,
			mb->three_regs_opcode_flags.input_reg1,
			mb->three_regs_opcode_flags.input_reg2,
			mb->three_regs_opcode_flags.updateflags);
}

/**
 * Macroblock: AND a 16 bit immediate to the lower half word of a register and put it into a new register
 */
void comp_macroblock_push_and_low_register_imm(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, uae_u16 immediate)
{
	comp_mb_init(mb,
				comp_macroblock_impl_and_low_register_imm,
				regsin, regsout);
	mb->two_regs_imm_opcode.output_reg = output_reg;
	mb->two_regs_imm_opcode.input_reg = input_reg;
	mb->two_regs_imm_opcode.immediate = immediate;
}

void comp_macroblock_impl_and_low_register_imm(union comp_compiler_mb_union* mb)
{
	comp_ppc_andi(
			mb->two_regs_imm_opcode.output_reg,
			mb->two_regs_imm_opcode.input_reg,
			mb->two_regs_imm_opcode.immediate);
}

/**
 * Macroblock: AND a 16 bit immediate to the higher half word of a register and put it into a new register
 */
void comp_macroblock_push_and_high_register_imm(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, uae_u16 immediate)
{
	comp_mb_init(mb,
				comp_macroblock_impl_and_high_register_imm,
				regsin, regsout);
	mb->two_regs_imm_opcode.output_reg = output_reg;
	mb->two_regs_imm_opcode.input_reg = input_reg;
	mb->two_regs_imm_opcode.immediate = immediate;
}

void comp_macroblock_impl_and_high_register_imm(union comp_compiler_mb_union* mb)
{
	comp_ppc_andis(
			mb->two_regs_imm_opcode.output_reg,
			mb->two_regs_imm_opcode.input_reg,
			mb->two_regs_imm_opcode.immediate);
}

/**
 * Macroblock: AND a register to another register
 */
void comp_macroblock_push_and_register_register(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg1, comp_ppc_reg input_reg2, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_and_register_register,
				regsin, regsout);
	mb->three_regs_opcode_flags.output_reg = output_reg;
	mb->three_regs_opcode_flags.input_reg1 = input_reg1;
	mb->three_regs_opcode_flags.input_reg2 = input_reg2;
	mb->three_regs_opcode_flags.updateflags = updateflags;
}

void comp_macroblock_impl_and_register_register(union comp_compiler_mb_union* mb)
{
	comp_ppc_and(
			mb->three_regs_opcode_flags.output_reg,
			mb->three_regs_opcode_flags.input_reg1,
			mb->three_regs_opcode_flags.input_reg2,
			mb->three_regs_opcode_flags.updateflags);
}

/**
 * Macroblock: AND a register to another register's complement
 */
void comp_macroblock_push_and_register_complement_register(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg1, comp_ppc_reg input_compl_reg2, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_and_register_complement_register,
				regsin, regsout);
	mb->three_regs_opcode_flags.output_reg = output_reg;
	mb->three_regs_opcode_flags.input_reg1 = input_reg1;
	mb->three_regs_opcode_flags.input_reg2 = input_compl_reg2;
	mb->three_regs_opcode_flags.updateflags = updateflags;
}

void comp_macroblock_impl_and_register_complement_register(union comp_compiler_mb_union* mb)
{
	comp_ppc_andc(
			mb->three_regs_opcode_flags.output_reg,
			mb->three_regs_opcode_flags.input_reg1,
			mb->three_regs_opcode_flags.input_reg2,
			mb->three_regs_opcode_flags.updateflags);
}

/**
 * Macroblock: XOR a register to another register
 */
void comp_macroblock_push_xor_register_register(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg1, comp_ppc_reg input_reg2, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_xor_register_register,
				regsin, regsout);
	mb->three_regs_opcode_flags.output_reg = output_reg;
	mb->three_regs_opcode_flags.input_reg1 = input_reg1;
	mb->three_regs_opcode_flags.input_reg2 = input_reg2;
	mb->three_regs_opcode_flags.updateflags = updateflags;
}

void comp_macroblock_impl_xor_register_register(union comp_compiler_mb_union* mb)
{
	comp_ppc_xor(
			mb->three_regs_opcode_flags.output_reg,
			mb->three_regs_opcode_flags.input_reg1,
			mb->three_regs_opcode_flags.input_reg2,
			mb->three_regs_opcode_flags.updateflags);
}

/**
 * Macroblock: XOR an immediate to the low half word of a register
 */
void comp_macroblock_push_xor_low_register_imm(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, uae_u16 immediate)
{
	comp_mb_init(mb,
				comp_macroblock_impl_xor_low_register_imm,
				regsin, regsout);
	mb->two_regs_imm_opcode.output_reg = output_reg;
	mb->two_regs_imm_opcode.input_reg = input_reg;
	mb->two_regs_imm_opcode.immediate = immediate;
}

void comp_macroblock_impl_xor_low_register_imm(union comp_compiler_mb_union* mb)
{
	comp_ppc_xori(
			mb->two_regs_imm_opcode.output_reg,
			mb->two_regs_imm_opcode.input_reg,
			mb->two_regs_imm_opcode.immediate);
}

/**
 * Macroblock: XOR an immediate to the high half word of a register
 */
void comp_macroblock_push_xor_high_register_imm(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, uae_u16 immediate)
{
	comp_mb_init(mb,
				comp_macroblock_impl_xor_high_register_imm,
				regsin, regsout);
	mb->two_regs_imm_opcode.output_reg = output_reg;
	mb->two_regs_imm_opcode.input_reg = input_reg;
	mb->two_regs_imm_opcode.immediate = immediate;
}

void comp_macroblock_impl_xor_high_register_imm(union comp_compiler_mb_union* mb)
{
	comp_ppc_xoris(
			mb->two_regs_imm_opcode.output_reg,
			mb->two_regs_imm_opcode.input_reg,
			mb->two_regs_imm_opcode.immediate);
}

/**
 * Macroblock: Add a word immediate to a register and put it into a new register
 * Note: the immediate is sign extended to 32 bit.
 */
void comp_macroblock_push_add_register_imm(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, uae_u16 imm)
{
	comp_mb_init(mb,
				comp_macroblock_impl_add_register_imm,
				regsin, regsout);
	mb->two_regs_imm_opcode.output_reg = output_reg;
	mb->two_regs_imm_opcode.input_reg = input_reg;
	mb->two_regs_imm_opcode.immediate = imm;
}

void comp_macroblock_impl_add_register_imm(union comp_compiler_mb_union* mb)
{
	comp_ppc_addi(
			mb->two_regs_imm_opcode.output_reg,
			mb->two_regs_imm_opcode.input_reg,
			mb->two_regs_imm_opcode.immediate);
}

/**
 * Macroblock: Add a word immediate to the high half word of a register and
 * put it into a new register
 * Note: the immediate is sign extended to 32 bit.
 */
void comp_macroblock_push_add_high_register_imm(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, uae_u16 imm)
{
	comp_mb_init(mb,
				comp_macroblock_impl_add_high_register_imm,
				regsin, regsout);
	mb->two_regs_imm_opcode.output_reg = output_reg;
	mb->two_regs_imm_opcode.input_reg = input_reg;
	mb->two_regs_imm_opcode.immediate = imm;
}

void comp_macroblock_impl_add_high_register_imm(union comp_compiler_mb_union* mb)
{
	comp_ppc_addis(
			mb->two_regs_imm_opcode.output_reg,
			mb->two_regs_imm_opcode.input_reg,
			mb->two_regs_imm_opcode.immediate);
}

/**
 * Macroblock: Negate register into another register with overflow flag set.
 */
void comp_macroblock_push_negate_with_overflow(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_negate_with_overflow,
				regsin,
				regsout);
	mb->two_regs_opcode_flags.input_reg = input_reg;
	mb->two_regs_opcode_flags.output_reg = output_reg;
	mb->two_regs_opcode_flags.updateflags = updateflags;
}

void comp_macroblock_impl_negate_with_overflow(union comp_compiler_mb_union* mb)
{
	comp_ppc_nego(mb->two_regs_opcode_flags.output_reg, mb->two_regs_opcode_flags.input_reg, mb->two_regs_opcode_flags.updateflags);
}

/**
 * Macroblock: AND a register to another register and put it into a new register
 * Note: the higher half word of the register will be cleared (andi instruction)
 */
void comp_macroblock_push_and_registers(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg1, comp_ppc_reg input_reg2)
{
	comp_mb_init(mb,
				comp_macroblock_impl_and_registers,
				regsin, regsout);
	mb->three_regs_opcode.output_reg = output_reg;
	mb->three_regs_opcode.input_reg1 = input_reg1;
	mb->three_regs_opcode.input_reg2 = input_reg2;
}

void comp_macroblock_impl_and_registers(union comp_compiler_mb_union* mb)
{
	comp_ppc_and(
			mb->three_regs_opcode.output_reg,
			mb->three_regs_opcode.input_reg1,
			mb->three_regs_opcode.input_reg2, FALSE);
}

/**
 * Macroblock: multiply register by another register and set the arithmetic flags if needed
 */
void comp_macroblock_push_multiply_registers(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg1, comp_ppc_reg input_reg2, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_multiply_registers,
				regsin, regsout);
	mb->three_regs_opcode_flags.output_reg = output_reg;
	mb->three_regs_opcode_flags.input_reg1 = input_reg1;
	mb->three_regs_opcode_flags.input_reg2 = input_reg2;
	mb->three_regs_opcode_flags.updateflags = updateflags;
}

void comp_macroblock_impl_multiply_registers(union comp_compiler_mb_union* mb)
{
	if (mb->three_regs_opcode_flags.updateflags)
	{
		comp_ppc_mullwo(
				mb->three_regs_opcode_flags.output_reg,
				mb->three_regs_opcode_flags.input_reg1,
				mb->three_regs_opcode_flags.input_reg2,
				TRUE);
	} else {
		comp_ppc_mullw(
				mb->three_regs_opcode_flags.output_reg,
				mb->three_regs_opcode_flags.input_reg1,
				mb->three_regs_opcode_flags.input_reg2,
				FALSE);
	}
}

/**
 * Macroblock: multiply register by another register and set the arithmetic flags if needed,
 * the output is the high 32 bit part of the result.
 */
void comp_macroblock_push_multiply_registers_high(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg1, comp_ppc_reg input_reg2, BOOL is_signed, BOOL updateflags)
{
	if (is_signed)
	{
		comp_mb_init(mb,
					comp_macroblock_impl_multiply_registers_signed_high,
					regsin, regsout);
		mb->three_regs_opcode_flags.output_reg = output_reg;
		mb->three_regs_opcode_flags.input_reg1 = input_reg1;
		mb->three_regs_opcode_flags.input_reg2 = input_reg2;
		mb->three_regs_opcode_flags.updateflags = updateflags;
	} else {
		comp_mb_init(mb,
					comp_macroblock_impl_multiply_registers_unsigned_high,
					regsin, regsout);
		mb->three_regs_opcode_flags.output_reg = output_reg;
		mb->three_regs_opcode_flags.input_reg1 = input_reg1;
		mb->three_regs_opcode_flags.input_reg2 = input_reg2;
		mb->three_regs_opcode_flags.updateflags = updateflags;
	}
}

void comp_macroblock_impl_multiply_registers_unsigned_high(union comp_compiler_mb_union* mb)
{
		comp_ppc_mulhwu(
				mb->three_regs_opcode_flags.output_reg,
				mb->three_regs_opcode_flags.input_reg1,
				mb->three_regs_opcode_flags.input_reg2,
				mb->three_regs_opcode_flags.updateflags);
}

void comp_macroblock_impl_multiply_registers_signed_high(union comp_compiler_mb_union* mb)
{
		comp_ppc_mulhw(
				mb->three_regs_opcode_flags.output_reg,
				mb->three_regs_opcode_flags.input_reg1,
				mb->three_regs_opcode_flags.input_reg2,
				mb->three_regs_opcode_flags.updateflags);
}

/**
 * Macroblock: Copy all (N, Z, C, V) PPC flag registers into a GPR register
 * Note: this macroblock depends on the internal PPC flags: N, Z, C, V
 */
void comp_macroblock_push_copy_nzcv_flags_to_register(uae_u64 regsout, comp_ppc_reg output_reg)
{
	comp_mb_init(mb,
				comp_macroblock_impl_copy_nzcv_flags_to_register,
				COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGC | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGV,
				regsout);
	mb->one_reg_opcode.reg = output_reg;
}

void comp_macroblock_impl_copy_nzcv_flags_to_register(union comp_compiler_mb_union* mb)
{
	//Copy XER to CR2 (v18: comp_ppc_mcrxr() decides at run time
	//between mcrxr and its replacement; was _ARCH_PWR4 at compile time)
	comp_ppc_mcrxr(PPCR_CR_TMP2);
	//Copy CR0-7 to the output register
	comp_ppc_mfcr(mb->one_reg_opcode.reg);
}

/**
 * Macroblock: Copy N and Z PPC flag registers into a GPR register
 * Note: this macroblock depends on the internal PPC flags: N and Z
 */
void comp_macroblock_push_copy_nz_flags_to_register(uae_u64 regsout, comp_ppc_reg output_reg)
{
	comp_mb_init(mb,
				comp_macroblock_impl_copy_nz_flags_to_register,
				COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ,
				regsout);
	mb->one_reg_opcode.reg = output_reg;
}

void comp_macroblock_impl_copy_nz_flags_to_register(union comp_compiler_mb_union* mb)
{
	comp_ppc_mfcr(mb->one_reg_opcode.reg);	//v18: mfocrf only in PWR4 builds, mfcr works everywhere
}

/**
 * Macroblock: Copy C and V PPC flag registers into a GPR register
 * Note: this macroblock depends on the internal PPC flags: C and V
 */
void comp_macroblock_push_copy_cv_flags_to_register(uae_u64 regsout, comp_ppc_reg output_reg)
{
	comp_mb_init(mb,
				comp_macroblock_impl_copy_cv_flags_to_register,
				COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGC | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGV,
				regsout);
	mb->one_reg_opcode.reg = output_reg;
}

void comp_macroblock_impl_copy_cv_flags_to_register(union comp_compiler_mb_union* mb)
{
	comp_ppc_mfxer(mb->one_reg_opcode.reg);
}

/**
 * Macroblock: Copy GPR register to C and V PowerPC flag register (overwrite XER register)
 * Note: this macroblock overwrites these internal PPC flags: C and V,
 * the previous content is not preserved. There is no check for the content in the rest of the register,
 * on some PowerPC implementations masking of the required flags might be needed.
 */
void comp_macroblock_push_copy_register_to_cv_flags(uae_u64 regsin, comp_ppc_reg input_reg)
{
	comp_mb_init(mb,
				comp_macroblock_impl_copy_register_to_cv_flags,
				regsin,
				COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGC | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGV);
	mb->one_reg_opcode.reg = input_reg;
}

void comp_macroblock_impl_copy_register_to_cv_flags(union comp_compiler_mb_union* mb)
{
	comp_ppc_mtxer(mb->one_reg_opcode.reg);
}

/**
 * Macroblock: Check long-sized register value and set the PPC N and Z flag according
 * to the content of the register half word.
 */
void comp_macroblock_push_check_long_register(uae_u64 regsin, comp_ppc_reg input_reg)
{
	//It is a simple register move with flag checking
	//The target register is irrelevant, so we can use r0
	comp_macroblock_push_copy_register_long_with_flags(
				regsin,
				COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN,
				PPCR_SPECTMP_MAPPED,
				input_reg);
}

/**
 * Macroblock: Check word-sized register value and set the PPC N and Z flag according
 * to the content of the register half word.
 */
void comp_macroblock_push_check_word_register(uae_u64 regsin, comp_ppc_reg input_reg)
{
	comp_mb_init(mb,
				comp_macroblock_impl_check_word_register,
				regsin,
				COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN);
	mb->one_reg_opcode.reg = input_reg;
}

void comp_macroblock_impl_check_word_register(union comp_compiler_mb_union* mb)
{
	comp_ppc_extsh(PPCR_SPECTMP_MAPPED, mb->one_reg_opcode.reg, TRUE);
}

/**
 * Macroblock: Sign-extend lower word content of the specified register
 * into another register.
 */
void comp_macroblock_push_copy_register_word_extended(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_copy_register_word_extended,
				regsin,
				regsout);
	mb->two_regs_opcode_flags.input_reg = input_reg;
	mb->two_regs_opcode_flags.output_reg = output_reg;
	mb->two_regs_opcode_flags.updateflags = updateflags;
}

void comp_macroblock_impl_copy_register_word_extended(union comp_compiler_mb_union* mb)
{
	comp_ppc_extsh(mb->two_regs_opcode_flags.output_reg, mb->two_regs_opcode_flags.input_reg, mb->two_regs_opcode_flags.updateflags);
}

/**
 * Macroblock: Sign-extend lowest byte content of the specified register
 * into another register.
 */
void comp_macroblock_push_copy_register_byte_extended(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_copy_register_byte_extended,
				regsin,
				regsout);
	mb->two_regs_opcode_flags.input_reg = input_reg;
	mb->two_regs_opcode_flags.output_reg = output_reg;
	mb->two_regs_opcode_flags.updateflags = updateflags;
}

void comp_macroblock_impl_copy_register_byte_extended(union comp_compiler_mb_union* mb)
{
	comp_ppc_extsb(mb->two_regs_opcode.output_reg, mb->two_regs_opcode.input_reg, mb->two_regs_opcode_flags.updateflags);
}

/**
 * Macroblock: Check byte-sized register value and set the PPC N and Z flag according
 * to the content of the register half word.
 */
void comp_macroblock_push_check_byte_register(uae_u64 regsin, comp_ppc_reg input_reg)
{
	comp_mb_init(mb,
				comp_macroblock_impl_check_byte_register,
				regsin,
				COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN);
	mb->one_reg_opcode.reg = input_reg;
}

void comp_macroblock_impl_check_byte_register(union comp_compiler_mb_union* mb)
{
	comp_ppc_extsb(PPCR_SPECTMP_MAPPED, mb->one_reg_opcode.reg, TRUE);
}

/**
 * Macroblock: Rotate and copy specified bits
 * Note: when flag update is specified then the output registers will specify
 * internal flag update
 */
void comp_macroblock_push_rotate_and_copy_bits(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, uae_u8 shift, uae_u8 maskb, uae_u8 maske, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_rotate_and_copy_bits,
				regsin,
				regsout | (updateflags ?
								COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ :
								COMP_COMPILER_MACROBLOCK_REG_NONE));
	mb->shift_opcode_imm_with_mask.output_reg = output_reg;
	mb->shift_opcode_imm_with_mask.input_reg = input_reg;
	mb->shift_opcode_imm_with_mask.shift = shift;
	mb->shift_opcode_imm_with_mask.begin_mask = maskb;
	mb->shift_opcode_imm_with_mask.end_mask = maske;
	mb->shift_opcode_imm_with_mask.update_flags = updateflags;
}

void comp_macroblock_impl_rotate_and_copy_bits(union comp_compiler_mb_union* mb)
{
	comp_ppc_rlwimi(
			mb->shift_opcode_imm_with_mask.output_reg,
			mb->shift_opcode_imm_with_mask.input_reg,
			mb->shift_opcode_imm_with_mask.shift,
			mb->shift_opcode_imm_with_mask.begin_mask,
			mb->shift_opcode_imm_with_mask.end_mask,
			mb->shift_opcode_imm_with_mask.update_flags);
}

/**
 * Macroblock: Rotate by immediate and mask specified bits
 * Note: when flag update is specified then the output registers will specify
 * internal flag update
 */
void comp_macroblock_push_rotate_and_mask_bits(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, uae_u8 shift, uae_u8 maskb, uae_u8 maske, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_rotate_and_mask_bits,
				regsin,
				regsout | (updateflags ?
								COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ :
								COMP_COMPILER_MACROBLOCK_REG_NONE));
	mb->shift_opcode_imm_with_mask.output_reg = output_reg;
	mb->shift_opcode_imm_with_mask.input_reg = input_reg;
	mb->shift_opcode_imm_with_mask.shift = shift;
	mb->shift_opcode_imm_with_mask.begin_mask = maskb;
	mb->shift_opcode_imm_with_mask.end_mask = maske;
	mb->shift_opcode_imm_with_mask.update_flags = updateflags;
}

void comp_macroblock_impl_rotate_and_mask_bits(union comp_compiler_mb_union* mb)
{
	comp_ppc_rlwinm(
			mb->shift_opcode_imm_with_mask.output_reg,
			mb->shift_opcode_imm_with_mask.input_reg,
			mb->shift_opcode_imm_with_mask.shift,
			mb->shift_opcode_imm_with_mask.begin_mask,
			mb->shift_opcode_imm_with_mask.end_mask,
			mb->shift_opcode_imm_with_mask.update_flags);
}

/**
 * Macroblock: Rotate by register and mask specified bits
 * Note: when flag update is specified then the output registers will specify
 * internal flag update
 */
void comp_macroblock_push_rotate_and_mask_bits_register(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, comp_ppc_reg shift_reg, uae_u8 maskb, uae_u8 maske, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_rotate_and_mask_bits_register,
				regsin,
				regsout | (updateflags ?
								COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ :
								COMP_COMPILER_MACROBLOCK_REG_NONE));
	mb->shift_opcode_reg_with_mask.output_reg = output_reg;
	mb->shift_opcode_reg_with_mask.input_reg = input_reg;
	mb->shift_opcode_reg_with_mask.shift_reg = shift_reg;
	mb->shift_opcode_reg_with_mask.begin_mask = maskb;
	mb->shift_opcode_reg_with_mask.end_mask = maske;
	mb->shift_opcode_reg_with_mask.update_flags = updateflags;
}

void comp_macroblock_impl_rotate_and_mask_bits_register(union comp_compiler_mb_union* mb)
{
	comp_ppc_rlwnm(
			mb->shift_opcode_reg_with_mask.output_reg,
			mb->shift_opcode_reg_with_mask.input_reg,
			mb->shift_opcode_reg_with_mask.shift_reg,
			mb->shift_opcode_reg_with_mask.begin_mask,
			mb->shift_opcode_reg_with_mask.end_mask,
			mb->shift_opcode_reg_with_mask.update_flags);
}

/**
 * Macroblock: Arithmetic shift to the right of the register using immediate for the steps
 * Note: when flag update is specified then the output registers will specify
 * internal flag update
 */
void comp_macroblock_push_arithmetic_shift_right_register_imm(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, uae_u8 shift, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_arithmetic_shift_right_register_imm,
				regsin,
				regsout | (updateflags ?
								COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ :
								COMP_COMPILER_MACROBLOCK_REG_NONE));
	mb->shift_opcode_imm.output_reg = output_reg;
	mb->shift_opcode_imm.input_reg = input_reg;
	mb->shift_opcode_imm.shift = shift;
	mb->shift_opcode_imm.update_flags = updateflags;
}

void comp_macroblock_impl_arithmetic_shift_right_register_imm(union comp_compiler_mb_union* mb)
{
	comp_ppc_srawi(
			mb->shift_opcode_imm.output_reg,
			mb->shift_opcode_imm.input_reg,
			mb->shift_opcode_imm.shift,
			mb->shift_opcode_imm.update_flags);
}

/**
 * Macroblock: Logic shift to the left of the register using a register for the steps
 * Note: when flag update is specified then the output registers will specify
 * internal flag update
 */
void comp_macroblock_push_logic_shift_left_register_register(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, comp_ppc_reg shift_reg, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_logic_shift_left_register_register,
				regsin,
				regsout | (updateflags ?
								COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ :
								COMP_COMPILER_MACROBLOCK_REG_NONE));
	mb->shift_opcode_reg.output_reg = output_reg;
	mb->shift_opcode_reg.input_reg = input_reg;
	mb->shift_opcode_reg.shift_reg = shift_reg;
	mb->shift_opcode_reg.update_flags = updateflags;
}

void comp_macroblock_impl_logic_shift_left_register_register(union comp_compiler_mb_union* mb)
{
	comp_ppc_slw(
			mb->shift_opcode_reg.output_reg,
			mb->shift_opcode_reg.input_reg,
			mb->shift_opcode_reg.shift_reg,
			mb->shift_opcode_reg.update_flags);
}

/**
 * Macroblock: Logic shift to the right of the register using a register for the steps
 * Note: when flag update is specified then the output registers will specify
 * internal flag update
 */
void comp_macroblock_push_logic_shift_right_register_register(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, comp_ppc_reg shift_reg, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_logic_shift_right_register_register,
				regsin,
				regsout | (updateflags ?
								COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ :
								COMP_COMPILER_MACROBLOCK_REG_NONE));
	mb->shift_opcode_reg.output_reg = output_reg;
	mb->shift_opcode_reg.input_reg = input_reg;
	mb->shift_opcode_reg.shift_reg = shift_reg;
	mb->shift_opcode_reg.update_flags = updateflags;
}

void comp_macroblock_impl_logic_shift_right_register_register(union comp_compiler_mb_union* mb)
{
	comp_ppc_srw(
			mb->shift_opcode_reg.output_reg,
			mb->shift_opcode_reg.input_reg,
			mb->shift_opcode_reg.shift_reg,
			mb->shift_opcode_reg.update_flags);
}

/**
 * Macroblock: Arithmetic shift to the right of the register using a register for the steps
 * Note: when flag update is specified then the output registers will specify
 * internal flag update
 */
void comp_macroblock_push_arithmetic_shift_right_register_register(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, comp_ppc_reg shift_reg, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_arithmetic_shift_right_register_register,
				regsin,
				regsout | (updateflags ?
								COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ :
								COMP_COMPILER_MACROBLOCK_REG_NONE));
	mb->shift_opcode_reg.output_reg = output_reg;
	mb->shift_opcode_reg.input_reg = input_reg;
	mb->shift_opcode_reg.shift_reg = shift_reg;
	mb->shift_opcode_reg.update_flags = updateflags;
}

void comp_macroblock_impl_arithmetic_shift_right_register_register(union comp_compiler_mb_union* mb)
{
	comp_ppc_sraw(
			mb->shift_opcode_reg.output_reg,
			mb->shift_opcode_reg.input_reg,
			mb->shift_opcode_reg.shift_reg,
			mb->shift_opcode_reg.update_flags);
}

/**
 * Macroblock: count leading zero bits into a register
 */
void comp_macroblock_push_count_leading_zeroes_register(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, BOOL updateflags)
{
	comp_mb_init(mb,
				comp_macroblock_impl_count_leading_zeroes_register,
				regsin, regsout);
	mb->two_regs_opcode_flags.output_reg = output_reg;
	mb->two_regs_opcode_flags.input_reg = input_reg;
	mb->two_regs_opcode_flags.updateflags = updateflags;
}

void comp_macroblock_impl_count_leading_zeroes_register(union comp_compiler_mb_union* mb)
{
	comp_ppc_cntlzw(
			mb->two_regs_opcode_flags.output_reg,
			mb->two_regs_opcode_flags.input_reg,
			mb->two_regs_opcode_flags.updateflags);
}

/**
 * Macroblock: Calculate V flag for arithmetic left shift, inserts the V flag directly
 * into the flag emulation register
 */
void comp_macroblock_push_arithmetic_left_shift_extract_v_flag(uae_u64 regsin, comp_ppc_reg input_reg, comp_ppc_reg shift_reg, comp_ppc_reg tmp_reg)
{
	comp_mb_init(mb,
				comp_macroblock_impl_arithmetic_left_shift_extract_v_flag,
				regsin,
				COMP_COMPILER_MACROBLOCK_REG_FLAGV);
	mb->extract_v_flag_arithmetic_left_shift.input_reg = input_reg;
	mb->extract_v_flag_arithmetic_left_shift.shift_reg = shift_reg;
	mb->extract_v_flag_arithmetic_left_shift.temp_reg = tmp_reg;
}

void comp_macroblock_impl_arithmetic_left_shift_extract_v_flag(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg input_reg = mb->extract_v_flag_arithmetic_left_shift.input_reg;
	comp_ppc_reg temp_reg = mb->extract_v_flag_arithmetic_left_shift.temp_reg;
	comp_ppc_reg shift_reg = mb->extract_v_flag_arithmetic_left_shift.shift_reg;

	//Count leading 0 bits to R0
	comp_ppc_cntlzw(PPCR_SPECTMP_MAPPED, input_reg, FALSE);

	//Invert the source register into the temp register
	comp_ppc_nor(temp_reg, input_reg, input_reg, FALSE);

	//Count leading 0 bits again to the temp register
	//(counting leading 1 bits by using the result from the previous inversion)
	comp_ppc_cntlzw(temp_reg, temp_reg, FALSE);

	//Calculate the distance of the first changing bit to temp register,
	//also clear XER[CA] (Carry flag) for the next instruction
	comp_ppc_addc(temp_reg, temp_reg, PPCR_SPECTMP_MAPPED, FALSE);

	//Subtract the number of shifting steps from the calculated distance
	//and decrease it by one (tmpreg = tmpreg - shift - 1).
	//By this we get a negative number if the number of the steps is
	//equal or less than the distance of the first bit change.
	comp_ppc_subfe(temp_reg, shift_reg, temp_reg, FALSE);

	//If the result is negative then the overflow happens while shifting
	//V flag should be set according to the MSB
	comp_ppc_rlwimi(PPCR_FLAGS_MAPPED, temp_reg, 23, 9, 9, FALSE);
}

/**
 * Macroblock: Calculate C flag for shift, inserts the C flag directly
 * into the flag emulation register.
 * If shift equals to 0 and left_shift is TRUE, or shift equals to 32 and left_shift is FALSE,
 * then the C flag cleared (no bit shift was done).
 * Note: this macroblock uses CRF0 for comparison.
 */
void comp_macroblock_push_shift_extract_c_flag(uae_u64 regsin, comp_ppc_reg input_reg, comp_ppc_reg shift_reg, BOOL left_shift)
{
	//v18: C is only changed if the shift count is not zero: the
	//previous C (cleared before) is an input, otherwise the optimizer
	//removes the clearing
	comp_mb_init(mb,
				comp_macroblock_impl_shift_extract_c_flag,
				regsin | COMP_COMPILER_MACROBLOCK_REG_FLAGC,
				COMP_COMPILER_MACROBLOCK_REG_FLAGC);
	mb->extract_c_flag_shift.input_reg = input_reg;
	mb->extract_c_flag_shift.shift_reg = shift_reg;
	mb->extract_c_flag_shift.left_shift = left_shift;
}

void comp_macroblock_impl_shift_extract_c_flag(union comp_compiler_mb_union* mb)
{
	//Is the shift register equals to zero (left shift) or 32 (right shift)?
	comp_ppc_cmplwi(PPCR_CR_TMP0,
				    mb->extract_c_flag_shift.shift_reg,
				    mb->extract_c_flag_shift.left_shift ? 0 : 32);

	//Then skip the extraction
	comp_ppc_bc(PPC_B_CR_TMP0_EQ, 0);

	//Extract C flag from lowest bit to temp reg
	comp_ppc_rlwnm(PPCR_SPECTMP_MAPPED,
				  mb->extract_c_flag_shift.input_reg,
				  mb->extract_c_flag_shift.shift_reg,
				  0, 31, FALSE);

	//Copy to flag register
	comp_ppc_rlwimi(PPCR_FLAGS_MAPPED,
				  PPCR_SPECTMP_MAPPED,
				  mb->extract_c_flag_shift.left_shift ? 21 : 22,
				  10, 10, FALSE);

	//Branch target 0 is reached
	comp_ppc_branch_target(0);
}

/**
 * Macroblock: Calculate C and X flag for shift, inserts both flags directly
 * into the flag emulation register.
 * If shift equals to 0 and left_shift is TRUE, or shift equals to 32 and left_shift is FALSE,
 * then the C flag cleared (no bit shift was done), X flag will be unchanged.
 * Note: this macroblock uses CRF0 for comparison.
 */
void comp_macroblock_push_shift_extract_cx_flag(uae_u64 regsin, comp_ppc_reg input_reg, comp_ppc_reg shift_reg, BOOL left_shift)
{
	//v18: X stays unchanged for a shift count of zero, so the previous
	//X is an input (before, the optimizer removed the loading of X)
	comp_mb_init(mb,
				comp_macroblock_impl_shift_extract_cx_flag,
				regsin | COMP_COMPILER_MACROBLOCK_REG_FLAGC | COMP_COMPILER_MACROBLOCK_REG_FLAGX,
				COMP_COMPILER_MACROBLOCK_REG_FLAGC | COMP_COMPILER_MACROBLOCK_REG_FLAGX);
	mb->extract_c_flag_shift.shift_reg = shift_reg;
	mb->extract_c_flag_shift.input_reg = input_reg;
	mb->extract_c_flag_shift.left_shift = left_shift;
}

void comp_macroblock_impl_shift_extract_cx_flag(union comp_compiler_mb_union* mb)
{
	//Is the shift register equals to zero (left shift) or 32 (right shift)?
	comp_ppc_cmplwi(PPCR_CR_TMP0,
				    mb->extract_c_flag_shift.shift_reg,
				    mb->extract_c_flag_shift.left_shift ? 0 : 32);

	//Then skip the extraction
	comp_ppc_bc(PPC_B_CR_TMP0_EQ, 0);

	//Extract C flag from lowest bit to temp reg
	comp_ppc_rlwnm(PPCR_SPECTMP_MAPPED,
				  mb->extract_c_flag_shift.input_reg,
				  mb->extract_c_flag_shift.shift_reg,
				  0, 31, FALSE);

	//Copy to flag register
	comp_ppc_rlwimi(PPCR_FLAGS_MAPPED,
				  PPCR_SPECTMP_MAPPED,
				  mb->extract_c_flag_shift.left_shift ? 21 : 22,
				  10, 10, FALSE);

	//Copy C flag to X flag in flag register
	comp_ppc_rlwimi(PPCR_FLAGS_MAPPED,
					PPCR_FLAGS_MAPPED,
					16, 26, 26, FALSE);

	//Branch target 0 is reached
	comp_ppc_branch_target(0);
}

/**
 * Macroblock: TRAP PPC opcode to the top of the buffer
 * Note: this opcode won't be optimized away
 */
void comp_macroblock_push_stop()
{
	//Registers are not required for input to avoid any interfere with the optimization
	comp_mb_init(mb,
				comp_macroblock_impl_stop,
				COMP_COMPILER_MACROBLOCK_REG_NONE,
				COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM);
}

void comp_macroblock_impl_stop(union comp_compiler_mb_union* mb)
{
	comp_ppc_trap();
}

/**
 * Macroblock: NOP PPC opcode to the top of the buffer, useless instruction just to mark some location in the output
 * Note: this opcode won't be optimized away
 */
void comp_macroblock_push_nop()
{
	//Registers are not required for input to avoid any interfere with the optimization
	comp_mb_init(mb,
				comp_macroblock_impl_nop,
				COMP_COMPILER_MACROBLOCK_REG_NONE,
				COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM);
}

void comp_macroblock_impl_nop(union comp_compiler_mb_union* mb)
{
	comp_ppc_nop();
}

/**
 * Macroblock: null operation.
 * This macroblock has no output in the compiled code, can be used for
 * specific tweaking on the register flow optimizations.
 */
void comp_macroblock_push_null_operation(uae_u64 regsin, uae_u64 regsout)
{
	//There is no real macroblock behind this, only input/output register specification
	comp_mb_init(mb,
				comp_macroblock_impl_null_operation,
				regsin,
				regsout);
}

void comp_macroblock_impl_null_operation(union comp_compiler_mb_union* mb)
{
	//Do nothing
}

/**
 * Macroblock: saves the specified register into the next slot in the Regs structure.
 */
void comp_macroblock_push_save_register_to_context(uae_u64 regsin, comp_ppc_reg input_reg)
{
	int slot = comp_next_free_register_slot();

	//Registers are not required for input to avoid any interfere with the optimization
	comp_mb_init(mb,
				comp_macroblock_impl_save_register_to_context,
				regsin,
				COMP_COMPILER_MACROBLOCK_REG_NONE);
	mb->reg_in_slot.slot = slot;
	mb->reg_in_slot.reg = input_reg;
}

void comp_macroblock_impl_save_register_to_context(union comp_compiler_mb_union* mb)
{
	comp_ppc_stw(mb->reg_in_slot.reg,
					COMP_GET_OFFSET_IN_REGS(regslots) + (mb->reg_in_slot.slot * 4),
					PPCR_REGS_BASE_MAPPED);
}

/**
 * Macroblock: restores the specified register from the last slot in the Regs structure.
 */
void comp_macroblock_push_restore_register_from_context(uae_u64 regsout, comp_ppc_reg output_reg)
{
	int slot = comp_last_register_slot();

	//Registers are not required for input to avoid any interfere with the optimization
	comp_mb_init(mb,
				comp_macroblock_impl_restore_register_from_context,
				COMP_COMPILER_MACROBLOCK_REG_NONE,
				regsout);
	mb->reg_in_slot.slot = slot;
	mb->reg_in_slot.reg = output_reg;
}

void comp_macroblock_impl_restore_register_from_context(union comp_compiler_mb_union* mb)
{
	comp_ppc_lwz(mb->reg_in_slot.reg,
					COMP_GET_OFFSET_IN_REGS(regslots) + (mb->reg_in_slot.slot * 4),
					PPCR_REGS_BASE_MAPPED);
}

/**
 * Macroblock: load the specified 68k address into the PC register.
 * The specified address is in the 68k emulated address space which is translated
 * to the physical memory address and load into the emulated PC register.
 */
void comp_macroblock_push_load_pc_from_register(uae_u64 regsin, comp_ppc_reg address_reg)
{
	//TODO: this macroblock is more like a helper function, must be moved out
	comp_tmp_reg* temp_reg = comp_allocate_temp_register(NULL, PPC_TMP_REG_NOTUSED_MAPPED);

	//TODO: odd address must trigger exception

	//Save it to emulated instruction pointer (PC register)
	comp_macroblock_push_save_memory_long(
			regsin,
			COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM,
			address_reg,
			PPCR_REGS_BASE_MAPPED,
			COMP_GET_OFFSET_IN_REGS(pc));

	//Get memory address into the temp register
	comp_macroblock_push_map_physical_mem(
			regsin,
			temp_reg->reg_usage_mapping,
			temp_reg->mapped_reg_num,
			address_reg);

	//Save physical memory pointer for the executed instruction
	comp_macroblock_push_save_memory_long(
			temp_reg->reg_usage_mapping,
			COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM,
			temp_reg->mapped_reg_num,
			PPCR_REGS_BASE_MAPPED,
			COMP_GET_OFFSET_IN_REGS(pc_p));

	//Synchronize the executed instruction pointer from the block start to the actual
	comp_macroblock_push_save_memory_long(
			temp_reg->reg_usage_mapping,
			COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM,
			temp_reg->mapped_reg_num,
			PPCR_REGS_BASE_MAPPED,
			COMP_GET_OFFSET_IN_REGS(pc_oldp));

	comp_free_temp_register(temp_reg);
}

/**
 * Macroblock: load the specified 68k address into the PC register
 * or do nothing according to the Z flag state.
 * Target address is loaded to the PC when PPC Z flag is set and negate is FALSE or
 * PPC Z flag is not set and negate is TRUE.
 * The specified address is in the 68k emulated address space which is translated
 * to the physical memory address and load into the emulated PC register.
 * Skip address is loaded if the condition was false.
 * This macroblock needs two preallocated temporary registers.
 * Parameters:
 *    target_address - target address for emulated PC if evaluation came up as TRUE
 *    skip_address - skip address for emulated PC if evaluation came up as FALSE
 *    negate - if TRUE then the condition evaluation is negated (will be TRUE if Z flag is FALSE)
 *    address_reg - mapped temporary register for the operation
 *    tmp_reg - mapped temporary register for the operation
 */
void comp_macroblock_push_load_pc_from_immediate_conditional(uae_u32 target_address, uae_u32 skip_address, BOOL negate, comp_ppc_reg address_reg, comp_ppc_reg tmp_reg)
{
	comp_mb_init(mb,
				comp_macroblock_impl_load_pc_from_immediate_conditional,
				COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ,
				COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM);
	mb->set_pc_on_z_flag.negate = negate;
	mb->set_pc_on_z_flag.target_address = target_address;
	mb->set_pc_on_z_flag.skip_address = skip_address;
	mb->set_pc_on_z_flag.address_reg = address_reg;
	mb->set_pc_on_z_flag.tmp_reg = tmp_reg;
}

void comp_macroblock_impl_load_pc_from_immediate_conditional(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg address_reg = mb->set_pc_on_z_flag.address_reg;
	comp_ppc_reg tmp_reg = mb->set_pc_on_z_flag.tmp_reg;

	//Condition is evaluated into the CRF0 Z flag by the source address mode handler or
	//any other previous evaluation process.
	//Branch accordingly to the previously calculated flag.
	//If Z flag was set then the condition was evaluated to FALSE.
	//Check negate option and invert the branch instruction if it was set.
	comp_ppc_bc(mb->set_pc_on_z_flag.negate ?
					PPC_B_CR_TMP0_NE : PPC_B_CR_TMP0_EQ, 0);

	//Load target address immediate into the address register
	comp_ppc_liw(address_reg, mb->set_pc_on_z_flag.target_address);

	//Skip to the storing
	comp_ppc_b(0, 1);

	//Branch target #0 reached, set it
	comp_ppc_branch_target(0);

	//Load skip address immediate into the address register
	comp_ppc_liw(address_reg, mb->set_pc_on_z_flag.skip_address);

	//Branch target #1 is reached, set it
	comp_ppc_branch_target(1);

	//Save it to emulated instruction pointer (PC register)
	comp_ppc_stw(address_reg, COMP_GET_OFFSET_IN_REGS(pc), PPCR_REGS_BASE_MAPPED);

	//Get memory address into the address register
	helper_map_physical_mem(address_reg, address_reg, tmp_reg);

	//Save physical memory pointer for the executed instruction
	comp_ppc_stw(address_reg, COMP_GET_OFFSET_IN_REGS(pc_p), PPCR_REGS_BASE_MAPPED);

	//Synchronize the executed instruction pointer from the block start to the actual
	comp_ppc_stw(address_reg, COMP_GET_OFFSET_IN_REGS(pc_oldp), PPCR_REGS_BASE_MAPPED);
}

/**
 * Macroblock: load the specified 68k address into the PC register
 * or do nothing according to the Z flag state and when decremented register reaches -1.
 * Skip address is loaded to the PC when PPC Z flag is not set and negate is FALSE or
 * PPC Z flag is set and negate is TRUE.
 * If the previous condition was FALSE then the specified decrement register will be
 * decremented, Z flag set to TRUE when it reaches -1. Condition is
 * evaluated again, and skip address loaded if Z flag is FALSE or target address otherwise.
 * The specified address is in the 68k emulated address space which is translated
 * to the physical memory address and load into the emulated PC register.
 * Skip address is loaded if the condition was false.
 * This macroblock needs two preallocated temporary registers.
 *
 * Note: if this macroblock seems to be confusing then have a look on
 * the behavior of M68k DBcc.W instruction.
 *
 * Parameters:
 *    regsin - input and output register dependency (register to decrement)
 *    decrement_reg - mapped register to decrement
 *    target_address - target address for emulated PC if evaluation came up as TRUE
 *    skip_address - skip address for emulated PC if evaluation came up as FALSE
 *    negate - if TRUE then the condition evaluation is negated (will be TRUE if Z flag is FALSE)
 *    address_reg - mapped temporary register for the operation
 *    tmp_reg - mapped temporary register for the operation
 */
void comp_macroblock_push_load_pc_from_immediate_conditional_decrement_register(uae_u64 regsin, comp_ppc_reg decrement_reg, uae_u32 target_address, uae_u32 skip_address, BOOL negate, comp_ppc_reg address_reg, comp_ppc_reg tmp_reg)
{
	comp_mb_init(mb,
				comp_macroblock_impl_load_pc_from_immediate_conditional_decrement_register,
				regsin | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ,
				regsin | COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM);
	mb->set_pc_on_z_flag.negate = negate;
	mb->set_pc_on_z_flag.target_address = target_address;
	mb->set_pc_on_z_flag.skip_address = skip_address;
	mb->set_pc_on_z_flag.address_reg = address_reg;
	mb->set_pc_on_z_flag.tmp_reg = tmp_reg;
	mb->set_pc_on_z_flag.decrement_reg = decrement_reg;
}

void comp_macroblock_impl_load_pc_from_immediate_conditional_decrement_register(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg address_reg = mb->set_pc_on_z_flag.address_reg;
	comp_ppc_reg tmp_reg = mb->set_pc_on_z_flag.tmp_reg;
	comp_ppc_reg decrement_reg = mb->set_pc_on_z_flag.decrement_reg;

	//Condition is evaluated into the CRF0 Z flag by the source address mode handler or
	//any other previous evaluation process.
	//Branch accordingly to the previously calculated flag.
	//If Z flag was set then the condition was evaluated to FALSE.
	//Check negate option and invert the branch instruction if it was set.
	comp_ppc_bc(mb->set_pc_on_z_flag.negate ?
					PPC_B_CR_TMP0_EQ : PPC_B_CR_TMP0_NE, 0);

	//Condition evaluated to FALSE, decrement register
	//Word sized operation: mask out lowest word from target register
	//This will also set the Z flag on PPC
	comp_ppc_andi(tmp_reg, decrement_reg, 0xffff);

	//Decrease target register
	comp_ppc_addi(tmp_reg, tmp_reg, -1);

	//Insert result back to source register
	comp_ppc_rlwimi(decrement_reg, tmp_reg, 0, 16, 31, FALSE);

	//Jump if Z flag is set (decremented register was 0 before the operation)
	comp_ppc_bc(PPC_B_CR_TMP0_EQ, 1);

	//Load target address immediate into the address register
	comp_ppc_liw(address_reg, mb->set_pc_on_z_flag.target_address);

	//Skip to the storing
	comp_ppc_b(0, 2);

	//Branch target #1 reached, set it
	comp_ppc_branch_target(1);

	//Branch target #0 reached, set it
	comp_ppc_branch_target(0);

	//Load skip address immediate into the address register
	comp_ppc_liw(address_reg, mb->set_pc_on_z_flag.skip_address);

	//Branch target #2 reached, set it
	comp_ppc_branch_target(2);

	//Save it to emulated instruction pointer (PC register)
	comp_ppc_stw(address_reg, COMP_GET_OFFSET_IN_REGS(pc), PPCR_REGS_BASE_MAPPED);

	//Get memory address into the address register
	helper_map_physical_mem(address_reg, address_reg, tmp_reg);

	//Save physical memory pointer for the executed instruction
	comp_ppc_stw(address_reg, COMP_GET_OFFSET_IN_REGS(pc_p), PPCR_REGS_BASE_MAPPED);

	//Synchronize the executed instruction pointer from the block start to the actual
	comp_ppc_stw(address_reg, COMP_GET_OFFSET_IN_REGS(pc_oldp), PPCR_REGS_BASE_MAPPED);
}

/**
 * v18 (TODO 7.5): leaving a block from the middle (the other path of a
 * conditional branch inside the block): write back the changed mapped 68k
 * registers and the flags, set the PC to the exit address and continue
 * like at the end of a block (chaining stub). The temporary registers are
 * free to use here: their 68k values are written back first.
 */
static void helper_trace_exit_code(struct comp_compiler_mb_trace_exit* te)
{
	comp_ppc_save_mapped_registers_from_list(te->mapped_regs);
	comp_ppc_save_flags();
	comp_ppc_liw(PPCR_PARAM1_MAPPED, te->exit_address);
	comp_ppc_stw(PPCR_PARAM1_MAPPED, COMP_GET_OFFSET_IN_REGS(pc), PPCR_REGS_BASE_MAPPED);
	helper_map_physical_mem(PPCR_PARAM1_MAPPED, PPCR_PARAM1_MAPPED, PPCR_PARAM2_MAPPED);
	comp_ppc_stw(PPCR_PARAM1_MAPPED, COMP_GET_OFFSET_IN_REGS(pc_p), PPCR_REGS_BASE_MAPPED);
	comp_ppc_stw(PPCR_PARAM1_MAPPED, COMP_GET_OFFSET_IN_REGS(pc_oldp), PPCR_REGS_BASE_MAPPED);
	comp_ppc_block_exit(te->cycles);
}

/* Cycles charged when a block is left in the middle, set by compile_block() */
int comp_trace_exit_cycles = 0;

/**
 * v18: dependencies of code that leaves the block in the middle (or calls
 * the exception handler): all flags and every changed mapped 68k register,
 * so that the optimizer keeps their values up to this point.
 */
uae_u64 comp_exit_deps(void)
{
	uae_s8 list[16];
	uae_u64 deps = COMP_COMPILER_MACROBLOCK_REG_FLAG_ALL;
	int i;

	comp_get_changed_mapped_regs_list(list);
	for (i = 0; i < 16; i++)
		if (list[i] != -1)
			deps |= COMP_COMPILER_MACROBLOCK_REG_DX_OR_AX(i);

	//v18 (TODO 7.4): the fixed registers are written back when the
	//translated code is left (or around the exception handler)
	return deps | comp_fixregs_deps();
}

/**
 * Macroblock: v18 (TODO 7.5): Bcc or DBF inside a block. The condition is
 * in CR0 (see comp_macroblock_push_load_pc_from_immediate_conditional());
 * exit_bibo is the condition for leaving the block to exit_address.
 */
void comp_macroblock_push_trace_exit(int exit_bibo, uae_u32 exit_address)
{
	comp_mb_init(mb,
				comp_macroblock_impl_trace_exit,
				COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ | comp_exit_deps(),
				COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM);
	mb->trace_exit.exit_bibo = exit_bibo;
	mb->trace_exit.exit_address = exit_address;
	mb->trace_exit.cycles = comp_trace_exit_cycles;
	comp_get_changed_mapped_regs_list(mb->trace_exit.mapped_regs);
}

void comp_macroblock_impl_trace_exit(union comp_compiler_mb_union* mb)
{
	//Stay in the block: skip the exit code
	comp_ppc_bc(mb->trace_exit.exit_bibo ^ 0x100, 3);
	helper_trace_exit_code(&mb->trace_exit);
	comp_ppc_branch_target(3);
}

/**
 * Macroblock: v18 (TODO 7.5): DBcc inside a block, same as
 * comp_macroblock_push_load_pc_from_immediate_conditional_decrement_register(),
 * but the block continues on the path of the interpreter (follow_taken:
 * at the branch target) and the other path leaves the block.
 */
void comp_macroblock_push_trace_dbcc(uae_u64 regsin, comp_ppc_reg decrement_reg, comp_ppc_reg tmp_reg, BOOL negate, BOOL follow_taken, uae_u32 exit_address)
{
	comp_mb_init(mb,
				comp_macroblock_impl_trace_dbcc,
				regsin | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ | comp_exit_deps(),
				regsin | COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM);
	mb->trace_exit.decrement_reg = decrement_reg;
	mb->trace_exit.tmp_reg = tmp_reg;
	mb->trace_exit.negate = negate;
	mb->trace_exit.follow_taken = follow_taken;
	mb->trace_exit.exit_address = exit_address;
	mb->trace_exit.cycles = comp_trace_exit_cycles;
	comp_get_changed_mapped_regs_list(mb->trace_exit.mapped_regs);
}

void comp_macroblock_impl_trace_dbcc(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg tmp_reg = mb->trace_exit.tmp_reg;
	comp_ppc_reg decrement_reg = mb->trace_exit.decrement_reg;

	//Condition true: no branch (same test as the DBcc macroblock above)
	comp_ppc_bc(mb->trace_exit.negate ? PPC_B_CR_TMP0_EQ : PPC_B_CR_TMP0_NE, 3);

	//Decrement the low word, Z: the word was 0 (now -1: no branch)
	comp_ppc_andi(tmp_reg, decrement_reg, 0xffff);
	comp_ppc_addi(tmp_reg, tmp_reg, -1);
	comp_ppc_rlwimi(decrement_reg, tmp_reg, 0, 16, 31, FALSE);
	comp_ppc_bc(PPC_B_CR_TMP0_EQ, 4);

	//Branch taken
	if (mb->trace_exit.follow_taken)
		comp_ppc_b(0, 5);
	else
		helper_trace_exit_code(&mb->trace_exit);

	//No branch
	comp_ppc_branch_target(3);
	comp_ppc_branch_target(4);
	if (mb->trace_exit.follow_taken)
	{
		helper_trace_exit_code(&mb->trace_exit);
		comp_ppc_branch_target(5);
	}
}

void comp_macroblock_push_set_byte_from_z_flag(uae_u64 regsout, comp_ppc_reg output_reg, BOOL negate)
{
	comp_mb_init(mb,
				comp_macroblock_impl_set_byte_from_z_flag,
				regsout | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ,
				regsout);
	mb->set_byte_from_z_flag.negate = negate;
	mb->set_byte_from_z_flag.output_reg = output_reg;
}

void comp_macroblock_impl_set_byte_from_z_flag(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg output_reg = mb->set_byte_from_z_flag.output_reg;

	//Condition is evaluated into the CRF0 Z flag by the source address mode handler.

	//Set byte to 0 (false) by default
	comp_ppc_rlwinm(output_reg, output_reg, 0, 0, 23, FALSE);

	//Branch accordingly to the previously calculated flag.
	//If Z flag was set then the condition was evaluated to FALSE.
	//Check negate option and invert the branch instruction if it was set.
	comp_ppc_bc(mb->set_byte_from_z_flag.negate ?
					PPC_B_CR_TMP0_NE : PPC_B_CR_TMP0_EQ, 0);

	//Set byte to 0xff (true)
	comp_ppc_ori(output_reg, output_reg, 0xff);

	//Branch target reached, set it
	comp_ppc_branch_target(0);
}

/* This is a very specific macroblock for BFEXTS instruction:
 * inverse of the provided bit mask is or'ed to the target register if the emulated N flag
 * is set.
 * Note: this macroblock uses the spec temp (R0) register.
 * TODO: refactor this macroblock into the instruction without branching, if possible.
 */
void comp_macroblock_push_or_negative_mask_if_n_flag_set(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg mask_reg)
{
	comp_mb_init(mb,
				comp_macroblock_impl_or_negative_mask_if_n_flag_set,
				regsin | COMP_COMPILER_MACROBLOCK_REG_FLAGN,
				regsout);
	mb->two_regs_opcode.input_reg = mask_reg;
	mb->two_regs_opcode.output_reg = output_reg;
}

void comp_macroblock_impl_or_negative_mask_if_n_flag_set(union comp_compiler_mb_union* mb)
{
	//Set the Z flag accordingly to the emulated N flag
	comp_ppc_andis(PPCR_SPECTMP_MAPPED, PPCR_FLAGS_MAPPED, (PPCR_REG_BIT(FLAGBIT_N) >> 16));

	//Branch if N flag was not set
	comp_ppc_bc(PPC_B_CR_TMP0_EQ, 0);

	//Complement-or the mask to the target register
	comp_ppc_orc(mb->two_regs_opcode.output_reg,
					mb->two_regs_opcode.output_reg,
					mb->two_regs_opcode.input_reg,
					FALSE);

	//Branch target reached, set it
	comp_ppc_branch_target(0);
}

/**
 * Converts the CCR (68k flag register) format to the intermediate flag format which can be
 * stored in the PPCR_FLAGS register.
 * The bits which are not involved in the operation will be ignored.
 */
void comp_macroblock_push_convert_ccr_to_internal(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg)
{
	comp_mb_init(mb,
				comp_macroblock_impl_convert_ccr_to_internal,
				regsin,
				regsout);
	mb->two_regs_opcode.input_reg = input_reg;
	mb->two_regs_opcode.output_reg = output_reg;
}

void comp_macroblock_impl_convert_ccr_to_internal(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg input_reg = mb->two_regs_opcode.input_reg;
	comp_ppc_reg output_reg = mb->two_regs_opcode.output_reg;

	//Copy V and C flags to the right place, clear the rest of the bits in the target register
	comp_ppc_rlwinm(output_reg, input_reg, 21, 9, 10, FALSE);

	//Insert the N flag
	comp_ppc_rlwimi(output_reg, input_reg, 28, 0, 0, FALSE);

	//Insert the Z flag
	comp_ppc_rlwimi(output_reg, input_reg, 27, 2, 2, FALSE);

	//Insert the X flag
	comp_ppc_rlwimi(output_reg, input_reg, 1, 26, 26, FALSE);
}

/**
 * Converts the intermediate flag format to the CCR (68k flag register).
 * The bits which are not involved in the operation will be cleared in the target register.
 */
void comp_macroblock_push_convert_internal_to_ccr(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg)
{
	comp_mb_init(mb,
				comp_macroblock_impl_convert_internal_to_ccr,
				regsin,
				regsout);
	mb->two_regs_opcode.input_reg = input_reg;
	mb->two_regs_opcode.output_reg = output_reg;
}

void comp_macroblock_impl_convert_internal_to_ccr(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg input_reg = mb->two_regs_opcode.input_reg;
	comp_ppc_reg output_reg = mb->two_regs_opcode.output_reg;

	//Copy V and C flags to the right place, clear the rest of the bits in the target register
	comp_ppc_rlwinm(output_reg, input_reg, 11, 30, 31, FALSE);

	//Insert the N flag
	comp_ppc_rlwimi(output_reg, input_reg, 4, 28, 28, FALSE);

	//Insert the Z flag
	comp_ppc_rlwimi(output_reg, input_reg, 5, 29, 29, FALSE);

	//Insert the X flag
	comp_ppc_rlwimi(output_reg, input_reg, 31, 27, 27, FALSE);
}

/**
 * Division of 32 bit register by 16 bit register, output is quotient (lower 16 bit) and remainder (higher 16 bit).
 * Flags are also set according to the inputs and result.
 * This macroblock can cause a division by zero exception.
 */
void comp_macroblock_push_division_32_16bit(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg dividend_reg, comp_ppc_reg divisor_reg, BOOL signed_division, uae_u32 next_address, uae_u32 next_location)
{
	comp_tmp_reg* tempreg1 = comp_allocate_temp_register(NULL, PPC_TMP_REG_NOTUSED_MAPPED);
	comp_tmp_reg* tempreg2 = comp_allocate_temp_register(NULL, PPC_TMP_REG_NOTUSED_MAPPED);

	comp_mb_init(mb,
				comp_macroblock_impl_division_32_16bit,
				regsin | comp_exit_deps(),
				regsout | COMP_COMPILER_MACROBLOCK_REG_FLAGN | COMP_COMPILER_MACROBLOCK_REG_FLAGZ | COMP_COMPILER_MACROBLOCK_REG_FLAGC | COMP_COMPILER_MACROBLOCK_REG_FLAGV);
	mb->division_two_reg_opcode.output_reg = output_reg;
	mb->division_two_reg_opcode.dividend_reg = dividend_reg;
	mb->division_two_reg_opcode.divisor_reg = divisor_reg;
	mb->division_two_reg_opcode.signed_division = signed_division;
	mb->division_two_reg_opcode.temp_reg1 = tempreg1->mapped_reg_num;
	mb->division_two_reg_opcode.temp_reg2 = tempreg2->mapped_reg_num;

	//Get the list of mapped registers directly into the list array
	comp_get_changed_mapped_regs_list(mb->division_two_reg_opcode.exception_data.mapped_regs);

	 //Next instruction will be used for the return address
	mb->division_two_reg_opcode.exception_data.next_address = next_address;
	mb->division_two_reg_opcode.exception_data.next_location = next_location;

	comp_free_temp_register(tempreg1);
	comp_free_temp_register(tempreg2);
}

void comp_macroblock_impl_division_32_16bit(union comp_compiler_mb_union* mb)
{
	//TODO: optimize register/flag compiling according to the calculated input/output for the macroblock

	comp_ppc_reg output_reg = mb->division_two_reg_opcode.output_reg;
	comp_ppc_reg dividend_reg = mb->division_two_reg_opcode.dividend_reg;
	comp_ppc_reg divisor_reg = mb->division_two_reg_opcode.divisor_reg;
	comp_ppc_reg temp_reg1 = mb->division_two_reg_opcode.temp_reg1;
	comp_ppc_reg temp_reg2 = mb->division_two_reg_opcode.temp_reg2;
	BOOL signed_division = mb->division_two_reg_opcode.signed_division;

	//Load next address into PC
	comp_ppc_load_pc(mb->division_two_reg_opcode.exception_data.next_address, mb->division_two_reg_opcode.exception_data.next_location);

	//Clear N, Z, C and V flags
	//This is not defined in the processor documentation and there are side-effects,
	//but the interpretive emulation is doing this.
	comp_ppc_andi(PPCR_FLAGS_MAPPED, PPCR_FLAGS_MAPPED, 0xffff);

	//Copy 16 bits of the divisor into the temp register#1
	if (signed_division)
	{
		//Signed division: sign extend
		comp_ppc_extsh(temp_reg1, divisor_reg, TRUE);
	} else {
		//Unsigned division: mask out higher 16 bit
		comp_ppc_andi(temp_reg1, divisor_reg, 0xFFFF);
	}

	//Check for division by zero and trigger exception if needed
	helper_division_by_zero_check(signed_division, dividend_reg, &mb->division_two_reg_opcode.exception_data);

	//Do the division and set flags, quotient goes to the temp register#2
	helper_divide_32_bit(signed_division, temp_reg2, dividend_reg, temp_reg1, TRUE);

	//Calculate remainder: multiply the result with divisor
	comp_ppc_mullwo(PPCR_SPECTMP_MAPPED, temp_reg2, temp_reg1, FALSE);

	//Then subtract from dividend
	comp_ppc_subf(PPCR_SPECTMP_MAPPED, PPCR_SPECTMP_MAPPED, dividend_reg, FALSE);

	//Copy quotient to the output register
	comp_ppc_mr(output_reg, temp_reg2, FALSE);

	//Insert remainder into the higher 16 bits
	comp_ppc_rlwimi(output_reg, PPCR_SPECTMP_MAPPED, 16, 0, 15, FALSE);

	//End of the instruction, in case of overflow this is where the execution continues
	comp_ppc_branch_target(1);
}

/**
 * Division of 32 bit register by 32 bit register, output is quotient (32 bit quotient register) and remainder (32 bit remainder register).
 * Flags are also set according to the inputs and result.
 * This macroblock can cause a division by zero exception.
 */
void comp_macroblock_push_division_32_32bit(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg quotient_reg, comp_ppc_reg remainder_reg, comp_ppc_reg divisor_reg, BOOL signed_division, uae_u32 next_address, uae_u32 next_location)
{
	comp_tmp_reg* tempreg = comp_allocate_temp_register(NULL, PPC_TMP_REG_NOTUSED_MAPPED);

	comp_mb_init(mb,
				comp_macroblock_impl_division_32_32bit,
				regsin | comp_exit_deps(),
				regsout | COMP_COMPILER_MACROBLOCK_REG_FLAGN | COMP_COMPILER_MACROBLOCK_REG_FLAGZ | COMP_COMPILER_MACROBLOCK_REG_FLAGC | COMP_COMPILER_MACROBLOCK_REG_FLAGV);
	mb->division_three_reg_opcode.quotient_reg = quotient_reg;
	mb->division_three_reg_opcode.remainder_reg = remainder_reg;
	mb->division_three_reg_opcode.divisor_reg = divisor_reg;
	mb->division_three_reg_opcode.signed_division = signed_division;
	mb->division_three_reg_opcode.temp_reg = tempreg->mapped_reg_num;

	//Get the list of mapped registers directly into the list array
	comp_get_changed_mapped_regs_list(mb->division_three_reg_opcode.exception_data.mapped_regs);

	 //Next instruction will be used for the return address
	mb->division_three_reg_opcode.exception_data.next_address = next_address;
	mb->division_three_reg_opcode.exception_data.next_location = next_location;

	comp_free_temp_register(tempreg);
}

void comp_macroblock_impl_division_32_32bit(union comp_compiler_mb_union* mb)
{
	//TODO: optimize register/flag compiling according to the calculated input/output for the macroblock

	comp_ppc_reg quotient_reg = mb->division_three_reg_opcode.quotient_reg;
	comp_ppc_reg divisor_reg = mb->division_three_reg_opcode.divisor_reg;
	comp_ppc_reg remainder_reg = mb->division_three_reg_opcode.remainder_reg;
	comp_ppc_reg temp_reg = mb->division_three_reg_opcode.temp_reg;
	BOOL signed_division = mb->division_three_reg_opcode.signed_division;

	//Load next address into PC
	comp_ppc_load_pc(mb->division_three_reg_opcode.exception_data.next_address, mb->division_three_reg_opcode.exception_data.next_location);

	//Clear N, Z, C and V flags
	//This is not defined in the processor documentation and there are side-effects,
	//but the interpretive emulation is doing this.
	comp_ppc_andi(PPCR_FLAGS_MAPPED, PPCR_FLAGS_MAPPED, 0xffff);

	//Check for division by zero: is divisor equal to zero?
	comp_ppc_mr(PPCR_SPECTMP_MAPPED, divisor_reg, TRUE);

	//Check for division by zero and trigger exception if needed
	helper_division_by_zero_check(signed_division, quotient_reg, &mb->division_three_reg_opcode.exception_data);

	//Execute division, quotient register is the input for dividend, temp register is the output for quotient
	//No need for overflow checking, it is not possible for 32/32 bit division.
	helper_divide_32_bit(signed_division, temp_reg, quotient_reg, divisor_reg, FALSE);

	//Calculate remainder: multiply the result with divisor
	comp_ppc_mullwo(PPCR_SPECTMP_MAPPED, temp_reg, divisor_reg, FALSE);

	//Then subtract from dividend
	comp_ppc_subf(remainder_reg, PPCR_SPECTMP_MAPPED, quotient_reg, FALSE);

	//Copy quotient to the output register
	comp_ppc_mr(quotient_reg, temp_reg, FALSE);
}

/**
 * Division of 32 bit register by 32 bit register, output is quotient (32 bit quotient register).
 * Flags are also set according to the inputs and result.
 * This macroblock can cause a division by zero exception.
 */
void comp_macroblock_push_division_32_32bit_no_remainder(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg quotient_reg, comp_ppc_reg divisor_reg, BOOL signed_division, uae_u32 next_address, uae_u32 next_location)
{
	comp_tmp_reg* tempreg = comp_allocate_temp_register(NULL, PPC_TMP_REG_NOTUSED_MAPPED);

	comp_mb_init(mb,
				comp_macroblock_impl_division_32_32bit_no_remainder,
				regsin | comp_exit_deps(),
				regsout | COMP_COMPILER_MACROBLOCK_REG_FLAGN | COMP_COMPILER_MACROBLOCK_REG_FLAGZ | COMP_COMPILER_MACROBLOCK_REG_FLAGC | COMP_COMPILER_MACROBLOCK_REG_FLAGV);

	//Two register descriptor is hijacked a bit: only the needed fields are populated *tsk... tsk...*
	mb->division_two_reg_opcode.output_reg = quotient_reg;
	mb->division_two_reg_opcode.divisor_reg = divisor_reg;
	mb->division_two_reg_opcode.signed_division = signed_division;

	//Get the list of mapped registers directly into the list array
	comp_get_changed_mapped_regs_list(mb->division_three_reg_opcode.exception_data.mapped_regs);

	 //Next instruction will be used for the return address
	mb->division_two_reg_opcode.exception_data.next_address = next_address;
	mb->division_two_reg_opcode.exception_data.next_location = next_location;

	comp_free_temp_register(tempreg);
}

void comp_macroblock_impl_division_32_32bit_no_remainder(union comp_compiler_mb_union* mb)
{
	//TODO: optimize register/flag compiling according to the calculated input/output for the macroblock

	comp_ppc_reg quotient_reg = mb->division_two_reg_opcode.output_reg;
	comp_ppc_reg divisor_reg = mb->division_two_reg_opcode.divisor_reg;
	BOOL signed_division = mb->division_two_reg_opcode.signed_division;

	//Load next address into PC
	comp_ppc_load_pc(mb->division_two_reg_opcode.exception_data.next_address, mb->division_two_reg_opcode.exception_data.next_location);

	//Clear N, Z, C and V flags
	//This is not defined in the processor documentation and there are side-effects,
	//but the interpretive emulation is doing this.
	comp_ppc_andi(PPCR_FLAGS_MAPPED, PPCR_FLAGS_MAPPED, 0xffff);

	//Check for division by zero: is divisor equal to zero?
	comp_ppc_mr(PPCR_SPECTMP_MAPPED, divisor_reg, TRUE);

	//Check for division by zero and trigger exception if needed
	helper_division_by_zero_check(signed_division, quotient_reg, &mb->division_two_reg_opcode.exception_data);

	//Execute division, quotient register is the input for dividend and the output for quotient also
	//No need for overflow checking, it is not possible for 32/32 bit division.
	helper_divide_32_bit(signed_division, quotient_reg, quotient_reg, divisor_reg, FALSE);
}

/**
 * Division of 64 bit register by 32 bit register, output is quotient (32 bit quotient register) and remainder (32 bit remainder register).
 * Flags are also set according to the inputs and result.
 * This macroblock can cause a division by zero exception.
 */
void comp_macroblock_push_division_64_32bit(uae_u64 regsin, uae_u64 regsout, int dividend_high_reg_num, int dividend_low_reg_num, comp_ppc_reg divisor_reg, BOOL signed_division, uae_u32 next_address, uae_u32 next_location)
{
	//TODO: get rid of the external function calling and the flushing of temporary registers for the 64 bit division
	//Flush temporary registers
	comp_flush_temp_registers(TRUE);

	//v18 (TODO 7.4): the C function reads and writes regs.regs[]
	comp_macroblock_push_fixregs_store();

	{
	comp_mb_init(mb,
				comp_macroblock_impl_division_64_32bit,
				regsin | comp_exit_deps(),
				regsout | COMP_COMPILER_MACROBLOCK_REG_FLAGN | COMP_COMPILER_MACROBLOCK_REG_FLAGZ | COMP_COMPILER_MACROBLOCK_REG_FLAGC | COMP_COMPILER_MACROBLOCK_REG_FLAGV);

	mb->division_64_bit_opcode.dividend_high_reg_num = dividend_high_reg_num;
	mb->division_64_bit_opcode.dividend_low_reg_num = dividend_low_reg_num;
	mb->division_64_bit_opcode.divisor_reg = divisor_reg;
	mb->division_64_bit_opcode.signed_division = signed_division;

	//The list of mapped registers will be empty, but we still need it anyway
	comp_get_changed_mapped_regs_list(mb->division_64_bit_opcode.exception_data.mapped_regs);

	 //Next instruction will be used for the return address
	mb->division_64_bit_opcode.exception_data.next_address = next_address;
	mb->division_64_bit_opcode.exception_data.next_location = next_location;
	}

	comp_macroblock_push_fixregs_load();
}

void comp_macroblock_impl_division_64_32bit(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg divisor_reg = mb->division_64_bit_opcode.divisor_reg;
	int dividend_high_reg_num = mb->division_64_bit_opcode.dividend_high_reg_num;
	int dividend_low_reg_num = mb->division_64_bit_opcode.dividend_low_reg_num;
	BOOL signed_division = mb->division_64_bit_opcode.signed_division;

	//Load next address into PC
	comp_ppc_load_pc(mb->division_64_bit_opcode.exception_data.next_address, mb->division_64_bit_opcode.exception_data.next_location);

	//Clear N, Z, C and V flags
	//This is not defined in the processor documentation and there are side-effects,
	//but the interpretive emulation is doing this.
	comp_ppc_andi(PPCR_FLAGS_MAPPED, PPCR_FLAGS_MAPPED, 0xffff);

	//Copy divisor into param #1 register and set the zero flag accordingly to the value
	comp_ppc_mr(PPCR_PARAM1_MAPPED, divisor_reg, TRUE);

	//Load dividend high part into param #2 register
	comp_ppc_lwz(PPCR_PARAM2_MAPPED, dividend_high_reg_num * 4, PPCR_REGS_BASE_MAPPED);

	//Check for division by zero and trigger exception if needed
	helper_division_by_zero_check(signed_division, PPCR_PARAM2_MAPPED, &mb->division_two_reg_opcode.exception_data);

	//Load dividend high 32 bit into param #2
	comp_ppc_li(PPCR_PARAM2_MAPPED, dividend_high_reg_num);

	//Load dividend low 32 bit into param #3
	comp_ppc_li(PPCR_PARAM3_MAPPED, dividend_low_reg_num);

	//Call external division function
	comp_ppc_call(PPCR_SPECTMP_MAPPED, signed_division ? (uae_uintptr)comp_signed_divide_64_bit : (uae_uintptr)comp_unsigned_divide_64_bit);

	//Returned: if non-zero then overflow has happened
	comp_ppc_mr(PPCR_PARAM1_MAPPED, PPCR_PARAM1_MAPPED, TRUE);

	//Skip setting V flag and leaving early
	comp_ppc_bc(PPC_B_CR_TMP0_EQ, 0);

	//Set V and N flags (again: N flag supposed to be undefined, but the interpretive sets it)
	comp_ppc_oris(PPCR_FLAGS_MAPPED, PPCR_FLAGS_MAPPED, (1 << (PPCR_FLAG_V - 16)) | (1 << (PPCR_FLAG_N - 16)));

	//Leave the instruction without any other changes
	comp_ppc_b(0, 1);

	//Continue
	comp_ppc_branch_target(0);

	//Load result to spec temp
	comp_ppc_lwz(PPCR_SPECTMP_MAPPED, dividend_low_reg_num * 4, PPCR_REGS_BASE_MAPPED);

	//Set N and Z flag according to the result
	comp_ppc_mr(PPCR_SPECTMP_MAPPED, PPCR_SPECTMP_MAPPED, TRUE);

	//Save N and Z flags
	comp_ppc_mfcr(PPCR_SPECTMP_MAPPED);	//v18: mfocrf only in PWR4 builds, mfcr works everywhere
	comp_ppc_rlwimi(PPCR_FLAGS_MAPPED, PPCR_SPECTMP_MAPPED, 0, 0, 2, FALSE);

	//End of the division instruction
	comp_ppc_branch_target(1);
}

/**
 * Helper for division instructions: checks the Z flag where it receives the status of the divisor and
 * triggers the division by zero exception if it was set.
 * Parameters:
 *    signed_division - if TRUE then the division is signed, otherwise unsigned
 *    dividend_reg - dividend in a mapped register
 *    exception_data - pointer to the exception data of the division instruction
 * Note: the internal Z flag must be initialized by the compiled code before calling this helper: set if
 * the divisor is zero, cleared otherwise.
 */
STATIC_INLINE void helper_division_by_zero_check(BOOL signed_division, comp_ppc_reg dividend_reg, comp_exception_data* exception_data)
{
	//If Z flag is set then skip exception if it is non-zero
	comp_ppc_bc(PPC_B_CR_TMP0_NE, 0);

	//According to the processor documentation N, Z and V flags are undefined at the exception,
	//but the interpretive sets these flags too, so I copied the steps.
	if (signed_division)
	{
		//Set Z and V flags
		comp_ppc_oris(PPCR_FLAGS_MAPPED, PPCR_FLAGS_MAPPED, (1 << (PPCR_FLAG_Z - 16)) | (1 << (PPCR_FLAG_V - 16)));
	} else {
		//Set V flag
		comp_ppc_oris(PPCR_FLAGS_MAPPED, PPCR_FLAGS_MAPPED, (1 << (PPCR_FLAG_V - 16)));

		//If unsigned operation then N flag is set when dividend is negative
		comp_ppc_andis(PPCR_SPECTMP_MAPPED, dividend_reg, 0x8000);
		comp_ppc_or(PPCR_FLAGS_MAPPED, PPCR_FLAGS_MAPPED, PPCR_SPECTMP_MAPPED, FALSE);
	}

	//Trigger exception (leaves the block)
	comp_ppc_exception(5, exception_data);

	//Continue after the exception code
	comp_ppc_branch_target(0);
}

/**
 * Execute division for two 32 bit registers
 * Parameters:
 *    signed_division - if TRUE then the division is signed, otherwise unsigned
 *    quotient_reg - mapped register for quotient
 *    dividend_reg - mapped register with the dividend
 *    divisor_reg - mapped register with the divisor
 *    check_overflow - if TRUE then the overflow checking will be compiled, otherwise skipped
 *  Note: if the overflow checking is set then the function will set up a branch to the end of the
 *  compiled instruction in case of an overflow happened. The caller must set up the branch target at
 *  the end of the division instruction using #1 as target ID.
 */
STATIC_INLINE void helper_divide_32_bit(BOOL signed_division, comp_ppc_reg quotient_reg, comp_ppc_reg dividend_reg, comp_ppc_reg divisor_reg, BOOL check_overflow)
{
	//Execute division
	if (signed_division)
	{
		comp_ppc_divwo(quotient_reg, dividend_reg, divisor_reg, TRUE);
	} else {
		comp_ppc_divwuo(quotient_reg, dividend_reg, divisor_reg, TRUE);
	}

	//Do we need to check for overflow?
	if (check_overflow)
	{
		//Check for overflow
		if (signed_division)
		{
			//Extend result to 32 bit
			comp_ppc_extsh(PPCR_SPECTMP_MAPPED, quotient_reg, FALSE);

			//If it is the same as the original then there is no overflow
			comp_ppc_cmplw(PPCR_CR_TMP1, PPCR_SPECTMP_MAPPED, quotient_reg);
		} else {
			//Extract higher 16 bits
			comp_ppc_rlwinm(PPCR_SPECTMP_MAPPED, quotient_reg, 16, 16, 31, FALSE);

			//Is it zero?
			comp_ppc_cmplwi(PPCR_CR_TMP1, PPCR_SPECTMP_MAPPED, 0);
		}

		//Skip setting V flag and leaving early
		comp_ppc_bc(PPC_B_CR_TMP1_EQ, 0);

		//Set V and N flags (again: N flag supposed to be undefined, but the interpretive sets it)
		comp_ppc_oris(PPCR_FLAGS_MAPPED, PPCR_FLAGS_MAPPED, (1 << (PPCR_FLAG_V - 16)) | (1 << (PPCR_FLAG_N - 16)));

		//Leave the instruction without any other changes
		comp_ppc_b(0, 1);

		//Continue
		comp_ppc_branch_target(0);
	}

	//Save N and Z flags
	comp_ppc_mfcr(PPCR_SPECTMP_MAPPED);	//v18: mfocrf only in PWR4 builds, mfcr works everywhere
	comp_ppc_rlwimi(PPCR_FLAGS_MAPPED, PPCR_SPECTMP_MAPPED, 0, 0, 2, FALSE);
}

/**
 * Summary of two decimal numbers stored in the lowest byte of the registers and the X flag (0 or 1).
 * The output is copied to the lowest byte of the destination register and C flag is returned
 * in the xflag_reg register (0 or 1).
 * Note: this macroblock uses the spec temp register.
 */
void comp_macroblock_push_add_decimal(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, comp_ppc_reg xflag_reg)
{
	//TODO: it is not ideal that parts of the carry flag handling is folded into the macroblock: cannot be optimized away
	comp_tmp_reg* temp_reg = comp_allocate_temp_register(NULL, PPC_TMP_REG_NOTUSED_MAPPED);

	comp_mb_init(mb,
				comp_macroblock_impl_add_decimal,
				regsin,
				regsout);
	mb->three_regs_opcode.input_reg1 = input_reg;
	mb->three_regs_opcode.input_reg2 = xflag_reg;
	mb->three_regs_opcode.output_reg = output_reg;
	mb->three_regs_opcode.temp_reg = temp_reg->mapped_reg_num;

	comp_free_temp_register(temp_reg);
}

void comp_macroblock_impl_add_decimal(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg input_reg = mb->three_regs_opcode.input_reg1;
	comp_ppc_reg xflag_reg = mb->three_regs_opcode.input_reg2;
	comp_ppc_reg output_reg = mb->three_regs_opcode.output_reg;
	comp_ppc_reg temp_reg = mb->three_regs_opcode.temp_reg;

	//Copy lower decimal digit from source to the spec temp
	comp_ppc_andi(PPCR_SPECTMP_MAPPED, input_reg, 0xf);

	//Copy lower decimal digit from output to the temp reg
	comp_ppc_andi(temp_reg, output_reg, 0xf);

	//Summarize the digits from the registers and the X flag
	comp_ppc_add(temp_reg, temp_reg, PPCR_SPECTMP_MAPPED, FALSE);
	comp_ppc_add(temp_reg, temp_reg, xflag_reg, FALSE);

	//Copy higher decimal digit from source to the provided temp
	comp_ppc_andi(xflag_reg, input_reg, 0xf0);

	//Is the summary result lower digit lower than 10?
	comp_ppc_cmplwi(PPCR_CR_TMP0, temp_reg, 10);

	//Then skip the carry over processing
	comp_ppc_bc(PPC_B_CR_TMP0_LT, 0);

	//Adjust decimal digit by adding 6
	comp_ppc_addi(temp_reg, temp_reg, 6);

	//Increment the higher decimal digit (carry over)
	comp_ppc_addi(xflag_reg, xflag_reg, 0x10);

	//Continue here when there was no carry over
	comp_ppc_branch_target(0);

	//Insert the lower decimal digit into the destination
	comp_ppc_rlwimi(output_reg, temp_reg, 0, 28, 31, FALSE);

	//Copy higher decimal digit from output to the temp reg
	comp_ppc_andi(temp_reg, output_reg, 0xf0);

	//Summarize higher decimal digits
	comp_ppc_add(temp_reg, temp_reg, xflag_reg, FALSE);

	//Clear carry flag (returned in xflag_reg)
	comp_ppc_li(xflag_reg, 0);

	//Is the summary result lower digit lower than 0xa0?
	comp_ppc_cmplwi(PPCR_CR_TMP0, temp_reg, 0xa0);

	//Then skip the carry over processing
	comp_ppc_bc(PPC_B_CR_TMP0_LT, 0);

	//Adjust decimal digit by adding 0x60
	comp_ppc_addi(temp_reg, temp_reg, 0x60);

	//Set carry flag (returned in xflag_reg)
	comp_ppc_li(xflag_reg, 1);

	//Continue here when there was no carry over
	comp_ppc_branch_target(0);

	//Insert the higher decimal digit into the destination
	comp_ppc_rlwimi(output_reg, temp_reg, 0, 24, 27, FALSE);
}

/**
 * Subtraction of two decimal numbers stored in the lowest byte of the registers and the X flag (0 or 1).
 * The output is copied to the lowest byte of the destination register and C flag is returned
 * in the xflag_reg register (0 or 1).
 * Note: this macroblock uses the spec temp register.
 */
void comp_macroblock_push_sub_decimal(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg input_reg, comp_ppc_reg xflag_reg)
{
	//TODO: it is not ideal that parts of the carry flag handling is folded into the macroblock: cannot be optimized away
	comp_tmp_reg* temp_reg = comp_allocate_temp_register(NULL, PPC_TMP_REG_NOTUSED_MAPPED);

	comp_mb_init(mb,
				comp_macroblock_impl_sub_decimal,
				regsin,
				regsout);
	mb->three_regs_opcode.input_reg1 = input_reg;
	mb->three_regs_opcode.input_reg2 = xflag_reg;
	mb->three_regs_opcode.output_reg = output_reg;
	mb->three_regs_opcode.temp_reg = temp_reg->mapped_reg_num;

	comp_free_temp_register(temp_reg);
}

void comp_macroblock_impl_sub_decimal(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg input_reg = mb->three_regs_opcode.input_reg1;
	comp_ppc_reg xflag_reg = mb->three_regs_opcode.input_reg2;
	comp_ppc_reg output_reg = mb->three_regs_opcode.output_reg;
	comp_ppc_reg temp_reg = mb->three_regs_opcode.temp_reg;

	//Copy lower decimal digit from source to the spec temp
	comp_ppc_andi(PPCR_SPECTMP_MAPPED, input_reg, 0xf);

	//Copy lower decimal digit from output to the temp reg
	comp_ppc_andi(temp_reg, output_reg, 0xf);

	//Subtract the source digits and the X flag from the destination register
	comp_ppc_subf(temp_reg, PPCR_SPECTMP_MAPPED, temp_reg, FALSE);
	comp_ppc_subf(temp_reg, xflag_reg, temp_reg, FALSE);

	//Copy higher decimal digit from source to the provided temp
	comp_ppc_andi(xflag_reg, input_reg, 0xf0);

	//Is the subtraction result lower digit lower than 10?
	comp_ppc_cmplwi(PPCR_CR_TMP0, temp_reg, 10);

	//Then skip the carry over processing
	comp_ppc_bc(PPC_B_CR_TMP0_LT, 0);

	//Adjust decimal digit by subtracting 6
	comp_ppc_addi(temp_reg, temp_reg, -6);

	//Increment the source higher decimal digit (carry over)
	comp_ppc_addi(xflag_reg, xflag_reg, 0x10);

	//Continue here when there was no carry over
	comp_ppc_branch_target(0);

	//Insert the lower decimal digit into the destination
	comp_ppc_rlwimi(output_reg, temp_reg, 0, 28, 31, FALSE);

	//Copy higher decimal digit from output to the temp reg
	comp_ppc_andi(temp_reg, output_reg, 0xf0);

	//Subtract higher decimal digits
	comp_ppc_subf(temp_reg, xflag_reg, temp_reg, FALSE);

	//Clear carry flag (returned in xflag_reg)
	comp_ppc_li(xflag_reg, 0);

	//Is the subtraction result lower digit lower than 0xa0?
	comp_ppc_cmplwi(PPCR_CR_TMP0, temp_reg, 0xa0);

	//Then skip the carry over processing
	comp_ppc_bc(PPC_B_CR_TMP0_LT, 0);

	//Adjust decimal digit by subtracting 0x60
	comp_ppc_addi(temp_reg, temp_reg, -0x60);

	//Set carry flag (returned in xflag_reg)
	comp_ppc_li(xflag_reg, 1);

	//Continue here when there was no carry over
	comp_ppc_branch_target(0);

	//Insert the higher decimal digit into the destination
	comp_ppc_rlwimi(output_reg, temp_reg, 0, 24, 27, FALSE);
}

/**
 * Subtraction of a decimal number stored in the lowest byte of the registers and the X flag (0 or 1) from zero.
 * The output is copied to the lowest byte of the destination register and C flag is returned
 * in the xflag_reg register (0 or 1).
 * Note: this macroblock uses the spec temp register.
 */
void comp_macroblock_push_negate_decimal(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg output_reg, comp_ppc_reg xflag_reg)
{
	//TODO: it is not ideal that parts of the carry flag handling is folded into the macroblock: cannot be optimized away
	comp_tmp_reg* temp_reg = comp_allocate_temp_register(NULL, PPC_TMP_REG_NOTUSED_MAPPED);

	comp_mb_init(mb,
				comp_macroblock_impl_negate_decimal,
				regsin,
				regsout);
	mb->three_regs_opcode.input_reg2 = xflag_reg;
	mb->three_regs_opcode.output_reg = output_reg;
	mb->three_regs_opcode.temp_reg = temp_reg->mapped_reg_num;

	comp_free_temp_register(temp_reg);
}

void comp_macroblock_impl_negate_decimal(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg xflag_reg = mb->three_regs_opcode.input_reg2;
	comp_ppc_reg output_reg = mb->three_regs_opcode.output_reg;
	comp_ppc_reg temp_reg = mb->three_regs_opcode.temp_reg;

	//Copy lower decimal digit from output to the temp reg
	comp_ppc_andi(temp_reg, output_reg, 0xf);

	//Subtract destination digit and the X flag from zero
	comp_ppc_add(temp_reg, xflag_reg, temp_reg, FALSE);
	comp_ppc_subfic(temp_reg, temp_reg, 0);

	//Copy higher decimal digit from destination to the provided temp
	comp_ppc_andi(xflag_reg, output_reg, 0xf0);

	//Is the subtraction result lower digit lower than 10?
	comp_ppc_cmplwi(PPCR_CR_TMP0, temp_reg, 10);

	//Then skip the carry over processing
	comp_ppc_bc(PPC_B_CR_TMP0_LT, 0);

	//Adjust decimal digit by subtracting 6
	comp_ppc_addi(temp_reg, temp_reg, -6);

	//Increment the destination higher decimal digit (carry over)
	comp_ppc_addi(xflag_reg, xflag_reg, 0x10);

	//Continue here when there was no carry over
	comp_ppc_branch_target(0);

	//Insert the lower decimal digit into the destination
	comp_ppc_rlwimi(output_reg, temp_reg, 0, 28, 31, FALSE);

	//Subtract higher decimal digits from zero
	comp_ppc_subfic(temp_reg, xflag_reg, 0);

	//Clear carry flag (returned in xflag_reg)
	comp_ppc_li(xflag_reg, 0);

	//Is the subtraction result lower digit lower than 0xa0?
	comp_ppc_cmplwi(PPCR_CR_TMP0, temp_reg, 0xa0);

	//Then skip the carry over processing
	comp_ppc_bc(PPC_B_CR_TMP0_LT, 0);

	//Adjust decimal digit by subtracting 0x60
	comp_ppc_addi(temp_reg, temp_reg, -0x60);

	//Set carry flag (returned in xflag_reg)
	comp_ppc_li(xflag_reg, 1);

	//Continue here when there was no carry over
	comp_ppc_branch_target(0);

	//Insert the higher decimal digit into the destination
	comp_ppc_rlwimi(output_reg, temp_reg, 0, 24, 27, FALSE);
}

/**
 * Macroblock: v18: ASL/ASR/LSL/LSR/ROL/ROR/ROXL/ROXR with the shift
 * count in a register, as the 68k does it: count = register modulo 64,
 * a count larger than the operand size gives 0 (LSx, ASL), the sign
 * (ASR) or a full rotation; C is the last bit shifted out (0 for a count
 * of 0, ROXx: X), X = C unless the count is 0 (ROL/ROR: X unchanged),
 * V for ASL: the most significant bit changed during the shift, else 0.
 * The flags are written directly into the flag register.
 * Uses r0, CR0 and CR1, CTR (ROXx).
 * Parameters:
 *    kind - COMP_SHIFT_* constant
 *    size - 1, 2 or 4
 *    dest_reg - data register (operand and result, only the low size bytes change)
 *    count_reg - register with the shift count (may be the same as dest_reg)
 *    t0-t3 - temporary registers, trashed
 */
void comp_macroblock_push_shift_count(uae_u64 regsin, uae_u64 regsout, int kind, int size, comp_ppc_reg dest_reg, comp_ppc_reg count_reg, comp_ppc_reg t0, comp_ppc_reg t1, comp_ppc_reg t2, comp_ppc_reg t3)
{
	comp_mb_init(mb,
				comp_macroblock_impl_shift_count,
				regsin | COMP_COMPILER_MACROBLOCK_REG_FLAGX,
				regsout | COMP_COMPILER_MACROBLOCK_REG_FLAGN | COMP_COMPILER_MACROBLOCK_REG_FLAGZ | COMP_COMPILER_MACROBLOCK_REG_FLAGV | COMP_COMPILER_MACROBLOCK_REG_FLAGC | COMP_COMPILER_MACROBLOCK_REG_FLAGX
				| COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ);
	mb->shift_count.dest_reg = dest_reg;
	mb->shift_count.count_reg = count_reg;
	mb->shift_count.temp_reg[0] = t0;
	mb->shift_count.temp_reg[1] = t1;
	mb->shift_count.temp_reg[2] = t2;
	mb->shift_count.temp_reg[3] = t3;
	mb->shift_count.kind = kind;
	mb->shift_count.size = size;
}

void comp_macroblock_impl_shift_count(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg d = mb->shift_count.dest_reg;
	comp_ppc_reg cnt = mb->shift_count.temp_reg[0];
	comp_ppc_reg v = mb->shift_count.temp_reg[1];
	comp_ppc_reg r = mb->shift_count.temp_reg[2];
	comp_ppc_reg c = mb->shift_count.temp_reg[3];
	comp_ppc_reg r0 = PPCR_SPECTMP_MAPPED;
	comp_ppc_reg f = PPCR_FLAGS_MAPPED;
	int kind = mb->shift_count.kind;
	int n = mb->shift_count.size * 8;
	int s = 32 - n;

	//Count modulo 64
	comp_ppc_andi(cnt, mb->shift_count.count_reg, 63);

	//Clear N, Z, V and C in the flag register (X stays)
	comp_ppc_liw(r0, ~((1U << PPCR_FLAG_N) | (1U << PPCR_FLAG_Z) | (1U << PPCR_FLAG_V) | (1U << PPCR_FLAG_C)));
	comp_ppc_and(f, f, r0, FALSE);

	switch (kind)
	{
	case COMP_SHIFT_ASL:
	case COMP_SHIFT_LSL:
		//v = operand in the top bits
		if (s) comp_ppc_rlwinm(v, d, s, 0, 31 - s, FALSE); else comp_ppc_mr(v, d, FALSE);
		//r = shifted (0 for a count >= 32, which covers count > size too)
		comp_ppc_slw(r, v, cnt, FALSE);
		//C = bit (32 - count) of v (0 for count 0 and count > size)
		comp_ppc_subfic(r0, cnt, 32);
		comp_ppc_srw(c, v, r0, FALSE);
		comp_ppc_andi(c, c, 1);
		if (kind == COMP_SHIFT_ASL)
		{
			//V: shifting back arithmetically must give the operand
			comp_ppc_sraw(r0, r, cnt, FALSE);
			comp_ppc_xor(r0, r0, v, FALSE);
			comp_ppc_cntlzw(r0, r0, FALSE);
			comp_ppc_rlwinm(r0, r0, 27, 31, 31, FALSE);	//1 = unchanged
			comp_ppc_xori(r0, r0, 1);
			comp_ppc_rlwimi(f, r0, PPCR_FLAG_V, 31 - PPCR_FLAG_V, 31 - PPCR_FLAG_V, FALSE);
		}
		//Result into the low bits of the data register
		if (s) comp_ppc_rlwimi(d, r, n, s, 31, FALSE); else comp_ppc_mr(d, r, FALSE);
		break;

	case COMP_SHIFT_LSR:
	case COMP_SHIFT_ASR:
		//v = operand zero or sign extended
		if (kind == COMP_SHIFT_LSR)
		{
			if (s) comp_ppc_rlwinm(v, d, 0, s, 31, FALSE); else comp_ppc_mr(v, d, FALSE);
			comp_ppc_srw(r, v, cnt, FALSE);
			//C = bit (count - 1); count 0: shift by 63 gives 0
			comp_ppc_addi(r0, cnt, (uae_u16) -1);
			comp_ppc_srw(c, v, r0, FALSE);
			comp_ppc_andi(c, c, 1);
		} else {
			if (n == 8) comp_ppc_extsb(v, d, FALSE);
			else if (n == 16) comp_ppc_extsh(v, d, FALSE);
			else comp_ppc_mr(v, d, FALSE);
			comp_ppc_sraw(r, v, cnt, FALSE);
			comp_ppc_addi(r0, cnt, (uae_u16) -1);
			comp_ppc_sraw(c, v, r0, FALSE);
			comp_ppc_andi(c, c, 1);
			//Count 0: C = 0 (the shift by 63 gave the sign)
			comp_ppc_cmplwi(PPCR_CR_TMP1, cnt, 0);
			comp_ppc_bc(PPC_B_CR_TMP1_NE, 0);
			comp_ppc_li(c, 0);
			comp_ppc_branch_target(0);
		}
		//Result into the low bits, then r = result in the top bits for N and Z
		if (s)
		{
			comp_ppc_rlwimi(d, r, 0, s, 31, FALSE);
			comp_ppc_rlwinm(r, r, s, 0, 31 - s, FALSE);
		} else {
			comp_ppc_mr(d, r, FALSE);
		}
		break;

	case COMP_SHIFT_ROL:
	case COMP_SHIFT_ROR:
		//v = operand repeated over the whole register, so a 32 bit
		//rotation by (count modulo 32) rotates every copy by (count modulo size)
		if (s)
		{
			comp_ppc_rlwinm(v, d, 0, s, 31, FALSE);
			if (n == 8) comp_ppc_rlwimi(v, v, 8, 16, 23, FALSE);
			comp_ppc_rlwimi(v, v, 16, 0, 15, FALSE);
		} else {
			comp_ppc_mr(v, d, FALSE);
		}
		if (kind == COMP_SHIFT_ROL)
		{
			comp_ppc_rlwnm(r, v, cnt, 0, 31, FALSE);
			//C = lowest bit of the result
			comp_ppc_rlwinm(c, r, 0, 31, 31, FALSE);
		} else {
			comp_ppc_neg(r0, cnt, FALSE);
			comp_ppc_rlwnm(r, v, r0, 0, 31, FALSE);
			//C = highest bit of the result
			comp_ppc_rlwinm(c, r, (33 - n) & 31, 31, 31, FALSE);
		}
		//Count 0: C = 0
		comp_ppc_cmplwi(PPCR_CR_TMP1, cnt, 0);
		comp_ppc_bc(PPC_B_CR_TMP1_NE, 0);
		comp_ppc_li(c, 0);
		comp_ppc_branch_target(0);
		if (s)
		{
			comp_ppc_rlwimi(d, r, 0, s, 31, FALSE);
			comp_ppc_rlwinm(r, r, s, 0, 31 - s, FALSE);
		} else {
			comp_ppc_mr(d, r, FALSE);
		}
		break;

	case COMP_SHIFT_ROXL:
	case COMP_SHIFT_ROXR:
		//v = operand in the top bits, c = X; one bit per loop step
		if (s) comp_ppc_rlwinm(v, d, s, 0, 31 - s, FALSE); else comp_ppc_mr(v, d, FALSE);
		comp_ppc_rlwinm(c, f, 32 - PPCR_FLAG_X, 31, 31, FALSE);
		comp_ppc_cmplwi(PPCR_CR_TMP1, cnt, 0);
		comp_ppc_bc(PPC_B_CR_TMP1_EQ, 0);
		comp_ppc_mtctr(cnt);
		comp_ppc_branch_target(1);
		if (kind == COMP_SHIFT_ROXL)
		{
			comp_ppc_rlwinm(r, v, 1, 31, 31, FALSE);		//bit shifted out
			comp_ppc_rlwinm(v, v, 1, 0, 30, FALSE);
			comp_ppc_rlwimi(v, c, s, 31 - s, 31 - s, FALSE);	//X in at the bottom
		} else {
			comp_ppc_rlwinm(r, v, (32 - s) & 31, 31, 31, FALSE);	//bit shifted out
			comp_ppc_rlwinm(v, v, 31, 1, 31 - s, FALSE);
			comp_ppc_rlwimi(v, c, 31, 0, 0, FALSE);			//X in at the top
		}
		comp_ppc_mr(c, r, FALSE);
		comp_ppc_bc(0x0200, 1);		//bdnz
		comp_ppc_branch_target(0);
		//C = X = last bit shifted out (count 0: C = X, X unchanged)
		comp_ppc_rlwimi(f, c, PPCR_FLAG_X, 31 - PPCR_FLAG_X, 31 - PPCR_FLAG_X, FALSE);
		if (s) comp_ppc_rlwimi(d, v, n, s, 31, FALSE); else comp_ppc_mr(d, v, FALSE);
		comp_ppc_mr(r, v, FALSE);
		break;
	}

	//N, Z from the result in the top bits, C
	comp_ppc_rlwimi(f, r, 0, 0, 0, FALSE);
	comp_ppc_cntlzw(r0, r, FALSE);
	comp_ppc_rlwimi(f, r0, PPCR_FLAG_Z - 5, 31 - PPCR_FLAG_Z, 31 - PPCR_FLAG_Z, FALSE);
	comp_ppc_rlwimi(f, c, PPCR_FLAG_C, 31 - PPCR_FLAG_C, 31 - PPCR_FLAG_C, FALSE);

	//X = C for the shifts if the count was not 0
	if (kind == COMP_SHIFT_ASL || kind == COMP_SHIFT_ASR || kind == COMP_SHIFT_LSL || kind == COMP_SHIFT_LSR)
	{
		comp_ppc_cmplwi(PPCR_CR_TMP1, cnt, 0);
		comp_ppc_bc(PPC_B_CR_TMP1_EQ, 2);
		comp_ppc_rlwimi(f, c, PPCR_FLAG_X, 31 - PPCR_FLAG_X, 31 - PPCR_FLAG_X, FALSE);
		comp_ppc_branch_target(2);
	}
}

/**
 * Macroblocks: v18: conditional write of the fifth byte of a bit field
 * in memory (see helper_bit_field_mem_save()).
 * mark: sets bit 31 of non-volatile register #1 if the mask register is not zero.
 * skip: branches to the skip target if bit 31 of non-volatile register #1 is clear.
 * Uses r0 and CR0. Branch reference 2 (not used by the memory access in between).
 */
void comp_macroblock_push_bitfield_mark_low_byte(uae_u64 regsin, uae_u64 regsout, comp_ppc_reg mask_reg)
{
	comp_mb_init(mb,
				comp_macroblock_impl_bitfield_mark_low_byte,
				regsin, regsout);
	mb->one_reg_opcode.reg = mask_reg;
}

void comp_macroblock_impl_bitfield_mark_low_byte(union comp_compiler_mb_union* mb)
{
	comp_ppc_neg(PPCR_SPECTMP_MAPPED, mb->one_reg_opcode.reg, FALSE);
	comp_ppc_or(PPCR_SPECTMP_MAPPED, PPCR_SPECTMP_MAPPED, mb->one_reg_opcode.reg, FALSE);
	comp_ppc_rlwimi(PPCR_TMP_NONVOL1_MAPPED, PPCR_SPECTMP_MAPPED, 0, 0, 0, FALSE);
}

void comp_macroblock_push_bitfield_skip_low_byte(uae_u64 regsin)
{
	comp_mb_init(mb,
				comp_macroblock_impl_bitfield_skip_low_byte,
				regsin, COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM);
}

void comp_macroblock_impl_bitfield_skip_low_byte(union comp_compiler_mb_union* mb)
{
	comp_ppc_andis(PPCR_SPECTMP_MAPPED, PPCR_TMP_NONVOL1_MAPPED, 0x8000);
	comp_ppc_bc(PPC_B_CR_TMP0_EQ, 2);
}

void comp_macroblock_push_bitfield_skip_target(void)
{
	comp_mb_init(mb,
				comp_macroblock_impl_bitfield_skip_target,
				COMP_COMPILER_MACROBLOCK_REG_NONE, COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM);
}

void comp_macroblock_impl_bitfield_skip_target(union comp_compiler_mb_union* mb)
{
	comp_ppc_branch_target(2);
}

/**
 * Macroblock: v18: MULU.L/MULS.L as newcpu.c m68k_mull():
 * 32 bit result: Dl = low 32 bits, N/Z from them, V = result does not fit
 * into 32 bits; 64 bit result: Dh:Dl, N from bit 63, Z from all 64 bits,
 * V = 0; C = 0. The flags are written directly into the flag register.
 * Uses r0.
 * Parameters:
 *    high_last - 64 bit, Dh == Dl: TRUE writes the high word last (68020/030)
 */
void comp_macroblock_push_mull(uae_u64 regsin, uae_u64 regsout, BOOL is_signed, BOOL is_64, BOOL high_last, comp_ppc_reg src_reg, comp_ppc_reg dl_reg, comp_ppc_reg dh_reg, comp_ppc_reg t0, comp_ppc_reg t1)
{
	comp_mb_init(mb,
				comp_macroblock_impl_mull,
				regsin,
				regsout | COMP_COMPILER_MACROBLOCK_REG_FLAGN | COMP_COMPILER_MACROBLOCK_REG_FLAGZ | COMP_COMPILER_MACROBLOCK_REG_FLAGV | COMP_COMPILER_MACROBLOCK_REG_FLAGC);
	mb->mull.src_reg = src_reg;
	mb->mull.dl_reg = dl_reg;
	mb->mull.dh_reg = dh_reg;
	mb->mull.t0 = t0;
	mb->mull.t1 = t1;
	mb->mull.is_signed = is_signed;
	mb->mull.is_64 = is_64;
	mb->mull.high_last = high_last;
}

void comp_macroblock_impl_mull(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg lo = mb->mull.t0;
	comp_ppc_reg hi = mb->mull.t1;
	comp_ppc_reg r0 = PPCR_SPECTMP_MAPPED;
	comp_ppc_reg f = PPCR_FLAGS_MAPPED;

	comp_ppc_mullw(lo, mb->mull.dl_reg, mb->mull.src_reg, FALSE);
	if (mb->mull.is_signed)
		comp_ppc_mulhw(hi, mb->mull.dl_reg, mb->mull.src_reg, FALSE);
	else
		comp_ppc_mulhwu(hi, mb->mull.dl_reg, mb->mull.src_reg, FALSE);

	//Clear N, Z, V and C (X stays)
	comp_ppc_liw(r0, ~((1U << PPCR_FLAG_N) | (1U << PPCR_FLAG_Z) | (1U << PPCR_FLAG_V) | (1U << PPCR_FLAG_C)));
	comp_ppc_and(f, f, r0, FALSE);

	if (mb->mull.is_64)
	{
		comp_ppc_rlwimi(f, hi, 0, 0, 0, FALSE);
		comp_ppc_or(r0, lo, hi, FALSE);
		comp_ppc_cntlzw(r0, r0, FALSE);
		comp_ppc_rlwimi(f, r0, PPCR_FLAG_Z - 5, 31 - PPCR_FLAG_Z, 31 - PPCR_FLAG_Z, FALSE);
		if (mb->mull.high_last)
		{
			comp_ppc_mr(mb->mull.dl_reg, lo, FALSE);
			comp_ppc_mr(mb->mull.dh_reg, hi, FALSE);
		} else {
			comp_ppc_mr(mb->mull.dh_reg, hi, FALSE);
			comp_ppc_mr(mb->mull.dl_reg, lo, FALSE);
		}
	} else {
		comp_ppc_rlwimi(f, lo, 0, 0, 0, FALSE);
		comp_ppc_cntlzw(r0, lo, FALSE);
		comp_ppc_rlwimi(f, r0, PPCR_FLAG_Z - 5, 31 - PPCR_FLAG_Z, 31 - PPCR_FLAG_Z, FALSE);
		//V: high word not the extension of the low word
		if (mb->mull.is_signed)
		{
			comp_ppc_srawi(r0, lo, 31, FALSE);
			comp_ppc_xor(r0, r0, hi, FALSE);
		} else {
			comp_ppc_mr(r0, hi, FALSE);
		}
		comp_ppc_cntlzw(r0, r0, FALSE);
		comp_ppc_rlwinm(r0, r0, 27, 31, 31, FALSE);
		comp_ppc_xori(r0, r0, 1);
		comp_ppc_rlwimi(f, r0, PPCR_FLAG_V, 31 - PPCR_FLAG_V, 31 - PPCR_FLAG_V, FALSE);
		comp_ppc_mr(mb->mull.dl_reg, lo, FALSE);
	}
}

/**
 * Macroblock: v18: ABCD/SBCD/NBCD on the low bytes of two registers,
 * the same calculation as the interpreter (gencpu.c): result into the low
 * byte of dest_reg, C = X = decimal carry/borrow, Z cleared if the result
 * is not zero (unchanged otherwise), N = bit 7, V as the interpreter
 * (NBCD: V unchanged). The flags are written directly into the flag
 * register. Uses r0, CR0 and CR1.
 * Parameters:
 *    kind - COMP_BCD_* (NBCD: src_reg is not used, dest_reg is the operand)
 *    t0-t4 - temporary registers, trashed
 */
void comp_macroblock_push_bcd(uae_u64 regsin, uae_u64 regsout, int kind, comp_ppc_reg src_reg, comp_ppc_reg dest_reg, comp_ppc_reg t0, comp_ppc_reg t1, comp_ppc_reg t2, comp_ppc_reg t3, comp_ppc_reg t4)
{
	comp_mb_init(mb,
				comp_macroblock_impl_bcd,
				regsin | COMP_COMPILER_MACROBLOCK_REG_FLAGX | COMP_COMPILER_MACROBLOCK_REG_FLAGZ | (kind == COMP_BCD_NBCD ? COMP_COMPILER_MACROBLOCK_REG_FLAGV : 0),
				regsout | COMP_COMPILER_MACROBLOCK_REG_FLAGN | COMP_COMPILER_MACROBLOCK_REG_FLAGZ | COMP_COMPILER_MACROBLOCK_REG_FLAGV | COMP_COMPILER_MACROBLOCK_REG_FLAGC | COMP_COMPILER_MACROBLOCK_REG_FLAGX
				| COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ);
	mb->bcd.src_reg = src_reg;
	mb->bcd.dest_reg = dest_reg;
	mb->bcd.t[0] = t0;
	mb->bcd.t[1] = t1;
	mb->bcd.t[2] = t2;
	mb->bcd.t[3] = t3;
	mb->bcd.t[4] = t4;
	mb->bcd.kind = kind;
}

#define PPC_B_CR_TMP1_LE	0x0085	//branch if CR1 GT is clear

void comp_macroblock_impl_bcd(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg s = mb->bcd.src_reg;
	comp_ppc_reg d = mb->bcd.dest_reg;
	comp_ppc_reg lo = mb->bcd.t[0];		//low nibble sum, later C
	comp_ppc_reg tmp = mb->bcd.t[1];	//tmp_newv, later V
	comp_ppc_reg nv = mb->bcd.t[2];		//newv
	comp_ppc_reg x = mb->bcd.t[3];		//X
	comp_ppc_reg aux = mb->bcd.t[4];
	comp_ppc_reg r0 = PPCR_SPECTMP_MAPPED;
	comp_ppc_reg f = PPCR_FLAGS_MAPPED;
	int kind = mb->bcd.kind;

	comp_ppc_rlwinm(x, f, 32 - PPCR_FLAG_X, 31, 31, FALSE);

	switch (kind)
	{
	case COMP_BCD_ABCD:
		//lo = (s & 0xf) + (d & 0xf) + X, tmp = (s & 0xf0) + (d & 0xf0) + lo
		comp_ppc_rlwinm(lo, s, 0, 28, 31, FALSE);
		comp_ppc_rlwinm(r0, d, 0, 28, 31, FALSE);
		comp_ppc_add(lo, lo, r0, FALSE);
		comp_ppc_add(lo, lo, x, FALSE);
		comp_ppc_rlwinm(tmp, s, 0, 24, 27, FALSE);
		comp_ppc_rlwinm(r0, d, 0, 24, 27, FALSE);
		comp_ppc_add(tmp, tmp, r0, FALSE);
		comp_ppc_add(tmp, tmp, lo, FALSE);
		comp_ppc_mr(nv, tmp, FALSE);
		//if (lo > 9) newv += 6
		comp_ppc_cmplwi(PPCR_CR_TMP1, lo, 9);
		comp_ppc_bc(PPC_B_CR_TMP1_LE, 0);
		comp_ppc_addi(nv, nv, 6);
		comp_ppc_branch_target(0);
		//c = (newv & 0x3f0) > 0x90; if (c) newv += 0x60
		comp_ppc_rlwinm(r0, nv, 0, 22, 27, FALSE);
		comp_ppc_li(lo, 0);
		comp_ppc_cmplwi(PPCR_CR_TMP1, r0, 0x90);
		comp_ppc_bc(PPC_B_CR_TMP1_LE, 1);
		comp_ppc_addi(nv, nv, 0x60);
		comp_ppc_li(lo, 1);
		comp_ppc_branch_target(1);
		//V = (tmp & 0x80) == 0 && (newv & 0x80) != 0
		comp_ppc_andc(tmp, nv, tmp, FALSE);
		comp_ppc_rlwinm(tmp, tmp, 25, 31, 31, FALSE);
		break;

	case COMP_BCD_SBCD:
		//lo = (d & 0xf) - (s & 0xf) - X, tmp = (d & 0xf0) - (s & 0xf0) + lo
		comp_ppc_rlwinm(lo, d, 0, 28, 31, FALSE);
		comp_ppc_rlwinm(r0, s, 0, 28, 31, FALSE);
		comp_ppc_subf(lo, r0, lo, FALSE);
		comp_ppc_subf(lo, x, lo, FALSE);
		comp_ppc_rlwinm(tmp, d, 0, 24, 27, FALSE);
		comp_ppc_rlwinm(r0, s, 0, 24, 27, FALSE);
		comp_ppc_subf(tmp, r0, tmp, FALSE);
		comp_ppc_add(tmp, tmp, lo, FALSE);
		comp_ppc_mr(nv, tmp, FALSE);
		//bcd = (lo & 0xf0) ? 6 : 0; newv -= bcd (lo becomes bcd)
		comp_ppc_andi(r0, lo, 0xf0);
		comp_ppc_li(lo, 0);
		comp_ppc_bc(PPC_B_CR_TMP0_EQ, 0);
		comp_ppc_li(lo, 6);
		comp_ppc_addi(nv, nv, (uae_u16) -6);
		comp_ppc_branch_target(0);
		//r0 = (d & 0xff) - (s & 0xff) - X; if (r0 & 0x100) newv -= 0x60
		comp_ppc_rlwinm(aux, s, 0, 24, 31, FALSE);
		comp_ppc_rlwinm(r0, d, 0, 24, 31, FALSE);
		comp_ppc_subf(r0, aux, r0, FALSE);
		comp_ppc_subf(r0, x, r0, FALSE);
		comp_ppc_andi(aux, r0, 0x100);
		comp_ppc_bc(PPC_B_CR_TMP0_EQ, 1);
		comp_ppc_addi(nv, nv, (uae_u16) -0x60);
		comp_ppc_branch_target(1);
		//c = ((d & 0xff) - (s & 0xff) - bcd - X) & 0x300
		comp_ppc_subf(r0, lo, r0, FALSE);
		comp_ppc_andi(r0, r0, 0x300);
		comp_ppc_li(lo, 0);
		comp_ppc_bc(PPC_B_CR_TMP0_EQ, 2);
		comp_ppc_li(lo, 1);
		comp_ppc_branch_target(2);
		//V = (tmp & 0x80) != 0 && (newv & 0x80) == 0
		comp_ppc_andc(tmp, tmp, nv, FALSE);
		comp_ppc_rlwinm(tmp, tmp, 25, 31, 31, FALSE);
		break;

	case COMP_BCD_NBCD:
		//lo = -(d & 0xf) - X, hi = -(d & 0xf0)
		comp_ppc_rlwinm(lo, d, 0, 28, 31, FALSE);
		comp_ppc_add(lo, lo, x, FALSE);
		comp_ppc_neg(lo, lo, FALSE);
		comp_ppc_rlwinm(tmp, d, 0, 24, 27, FALSE);
		comp_ppc_neg(tmp, tmp, FALSE);
		//lo > 9 (as unsigned 16 bit) is lo != 0 here: lo -= 6
		comp_ppc_cmplwi(PPCR_CR_TMP1, lo, 0);
		comp_ppc_bc(PPC_B_CR_TMP1_EQ, 0);
		comp_ppc_addi(lo, lo, (uae_u16) -6);
		comp_ppc_branch_target(0);
		//newv = hi + lo; c = (newv & 0x1f0) > 0x90; if (c) newv -= 0x60
		comp_ppc_add(nv, tmp, lo, FALSE);
		comp_ppc_rlwinm(r0, nv, 0, 23, 27, FALSE);
		comp_ppc_li(lo, 0);
		comp_ppc_cmplwi(PPCR_CR_TMP1, r0, 0x90);
		comp_ppc_bc(PPC_B_CR_TMP1_LE, 1);
		comp_ppc_addi(nv, nv, (uae_u16) -0x60);
		comp_ppc_li(lo, 1);
		comp_ppc_branch_target(1);
		break;
	}

	//Clear N, C (and V except NBCD)
	comp_ppc_liw(r0, ~((1U << PPCR_FLAG_N) | (1U << PPCR_FLAG_C) | (kind == COMP_BCD_NBCD ? 0 : (1U << PPCR_FLAG_V))));
	comp_ppc_and(f, f, r0, FALSE);
	//N = bit 7 of the result
	comp_ppc_rlwimi(f, nv, 24, 0, 0, FALSE);
	//Z: cleared if the result is not zero
	comp_ppc_andi(r0, nv, 0xff);
	comp_ppc_bc(PPC_B_CR_TMP0_EQ, 0);
	comp_ppc_rlwinm(f, f, 0, 32 - PPCR_FLAG_Z, 30 - PPCR_FLAG_Z, FALSE);
	comp_ppc_branch_target(0);
	if (kind != COMP_BCD_NBCD)
		comp_ppc_rlwimi(f, tmp, PPCR_FLAG_V, 31 - PPCR_FLAG_V, 31 - PPCR_FLAG_V, FALSE);
	comp_ppc_rlwimi(f, lo, PPCR_FLAG_C, 31 - PPCR_FLAG_C, 31 - PPCR_FLAG_C, FALSE);
	comp_ppc_rlwimi(f, lo, PPCR_FLAG_X, 31 - PPCR_FLAG_X, 31 - PPCR_FLAG_X, FALSE);
	//Result into the low byte
	comp_ppc_rlwimi(d, nv, 0, 24, 31, FALSE);
}

/**
 * Macroblock: v18: BFTST/BFEXTU/BFEXTS/BFFFO/BFCHG/BFCLR/BFSET/BFINS on a
 * data register as the interpreter does it: offset modulo 32, width 1-32,
 * the field wraps around from bit 0 to bit 31. N/Z from the field (BFINS:
 * from the inserted value), V = C = 0, written directly into the flag
 * register. Uses r0, CR1.
 * Parameters:
 *    d - the data register with the field
 *    r - BFINS: source register; BFEXTx/BFFFO: destination register
 *    off, wid - registers with the offset and the width (as given)
 *    t0-t3 - temporary registers, trashed
 */
void comp_macroblock_push_bitfield_reg(uae_u64 regsin, uae_u64 regsout, int kind, comp_ppc_reg d, comp_ppc_reg r, comp_ppc_reg off, comp_ppc_reg wid, comp_ppc_reg t0, comp_ppc_reg t1, comp_ppc_reg t2, comp_ppc_reg t3)
{
	comp_mb_init(mb,
				comp_macroblock_impl_bitfield_reg,
				regsin,
				regsout | COMP_COMPILER_MACROBLOCK_REG_FLAGN | COMP_COMPILER_MACROBLOCK_REG_FLAGZ | COMP_COMPILER_MACROBLOCK_REG_FLAGV | COMP_COMPILER_MACROBLOCK_REG_FLAGC
				| COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ);
	mb->bitfield_reg.d = d;
	mb->bitfield_reg.r = r;
	mb->bitfield_reg.off = off;
	mb->bitfield_reg.wid = wid;
	mb->bitfield_reg.t[0] = t0;
	mb->bitfield_reg.t[1] = t1;
	mb->bitfield_reg.t[2] = t2;
	mb->bitfield_reg.t[3] = t3;
	mb->bitfield_reg.kind = kind;
}

void comp_macroblock_impl_bitfield_reg(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg d = mb->bitfield_reg.d;
	comp_ppc_reg r = mb->bitfield_reg.r;
	comp_ppc_reg o = mb->bitfield_reg.t[0];
	comp_ppc_reg w = mb->bitfield_reg.t[1];
	comp_ppc_reg rot = mb->bitfield_reg.t[2];
	comp_ppc_reg mask = mb->bitfield_reg.t[3];
	comp_ppc_reg r0 = PPCR_SPECTMP_MAPPED;
	comp_ppc_reg f = PPCR_FLAGS_MAPPED;
	int kind = mb->bitfield_reg.kind;

	//o = offset & 31, w = ((width - 1) & 31) + 1
	comp_ppc_andi(o, mb->bitfield_reg.off, 31);
	comp_ppc_addi(w, mb->bitfield_reg.wid, (uae_u16) -1);
	comp_ppc_andi(w, w, 31);
	comp_ppc_addi(w, w, 1);
	//rot = field at the top, mask = top w bits
	comp_ppc_rlwnm(rot, d, o, 0, 31, FALSE);
	comp_ppc_subfic(r0, w, 32);
	comp_ppc_li(mask, (uae_u16) -1);
	comp_ppc_slw(mask, mask, r0, FALSE);

	if (kind == COMP_BF_INS)
	{
		//r0 = inserted value at the top
		comp_ppc_slw(r0, r, r0, FALSE);
		comp_ppc_andc(rot, rot, mask, FALSE);
		comp_ppc_or(rot, rot, r0, FALSE);
	} else {
		comp_ppc_and(r0, rot, mask, FALSE);
	}

	//Flags from r0: N, Z; V = C = 0
	comp_ppc_rlwinm(f, f, 0, 32 - PPCR_FLAG_C, 30 - PPCR_FLAG_C, FALSE);	//clear C
	comp_ppc_rlwinm(f, f, 0, 32 - PPCR_FLAG_V, 30 - PPCR_FLAG_V, FALSE);	//clear V
	comp_ppc_rlwimi(f, r0, 0, 0, 0, FALSE);
	comp_ppc_cmplwi(PPCR_CR_TMP1, r0, 0);
	comp_ppc_rlwinm(f, f, 0, 32 - PPCR_FLAG_Z, 30 - PPCR_FLAG_Z, FALSE);	//clear Z
	comp_ppc_bc(PPC_B_CR_TMP1_NE, 0);
	comp_ppc_oris(f, f, (1 << PPCR_FLAG_Z) >> 16);
	comp_ppc_branch_target(0);

	switch (kind)
	{
	case COMP_BF_EXTU:
		comp_ppc_subfic(o, w, 32);
		comp_ppc_srw(r, r0, o, FALSE);
		break;
	case COMP_BF_EXTS:
		comp_ppc_subfic(o, w, 32);
		comp_ppc_sraw(r, r0, o, FALSE);
		break;
	case COMP_BF_FFO:
		//result = offset (as given) + leading zeros of the field (max. width)
		comp_ppc_cntlzw(r0, r0, FALSE);
		comp_ppc_cmplw(PPCR_CR_TMP1, r0, w);
		comp_ppc_bc(PPC_B_CR_TMP1_LE, 1);
		comp_ppc_mr(r0, w, FALSE);
		comp_ppc_branch_target(1);
		comp_ppc_add(r, mb->bitfield_reg.off, r0, FALSE);
		break;
	case COMP_BF_CHG:
		comp_ppc_xor(rot, rot, mask, FALSE);
		break;
	case COMP_BF_CLR:
		comp_ppc_andc(rot, rot, mask, FALSE);
		break;
	case COMP_BF_SET:
		comp_ppc_or(rot, rot, mask, FALSE);
		break;
	}

	//Rotate back into the data register
	if (kind == COMP_BF_CHG || kind == COMP_BF_CLR || kind == COMP_BF_SET || kind == COMP_BF_INS)
	{
		comp_ppc_neg(r0, o, FALSE);
		comp_ppc_rlwnm(d, rot, r0, 0, 31, FALSE);
	}
}

/**
 * Macroblock: v18: bit field instructions in memory as the interpreter
 * does it. hi = long at (address + offset/8), lo = byte behind it; the
 * field starts at bit (offset & 7) of hi. Flags as
 * comp_macroblock_push_bitfield_reg(). BFCHG/BFCLR/BFSET/BFINS: hi gets
 * the new long, lo the new byte in bits 0-7 and in bit 31 whether the byte
 * has to be written (the field reaches into it).
 * Parameters:
 *    r - BFINS: source register; BFEXTx/BFFFO: destination register
 *    off, wid - registers with the offset (full value) and the width
 */
void comp_macroblock_push_bitfield_mem(uae_u64 regsin, uae_u64 regsout, int kind, comp_ppc_reg hi, comp_ppc_reg lo, comp_ppc_reg r, comp_ppc_reg off, comp_ppc_reg wid, comp_ppc_reg t0, comp_ppc_reg t1, comp_ppc_reg t2, comp_ppc_reg t3)
{
	comp_mb_init(mb,
				comp_macroblock_impl_bitfield_mem,
				regsin,
				regsout | COMP_COMPILER_MACROBLOCK_REG_FLAGN | COMP_COMPILER_MACROBLOCK_REG_FLAGZ | COMP_COMPILER_MACROBLOCK_REG_FLAGV | COMP_COMPILER_MACROBLOCK_REG_FLAGC
				| COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGN | COMP_COMPILER_MACROBLOCK_INTERNAL_FLAGZ);
	mb->bitfield_reg.d = hi;
	mb->bitfield_reg.lo = lo;
	mb->bitfield_reg.r = r;
	mb->bitfield_reg.off = off;
	mb->bitfield_reg.wid = wid;
	mb->bitfield_reg.t[0] = t0;
	mb->bitfield_reg.t[1] = t1;
	mb->bitfield_reg.t[2] = t2;
	mb->bitfield_reg.t[3] = t3;
	mb->bitfield_reg.kind = kind;
}

void comp_macroblock_impl_bitfield_mem(union comp_compiler_mb_union* mb)
{
	comp_ppc_reg hi = mb->bitfield_reg.d;
	comp_ppc_reg lo = mb->bitfield_reg.lo;
	comp_ppc_reg r = mb->bitfield_reg.r;
	comp_ppc_reg bo = mb->bitfield_reg.t[0];
	comp_ppc_reg w = mb->bitfield_reg.t[1];
	comp_ppc_reg fld = mb->bitfield_reg.t[2];
	comp_ppc_reg mask = mb->bitfield_reg.t[3];
	comp_ppc_reg r0 = PPCR_SPECTMP_MAPPED;
	comp_ppc_reg f = PPCR_FLAGS_MAPPED;
	int kind = mb->bitfield_reg.kind;

	//bo = offset & 7, w = ((width - 1) & 31) + 1
	comp_ppc_andi(bo, mb->bitfield_reg.off, 7);
	comp_ppc_addi(w, mb->bitfield_reg.wid, (uae_u16) -1);
	comp_ppc_andi(w, w, 31);
	comp_ppc_addi(w, w, 1);
	//fld = 32 bits starting at the field: (hi << bo) | (lo >> (8 - bo))
	comp_ppc_andi(lo, lo, 0xff);
	comp_ppc_slw(fld, hi, bo, FALSE);
	comp_ppc_subfic(r0, bo, 8);
	comp_ppc_srw(r0, lo, r0, FALSE);
	comp_ppc_or(fld, fld, r0, FALSE);
	//mask = top w bits
	comp_ppc_subfic(r0, w, 32);
	comp_ppc_li(mask, (uae_u16) -1);
	comp_ppc_slw(mask, mask, r0, FALSE);

	if (kind == COMP_BF_INS)
	{
		comp_ppc_slw(r0, r, r0, FALSE);
		comp_ppc_andc(fld, fld, mask, FALSE);
		comp_ppc_or(fld, fld, r0, FALSE);
	} else {
		comp_ppc_and(r0, fld, mask, FALSE);
	}

	//Flags from r0: N, Z; V = C = 0
	comp_ppc_rlwinm(f, f, 0, 32 - PPCR_FLAG_C, 30 - PPCR_FLAG_C, FALSE);
	comp_ppc_rlwinm(f, f, 0, 32 - PPCR_FLAG_V, 30 - PPCR_FLAG_V, FALSE);
	comp_ppc_rlwimi(f, r0, 0, 0, 0, FALSE);
	comp_ppc_cmplwi(PPCR_CR_TMP1, r0, 0);
	comp_ppc_rlwinm(f, f, 0, 32 - PPCR_FLAG_Z, 30 - PPCR_FLAG_Z, FALSE);
	comp_ppc_bc(PPC_B_CR_TMP1_NE, 0);
	comp_ppc_oris(f, f, (1 << PPCR_FLAG_Z) >> 16);
	comp_ppc_branch_target(0);

	switch (kind)
	{
	case COMP_BF_EXTU:
		comp_ppc_subfic(bo, w, 32);
		comp_ppc_srw(r, r0, bo, FALSE);
		return;
	case COMP_BF_EXTS:
		comp_ppc_subfic(bo, w, 32);
		comp_ppc_sraw(r, r0, bo, FALSE);
		return;
	case COMP_BF_FFO:
		comp_ppc_cntlzw(r0, r0, FALSE);
		comp_ppc_cmplw(PPCR_CR_TMP1, r0, w);
		comp_ppc_bc(PPC_B_CR_TMP1_LE, 1);
		comp_ppc_mr(r0, w, FALSE);
		comp_ppc_branch_target(1);
		comp_ppc_add(r, mb->bitfield_reg.off, r0, FALSE);
		return;
	case COMP_BF_TST:
		return;
	case COMP_BF_CHG:
		comp_ppc_xor(fld, fld, mask, FALSE);
		break;
	case COMP_BF_CLR:
		comp_ppc_andc(fld, fld, mask, FALSE);
		break;
	case COMP_BF_SET:
		comp_ppc_or(fld, fld, mask, FALSE);
		break;
	}

	//hi = (hi & ~(mask >> bo)) | ((fld >> bo) & (mask >> bo))
	comp_ppc_srw(r0, mask, bo, FALSE);
	comp_ppc_srw(w, fld, bo, FALSE);
	comp_ppc_and(w, w, r0, FALSE);
	comp_ppc_andc(hi, hi, r0, FALSE);
	comp_ppc_or(hi, hi, w, FALSE);
	//lo mask = (mask << (8 - bo)) & 0xff, lo = (lo & ~lomask) | ((fld << (8 - bo)) & lomask)
	comp_ppc_subfic(bo, bo, 8);
	comp_ppc_slw(r0, mask, bo, FALSE);
	comp_ppc_andi(r0, r0, 0xff);
	comp_ppc_slw(w, fld, bo, FALSE);
	comp_ppc_and(w, w, r0, FALSE);
	comp_ppc_andc(lo, lo, r0, FALSE);
	comp_ppc_or(lo, lo, w, FALSE);
	//bit 31 of lo: the byte has to be written
	comp_ppc_neg(w, r0, FALSE);
	comp_ppc_or(w, w, r0, FALSE);
	comp_ppc_rlwimi(lo, w, 0, 0, 0, FALSE);
}

/** ------------------------------------------------------------------------------
 * v18 (TODO 7.3): FPU instructions
 *
 * The FPU registers stay in regs.fp[] (doubles, as in the interpreter).
 * A translated instruction loads the source operand into f1, works with f0,
 * f1, f2 (volatile, nothing lives in them between the macroblocks of
 * different instructions) and writes the result to regs.fp[] and
 * regs.fp_result (the condition codes of the interpreter are derived from
 * it). Memory operands go through regs.jit_fpu_tmp, integer conversions
 * through regs.jit_fpu_conv. Rarely used operations and the conversion
 * to integer call the interpreter functions fpp_jit_arith() and
 * fpp_jit_toint(), so every result is the same as in the interpreter.
 * Only translated when the host has a classic FPU (not the e500 of the
 * A1222), see comp_fpu_compilable().
 */
#ifdef FPUEMU

#define COMP_FPU_OFF_FP(n)	(COMP_GET_OFFSET_IN_REGS(fp[0]) + (n) * 8)
#define COMP_FPU_OFF_RESULT	COMP_GET_OFFSET_IN_REGS(fp_result)
#define COMP_FPU_OFF_FPSR	COMP_GET_OFFSET_IN_REGS(fpsr)
#define COMP_FPU_OFF_TMP	COMP_GET_OFFSET_IN_REGS(jit_fpu_tmp)
#define COMP_FPU_OFF_CONV	COMP_GET_OFFSET_IN_REGS(jit_fpu_conv)
#define COMP_FPU_OFF_MAGIC	COMP_GET_OFFSET_IN_REGS(jit_fpu_magic)
#define COMP_FPU_OFF_ZERO	COMP_GET_OFFSET_IN_REGS(jit_fpu_zero)

/* Floating point load/store: frt, offset(r14) */
STATIC_INLINE void comp_ppc_fpmem(uae_u32 op, int fr, uae_u16 offset)
{
	comp_ppc_emit_word(op | (fr << 21) | (PPCR_REGS_BASE << 16) | offset);
}
#define COMP_PPC_LFD	0xC8000000
#define COMP_PPC_LFS	0xC0000000
#define COMP_PPC_STFD	0xD8000000
#define COMP_PPC_STFS	0xD0000000

/* A-form / X-form floating point instruction: frt, fra, frb (frc for fmul) */
STATIC_INLINE void comp_ppc_fpop(uae_u32 op, int t, int a, int b)
{
	comp_ppc_emit_word(op | (t << 21) | (a << 16) | (b << 11));
}
#define COMP_PPC_FADD	0xFC00002A
#define COMP_PPC_FSUB	0xFC000028
#define COMP_PPC_FDIV	0xFC000024
#define COMP_PPC_FMR	0xFC000090
#define COMP_PPC_FABS	0xFC000210
#define COMP_PPC_FNEG	0xFC000050
#define COMP_PPC_FRSP	0xFC000018
#define COMP_PPC_FCMPU	0xFC000000	/* crf << 2 in t */
STATIC_INLINE void comp_ppc_fmul(int t, int a, int c)
{
	comp_ppc_emit_word(0xFC000032 | (t << 21) | (a << 16) | (c << 6));
}
/* CR logical operation: crbt = crba op crbb */
STATIC_INLINE void comp_ppc_crop(uae_u32 xo, int bt, int ba, int bb)
{
	comp_ppc_emit_word(0x4C000000 | (bt << 21) | (ba << 16) | (bb << 11) | xo);
}
#define COMP_PPC_CROR	0x382
#define COMP_PPC_CRNOR	0x042
#define COMP_PPC_CREQV	0x242
#define COMP_PPC_CRXOR	0x182

/* f1 = (double) r0 (signed 32 bit), exact */
STATIC_INLINE void helper_fpu_int_to_double(void)
{
	comp_ppc_xoris(PPCR_SPECTMP_MAPPED, PPCR_SPECTMP_MAPPED, 0x8000);
	comp_ppc_stw(PPCR_SPECTMP_MAPPED, COMP_FPU_OFF_CONV + 4, PPCR_REGS_BASE_MAPPED);
	comp_ppc_lis(PPCR_SPECTMP_MAPPED, 0x4330);
	comp_ppc_stw(PPCR_SPECTMP_MAPPED, COMP_FPU_OFF_CONV, PPCR_REGS_BASE_MAPPED);
	comp_ppc_fpmem(COMP_PPC_LFD, 1, COMP_FPU_OFF_CONV);
	comp_ppc_fpmem(COMP_PPC_LFD, 0, COMP_FPU_OFF_MAGIC);
	comp_ppc_fpop(COMP_PPC_FSUB, 1, 1, 0);
}

/* r0 = r0 or gpr sign extended from size */
STATIC_INLINE void helper_fpu_extend(comp_ppc_reg src, int size)
{
	switch (size)
	{
	case 1: comp_ppc_extsb(PPCR_SPECTMP_MAPPED, src, FALSE); break;
	case 2: comp_ppc_extsh(PPCR_SPECTMP_MAPPED, src, FALSE); break;
	default: if (src.r != PPCR_SPECTMP) comp_ppc_mr(PPCR_SPECTMP_MAPPED, src, FALSE); break;
	}
}

/* Operations translated directly (the others call fpp_jit_arith()) */
BOOL comp_fpu_inline_op(uae_u16 extra)
{
	switch (extra & 0x7f)
	{
	case 0x00: case 0x40: case 0x44:	/* FMOVE */
	case 0x22: case 0x62: case 0x66:	/* FADD */
	case 0x28: case 0x68: case 0x6c:	/* FSUB */
	case 0x23: case 0x63: case 0x67:	/* FMUL */
	case 0x20: case 0x60: case 0x64:	/* FDIV */
	case 0x24: case 0x27:				/* FSGLDIV, FSGLMUL */
	case 0x18: case 0x58: case 0x5c:	/* FABS */
	case 0x1a: case 0x5a: case 0x5e:	/* FNEG */
	case 0x38: case 0x3a:				/* FCMP, FTST */
		return TRUE;
	}
	return FALSE;
}

/**
 * Macroblock: v18 (TODO 7.3): one step of a translated FPU instruction,
 * see the COMP_FPU_* constants. Never removed by the optimizer (works on
 * regs.fp[] and the work area).
 */
void comp_macroblock_push_fpu(uae_u64 regsin, uae_u64 regsout, int kind, int fpreg, uae_u32 extra, int size, comp_ppc_reg gpr, uae_u32 hi, uae_u32 lo)
{
	comp_mb_init(mb,
				comp_macroblock_impl_fpu,
				regsin,
				regsout | COMP_COMPILER_MACROBLOCK_REG_NO_OPTIM);
	mb->fpu.kind = kind;
	mb->fpu.fpreg = fpreg;
	mb->fpu.extra = extra;
	mb->fpu.size = size;
	mb->fpu.gpr = gpr;
	mb->fpu.hi = hi;
	mb->fpu.lo = lo;
}

void comp_macroblock_impl_fpu(union comp_compiler_mb_union* mb)
{
	int n = mb->fpu.fpreg;
	uae_u32 extra = mb->fpu.extra;

	switch (mb->fpu.kind)
	{
	case COMP_FPU_SRC_FPREG:
		comp_ppc_fpmem(COMP_PPC_LFD, 1, COMP_FPU_OFF_FP(n));
		break;

	case COMP_FPU_SRC_INT_GPR:
		helper_fpu_extend(mb->fpu.gpr, mb->fpu.size);
		helper_fpu_int_to_double();
		break;

	case COMP_FPU_SRC_INT_TMP:
		comp_ppc_lwz(PPCR_SPECTMP_MAPPED, COMP_FPU_OFF_TMP, PPCR_REGS_BASE_MAPPED);
		helper_fpu_extend(PPCR_SPECTMP_MAPPED, mb->fpu.size);
		helper_fpu_int_to_double();
		break;

	case COMP_FPU_SRC_SINGLE_GPR:
		comp_ppc_stw(mb->fpu.gpr, COMP_FPU_OFF_TMP, PPCR_REGS_BASE_MAPPED);
		comp_ppc_fpmem(COMP_PPC_LFS, 1, COMP_FPU_OFF_TMP);
		break;

	case COMP_FPU_SRC_SINGLE_TMP:
		comp_ppc_fpmem(COMP_PPC_LFS, 1, COMP_FPU_OFF_TMP);
		break;

	case COMP_FPU_SRC_DOUBLE_TMP:
		comp_ppc_fpmem(COMP_PPC_LFD, 1, COMP_FPU_OFF_TMP);
		break;

	case COMP_FPU_SRC_CONST:
		comp_ppc_lis(PPCR_SPECTMP_MAPPED, (uae_u16) (mb->fpu.hi >> 16));
		comp_ppc_ori(PPCR_SPECTMP_MAPPED, PPCR_SPECTMP_MAPPED, (uae_u16) mb->fpu.hi);
		comp_ppc_stw(PPCR_SPECTMP_MAPPED, COMP_FPU_OFF_TMP, PPCR_REGS_BASE_MAPPED);
		comp_ppc_lis(PPCR_SPECTMP_MAPPED, (uae_u16) (mb->fpu.lo >> 16));
		comp_ppc_ori(PPCR_SPECTMP_MAPPED, PPCR_SPECTMP_MAPPED, (uae_u16) mb->fpu.lo);
		comp_ppc_stw(PPCR_SPECTMP_MAPPED, COMP_FPU_OFF_TMP + 4, PPCR_REGS_BASE_MAPPED);
		comp_ppc_fpmem(COMP_PPC_LFD, 1, COMP_FPU_OFF_TMP);
		break;

	case COMP_FPU_ARITH:
	{
		int dst = (extra >> 7) & 7;
		uae_u32 op = extra & 0x7f;

		switch (op)
		{
		case 0x38:	//FCMP: fp_result = (dst == src) ? 0.0 : dst - src, fpsr = 0
			comp_ppc_fpmem(COMP_PPC_LFD, 0, COMP_FPU_OFF_FP(dst));
			comp_ppc_fpop(COMP_PPC_FCMPU, 1 << 2, 0, 1);
			comp_ppc_fpop(COMP_PPC_FSUB, 2, 0, 1);
			comp_ppc_emit_word(0x40860008);	//bne cr1,+8
			comp_ppc_fpmem(COMP_PPC_LFD, 2, COMP_FPU_OFF_ZERO);
			comp_ppc_fpmem(COMP_PPC_STFD, 2, COMP_FPU_OFF_RESULT);
			comp_ppc_li(PPCR_SPECTMP_MAPPED, 0);
			comp_ppc_stw(PPCR_SPECTMP_MAPPED, COMP_FPU_OFF_FPSR, PPCR_REGS_BASE_MAPPED);
			return;
		case 0x3a:	//FTST: fp_result = src, fpsr = 0
			comp_ppc_fpmem(COMP_PPC_STFD, 1, COMP_FPU_OFF_RESULT);
			comp_ppc_li(PPCR_SPECTMP_MAPPED, 0);
			comp_ppc_stw(PPCR_SPECTMP_MAPPED, COMP_FPU_OFF_FPSR, PPCR_REGS_BASE_MAPPED);
			return;
		case 0x00: case 0x40: case 0x44:
			comp_ppc_fpop(COMP_PPC_FMR, 0, 0, 1);
			break;
		case 0x18: case 0x58: case 0x5c:
			comp_ppc_fpop(COMP_PPC_FABS, 0, 0, 1);
			break;
		case 0x1a: case 0x5a: case 0x5e:
			comp_ppc_fpop(COMP_PPC_FNEG, 0, 0, 1);
			break;
		case 0x22: case 0x62: case 0x66:
			comp_ppc_fpmem(COMP_PPC_LFD, 0, COMP_FPU_OFF_FP(dst));
			comp_ppc_fpop(COMP_PPC_FADD, 0, 0, 1);
			break;
		case 0x28: case 0x68: case 0x6c:
			comp_ppc_fpmem(COMP_PPC_LFD, 0, COMP_FPU_OFF_FP(dst));
			comp_ppc_fpop(COMP_PPC_FSUB, 0, 0, 1);
			break;
		case 0x23: case 0x63: case 0x67: case 0x27:
			comp_ppc_fpmem(COMP_PPC_LFD, 0, COMP_FPU_OFF_FP(dst));
			comp_ppc_fmul(0, 0, 1);
			break;
		case 0x20: case 0x60: case 0x64: case 0x24:
			comp_ppc_fpmem(COMP_PPC_LFD, 0, COMP_FPU_OFF_FP(dst));
			comp_ppc_fpop(COMP_PPC_FDIV, 0, 0, 1);
			break;
		default:
			write_log("JIT: Compiling error: FPU operation %02x is not translated\n", (unsigned) op);
			comp_compile_error();
			return;
		}
		//Single precision rounding of FSxxx as fround() in the interpreter
		if ((extra & 0x44) == 0x40)
			comp_ppc_fpop(COMP_PPC_FRSP, 0, 0, 0);
		comp_ppc_fpmem(COMP_PPC_STFD, 0, COMP_FPU_OFF_FP(dst));
		comp_ppc_fpmem(COMP_PPC_STFD, 0, COMP_FPU_OFF_RESULT);
		break;
	}

	case COMP_FPU_ARITH_CALL:
		//fpp_jit_arith(extra, f1): the temporary registers were flushed
		comp_ppc_liw(PPCR_PARAM1_MAPPED, extra);
		comp_ppc_call(PPCR_SPECTMP_MAPPED, (uae_uintptr) fpp_jit_arith);
		break;

	case COMP_FPU_DST_INT_CALL:
		//work area = fpp_jit_toint(FPn): the temporary registers were flushed
		comp_ppc_fpmem(COMP_PPC_LFD, 1, COMP_FPU_OFF_FP(n));
		comp_ppc_call(PPCR_SPECTMP_MAPPED, (uae_uintptr) fpp_jit_toint);
		comp_ppc_stw(PPCR_PARAM1_MAPPED, COMP_FPU_OFF_TMP, PPCR_REGS_BASE_MAPPED);
		break;

	case COMP_FPU_DST_SINGLE:
		comp_ppc_fpmem(COMP_PPC_LFD, 0, COMP_FPU_OFF_FP(n));
		comp_ppc_fpop(COMP_PPC_FRSP, 0, 0, 0);
		comp_ppc_fpmem(COMP_PPC_STFS, 0, COMP_FPU_OFF_TMP);
		break;

	case COMP_FPU_DST_DOUBLE:
		comp_ppc_fpmem(COMP_PPC_LFD, 0, COMP_FPU_OFF_FP(n));
		comp_ppc_fpmem(COMP_PPC_STFD, 0, COMP_FPU_OFF_TMP);
		break;

	case COMP_FPU_COND:
	{
		//CR1 = fp_result compared with 0.0 (lt = N, eq = Z, un = NaN);
		//CR0.EQ = condition false (as the integer conditions for Bcc)
		static const struct { uae_u32 xo; int a, b; } t[16] = {
			{ COMP_PPC_CREQV, 2, 2 },	/* F: always false */
			{ COMP_PPC_CRNOR, 6, 6 },	/* EQ */
			{ COMP_PPC_CRNOR, 5, 5 },	/* OGT */
			{ COMP_PPC_CRNOR, 5, 6 },	/* OGE */
			{ COMP_PPC_CRNOR, 4, 4 },	/* OLT */
			{ COMP_PPC_CRNOR, 4, 6 },	/* OLE */
			{ COMP_PPC_CRNOR, 4, 5 },	/* OGL */
			{ COMP_PPC_CROR, 7, 7 },	/* OR: !NaN */
			{ COMP_PPC_CRNOR, 7, 7 },	/* UN */
			{ COMP_PPC_CRNOR, 7, 6 },	/* UEQ */
			{ COMP_PPC_CRNOR, 7, 5 },	/* UGT */
			{ COMP_PPC_CROR, 4, 4 },	/* UGE: !N */
			{ COMP_PPC_CRNOR, 7, 4 },	/* ULT */
			{ COMP_PPC_CROR, 5, 5 },	/* ULE: !GT */
			{ COMP_PPC_CROR, 6, 6 },	/* NE */
			{ COMP_PPC_CRXOR, 2, 2 },	/* T: always true */
		};
		int c = extra & 15;
		comp_ppc_fpmem(COMP_PPC_LFD, 0, COMP_FPU_OFF_RESULT);
		comp_ppc_fpmem(COMP_PPC_LFD, 1, COMP_FPU_OFF_ZERO);
		comp_ppc_fpop(COMP_PPC_FCMPU, 1 << 2, 0, 1);
		comp_ppc_crop(t[c].xo, 2, t[c].a, t[c].b);
		break;
	}

	default:
		write_log("JIT: Compiling error: unknown FPU macroblock %d\n", mb->fpu.kind);
		comp_compile_error();
	}
}
#else
void comp_macroblock_push_fpu(uae_u64 regsin, uae_u64 regsout, int kind, int fpreg, uae_u32 extra, int size, comp_ppc_reg gpr, uae_u32 hi, uae_u32 lo)
{
	comp_compile_error();
}
BOOL comp_fpu_inline_op(uae_u16 extra)
{
	return FALSE;
}
#endif
