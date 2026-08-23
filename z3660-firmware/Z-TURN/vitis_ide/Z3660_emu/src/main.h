/*
 * main.h
 *
 *  Created on: 5 ene. 2023
 *      Author: shanshe
 */

#ifndef SRC_MAIN_H_
#define SRC_MAIN_H_

#include "cpu_emulator.h"
#include <inttypes.h>
extern int configured_z3;
extern int enabled_z3;
extern int configured_z2;
extern int enabled_z2;
extern int shutup_z3;
extern int shutup_z2;

#define INT_IPL_ON_THIS_CORE 1
#define AUTOCONFIG_ENABLED
#define CPU_RAM

// MUX signals
#define PS_MIO_0      0 // MIO  0
#define PS_MIO_8      8 // MIO  8
#define PS_MIO_9      9 // MIO  9
#define PS_MIO_12    12 // MIO 12
#define PS_MIO_13    13 // MIO 13
#define PS_MIO_15    15 // MIO 15

#define REG0 0x00
#define REG1 0x04
#define REG2 0x08
#define REG3 0x0C
#define REG4 0x10
#define REG5 0x14
#define REG6 0x18
#define REG7 0x1C
#define REG8 0x20

// Reset Input signal
#define n040RSTI     10 // MIO 10

#define REG_BASE_ADDRESS XPAR_Z3660_0_BASEADDR

#define write_reg(Offset,Data)   (*(volatile uint32_t *)(REG_BASE_ADDRESS+(Offset)))=(Data)

#define read_mem32(Offset) (*(volatile uint32_t *)(REG_BASE_ADDRESS+0x14000000+((Offset&0x00FFFFFF)<<2)))=(0)
#define read_mem16(Offset) (*(volatile uint32_t *)(REG_BASE_ADDRESS+0x18000000+((Offset&0x00FFFFFF)<<2)))=(0)
#define read_mem8(Offset)  (*(volatile uint32_t *)(REG_BASE_ADDRESS+0x1C000000+((Offset&0x00FFFFFF)<<2)))=(0)

#define write_mem32(Offset,Data) (*(volatile uint32_t *)(REG_BASE_ADDRESS+0x04000000+((Offset&0x00FFFFFF)<<2)))=(Data)
#define write_mem16(Offset,Data) (*(volatile uint32_t *)(REG_BASE_ADDRESS+0x08000000+((Offset&0x00FFFFFF)<<2)))=(Data)
#define write_mem8(Offset,Data)  (*(volatile uint32_t *)(REG_BASE_ADDRESS+0x0C000000+((Offset&0x00FFFFFF)<<2)))=(Data)
#define read_reg(Offset) (*(volatile uint32_t *)(REG_BASE_ADDRESS+(Offset)))

// Some amiga write and read definitions
#define WRITE_ (0<<1)
#define READ_  (1<<1)
#define LONG_  (0<<2)
#define BYTE_  (1<<2)
#define WORD_  (2<<2)
#define LINE_  (3<<2) // not used for now...

