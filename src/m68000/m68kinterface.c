//
// m68kinterface.c: Code interface to the UAE 68000 core and support code
//
// by James Hammons
// (C) 2011 Underground Software
//
// JLH = James Hammons <jlhamm@acm.org>
//
// Who  When        What
// ---  ----------  -------------------------------------------------------------
// JLH  10/28/2011  Created this file ;-)
//

// Include files
#include "m68kinterface.h"
#include "cpudefs.h"
#include "inlines.h"
#include "cpuextra.h"
#include "readcpu.h"

// Exception Vectors handled by emulation
#define EXCEPTION_BUS_ERROR                2
#define EXCEPTION_ADDRESS_ERROR            3
#define EXCEPTION_ILLEGAL_INSTRUCTION      4
#define EXCEPTION_ZERO_DIVIDE              5
#define EXCEPTION_CHK                      6
#define EXCEPTION_TRAPV                    7
#define EXCEPTION_PRIVILEGE_VIOLATION      8
#define EXCEPTION_TRACE                    9
#define EXCEPTION_1010                    10
#define EXCEPTION_1111                    11
#define EXCEPTION_FORMAT_ERROR            14
#define EXCEPTION_UNINITIALIZED_INTERRUPT 15
#define EXCEPTION_SPURIOUS_INTERRUPT      24
#define EXCEPTION_INTERRUPT_AUTOVECTOR    24
#define EXCEPTION_TRAP_BASE               32

// CPU tables
extern const struct cputbl op_smalltbl_4_ff[];
extern const struct cputbl op_smalltbl_5_ff[];

// Function prototypes
static INLINE void m68ki_check_interrupts(void);
void m68ki_exception_interrupt(uint32_t intLevel);
static INLINE uint32_t m68ki_init_exception(void);
static INLINE void m68ki_stack_frame_3word(uint32_t pc, uint32_t sr);
unsigned long IllegalOpcode(uint32_t opcode);
void BuildCPUFunctionTable(void);
void m68k_set_irq2(unsigned int intLevel);

// Global vars
static int32_t initialCycles;
static int32_t adjustedCycles;
static int32_t executedCycles;
static int m68k_clocks_mult = 1;  // Clock multiplier, default 1x

cpuop_func * cpuFunctionTable[65536];

// IRQ control
static int checkForIRQToHandle = 0;
static int IRQLevelToHandle = 0;

// Debug controls
void M68KDebugHalt(void)
{
	regs.spcflags |= SPCFLAG_DEBUGGER;
}

void M68KDebugResume(void)
{
	regs.spcflags &= ~SPCFLAG_DEBUGGER;
}

// Reset pulse to CPU
void m68k_pulse_reset(void)
{
	static uint32_t emulation_initialized = 0;

	if (!emulation_initialized)
	{
		read_table68k();
		do_merges();
		BuildCPUFunctionTable();
		emulation_initialized = 1;
	}

	regs.spcflags = 0;
	regs.stopped = 0;
	regs.remainingCycles = 0;

	regs.intmask = 0x07;
	regs.s = 1;

	m68k_areg(regs, 7) = m68k_read_memory_32(0);
	m68k_setpc(m68k_read_memory_32(4));
	refill_prefetch(m68k_getpc(), 0);
}

int m68k_execute(int num_cycles)
{
	if (regs.stopped)
	{
		regs.remainingCycles = 0;
		regs.interruptCycles = 0;
		executedCycles = 0;
		return num_cycles;
	}

	initialCycles = num_cycles;
	adjustedCycles = num_cycles * m68k_clocks_mult;
	executedCycles = 0;

	regs.remainingCycles = adjustedCycles;
	regs.remainingCycles -= regs.interruptCycles;
	regs.interruptCycles = 0;

	do
	{
		uint32_t opcode;
		int32_t cycles;

		if (regs.spcflags & SPCFLAG_DEBUGGER)
		{
			executedCycles = adjustedCycles - regs.remainingCycles;
			regs.remainingCycles = 0;
			regs.interruptCycles = 0;
			return executedCycles / m68k_clocks_mult;
		}

		if (checkForIRQToHandle)
		{
			checkForIRQToHandle = 0;
			m68k_set_irq2(IRQLevelToHandle);
		}

#ifdef M68K_HOOK_FUNCTION
		M68KInstructionHook();
#endif

		opcode = get_iword(0);
		cycles = (int32_t)(*cpuFunctionTable[opcode])(opcode);
		regs.remainingCycles -= cycles;

	} while (regs.remainingCycles > 0);

	executedCycles = adjustedCycles - regs.remainingCycles;
	regs.remainingCycles -= regs.interruptCycles;
	regs.interruptCycles = 0;

	return executedCycles / m68k_clocks_mult;
}

// IRQ setters
void m68k_set_irq(unsigned int intLevel)
{
	if (regs.stopped)
	{
		m68k_set_irq2(intLevel);
		return;
	}

	IRQLevelToHandle = intLevel;
	checkForIRQToHandle = 1;
}

