/*
 * smush_buffers.h -- where the SMUSH codec buffers (codec37.c, codec47.c) live.
 *
 * Each decoder (0..3) has a 16 MB stride from the base: codec37's delta buffer at +0; codec47's
 * big table at +0, small table at +8 MB and delta buffer at +16 MB.
 *
 * With a real 68060 (boot mode CPU, MOBOCPU) core 1 does not run the emulator, and the base is
 * 0x30000000, the emulator's region. In the emulated boot modes core 1 runs the emulator from
 * 0x30000000 (image, heap and stack: Z3660_emu/src/lscript.ld asserts that they end below
 * SMUSH_BUFFER_BASE_EMU), and core 0 decodes for a 68k program that writes REG_ZZ_ACC_OP, so
 * the buffers start above it.
 */
#ifndef SMUSH_BUFFERS_H
#define SMUSH_BUFFERS_H

#include <stdint.h>

#define SMUSH_BUFFER_BASE_CPU  0x30000000UL
#define SMUSH_BUFFER_BASE_EMU  0x37000000UL

extern int smush_emulated;            /* main.c: 1 when core 1 runs the 68k emulator */

static inline uint8_t *smush_buffer_base(void)
{
   return (uint8_t *)(smush_emulated ? SMUSH_BUFFER_BASE_EMU : SMUSH_BUFFER_BASE_CPU);
}

#endif
