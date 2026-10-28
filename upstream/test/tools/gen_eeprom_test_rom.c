/*
 * gen_eeprom_test_rom.c — Generate a minimal Atari Jaguar ROM that writes
 * known values to the EEPROM via the JERRY serial interface (NM93C14).
 *
 * The ROM writes 0xCAFE to address 0 and 0xBEEF to address 1, then loops.
 * After running for a few hundred frames, the SRAM buffer should contain
 * these values packed big-endian at the corresponding offsets.
 *
 * Build:  cc -o gen_eeprom_test_rom gen_eeprom_test_rom.c
 * Usage:  ./gen_eeprom_test_rom eeprom_test.j64
 *
 * Jaguar ROM layout:
 *   0x000-0x003: unused (gets overwritten by SSP in RAM)
 *   0x400-0x403: ROM flags (0x04040404)
 *   0x404-0x407: Entry point address (0x00802000)
 *   0x2000+:     68K code
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* Jaguar EEPROM register addresses */
#define JERRY_EE_DI   0x00F14801  /* Data In (write bit 0) */
#define JERRY_EE_CS   0x00F15001  /* Chip Select (write to assert) */

/* TOM interrupt control register — writing 0 masks all interrupts */
#define TOM_INT1      0x00F000E0  /* Interrupt control register 1 */

/* 68K instruction encoders.
 * All instructions are word-aligned (even byte count). */

/* MOVE.B #imm8, (abs32).L — 8 bytes */
static void emit_move_b_imm_abs(uint8_t *buf, int *pos, uint8_t val, uint32_t addr)
{
    buf[(*pos)++] = 0x13; buf[(*pos)++] = 0xFC;
    buf[(*pos)++] = 0x00; buf[(*pos)++] = val;
    buf[(*pos)++] = (addr >> 24) & 0xFF;
    buf[(*pos)++] = (addr >> 16) & 0xFF;
    buf[(*pos)++] = (addr >>  8) & 0xFF;
    buf[(*pos)++] = (addr >>  0) & 0xFF;
}

/* MOVE.W #imm16, (abs32).L — 8 bytes */
static void emit_move_w_imm_abs(uint8_t *buf, int *pos, uint16_t val, uint32_t addr)
{
    buf[(*pos)++] = 0x33; buf[(*pos)++] = 0xFC;
    buf[(*pos)++] = (val >> 8) & 0xFF;
    buf[(*pos)++] = val & 0xFF;
    buf[(*pos)++] = (addr >> 24) & 0xFF;
    buf[(*pos)++] = (addr >> 16) & 0xFF;
    buf[(*pos)++] = (addr >>  8) & 0xFF;
    buf[(*pos)++] = (addr >>  0) & 0xFF;
}

/* MOVE.L #imm32, (abs32).L — 12 bytes */
static void emit_move_l_imm_abs(uint8_t *buf, int *pos, uint32_t val, uint32_t addr)
{
    buf[(*pos)++] = 0x23; buf[(*pos)++] = 0xFC;
    buf[(*pos)++] = (val >> 24) & 0xFF;
    buf[(*pos)++] = (val >> 16) & 0xFF;
    buf[(*pos)++] = (val >>  8) & 0xFF;
    buf[(*pos)++] = (val >>  0) & 0xFF;
    buf[(*pos)++] = (addr >> 24) & 0xFF;
    buf[(*pos)++] = (addr >> 16) & 0xFF;
    buf[(*pos)++] = (addr >>  8) & 0xFF;
    buf[(*pos)++] = (addr >>  0) & 0xFF;
}

/* MOVE #imm16, SR — 4 bytes (set status register, must be in supervisor mode) */
static void emit_move_to_sr(uint8_t *buf, int *pos, uint16_t val)
{
    buf[(*pos)++] = 0x46; buf[(*pos)++] = 0xFC;
    buf[(*pos)++] = (val >> 8) & 0xFF;
    buf[(*pos)++] = val & 0xFF;
}

/* BRA.S offset (branch to self = 0x60FE for infinite loop) — 2 bytes */
static void emit_bra_self(uint8_t *buf, int *pos)
{
    buf[(*pos)++] = 0x60;
    buf[(*pos)++] = 0xFE;
}

/* Write a single bit to the EEPROM data-in register */
static void emit_ee_di(uint8_t *buf, int *pos, int bit)
{
    emit_move_b_imm_abs(buf, pos, bit & 1, JERRY_EE_DI);
}

/* Assert chip select */
static void emit_ee_cs(uint8_t *buf, int *pos)
{
    emit_move_b_imm_abs(buf, pos, 0x01, JERRY_EE_CS);
}

/*
 * Emit code to write a 16-bit value to a 6-bit EEPROM address.
 *
 * NM93C14 serial protocol:
 *   1. Assert CS (also enables writes in VJ's implementation)
 *   2. START bit: 1
 *   3. Opcode WRITE: 01
 *   4. 6-bit address (MSB first)
 *   5. 16-bit data (MSB first)
 */