void m68k_set_irq2(unsigned int intLevel)
{
	int oldLevel = regs.intLevel;
	regs.intLevel = intLevel;

	if (oldLevel != 0x07 && regs.intLevel == 0x07)
		m68ki_exception_interrupt(7);
	else
		m68ki_check_interrupts();
}

static INLINE void m68ki_check_interrupts(void)
{
	if (regs.intLevel > regs.intmask)
		m68ki_exception_interrupt(regs.intLevel);
}

void m68ki_exception_interrupt(uint32_t intLevel)
{
	uint32_t vector, sr, newPC;

	regs.stopped = 0;

	vector = irq_ack_handler(intLevel);

	if (vector == M68K_INT_ACK_AUTOVECTOR)
		vector = EXCEPTION_INTERRUPT_AUTOVECTOR + intLevel;
	else if (vector == M68K_INT_ACK_SPURIOUS)
		vector = EXCEPTION_SPURIOUS_INTERRUPT;
	else if (vector > 255)
		return;

	sr = m68ki_init_exception();
	regs.intmask = intLevel;
	newPC = m68k_read_memory_32(vector << 2);

	if (newPC == 0)
		newPC = m68k_read_memory_32(EXCEPTION_UNINITIALIZED_INTERRUPT << 2);

	m68ki_stack_frame_3word(regs.pc, sr);
	m68k_setpc(newPC);

	regs.interruptCycles += 56; // FIXME: accurate timing
}

static INLINE uint32_t m68ki_init_exception(void)
{
	MakeSR();
	uint32_t sr = regs.sr;
	regs.s = 1;
	return sr;
}

static INLINE void m68ki_stack_frame_3word(uint32_t pc, uint32_t sr)
{
	m68k_areg(regs, 7) -= 4;
	m68k_write_memory_32(m68k_areg(regs, 7), pc);
	m68k_areg(regs, 7) -= 2;
	m68k_write_memory_16(m68k_areg(regs, 7), sr);
}

unsigned int m68k_get_reg(void * context, m68k_register_t reg)
{
	if (reg <= M68K_REG_A7)
		return regs.regs[reg];
	else if (reg == M68K_REG_PC)
		return regs.pc;
	else if (reg == M68K_REG_SR)
	{
		MakeSR();
		return regs.sr;
	}
	else if (reg == M68K_REG_SP)
		return regs.regs[15];
	return 0;
}

void m68k_set_reg(m68k_register_t reg, unsigned int value)
{
	if (reg <= M68K_REG_A7)
		regs.regs[reg] = value;
	else if (reg == M68K_REG_PC)
		regs.pc = value;
	else if (reg == M68K_REG_SR)
	{
		regs.sr = value;
		MakeFromSR();
	}
	else if (reg == M68K_REG_SP)
		regs.regs[15] = value;
}

unsigned int m68k_is_valid_instruction(unsigned int instruction, unsigned int cpu_type)
{
	instruction &= 0xFFFF;
	return cpuFunctionTable[instruction] != IllegalOpcode;
}

int m68k_cycles_run(void)
{
	return executedCycles / m68k_clocks_mult;
}

int m68k_cycles_remaining(void)
{
	return regs.remainingCycles / m68k_clocks_mult;
}

void m68k_modify_timeslice(int cycles)
{
	regs.remainingCycles = cycles * m68k_clocks_mult;
}

void m68k_end_timeslice(void)
{
	initialCycles = regs.remainingCycles;
	regs.remainingCycles = 0;
}

unsigned long IllegalOpcode(uint32_t opcode)
{
	if ((opcode & 0xF000) == 0xF000)
	{
		Exception(0x0B, 0, M68000_EXC_SRC_CPU);
		return 4;
	}
	else if ((opcode & 0xF000) == 0xA000)
	{
		Exception(0x0A, 0, M68000_EXC_SRC_CPU);
		return 4;
	}

	Exception(0x04, 0, M68000_EXC_SRC_CPU);
	return 4;
}

void BuildCPUFunctionTable(void)
{
	int i;
	unsigned long opcode;
	const struct cputbl * tbl = op_smalltbl_5_ff;

	for (opcode = 0; opcode < 65536; opcode++)
		cpuFunctionTable[opcode] = IllegalOpcode;

	for (i = 0; tbl[i].handler != NULL; i++)
		cpuFunctionTable[tbl[i].opcode] = tbl[i].handler;

	for (opcode = 0; opcode < 65536; opcode++)
	{
		if (table68k[opcode].mnemo == i_ILLG || table68k[opcode].clev > 0)
			continue;

		if (table68k[opcode].handler != -1)
		{
			cpuop_func * f = cpuFunctionTable[table68k[opcode].handler];
			if (f == IllegalOpcode)
				abort();
			cpuFunctionTable[opcode] = f;
		}
	}
}

// Setter/getter for clock multiplier
void m68k_set_clocks_mult(int mult)
{
	m68k_clocks_mult = (mult >= 1) ? mult : 1;
}

int m68k_get_clocks_mult(void)
{
	return m68k_clocks_mult;
}