typedef struct {
   volatile uint32_t shared_data;         // 0xFFFF0000
   volatile uint32_t write_rtg;           // 0xFFFF0004
   volatile uint32_t write_rtg_addr;      // 0xFFFF0008
   volatile uint32_t write_rtg_data;      // 0xFFFF000C
   volatile uint32_t core0_hold;          // 0xFFFF0010
   volatile uint32_t core0_hold_ack;      // 0xFFFF0014
   volatile uint32_t irq;                 // 0xFFFF0018
   volatile uint32_t uart_semaphore;      // 0xFFFF001C
   volatile uint32_t jit_enabled;         // 0xFFFF0020
   volatile uint32_t reset_emulator;      // 0xFFFF0024
   volatile uint32_t load_rom_emu;        // 0xFFFF0028
   volatile uint32_t load_rom_addr;       // 0xFFFF002C
   volatile uint32_t int_available;       // 0xFFFF0030
   volatile uint32_t cfg_emu;             // 0xFFFF0034
   volatile uint32_t write_scsi;          // 0xFFFF0038
   volatile uint32_t write_scsi_addr;     // 0xFFFF003C
   volatile uint32_t write_scsi_data;     // 0xFFFF0040
   volatile uint32_t write_scsi_type;     // 0xFFFF0044
   volatile uint32_t read_scsi;           // 0xFFFF0048
   volatile uint32_t read_scsi_addr;      // 0xFFFF004C
   volatile uint32_t read_scsi_data;      // 0xFFFF0050
   volatile uint32_t read_scsi_type;      // 0xFFFF0054
   volatile uint32_t scsiboot_rom_loaded; // 0xFFFF0058
   volatile uint32_t write_scsi_in_progress; // 0xFFFF005C
   volatile uint32_t read_rtg;            // 0xFFFF0060
   volatile uint32_t read_rtg_addr;       // 0xFFFF0064
   volatile uint32_t read_rtg_data;       // 0xFFFF0068
   volatile uint32_t mmu_core1_add;       // 0xFFFF006C
   volatile uint32_t z3_enabled;          // 0xFFFF0070
   volatile uint32_t load_ext_rom_addr;   // 0xFFFF0074
   volatile uint32_t load_romext_emu;     // 0xFFFF0078
   volatile uint32_t nops_write;          // 0xFFFF007C
   volatile uint32_t nops_read;           // 0xFFFF0080
   volatile uint32_t disassemble;         // 0xFFFF0084
   volatile uint32_t musashi_step;        // 0xFFFF0088
   volatile uint32_t reset_emulator_dis;  // 0xFFFF008C
   volatile uint32_t z2_enabled;          // 0xFFFF0090
   volatile uint32_t printhist_dataabort; // 0xFFFF0094
   volatile uint32_t arm_freq_code;       // 0xFFFF0098
   volatile uint32_t write_floppy;        // 0xFFFF009C
   volatile uint32_t write_floppy_addr;   // 0xFFFF00A0
   volatile uint32_t write_floppy_data;   // 0xFFFF00A4
   volatile uint32_t write_floppy_type;   // 0xFFFF00A8
   volatile uint32_t read_floppy;         // 0xFFFF00AC
   volatile uint32_t read_floppy_addr;    // 0xFFFF00B0
   volatile uint32_t read_floppy_data;    // 0xFFFF00B4
   volatile uint32_t read_floppy_type;    // 0xFFFF00B8
   volatile uint32_t debug_emu;           // 0xFFFF00BC  emulator-core debug spam toggle (0=off,1=on): [PC]/[RTE-B-IF]/fixup (DEMU menu cmd)
   volatile uint32_t amix_mode;           // 0xFFFF00C0  AMIX memory contract on/off (control core sets from config.amix_mode)
   volatile uint32_t service_cadence;     // 0xFFFF00C4  030-MMU run loop: instructions between IPL/cross-core polls (1=every; perf knob, SERV cmd)
   volatile uint32_t perf_report;         // 0xFFFF00C8  030-MMU run loop: 1=print instr/sec to serial ~1Hz (PERF cmd)
   volatile uint32_t amix_ram;            // 0xFFFF00CC  AMIX main-RAM window size in MiB (16 = the historical default; bounds in Z3660/src/amix_ram.h)
   volatile uint32_t perf_stats;          // 0xFFFF00D0  MMU run loops: 1=per-instruction perf accounting ON (the historical default), 0=off (perf_stats key, PERFS cmd)
   volatile uint32_t emu_probes;          // 0xFFFF00D4  MMU run loops: 1=run-loop diagnostic probes ON (the historical default), 0=off (emu_probes key, PROBES cmd)
   volatile uint32_t prof_ctl;            // 0xFFFF00D8  CYBERSTORM C1 profiler command word: cmd in bits 15..0, arg in 31..16 (Z3660_PROF_CTL_*). Core1 consumes and clears it in do_specialties(). Inert unless core1 is the PROFILING build.
   volatile uint32_t prof_hz;             // 0xFFFF00DC  CYBERSTORM C1 sampler rate in Hz, read when prof_ctl says ARM (PROF cmd; 1..20000, default 1000)
   volatile uint32_t arm_wall_hz;         // 0xFFFF00E0  ARM global-timer rate in Hz AS RETUNED AT RUNTIME (= counts_per_second, main.c). Core0 publishes it beside arm_freq_code; core1's profiler scales its measured PMU:timer ratio by it. Exists because COUNTS_PER_SECOND is a COMPILE-TIME BSP constant frozen at the 666.67 MHz default while arm_frequency retunes the PLL -- which made every rate the C1 profiler printed low by 1.65x on an 1100 MHz board. 0 = not published (older core0) -> core1 falls back to the constant and says so.
   volatile uint32_t blk_chunk;           // 0xFFFF00E4  CYBERSTORM C2: iterations of a recognised block idiom (bzero/mcpy/CPUSHL loop) the 040 run loop services per pass. 0 = unset -> the built-in default; 0xFFFFFFFF = OFF (pure passthrough); anything else is the cap, clamped. The rule is z3660_blk.h's z3660_blk_resolve_chunk() and NOTHING else may re-implement it. Unset means DEFAULT, not off, so a core0 built before this field existed still runs the shipped behaviour.
   volatile uint32_t dopc_on;             // 0xFFFF00E8  CYBERSTORM C2 rung 1: the 040 run loop's decoded-op cache (4096 entries, 4-way, 64 KB). 0 = unset -> the built-in default (ON); 0xFFFFFFFF = OFF (pure passthrough: never consulted, never filled); anything else = ON. The rule is z3660_dopc.h's z3660_dopc_resolve() and NOTHING else may re-implement it. Unset means ON, not off, so a core0 built before this field existed still runs the shipped behaviour.
   volatile uint32_t dfast_on;            // 0xFFFF00EC  CYBERSTORM C2 rung 2: the 040 data accessors' fast path (inline TTR verdict + page-cache resolve + direct RAM access). 0 = unset -> the built-in default (ON); 0xFFFFFFFF = OFF (every access takes the original out-of-line accessor); anything else = ON. The rule is z3660_dfast.h's z3660_dfast_resolve() and NOTHING else may re-implement it. Unset means ON, not off, so a core0 built before this field existed still runs the shipped behaviour. OFF is the REFERENCE machine, which is what makes a PROFZ-fenced same-boot A/B possible without a second flash.
} SHARED;

enum BOOTMODE{
   MOBOCPU,
   CPU,
   MUSASHI,
   UAE_030,
   UAEJIT_030,
   UAE_040,
   UAEJIT_040,
   UAE_030_MMU,        // appended at end so stored indices/configs keep meaning
   UAE_040_MMU,        // appended at end so stored indices/configs keep meaning
   BOOTMODE_NUM
};
// Tripwire: this enum is replicated index-aligned in ../Z3660/src/config_file.h,
// z3660-drivers/ZTop/Ztop.c and ../Z3660/src/ARM_ztop. Keep all in sync; append
// new modes at the end only.
#if defined(__cplusplus)
static_assert(BOOTMODE_NUM == 9, "BOOTMODE changed: update the replicas in config_file.h / Ztop.c / ARM_ztop");
#endif
typedef struct {
   uint32_t load_rom_emu;
   uint32_t load_romext_emu;
   uint32_t z3_enabled;
   uint32_t z2_enabled;
} LOCAL;
#endif /* SRC_MAIN_H_ */