static void emit_eeprom_write(uint8_t *buf, int *pos, uint8_t addr, uint16_t data)
{
    int i;

    /* Assert CS — resets state machine & enables writes */
    emit_ee_cs(buf, pos);

    /* START bit */
    emit_ee_di(buf, pos, 1);

    /* Opcode: WRITE = 01 */
    emit_ee_di(buf, pos, 0);
    emit_ee_di(buf, pos, 1);

    /* 6-bit address, MSB first */
    for (i = 5; i >= 0; i--)
        emit_ee_di(buf, pos, (addr >> i) & 1);

    /* 16-bit data, MSB first */
    for (i = 15; i >= 0; i--)
        emit_ee_di(buf, pos, (data >> i) & 1);
}

int main(int argc, char **argv)
{
    FILE *fp;
    uint8_t rom[0x20000]; /* 128KB ROM — minimum for JST_ROM detection */
    int code_pos, rte_addr;

    if (argc != 2)
    {
        fprintf(stderr, "Usage: %s <output.j64>\n", argv[0]);
        return 1;
    }

    memset(rom, 0xFF, sizeof(rom));

    /* ROM header at offset 0x400 */
    rom[0x400] = 0x04; rom[0x401] = 0x04;
    rom[0x402] = 0x04; rom[0x403] = 0x04;

    /* Entry point at 0x802000 (ROM offset 0x2000) */
    rom[0x404] = 0x00; rom[0x405] = 0x80;
    rom[0x406] = 0x20; rom[0x407] = 0x00;

    /* 68K code starts at ROM offset 0x2000.
     *
     * The first thing we do is set up exception vectors in RAM so that
     * any interrupt (TOM halfline, etc.) doesn't jump to random garbage.
     * We place an RTE instruction in RAM and point all vectors at it.
     *
     * We also mask TOM interrupts to prevent the halfline callback
     * from interfering before we're done writing to EEPROM. But the
     * emulator's event system runs independently of 68K interrupts,
     * so frame boundaries still work. */
    code_pos = 0x2000;

    /* Disable 68K interrupts by setting SR interrupt mask to 7 (supervisor mode).
     * SR = 0x2700: supervisor mode, interrupt mask = 7 */
    emit_move_to_sr(rom, &code_pos, 0x2700);

    /* Place an RTE instruction at a known RAM address (0x1000).
     * Then point all exception vectors (0x08-0x3FC) at it. */
    rte_addr = 0x001000;

    /* Write RTE (0x4E73) at RAM 0x1000 */
    emit_move_w_imm_abs(rom, &code_pos, 0x4E73, rte_addr);

    /* Also write a BRA.S self (0x60FE) right after as a safety net */
    emit_move_w_imm_abs(rom, &code_pos, 0x60FE, rte_addr + 2);

    /* Set critical exception vectors to point to our RTE handler.
     * Vectors 0,1 are SSP and PC (already set by boot).
     * Only set the ones that might actually fire:
     *   2: Bus error, 3: Address error, 4: Illegal instruction,
     *   24: Spurious interrupt, 25-31: Autovector interrupts */
    {
        int vecs[] = { 2, 3, 4, 5, 6, 7, 8, 9, 10, 11,
                       24, 25, 26, 27, 28, 29, 30, 31, -1 };
        int v;
        for (v = 0; vecs[v] >= 0; v++)
            emit_move_l_imm_abs(rom, &code_pos, rte_addr, vecs[v] * 4);
    }

    /* Now do the actual EEPROM writes */

    /* Write 0xCAFE to EEPROM address 0 */
    emit_eeprom_write(rom, &code_pos, 0x00, 0xCAFE);

    /* Write 0xBEEF to EEPROM address 1 */
    emit_eeprom_write(rom, &code_pos, 0x01, 0xBEEF);

    /* Write 0xDEAD to EEPROM address 2 */
    emit_eeprom_write(rom, &code_pos, 0x02, 0xDEAD);

    /* Write 0x1234 to EEPROM address 63 (last address) */
    emit_eeprom_write(rom, &code_pos, 0x3F, 0x1234);

    /* Infinite loop */
    emit_bra_self(rom, &code_pos);

    if (code_pos > (int)sizeof(rom))
    {
        fprintf(stderr, "ERROR: code too large (%d bytes, max %d)\n",
                code_pos, (int)sizeof(rom));
        return 1;
    }

    /* Write ROM file */
    fp = fopen(argv[1], "wb");
    if (!fp)
    {
        perror("fopen");
        return 1;
    }
    fwrite(rom, 1, sizeof(rom), fp);
    fclose(fp);

    printf("Generated %s (%d bytes, code ends at offset 0x%04X)\n",
           argv[1], (int)sizeof(rom), code_pos);
    return 0;
}
