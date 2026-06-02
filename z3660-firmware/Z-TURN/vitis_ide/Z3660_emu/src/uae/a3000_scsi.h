/*
 * a3000_scsi.h - Emulated A3000 mainboard SCSI (Commodore SuperDMAC + WD33C93)
 *
 * Lets AMIX (Amiga Unix 2.1) find its root device: AMIX only has SCSI drivers
 * for the A3000 onboard controller. Ported from WinUAE a2091.cpp (commit
 * c7b24b37, GPLv2 - same provenance as the imported 030 MMU/cpuemu code),
 * COMMODORE_SDMAC path, made synchronous and retargeted at the Z3660 backend.
 *
 * Lives on core1 (the 68k emulator). MMIO window is the $00DD0000 page, wired
 * via a3000_scsi_bank in uae_emulator.cpp. Disk I/O is delegated to core0's
 * FatFS backend over the existing PISCSI cross-core channel (see AMIX_SCSI_design.md).
 */
#ifndef A3000_SCSI_H
#define A3000_SCSI_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One-time reset of the WD33C93/SuperDMAC state. Call from uae_emulator(). */
void a3000_scsi_init(void);

/* MMIO access to the $00DD0000 page. offset = address & 0xffff (0..0xffff).
 * size: 0 = byte, 1 = word, 2 = long (matches the z2scsi/bank handler convention). */
uint32_t a3000_scsi_read(uint32_t offset, int size);
void     a3000_scsi_write(uint32_t offset, uint32_t data, int size);

/* Software level-2 (Amiga INT2) line for the emulated controller. Defined in
 * main.cc next to intlev()/read_irq; a3000_scsi.cpp recomputes it. 0 or 2. */
extern volatile int a3000_scsi_irq;

#ifdef __cplusplus
}
#endif

#endif /* A3000_SCSI_H */
