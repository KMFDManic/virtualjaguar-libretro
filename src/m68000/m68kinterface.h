// 
// C interface to the UAE core
//
// by James Hammons
// (C) 2011 Underground Software
//
// Most of these functions are in place to help make it easy to replace the
// Musashi core with my bastardized UAE one. :-)
//

#ifndef __M68KINTERFACE_H__
#define __M68KINTERFACE_H__

#ifdef __cplusplus
extern "C" {
#endif

/* Registers used by m68k_get_reg() and m68k_set_reg() */
typedef enum
{
	/* Real registers */
	M68K_REG_D0,		/* Data registers */
	M68K_REG_D1,
	M68K_REG_D2,
	M68K_REG_D3,
	M68K_REG_D4,
	M68K_REG_D5,
	M68K_REG_D6,
	M68K_REG_D7,
	M68K_REG_A0,		/* Address registers */
	M68K_REG_A1,
	M68K_REG_A2,
	M68K_REG_A3,
	M68K_REG_A4,
	M68K_REG_A5,
	M68K_REG_A6,
	M68K_REG_A7,
	M68K_REG_PC,		/* Program Counter */
	M68K_REG_SR,		/* Status Register */
	M68K_REG_SP,		/* The current Stack Pointer (located in A7) */
	M68K_REG_USP,		/* User Stack Pointer */

	/* Assumed registers */
	M68K_REG_PREF_ADDR,	/* Last prefetch address */
	M68K_REG_PREF_DATA,	/* Last prefetch data */

	/* Convenience registers */
	M68K_REG_PPC,		/* Previous value in the program counter */
	M68K_REG_IR			/* Instruction register */
} m68k_register_t;

/* Special interrupt acknowledge values */
#define M68K_INT_ACK_AUTOVECTOR    0xFFFFFFFF
#define M68K_INT_ACK_SPURIOUS      0xFFFFFFFE

void m68k_pulse_reset(void);
int m68k_execute(int num_cycles);
void m68k_set_irq(unsigned int int_level);

// Functions that MUST be implemented by the user:

// Read from anywhere
unsigned int m68k_read_memory_8(unsigned int address);
unsigned int m68k_read_memory_16(unsigned int address);
unsigned int m68k_read_memory_32(unsigned int address);

// Write to anywhere
void m68k_write_memory_8(unsigned int address, unsigned int value);
void m68k_write_memory_16(unsigned int address, unsigned int value);
void m68k_write_memory_32(unsigned int address, unsigned int value);

int irq_ack_handler(int);

// Convenience functions

// Uncomment this to have the emulated CPU call a hook function after every instruction
#define M68K_HOOK_FUNCTION
#ifdef M68K_HOOK_FUNCTION
void M68KInstructionHook(void);
#endif

// Debugging helpers
void M68KDebugHalt(void);
void M68KDebugResume(void);

/* Peek at the internals of a CPU context.  If context is NULL, the currently running CPU context will be used. */
unsigned int m68k_get_reg(void * context, m68k_register_t reg);

/* Poke values into the internals of the currently running CPU context */
void m68k_set_reg(m68k_register_t reg, unsigned int value);

/* Check if an instruction is valid for the specified CPU type */
unsigned int m68k_is_valid_instruction(unsigned int instruction, unsigned int cpu_type);

/* These functions let you read/write/modify the number of cycles left to run during execution */
int m68k_cycles_run(void);
int m68k_cycles_remaining(void);
void m68k_modify_timeslice(int cycles);
void m68k_end_timeslice(void);

/* UAE M68K CPU clock multiplier interface */
void m68k_set_cpu_clock_multiplier(double multiplier);
double m68k_get_cpu_clock_multiplier(void);

#ifdef __cplusplus
}
#endif

#endif	// __M68KINTERFACE_H__

