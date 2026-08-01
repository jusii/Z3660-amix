/*
 * a3000_scsi.cpp - Emulated A3000 mainboard SCSI (Commodore SuperDMAC + WD33C93)
 *
 * Ported from WinUAE a2091.cpp (commit c7b24b37, GPLv2 - same provenance as the
 * imported 030 MMU/cpuemu code), COMMODORE_SDMAC + WD33C93 core. Two deliberate
 * divergences from WinUAE, documented in AMIX_SCSI_design.md:
 *   1. SYNCHRONOUS interrupt model. WinUAE spaces the WD status queue with an
 *      hsync cycle countdown; we have no hsync, so we drop the countdown but keep
 *      the 2-deep queue + the "one interrupt pending at a time" rule. The Amiga
 *      INT2 line is a pure level function of (SDMAC.CNTR & INTEN) && WD.ASR_INT,
 *      recomputed after every register access into the software a3000_scsi_irq.
 *   2. The SCSI *target* (disk) is the Z3660 backend, not WinUAE's scsi.cpp. We
 *      synthesize INQUIRY/READ CAPACITY/MODE SENSE/TUR/REQUEST SENSE and move
 *      READ/WRITE block data over the existing PISCSI cross-core channel to
 *      core0's FatFS (devs[] unit index == SCSI target id; Amix.hdf = target 6).
 *
 * Runs on core1 (the 68k emulator). MMIO = the $00DD0000 page (a3000_scsi_bank).
 */

#include "uae/types.h"
#include "sysconfig.h"
#include "sysdeps.h"         // TCHAR, z3660_printf, base UAE types
#include <string.h>          // memset/memcpy
#include "memory.h"          // put_byte/get_byte/put_long/get_long (bank dispatch)
#include "../memorymap.h"    // SCSI_NO_DMA_ADDRESS (host-DDR bounce buffer)
#include "xil_cache.h"
#include "a3000_scsi.h"

/* z3660_printf is declared (extern "C" void) by sysdeps.h. */
/* core1 -> core0 cross-core PISCSI register channel (defined in main.cc). */
extern "C" void     write_scsi_register(uint16_t zaddr, uint32_t zdata, int type);
extern "C" uint32_t read_scsi_register(uint16_t zaddr, int type);

#define A3000SCSI_LOG 0
#if A3000SCSI_LOG
#define dbg(...) z3660_printf(__VA_ARGS__)
/* budget-limited trace for high-frequency events (probe + start of boot, then
 * goes quiet so bulk kernel reads don't flood the 115200 serial). */
static int a3000_dbg_budget = 40000;
#define dbgn(...) do{ if(a3000_dbg_budget>0){ a3000_dbg_budget--; z3660_printf(__VA_ARGS__);} }while(0)
#include "xtime_l.h"   /* ARM global timer for per-block timing diagnosis */
static XTime t_dma_total=0, t_gap_total=0, t_last_exit=0;
static unsigned long t_blocks=0;
extern "C" volatile uint32_t ps_count, ps_chip, ps_cia, ps_custom, ps_other, ps_lastaddr;
extern "C" volatile uint32_t custom_hist[256];
static uint32_t t_ps_last=0, t_chip_last=0, t_cia_last=0, t_cust_last=0, t_oth_last=0;
#else
#define dbg(...) do{}while(0)
#define dbgn(...) do{}while(0)
#endif

/* ---- PISCSI cross-core command codes (== Z3660/src/scsi/z3660_scsi_enums.h) ---- */
#define PISCSI_CMD_WRITE        0x00
#define PISCSI_CMD_READ         0x04
#define PISCSI_CMD_DRVNUM       0x08
#define PISCSI_CMD_DRVTYPE      0x0C
#define PISCSI_CMD_BLOCKS       0x10
#define PISCSI_CMD_CYLS         0x14
#define PISCSI_CMD_HEADS        0x18
#define PISCSI_CMD_SECS         0x1C
#define PISCSI_CMD_READ_ADDR1   0x20   /* -> piscsi_u32_read[0] = block (LBA)   */
#define PISCSI_CMD_READ_ADDR2   0x24   /* -> piscsi_u32_read[1] = byte count    */
#define PISCSI_CMD_READ_ADDR3   0x28   /* -> piscsi_u32_read[2] = DMA target    */
#define PISCSI_CMD_BLOCKSIZE    0x84
#define PISCSI_CMD_PDT          0xA0   /* peripheral device type: 0x00 direct-access disk, 0x05 CD-ROM.
                                        * LOCKSTEP: this local mirror MUST equal the firmware register value
                                        * in Z3660/src/scsi/z3660_scsi_enums.h (0xA0, landed in 86c6fe8) --
                                        * this .cpp does NOT include that header, so keep the two in sync. */
#define PISCSI_CMD_WRITE_ADDR1  0x240  /* -> piscsi_u32_write[0] = block (LBA)  */
#define PISCSI_CMD_WRITE_ADDR2  0x244  /* -> piscsi_u32_write[1] = byte count   */
#define PISCSI_CMD_WRITE_ADDR3  0x248  /* -> piscsi_u32_write[2] = DMA source   */

/* core0 bounce buffer cap (handle_piscsi_reg_write warns at >= 0x180000). */
#define A3000_DMA_CHUNK 0x100000u

/* ============================ register definitions ============================ */
/* SuperDMAC CNTR bits */
#define SCNTR_TCEN   (1<<5)
#define SCNTR_PREST  (1<<4)
#define SCNTR_PDMD   (1<<3)
#define SCNTR_INTEN  (1<<2)
#define SCNTR_DDIR   (1<<1)
#define SCNTR_IO_DX  (1<<0)
/* ISTR bits */
#define ISTR_INT_F   (1<<7)
#define ISTR_INTS    (1<<6)
#define ISTR_E_INT   (1<<5)
#define ISTR_INT_P   (1<<4)
#define ISTR_UE_INT  (1<<3)
#define ISTR_OE_INT  (1<<2)
#define ISTR_FF_FLG  (1<<1)
#define ISTR_FE_FLG  (1<<0)
/* WD33C93 register indices */
#define WD_OWN_ID             0x00
#define WD_CONTROL            0x01
#define WD_TIMEOUT_PERIOD     0x02
#define WD_CDB_1              0x03
#define WD_TARGET_LUN         0x0f
#define WD_COMMAND_PHASE      0x10
#define WD_SYNCHRONOUS_TRANSFER 0x11
#define WD_TRANSFER_COUNT_MSB 0x12
#define WD_TRANSFER_COUNT     0x13
#define WD_TRANSFER_COUNT_LSB 0x14
#define WD_DESTINATION_ID     0x15
#define WD_SOURCE_ID          0x16
#define WD_SCSI_STATUS        0x17
#define WD_COMMAND            0x18
#define WD_DATA               0x19
#define WD_QUEUE_TAG          0x1a
#define WD_AUXILIARY_STATUS   0x1f
/* WD commands */
#define WD_CMD_RESET          0x00
#define WD_CMD_ABORT          0x01
#define WD_CMD_ASSERT_ATN     0x02
#define WD_CMD_NEGATE_ACK     0x03
#define WD_CMD_DISCONNECT     0x04
#define WD_CMD_SEL_ATN        0x06
#define WD_CMD_SEL            0x07
#define WD_CMD_SEL_ATN_XFER   0x08
#define WD_CMD_SEL_XFER       0x09
#define WD_CMD_TRANS_ADDR     0x18
#define WD_CMD_TRANS_INFO     0x20
#define WD_CMD_TRANSFER_PAD   0x21
/* CSR status codes (subset) */
#define CSR_MSGIN             0x20
#define CSR_SELECT            0x11
#define CSR_SEL_XFER_DONE     0x16
#define CSR_XFER_DONE         0x18
#define CSR_INVALID           0x40
#define CSR_UNEXP_DISC        0x41
#define CSR_TIMEOUT           0x42
#define CSR_BAD_STATUS        0x45
#define CSR_UNEXP             0x48
#define CSR_DISC              0x85
#define CSR_SRV_REQ           0x88
/* SCSI bus phases (low nibble of CSR codes) */
#define PHS_DATA_OUT          0x00
#define PHS_DATA_IN           0x01
#define PHS_COMMAND           0x02
#define PHS_STATUS            0x03
#define PHS_MESS_OUT          0x06
#define PHS_MESS_IN           0x07
/* Auxiliary status bits */
#define ASR_INT               0x80
#define ASR_LCI               0x40
#define ASR_BSY               0x20
#define ASR_CIP               0x10
#define ASR_PE                0x02
#define ASR_DBR               0x01
/* WD_CONTROL bits (0x01) */
#define CTL_DMA               0x80
#define CTL_DBA_DMA           0x40
#define CTL_BURST_DMA         0x20
#define CTL_HHP               0x10
#define CTL_EDI               0x08   /* Ending Disconnect Interrupt enable */
#define CTL_IDI               0x04   /* Intermediate Disconnect Interrupt enable */

#define WD_STATUS_QUEUE       16   /* was 4; the AMIX intmask-hold can queue several completions before the ISR rte's */

/* ================================ state ================================ */
struct status_data { uae_u8 status; int irq; };

struct wd_chip_state {
   uae_u8 wdregs[32];
   uae_u8 sasr;          /* current register index */
   uae_u8 auxstatus;     /* ASR_INT master flag */
   int wd_selected;
   int wd_busy;
   int wd_data_avail;    /* -1 DMA pending, +1 PIO byte ready, 0 idle */
   int wd_dataoffset;
   uae_u8 wd_data[64];   /* shadow of recent bytes (debug parity with WinUAE) */
   uae_u8 wd_phase;      /* CSR code being prepared */
   int resetnodelay_active;
   struct status_data status[WD_STATUS_QUEUE];
   int queue_index;
   int async_pending;    /* ASYNC-DMA: a SEL_ATN_XFER deferred from the WD_COMMAND write — execute it (DMA+status)
                          * only when the guest leaves the level-2 ISR (intmask<2) or polls, so a3091intr's frame
                          * never nests a full cross-core command execution (the synchronous-DMA queue-corruption fix). */
   uae_u8 async_cmd;     /* the deferred WD command byte */
};

struct sdmac_state {
   uae_u32 dmac_cntr, dmac_istr, dmac_wtc, dmac_acr, dmac_dawr;
   int dmac_dma;         /* 0 idle, 1 running, -1 done */
   int dmac_eop_delay;   /* 2026-06-04 SDMAC END-OF-PROCESS model: pump-tick countdown modeling the real
                          * SuperDMAC's WTC-underflow -> DMA-done interrupt, which on real A3000 takes many
                          * bus cycles AFTER the START strobe. The emulator's do_dma is zero-latency, so the
                          * autonomous block-I/O completion can ripen + be delivered (via the per-access
                          * settle) WHILE a3091intr's in-ISR re-issue is still on the ISR stack -> curunitp
                          * desync -> the ~2 stranded page-ins. This counter withholds the block-I/O
                          * SEL_XFER_DONE until it elapses on the STEADY hsync pump (NOT collapsible by an
                          * AUX poll or a per-access settle), re-creating the hardware temporal separation. */
};

/* The selected target's command/data context (one outstanding command). */
struct scsi_xfer {
   uae_u8 cmd[16];
   int cmd_len;
   int direction;        /* <0 in (to host), >0 out (from host), 0 none, 2 command */
   uae_u32 data_len;     /* expected data-phase byte count */
   int offset;           /* bytes streamed (synthesized buffer path) */
   uae_u8 status;        /* SCSI status byte (0 GOOD, 2 CHECK CONDITION) */
   uae_u8 message[8];
   uae_u8 buffer[512];   /* synthesized response buffer (INQUIRY/SENSE/MODE/CAP) */
   int is_block_io;      /* READ/WRITE 6/10 -> bulk disk DMA path */
   uae_u32 lba;
   uae_u32 blocks;
   int unit;             /* devs[] index == SCSI target id */
};

static struct wd_chip_state wc;
static struct sdmac_state   sd;
static struct scsi_xfer     cur;

/* latched REQUEST SENSE data (sense key / ASC / ASCQ) per controller */
static uae_u8 sense_key, sense_asc, sense_ascq;

/* Set when the emulated A3000 SCSI is active (AMIX/UAE_030_MMU mode). Used by
 * cpu_emulator.cpp to auto-inject one mouse-button click past the AMIX boot
 * loader's "Press a mouse button to continue." prompt while unattended. */
extern "C" { volatile int a3000_amix_mode = 0; }
extern "C" { volatile int amix_scsi_trace = 0; }  /* [SCSITRACE] armed at AMIX cinit; rm before commit */
/* Interrupt-delay scope. The WD33C93 interrupt-delay countdown (set_status delay) is needed ONLY by
 * AMIX's step-by-step kernel sd-driver open (bare SEL/SEL_ATN + TRANS_INFO, each phase armed in the
 * ISR) so the SELECT and SRV_REQ INT2s stay spaced. The Kickstart ROM boot loader uses the autonomous
 * SEL_ATN_XFER and does NOT want the gap (it waits for one completion INT2); applying the countdown to
 * it was the boot-load regression. wd_delay_mode = 1 only inside wd_cmd_sel; 0 for wd_cmd_sel_xfer. */
static int wd_delay_mode = 0;
/* [SCSITRACE] rm before commit: trace the first N WD events at boot-load (before amix_scsi_trace arms
 * at cinit) so a boot-load handshake stall is visible without flooding the multi-thousand-block load. */
static int wd_early_trace = 200;   /* boot-load budget (RDB/early reads), then quiet for the bulk load */
static int wd_mount_trace = 800;   /* root-mount budget once amix_scsi_trace arms at cinit, then quiet */
static int wd_trace_on(void)
{
   if (amix_scsi_trace) {            /* kernel at cinit/s5mountroot: trace the root open, capped */
      if (wd_mount_trace > 0) { wd_mount_trace--; return 1; }
      return 0;
   }
   if (wd_early_trace > 0) { wd_early_trace--; return 1; }
   return 0;
}
// Set once the kernel image starts loading from UNIX_Boot (high LBA): lets the auto-
// click stop after the 1st (boot-loader) prompt so the 2nd (AMIX) prompt is left clean
// for a real mouse click.
extern "C" { volatile int amix_kernel_loaded = 0; }
// Set once AMIX's kernel enables its (2KB-page) 030 MMU = it has relocated to $07000000
// and no longer needs the boot RAM at $08000000. After this, phys_get/put hide $08000000
// so AMIX's RAM probe doesn't count it (AMIX needs $08000000+ free; too much RAM => vatosde).
extern "C" { volatile int amix_mmu_on = 0; }
extern "C" int amix_cpu_intmask(void);   /* guest CPU SR interrupt mask (newcpu.cpp) — gates AMIX completion delivery */
extern "C" uae_u32 amix_cpu_pc(void);    /* TEMP diag: guest PC + frame, to record WHO reads WD_SCSI_STATUS */
extern "C" uae_u32 amix_cpu_a6(void);
extern "C" uae_u32 amix_cpu_kread(uae_u32);
volatile uae_u32 amix_statrd_pc[8], amix_statrd_c1[8], amix_statrd_c2[8]; volatile int amix_statrd_head=0;  /* TEMP */
/* TEMP: per-completion guest state filled from newcpu.cpp a3091intr hooks (head=curunit->head buf, unit=curunitp) */
extern "C" { volatile uae_u32 amix_compl_tick[16], amix_compl_istate[16], amix_compl_csr[16], amix_compl_unit[16], amix_compl_head[16]; volatile int amix_compl_h=0; }

/* per-unit geometry cache (lazy cross-core fetch). pdt = peripheral device type
 * (0x00 direct-access disk, 0x05 CD-ROM) read from the PISCSI_CMD_PDT register. */
static struct { int valid, present; uae_u32 nblocks, bsize, cyls, heads, secs, pdt; } geo[8];
static int amix_id6_drv = -1;   // cached id-6 -> backend-drive decision (see drvnum_for_target); reset each boot

static void a3000_recompute_irq(void);

/* ===================== cross-core disk backend (core0) ===================== */

/* AMIX's kernel sd-driver opens its root disk at SCSI target id 6 (the AMIX root device is baked to
 * controller 0 / target 6). Normally SCSI id N maps to PISCSI backend drive N (devs[N], populated from
 * config.scsi_num[N]). For AMIX id 6 we PREFER the real id-6 backend when it exists (scsi6 -> Amix.hdf
 * in devs[6]) so id 6 is a genuine, RDB-readable, Kickstart-bootable target -- this is what lets AMIX
 * autoboot from id 6 ALONE (the Kickstart 3.1 RDB scan is ID-agnostic; it boots by BootPri, not id).
 * Only when no id-6 backend exists (legacy configs with Amix.hdf on scsi0/devs[0] only) do we fall back
 * to the old id-6 -> drive-0 alias, so those installs don't regress (their id-6 root-mount still lands on
 * the disk). Decided once per boot (drvnum_for_target is also on the hot READ/WRITE path); amix_id6_drv
 * is reset in a3000_scsi_init so a reconfigured reset re-probes. AMIX-mode only; legacy non-AMIX
 * UAE_030_MMU keeps the plain identity map. */
static int backend_drive_present(int drv)   // cross-core DRVNUM+DRVTYPE probe: does this PISCSI drive hold a disk?
{
   write_scsi_register(PISCSI_CMD_DRVNUM, drv, 2);
   return read_scsi_register(PISCSI_CMD_DRVTYPE, 2) ? 1 : 0;
}
static int drvnum_for_target(int unit)
{
   if (a3000_amix_mode && unit == 6) {
      if (amix_id6_drv < 0)
         amix_id6_drv = backend_drive_present(6) ? 6 : 0;   // prefer the real id-6 disk; else legacy drive-0
      return amix_id6_drv;
   }
   return unit;
}

static void fetch_geometry(int unit)
{
   if (unit < 0 || unit >= 8) return;
   if (geo[unit].valid) return;
   geo[unit].valid = 1;
   write_scsi_register(PISCSI_CMD_DRVNUM, drvnum_for_target(unit), 2);
   geo[unit].present = read_scsi_register(PISCSI_CMD_DRVTYPE, 2) ? 1 : 0;
   if (!geo[unit].present) return;
   geo[unit].nblocks = read_scsi_register(PISCSI_CMD_BLOCKS, 2);
   geo[unit].bsize   = read_scsi_register(PISCSI_CMD_BLOCKSIZE, 2);
   if (geo[unit].bsize == 0) geo[unit].bsize = 512;
   geo[unit].cyls    = read_scsi_register(PISCSI_CMD_CYLS, 2);
   geo[unit].heads   = read_scsi_register(PISCSI_CMD_HEADS, 2);
   geo[unit].secs    = read_scsi_register(PISCSI_CMD_SECS, 2);
   /* drive already selected (DRVNUM written above); read its peripheral device type.
    * bsize is 2048 for a CD-ROM (returned by the mailbox), so the READ/WRITE length
    * math below needs no change -- it already multiplies by geo[unit].bsize. */
   geo[unit].pdt     = read_scsi_register(PISCSI_CMD_PDT, 2);
   dbg("[A3000SCSI] unit %d geom C=%lu H=%lu S=%lu blocks=%lu bsize=%lu\n",
       unit, (unsigned long)geo[unit].cyls, (unsigned long)geo[unit].heads,
       (unsigned long)geo[unit].secs, (unsigned long)geo[unit].nblocks,
       (unsigned long)geo[unit].bsize);
}

static int target_present(int unit)
{
   fetch_geometry(unit);
   return (unit >= 0 && unit < 8) ? geo[unit].present : 0;
}

/* Copy a byte stream host-DDR bounce buffer <-> guest physical RAM via the bank
 * dispatch (correct for chip RAM on the physical bus and any fast-RAM backing).
 * Byte-wise to preserve Amiga byte order regardless of host endianness. */
static void copy_bounce_to_guest(uae_u32 gaddr, const volatile uae_u8 *src, uae_u32 len)
{
   for (uae_u32 i = 0; i < len; i++)
      put_byte(gaddr + i, src[i]);
}
static void copy_guest_to_bounce(volatile uae_u8 *dst, uae_u32 gaddr, uae_u32 len)
{
   for (uae_u32 i = 0; i < len; i++)
      dst[i] = (uae_u8)get_byte(gaddr + i);
}
/* TEMP write-persistence test: do AMIX-runtime disk WRITES actually stick? (degraded SD / read-only hdf would make
 * fsck loop re-writing the same blocks forever — matching the observed write-loop.) After a write, read the block
 * back and compare. */
volatile uae_u32 amix_wv_n = 0, amix_wv_ok = 0, amix_wv_fail = 0;

/* Bulk block transfer between Amix.hdf (devs[unit], core0 FatFS) and guest RAM
 * at the SuperDMAC ACR. Reuses the proven PISCSI READ/WRITE path: core0 stages
 * the FatFS I/O through SCSI_NO_DMA_ADDRESS (forced by a non-window target=0),
 * core1 scatters/gathers to the real guest address. Returns 1 on success. */
// ===== AMIX SCSI command ring (TEMP — remove before commit): last reads/writes before the hang =====
extern "C" { volatile uae_u32 amix_scmd_unit[16], amix_scmd_lba[16], amix_scmd_n[16], amix_scmd_w[16]; volatile int amix_scmd_head=0; }
static int a3000_scsi_dma(int unit, uae_u32 lba, uae_u32 bytecount, uae_u32 gaddr, int write)
{
   { int h=amix_scmd_head&15; amix_scmd_unit[h]=unit; amix_scmd_lba[h]=lba; amix_scmd_n[h]=bytecount/512; amix_scmd_w[h]=write; amix_scmd_head++; }
   volatile uae_u8 *bounce = (volatile uae_u8 *)SCSI_NO_DMA_ADDRESS;
   uae_u32 bsize = (unit >= 0 && unit < 8 && geo[unit].bsize) ? geo[unit].bsize : 512;
   if (!write && lba > 1700000) amix_kernel_loaded = 1; // past the boot-loader prompt
   dbgn("[SCSI] %c lba=%lu n=%lu acr=%08lX\n", write ? 'W' : 'R', (unsigned long)lba,
        (unsigned long)(bytecount / 512), (unsigned long)gaddr);
#if A3000SCSI_LOG
   XTime t_enter; XTime_GetTime(&t_enter);
   if (t_last_exit) t_gap_total += (t_enter - t_last_exit);   // guest-CPU time between reads
#endif

   while (bytecount > 0) {
      uae_u32 chunk = bytecount > A3000_DMA_CHUNK ? A3000_DMA_CHUNK : bytecount;
      if (write) {
         copy_guest_to_bounce(bounce, gaddr, chunk);
         Xil_DCacheFlushRange((INTPTR)SCSI_NO_DMA_ADDRESS, chunk);
         write_scsi_register(PISCSI_CMD_WRITE_ADDR1, lba, 2);
         write_scsi_register(PISCSI_CMD_WRITE_ADDR2, chunk, 2);
         write_scsi_register(PISCSI_CMD_WRITE_ADDR3, 0, 2);   /* target 0 -> bounce */
         write_scsi_register(PISCSI_CMD_WRITE, drvnum_for_target(unit), 2);
         /* WRITE-VERIFY (TEMP, first 32 AMIX writes): read the block back + compare the first 16 bytes. */
         if (amix_mmu_on && amix_wv_n < 32) {
            amix_wv_n++;
            uae_u8 saved[16]; for (int k = 0; k < 16; k++) saved[k] = bounce[k];   /* what we just wrote */
            write_scsi_register(PISCSI_CMD_READ_ADDR1, lba, 2);
            write_scsi_register(PISCSI_CMD_READ_ADDR2, chunk, 2);
            write_scsi_register(PISCSI_CMD_READ_ADDR3, 0, 2);
            write_scsi_register(PISCSI_CMD_READ, drvnum_for_target(unit), 2);
            Xil_DCacheInvalidateRange((INTPTR)SCSI_NO_DMA_ADDRESS, chunk);
            int mismatch = 0; for (int k = 0; k < 16; k++) if (bounce[k] != saved[k]) mismatch = 1;
            if (mismatch) { amix_wv_fail++;
               if (amix_wv_fail <= 4) z3660_printf("[WVERIFY] lba=%lu WROTE %02X%02X%02X%02X READ %02X%02X%02X%02X = WRITE DID NOT STICK\r\n",
                  (unsigned long)lba, saved[0],saved[1],saved[2],saved[3], (unsigned)bounce[0],(unsigned)bounce[1],(unsigned)bounce[2],(unsigned)bounce[3]);
            } else amix_wv_ok++;
         }
      } else {
         write_scsi_register(PISCSI_CMD_READ_ADDR1, lba, 2);
         write_scsi_register(PISCSI_CMD_READ_ADDR2, chunk, 2);
         write_scsi_register(PISCSI_CMD_READ_ADDR3, 0, 2);    /* target 0 -> bounce */
         write_scsi_register(PISCSI_CMD_READ, drvnum_for_target(unit), 2);
         Xil_DCacheInvalidateRange((INTPTR)SCSI_NO_DMA_ADDRESS, chunk);
         copy_bounce_to_guest(gaddr, bounce, chunk);
      }
      bytecount -= chunk;
      gaddr     += chunk;
      lba       += chunk / bsize;
   }
#if A3000SCSI_LOG
   XTime t_exit; XTime_GetTime(&t_exit);
   t_dma_total += (t_exit - t_enter);     // time inside this dma (cross-core + copy)
   t_last_exit = t_exit;
   if (((++t_blocks) % 100) == 0) {
      uint32_t ps_now=ps_count, chip=ps_chip, cia=ps_cia, cust=ps_custom, oth=ps_other;
      z3660_printf("[SCSI] gap_avg=%lu us /blk: chip=%lu cia=%lu custom=%lu other=%lu (lastaddr=%08lX)\n",
         (unsigned long)(t_gap_total * 1000000ULL / COUNTS_PER_SECOND / 100),
         (unsigned long)((chip - t_chip_last)/100), (unsigned long)((cia - t_cia_last)/100),
         (unsigned long)((cust - t_cust_last)/100), (unsigned long)((oth - t_oth_last)/100),
         (unsigned long)ps_lastaddr);
      t_dma_total=0; t_gap_total=0; t_ps_last=ps_now;
      t_chip_last=chip; t_cia_last=cia; t_cust_last=cust; t_oth_last=oth;
      /* top-3 custom-chip registers hammered (DFF000 + 2*idx) */
      int m1=0,m2=0,m3=0;
      for (int i=0;i<256;i++){ uint32_t v=custom_hist[i];
         if(v>custom_hist[m1]){m3=m2;m2=m1;m1=i;} else if(v>custom_hist[m2]){m3=m2;m2=i;} else if(v>custom_hist[m3])m3=i; }
      z3660_printf("[SCSI]   top custom regs: DFF%03X=%lu DFF%03X=%lu DFF%03X=%lu\n",
         m1*2, (unsigned long)custom_hist[m1], m2*2, (unsigned long)custom_hist[m2], m3*2, (unsigned long)custom_hist[m3]);
      for (int i=0;i<256;i++) custom_hist[i]=0;
   }
#endif
   return 1;
}

/* ===================== SCSI target (CDB) emulation ===================== */

static void set_sense(uae_u8 key, uae_u8 asc, uae_u8 ascq)
{
   sense_key = key; sense_asc = asc; sense_ascq = ascq;
}

/* parse the CDB in cur.cmd -> cur.direction, cur.data_len, cur.cmd_len, block io.
 * returns 1 if the command is understood. */
static int scsi_emulate_analyze(void)
{
   uae_u8 op = cur.cmd[0];
   /* log only non-data commands here (probe); READ/WRITE are logged in a3000_scsi_dma */
   if (op != 0x08 && op != 0x0a && op != 0x28 && op != 0x2a)
      dbgn("[SCSI] cmd op=0x%02X unit=%d\n", op, cur.unit);
   cur.is_block_io = 0;
   cur.status = 0;
   uae_u32 alloc;
   switch (op) {
   case 0x00: /* TEST UNIT READY */
   case 0x1b: /* START STOP UNIT */
   case 0x1e: /* PREVENT ALLOW MEDIUM REMOVAL (no-op: medium always present this phase) */
      cur.cmd_len = 6; cur.direction = 0; cur.data_len = 0; return 1;
   case 0x12: /* INQUIRY */
      cur.cmd_len = 6; cur.direction = -1;
      cur.data_len = cur.cmd[4]; if (cur.data_len > 36) cur.data_len = 36; return 1;
   case 0x03: /* REQUEST SENSE */
      cur.cmd_len = 6; cur.direction = -1;
      cur.data_len = cur.cmd[4]; if (cur.data_len > 18) cur.data_len = 18; return 1;
   case 0x25: /* READ CAPACITY (10) */
      cur.cmd_len = 10; cur.direction = -1; cur.data_len = 8; return 1;
   case 0x1a: /* MODE SENSE (6) */
      cur.cmd_len = 6; cur.direction = -1;
      cur.data_len = cur.cmd[4]; if (cur.data_len > 64) cur.data_len = 64; return 1;
   case 0x5a: /* MODE SENSE (10) */
      cur.cmd_len = 10; cur.direction = -1;
      alloc = (cur.cmd[7] << 8) | cur.cmd[8];
      cur.data_len = alloc > 64 ? 64 : alloc; return 1;
   case 0x08: /* READ (6) */
   case 0x0a: /* WRITE (6) */
      cur.cmd_len = 6;
      cur.lba = ((cur.cmd[1] & 0x1f) << 16) | (cur.cmd[2] << 8) | cur.cmd[3];
      cur.blocks = cur.cmd[4] ? cur.cmd[4] : 256;
      goto rw;
   case 0x28: /* READ (10) */
   case 0x2a: /* WRITE (10) */
      cur.cmd_len = 10;
      cur.lba = ((uae_u32)cur.cmd[2] << 24) | (cur.cmd[3] << 16) | (cur.cmd[4] << 8) | cur.cmd[5];
      cur.blocks = (cur.cmd[7] << 8) | cur.cmd[8];
   rw: {
      uae_u32 bsize = (cur.unit >= 0 && cur.unit < 8 && geo[cur.unit].bsize) ? geo[cur.unit].bsize : 512;
      int is_write = (op == 0x0a || op == 0x2a);
      /* Optical media is read-only: reject WRITE(6)/WRITE(10) to a CD-ROM with a
       * CHECK CONDITION / DATA PROTECT sense (the drive is genuinely unwritable). */
      if (is_write && cur.unit >= 0 && cur.unit < 8 && geo[cur.unit].pdt == 0x05) {
         cur.is_block_io = 0;
         cur.direction = 0; cur.data_len = 0;
         cur.status = 0x02;                     /* CHECK CONDITION */
         set_sense(0x07, 0x27, 0x00);           /* DATA PROTECT / write protected */
         return 0;
      }
      cur.is_block_io = 1;
      cur.direction = is_write ? 1 : -1;
      cur.data_len = cur.blocks * bsize;
      return 1;
   }
   default:
      dbg("[A3000SCSI] unsupported CDB 0x%02X\n", op);
      cur.direction = 0; cur.data_len = 0; cur.status = 0x02;     /* CHECK CONDITION */
      set_sense(0x05, 0x20, 0x00);                                /* ILLEGAL REQUEST / invalid command */
      return 0;
   }
}

static void put_be32(uae_u8 *p, uae_u32 v) { p[0]=v>>24; p[1]=v>>16; p[2]=v>>8; p[3]=v; }

/* execute the command: fill cur.buffer for synthesized reads, set status.
 * block READ/WRITE data is moved in do_dma(), so here they only set status. */
static void scsi_emulate_cmd(void)
{
   uae_u8 op = cur.cmd[0];
   uae_u8 *b = cur.buffer;
   fetch_geometry(cur.unit);
   uae_u32 nblocks = geo[cur.unit].nblocks ? geo[cur.unit].nblocks : 1;
   uae_u32 bsize   = geo[cur.unit].bsize   ? geo[cur.unit].bsize   : 512;

   if (cur.is_block_io) { cur.status = 0; return; }

   memset(b, 0, sizeof cur.buffer);
   switch (op) {
   case 0x00: /* TEST UNIT READY */
   case 0x1b: /* START STOP UNIT */
   case 0x1e: /* PREVENT ALLOW MEDIUM REMOVAL (no-op) */
      cur.status = 0; set_sense(0, 0, 0); break;
   case 0x12: /* INQUIRY */
      if (geo[cur.unit].pdt == 0x05) {
         b[0] = 0x05;         /* peripheral device type = CD-ROM */
         b[1] = 0x80;         /* RMB = 1 (removable medium) */
         b[2] = 0x02;         /* ANSI version 2 */
         b[3] = 0x02;         /* response data format 2 */
         b[4] = 0x1f;         /* additional length = 31 (total 36) */
         memcpy(b + 8,  "Z3660   ", 8);
         memcpy(b + 16, "AMIX CD-ROM     ", 16);
         memcpy(b + 32, "0.1 ", 4);
      } else {
         b[0] = 0x00;         /* direct-access device, qualifier 0 */
         b[1] = 0x00;         /* RMB = 0 (fixed disk) */
         b[2] = 0x02;         /* ANSI version 2 */
         b[3] = 0x02;         /* response data format 2 */
         b[4] = 0x1f;         /* additional length = 31 (total 36) */
         memcpy(b + 8,  "Z3660   ", 8);
         memcpy(b + 16, "AMIX SCSI Disk  ", 16);
         memcpy(b + 32, "0.1 ", 4);
      }
      cur.status = 0; break;
   case 0x03: /* REQUEST SENSE (fixed format) */
      b[0] = 0x70;            /* response code, valid=0 */
      b[2] = sense_key;       /* sense key */
      b[7] = 0x0a;            /* additional sense length = 10 */
      b[12] = sense_asc;
      b[13] = sense_ascq;
      cur.status = 0; break;
   case 0x25: /* READ CAPACITY (10) */
      put_be32(b + 0, nblocks - 1);   /* last LBA */
      put_be32(b + 4, bsize);         /* block length */
      cur.status = 0; break;
   case 0x1a: /* MODE SENSE (6) */
   case 0x5a: { /* MODE SENSE (10) */
      int ten = (op == 0x5a);
      int is_cd = (geo[cur.unit].pdt == 0x05);   /* optical media: write-protected, no rigid geometry */
      uae_u8 page = cur.cmd[2] & 0x3f;
      uae_u8 *bd, *pg;
      int hdr, bdlen = 8;
      if (ten) {
         hdr = 8;
         /* b[0..1] data length filled below; b[2]=medium, b[3]=devspec, b[6..7]=bd len */
         b[3] = is_cd ? 0x80 : 0;   /* device-specific: WP bit set for read-only optical media */
         b[6] = 0; b[7] = bdlen;
      } else {
         hdr = 4;
         b[1] = 0;             /* medium type */
         b[2] = is_cd ? 0x80 : 0;   /* device-specific: WP bit set for read-only optical media */
         b[3] = bdlen;         /* block descriptor length */
      }
      bd = b + hdr;
      bd[0] = 0;                              /* density code */
      bd[1] = (nblocks >> 16) & 0xff;         /* number of blocks (24-bit) */
      bd[2] = (nblocks >> 8) & 0xff;
      bd[3] = nblocks & 0xff;
      bd[4] = 0;                              /* reserved */
      bd[5] = (bsize >> 16) & 0xff;           /* block length (24-bit) */
      bd[6] = (bsize >> 8) & 0xff;
      bd[7] = bsize & 0xff;
      pg = bd + bdlen;
      int pglen = 0;
      /* Rigid-disk mode pages (Format Device 0x03, Rigid Drive Geometry 0x04) are
       * meaningless for optical media -- omit them for a CD-ROM. A single-session
       * data ISO needs only INQUIRY + READ CAPACITY(2048) + READ(10) + TUR. */
      if (!is_cd && (page == 0x03 || page == 0x3f)) {     /* Format Device */
         pg[0] = 0x03; pg[1] = 0x16;
         pg[10] = (geo[cur.unit].secs >> 8) & 0xff;   /* sectors per track */
         pg[11] = geo[cur.unit].secs & 0xff;
         pg[12] = (bsize >> 8) & 0xff;                /* bytes per sector */
         pg[13] = bsize & 0xff;
         pg[15] = 0x01;                               /* interleave */
         pglen += 0x18;
         pg += 0x18;
      }
      if (!is_cd && (page == 0x04 || page == 0x3f)) {     /* Rigid Drive Geometry */
         pg[0] = 0x04; pg[1] = 0x16;
         pg[2] = (geo[cur.unit].cyls >> 16) & 0xff;   /* cylinders (24-bit) */
         pg[3] = (geo[cur.unit].cyls >> 8) & 0xff;
         pg[4] = geo[cur.unit].cyls & 0xff;
         pg[5] = geo[cur.unit].heads & 0xff;          /* heads */
         pg[20] = (5400 >> 8) & 0xff;                 /* medium rotation rate */
         pg[21] = 5400 & 0xff;
         pglen += 0x18;
      }
      if (ten) {
         uae_u32 total = hdr + bdlen + pglen - 2;     /* mode data length excludes its own 2 bytes */
         b[0] = (total >> 8) & 0xff; b[1] = total & 0xff;
      } else {
         b[0] = (uae_u8)(hdr + bdlen + pglen - 1);    /* mode data length excludes its own byte */
      }
      cur.status = 0; break;
   }
   default:
      cur.status = 0x02; set_sense(0x05, 0x20, 0x00); break;
   }
}

static void scsi_start_transfer(void) { cur.offset = 0; }

/* per-byte target stream (synthesized commands + PIO). returns 1 on last byte. */
static int scsi_receive_data(uae_u8 *b)   /* target -> host */
{
   *b = (cur.offset < (int)sizeof cur.buffer) ? cur.buffer[cur.offset] : 0;
   cur.offset++;
   return cur.offset >= (int)cur.data_len ? 1 : 0;
}
static int scsi_send_data(uae_u8 v)       /* host -> target */
{
   if (cur.direction == 2) {              /* command phase: collect CDB */
      if (cur.offset < (int)sizeof cur.cmd) cur.cmd[cur.offset] = v;
   } else {
      if (cur.offset < (int)sizeof cur.buffer) cur.buffer[cur.offset] = v;
   }
   cur.offset++;
   return cur.offset >= (int)cur.data_len ? 1 : 0;
}

/* ===================== WD33C93 low-level helpers ===================== */

static uae_u32 gettc(void)
{
   return wc.wdregs[WD_TRANSFER_COUNT_LSB] | (wc.wdregs[WD_TRANSFER_COUNT] << 8) |
          (wc.wdregs[WD_TRANSFER_COUNT_MSB] << 16);
}
static void settc(uae_u32 tc)
{
   wc.wdregs[WD_TRANSFER_COUNT_LSB] = tc & 0xff;
   wc.wdregs[WD_TRANSFER_COUNT]     = (tc >> 8) & 0xff;
   wc.wdregs[WD_TRANSFER_COUNT_MSB] = (tc >> 16) & 0xff;
}
static int decreasetc(void)
{
   uae_u32 tc = gettc();
   if (!tc) return 1;
   tc--;
   settc(tc);
   return tc == 0;
}
static void setphase(uae_u8 phase) { wc.wdregs[WD_COMMAND_PHASE] = phase; }

static void incsasr(int w)
{
   if (wc.sasr == WD_AUXILIARY_STATUS || wc.sasr == WD_DATA || wc.sasr == WD_COMMAND)
      return;
   if (w && wc.sasr == WD_SCSI_STATUS)
      return;
   wc.sasr++;
   wc.sasr &= 0x1f;
}
static int writeonlyreg(int reg) { return reg == WD_SCSI_STATUS ? 1 : 0; }
static void writewdreg(int sasr, uae_u8 val)
{
   if (sasr > WD_QUEUE_TAG && sasr < WD_AUXILIARY_STATUS) return;
   if (sasr == WD_QUEUE_TAG) return;            /* B-revision only; we emulate A */
   wc.wdregs[sasr] = val;
}

volatile uae_u32 amix_setst_n = 0, amix_assert_n = 0;   // TEMP: statuses queued vs INT2 0->2 assertions
volatile uae_u32 amix_burst_depth = 0;                  // TEMP: max wc.queue_index ever (FIFO pileup test: >2 => clustered burst; <=2 => serial, refutes interleave)
volatile uae_u32 amix_dlv_sxd = 0, amix_dlv_disc = 0, amix_dlv_other = 0;  // TEMP: delivered-status type counts (SEL_XFER_DONE vs DISC vs other)
volatile uae_u32 amix_dlv_seq[24]; volatile int amix_dlv_seqh = 0;         // TEMP: last-24 delivered CSR sequence (to see DISC interleaving SEL_XFER_DONEs)
volatile uae_u32 amix_tick = 0;                          // TEMP: ~guest-instruction counter (bumped in check_uae_int_request)
volatile uae_u32 amix_assert_tick = 0, amix_acklat = 0, amix_acklatmax = 0; // TEMP: INT2 assert->ack delivery latency
volatile uae_u32 amix_clobber_int = 0, amix_clobber_q = 0; // TEMP: pending completion / queued statuses clobbered by a new SEL_ATN_XFER
volatile uae_u32 amix_qovf = 0; // TEMP: status-queue overflow drops (a dropped completion = a stranded I/O)
volatile uae_u32 amix_disc_q = 0;          // TEMP: trailing CSR_DISC statuses QUEUED by wd_cmd_sel_xfer (is CTL_EDI clear?)
volatile uae_u32 amix_disc_dropped = 0;    // # trailing CSR_DISC dropped at istate==0 (the root-cause fix firing)
volatile uae_u32 amix_delv_istate0 = 0;    // # statuses DELIVERED while guest istate==0 (should drop toward 0 with the fix)
static uae_u32 amix_delv_csr[16], amix_delv_ist[16]; static int amix_delv_h = 0;  // ring of (csr,istate) at delivery
volatile uae_u32 amix_rnd_fire = 0;        // TEMP: resetnodelay queue-clears on a WD_SCSI_STATUS ack
volatile uae_u32 amix_rnd_disc = 0;        // TEMP: total queued statuses discarded by resetnodelay
volatile uae_u32 amix_rnd_compl = 0;       // TEMP: CSR_SEL_XFER_DONE completions discarded by resetnodelay (= LOST completions!)
volatile uae_u32 amix_async_exec_n = 0;    // TEMP: # deferred (async-DMA) SEL_ATN_XFER commands actually executed
static int amix_aux_streak = 0;   // consecutive WD AUX-status reads with no other register touch between (poll detector)
/* DESYNC DETECTOR (2026-06-03): a3091intr's COMPLETE arm biodones curunitp->4 (the unit's current request). If curunitp
 * (or its ->4) MOVED between the cmd=08 issue and the WD_SCSI_STATUS completion ack, the completion biodones the WRONG buf
 * = the intermittent lost-completion that strands a buf_breakup chunk. Capture curunitp->4 at issue vs at completion. */
volatile uae_u32 amix_issued_req = 0, amix_desync_n = 0, amix_match_n = 0;
volatile uae_u32 amix_dsy_iss[8], amix_dsy_cmp[8], amix_dsy_cu[8]; volatile int amix_dsy_h = 0;
static uae_u32 amix_cpu_curunitp_req(void)   /* curunitp->4 (the request a3091intr's COMPLETE arm will biodone) */
{
   #define GB4(a) ((uae_u32)(((uae_u32)get_byte(a)<<24)|((uae_u32)get_byte((a)+1)<<16)|((uae_u32)get_byte((a)+2)<<8)|(uae_u32)get_byte((a)+3)))
   uae_u32 cpa = GB4(0x0700D176u);            /* &curunitp (operand of a3091intr movea.l curunitp,a3 @d174) */
   uae_u32 cu  = GB4(cpa);
   return cu ? GB4(cu + 4) : 0;
   #undef GB4
}

/* SDMAC interrupt level == WinUAE isirq(COMMODORE_SDMAC), folded into the
 * software Amiga INT2 line a3000_scsi_irq. ASR_INT is the master. */
static void a3000_recompute_irq(void)
{
   if (wc.auxstatus & ASR_INT)
      sd.dmac_istr |= ISTR_INTS | ISTR_INT_F;
   else
      sd.dmac_istr &= ~ISTR_INT_F;
   int line = 0;
   if ((sd.dmac_cntr & SCNTR_INTEN) && (sd.dmac_istr & (ISTR_INTS | ISTR_E_INT)))
      line = 2;
   if (line == 2 && a3000_scsi_irq != 2) { amix_assert_n++; amix_assert_tick = amix_tick; }   // TEMP: count + stamp 0->2 INT2 assertions
   a3000_scsi_irq = line;
}

// TEMP diagnostic: dump the INT2-relevant SCSI state at a stall (remove before commit).
extern "C" void a3000_scsi_dumpstate(void)
{
   z3660_printf("[SCSIST] irq=%d mmu_on=%d cntr=%02X INTEN=%d asr=%02X ASRINT=%d qi=%d q0irq=%d busy=%d setst=%lu assert=%lu acklat=%lu acklatmax=%lu\r\n",
      (int)a3000_scsi_irq, (int)amix_mmu_on, (unsigned)sd.dmac_cntr, (sd.dmac_cntr & SCNTR_INTEN) ? 1 : 0,
      (unsigned)wc.auxstatus, (wc.auxstatus & ASR_INT) ? 1 : 0,
      wc.queue_index, wc.queue_index > 0 ? wc.status[0].irq : -1, wc.wd_busy,
      (unsigned long)amix_setst_n, (unsigned long)amix_assert_n,
      (unsigned long)amix_acklat, (unsigned long)amix_acklatmax);
   z3660_printf("[SCSIST2] clobber_int=%lu clobber_q=%lu qovf=%lu disc_q=%lu disc_drop=%lu delv_ist0=%lu rnd_fire=%lu rnd_disc=%lu rnd_compl=%lu\r\n",
      (unsigned long)amix_clobber_int, (unsigned long)amix_clobber_q, (unsigned long)amix_qovf,
      (unsigned long)amix_disc_q, (unsigned long)amix_disc_dropped, (unsigned long)amix_delv_istate0,
      (unsigned long)amix_rnd_fire, (unsigned long)amix_rnd_disc, (unsigned long)amix_rnd_compl);
   z3660_printf("[SCSIST3] async_exec=%lu async_pending=%d wverify_ok=%lu wverify_FAIL=%lu\r\n",
      (unsigned long)amix_async_exec_n, wc.async_pending, (unsigned long)amix_wv_ok, (unsigned long)amix_wv_fail);
   z3660_printf("[BURST] peak_queue_depth=%lu delivered: SEL_XFER_DONE=%lu DISC=%lu other=%lu\r\n",
      (unsigned long)amix_burst_depth, (unsigned long)amix_dlv_sxd, (unsigned long)amix_dlv_disc, (unsigned long)amix_dlv_other);
   { z3660_printf("[DLVSEQ]"); for (int k = 0; k < 24; k++) { int i = (amix_dlv_seqh - 24 + k); if (i < 0) i += 24; i %= 24;
        z3660_printf(" %02lX", (unsigned long)amix_dlv_seq[i]); } z3660_printf("  (16=SEL_XFER_DONE 85=DISC)\r\n"); }
   z3660_printf("[DESYNC] curunitp->4 moved issue->completion: n=%lu (matched=%lu) last_issued=%08lX\r\n",
      (unsigned long)amix_desync_n, (unsigned long)amix_match_n, (unsigned long)amix_issued_req);
   for (int k = 0; k < 8; k++) { int i = (amix_dsy_h - 8 + k) & 7;
      if (amix_dsy_iss[i] || amix_dsy_cmp[i])
         z3660_printf("[DESYNC] issued_req=%08lX completed_req=%08lX csr=%02lX\r\n",
            (unsigned long)amix_dsy_iss[i], (unsigned long)amix_dsy_cmp[i], (unsigned long)amix_dsy_cu[i]); }
   { z3660_printf("[DELV]"); for (int k = 0; k < 16; k++) { int i = (amix_delv_h - 16 + k) & 15;
        z3660_printf(" %02lX@%lu", (unsigned long)amix_delv_csr[i], (unsigned long)amix_delv_ist[i]); } z3660_printf("\r\n"); }
   for (int k = 0; k < 8; k++) { int i = (amix_statrd_head - 8 + k) & 7;
      z3660_printf("[STATRD] pc=%08lX caller=%08lX cc=%08lX\r\n",
         (unsigned long)amix_statrd_pc[i], (unsigned long)amix_statrd_c1[i], (unsigned long)amix_statrd_c2[i]); }
   for (int k = 0; k < 16; k++) { int i = (amix_compl_h - 16 + k) & 15;
      z3660_printf("[COMPL] t=%lu ist=%lu csr=%02lX unit=%08lX head=%08lX\r\n",
         (unsigned long)amix_compl_tick[i], (unsigned long)amix_compl_istate[i], (unsigned long)amix_compl_csr[i],
         (unsigned long)amix_compl_unit[i], (unsigned long)amix_compl_head[i]); }
}

/* TEMP: dump the GUEST a3091 driver queue directly. Kernel .data (istate/curunitp/units, VA 0x07xxxxxx) is
 * IDENTITY-mapped => read via get_byte (PHYSICAL); it is NOT in the srp page tables so amix_kget2 fails for it.
 * The sc/buf structs are at VA 0x40xxxxxx (page-table mapped) => read via amix_cpu_kread (srp walk). Addresses of
 * the .data globals come from the linker-patched operands in .text (a3091queue/startany/a3091intr). */
extern "C" void a3000_scsi_dumpqueue(void)
{
   #define PL(a) ((uae_u32)(((uae_u32)get_byte(a)<<24)|((uae_u32)get_byte((a)+1)<<16)|((uae_u32)get_byte((a)+2)<<8)|(uae_u32)get_byte((a)+3)))
   uae_u32 tst = PL(0x0700D014u);                 /* sanity: tst.l istate opcode, expect 0x4AB9xxxx */
   uae_u32 ia = PL(0x0700D016u);                  /* &istate   (operand of startany tst.l istate @d014) */
   uae_u32 cpa = PL(0x0700D176u);                 /* &curunitp (operand of a3091intr movea.l curunitp,a3 @d174) */
   uae_u32 sha = PL(0x0700D020u);                 /* &starthead(operand of startany movea.l starthead,a4 @d01e) */
   uae_u32 ua = PL(0x0700CF8Cu);                  /* units base(operand of a3091queue adda.l #units,a2 @cf8a) */
   uae_u32 istate = PL(ia), curunitp = PL(cpa), starthead = PL(sha);
   uae_u32 unit6 = ua + 0x60;                      /* units[6] (16-byte units, id<<4) */
   uae_u32 u6head = PL(unit6 + 4), u6tail = PL(unit6 + 8);
   z3660_printf("[QUEUE] tst=%08lX istate@%08lX=%lu curunitp@%08lX=%08lX starthead@%08lX=%08lX units=%08lX u6.head=%08lX u6.tail=%08lX\r\n",
      (unsigned long)tst, (unsigned long)ia, (unsigned long)istate, (unsigned long)cpa, (unsigned long)curunitp,
      (unsigned long)sha, (unsigned long)starthead, (unsigned long)ua, (unsigned long)u6head, (unsigned long)u6tail);
   /* unit6 command chain: head -> sc[0] -> sc[0] -> ... (a3091queue threads via sc+0x00); sc is a buf @0x40xxxxxx */
   uae_u32 b = u6head;
   for (int i = 0; i < 12 && b && b != 0xDEADBEEFu && (b >> 28) == 4; i++) {
      uae_u32 nxt = amix_cpu_kread(b + 0), blk = amix_cpu_kread(b + 0x28), bc = amix_cpu_kread(b + 0x20);
      z3660_printf("[QCHAIN] +%d sc=%08lX blkno=%lu bcount=%lX next=%08lX\r\n", i,
         (unsigned long)b, (unsigned long)blk, (unsigned long)bc, (unsigned long)nxt);
      b = nxt;
   }
   /* starthead ready-unit list: unit -> unit[0] -> ... (uqueue threads units via unit+0x00); units are .data 0x07xxxxxx */
   uae_u32 u = starthead;
   for (int i = 0; i < 6 && u && (u >> 24) == 7; i++) {
      uae_u32 unxt = PL(u + 0), uhead = PL(u + 4);
      z3660_printf("[QREADY] +%d unit=%08lX head=%08lX next=%08lX\r\n", i, (unsigned long)u, (unsigned long)uhead, (unsigned long)unxt);
      u = unxt;
   }
   /* ddtab device-state machine: ddtab@.data 0x36e0 -> runtime 0x070EA740; entry[lun0,tgt] = base + tgt*0x44;
    * +0=state(0/1/2), +4=head buf, +8=tail buf, +0xc=embedded q. If state!=0 (busy) while units[tgt] is empty
    * (idle), ddstrategy queues new bufs WITHOUT startio -> the page-in read never issues. Dump all 8 targets. */
   for (int t = 0; t < 8; t++) {
      uae_u32 dd = 0x070EA740u + (uae_u32)t * 0x44u;
      uae_u32 st = PL(dd + 0), hd = PL(dd + 4), tl = PL(dd + 8);
      if (st || hd || tl)
         z3660_printf("[DDTAB] tgt%d @%08lX state=%lu head=%08lX tail=%08lX\r\n", t, (unsigned long)dd,
            (unsigned long)st, (unsigned long)hd, (unsigned long)tl);
   }
   /* target-6 ddtab Q1 buf chain (av_forw @ bp+0xc; bufs 0x40xxxxxx -> amix_cpu_kread) + each buf's b_blkno(+0x28) */
   { uae_u32 b = PL(0x070EA740u + 6 * 0x44u + 4);   /* ddtab[6].head */
     for (int i = 0; i < 10 && b && (b >> 28) == 4; i++) {
        uae_u32 nxt = amix_cpu_kread(b + 0xc), blk = amix_cpu_kread(b + 0x28), fl = amix_cpu_kread(b + 0);
        z3660_printf("[DDQ1] +%d bp=%08lX bflags=%08lX blkno=%lu av_forw=%08lX\r\n", i,
           (unsigned long)b, (unsigned long)fl, (unsigned long)blk, (unsigned long)nxt);
        b = nxt;
     } }
   #undef PL
}

/* Read the GUEST a3091 driver's istate (.data, physical/identity-mapped). &istate comes from startany's patched
 * tst.l operand @ runtime 0x0700D016 (verified vs reloc R_68K_32->istate). Used to gate trailing completion statuses. */
static int amix_cpu_istate(void)
{
   #define GB4(a) ((uae_u32)(((uae_u32)get_byte(a)<<24)|((uae_u32)get_byte((a)+1)<<16)|((uae_u32)get_byte((a)+2)<<8)|(uae_u32)get_byte((a)+3)))
   uae_u32 ia = GB4(0x0700D016u);
   return (int)GB4(ia);
   #undef GB4
}
/* ROOT-CAUSE FIX + diag (workflow 2026-06-03): each autonomous SEL_ATN_XFER can queue a TRAILING CSR_DISC after the
 * real CSR_SEL_XFER_DONE. a3091intr is single-shot per INT2; when the unit queue has drained the driver is at istate==0,
 * and the trailing DISC's INT2 then dispatches BACK INTO the COMPLETE arm (atab[0*9+itab[0x85]]) which biodone's a
 * stale/advanced/NULL curunitp->head -> orphans the buf a process biowait's on -> hard idle. Fix: drop a trailing
 * CSR_DISC delivered at guest istate==0 (the real completion is always delivered at istate>=1, so it is untouched).
 * Counters amix_disc_q/amix_disc_dropped/amix_delv_istate0 + the [DELV] ring are defined above near amix_qovf. */

static void doscsistatus(uae_u8 status)
{
   if (wd_trace_on()) dbg("[WDINT] csr=%02X qd=%d irq=%d\n", status, wc.queue_index,
      a3000_scsi_irq);  /* [SCSITRACE] rm before commit */
   wc.wdregs[WD_SCSI_STATUS] = status;
   wc.auxstatus |= ASR_INT;
   if (status == CSR_SEL_XFER_DONE) amix_dlv_sxd++; else if (status == CSR_DISC) amix_dlv_disc++; else amix_dlv_other++;  // TEMP: delivered-type
   { int h = amix_dlv_seqh % 24; amix_dlv_seq[h] = status; amix_dlv_seqh++; }   // TEMP: delivery sequence ring
   amix_aux_streak = 0;   /* a fresh completion starts a new poll context (so a3091intr's entry cipwait can't inherit a streak) */
   if (amix_mmu_on) { int ist = amix_cpu_istate();   /* TEMP diag: which CSR delivered at which guest istate */
      int h = amix_delv_h & 15; amix_delv_csr[h] = status; amix_delv_ist[h] = (uae_u32)ist; amix_delv_h++;
      if (ist == 0) amix_delv_istate0++; }
   a3000_recompute_irq();
}

/* Queue a status with WinUAE's interrupt-delay countdown (a2091.cpp set_status). The delay (in
 * a3000_scsi_hsync() pump ticks) SPACES queued interrupts so the guest ISR runs BETWEEN them.
 * The AMIX sd-driver's step-by-step SEL+TRANS_INFO open needs that gap (the Kickstart ROM uses
 * the autonomous SEL_ATN_XFER and does not). irq counts DOWN one per pump tick, fires at 1;
 * delay 0 => fire on the next check. (Previously the delay was discarded => irq always 1 =>
 * CSR_SELECT and the following CSR_SRV_REQ were delivered back-to-back, mis-sequencing sdopen.) */
static void set_status(uae_u8 status, int delay)
{
   if (wc.queue_index >= WD_STATUS_QUEUE) { amix_qovf++; dbg("[A3000SCSI] int queue overflow\n"); return; }   // TEMP counter
   amix_setst_n++;   // TEMP: count queued statuses
   /* ROM load (autonomous SEL_ATN_XFER, amix_mmu_on==0) keeps every status immediate (irq=1) = the
    * proven kernel-load behavior. AMIX runtime (amix_mmu_on==1) AND the step-by-step open honor the
    * WinUAE countdown so the autonomous SEL_ATN_XFER's two completion statuses (SEL_XFER_DONE then
    * DISC) are spaced by hsync ticks, letting a3091intr run and rte between them.
    * (2026-06-03 PACED-COMPLETION FIX: the earlier `|| amix_mmu_on` attempt failed because the
    * band-aids (intmask-hold gate D + DISC-drop gate E) and the async-DMA defer double-deferred on top
    * of the delay; those are now REMOVED so the countdown is the SOLE deferral mechanism.) */
   int eff = (wd_delay_mode || amix_mmu_on) ? delay : 0;
   int irq = (eff == 0) ? 1 : (eff <= 2 ? 2 : eff);
   wc.status[wc.queue_index].status = status;
   wc.status[wc.queue_index].irq = irq;
   wc.queue_index++;
   if ((uae_u32)wc.queue_index > amix_burst_depth) amix_burst_depth = wc.queue_index;   // TEMP: peak FIFO depth
   if (wd_trace_on()) dbg("[WDQ] st=%02X dly=%d eff=%d irq=%d qi=%d m=%d\n",
      status, delay, eff, irq, wc.queue_index, wd_delay_mode);  /* [SCSITRACE] rm before commit */
}
static void set_status0(uae_u8 status) { set_status(status, 0); }

/* Deliver the next queued status honoring WinUAE's countdown (a2091.cpp 1453-1466). A status
 * fires only once its irq has counted down to 1. On a pump tick (checkonly==0) we decrement a
 * still-counting head. checkonly==1 (a probe/ack, e.g. the WD_SCSI_STATUS ack read or any
 * per-access settle) MAY deliver an already-ripe status but must NEVER advance the countdown,
 * so the next status cannot fire back-to-back inside the same access. One pending at a time. */
static void wd_check_interrupt(int checkonly, int force)
{
   if (wc.auxstatus & ASR_INT) return;        /* one pending at a time */
   if (wc.queue_index == 0) return;
   if (wc.status[0].irq > 1) {                /* still counting down */
      if (!checkonly) wc.status[0].irq--;
      if (!force) return;                     /* a genuine AUX poll (force=1) force-ripens NOW */
      wc.status[0].irq = 1;                   /* otherwise hold until the hsync tick ripens it */
   }
   /* PACED-COMPLETION FIX (2026-06-03): the per-status hsync countdown (set_status irq + the gate
    * above) now provides the spacing that keeps a3091intr's in-ISR re-issued completion from landing
    * before the ISR rte's. The two former band-aids -- the intmask>=2 hold and the CSR_DISC-at-istate0
    * drop -- are REMOVED: they double-deferred on top of the countdown and dropped a legitimate INT2. */
   wc.status[0].irq = 0;
   doscsistatus(wc.status[0].status);
   wc.wd_busy = 0;
   if (wc.queue_index >= 2) {                  /* shift the rest down, preserving each queued delay */
      for (int i = 1; i < wc.queue_index; i++)
         wc.status[i - 1] = wc.status[i];
      wc.queue_index--;
      /* WinUAE-style: the promoted head needs ONE MORE hsync tick after the guest acks the status we
       * just delivered (ASR_INT held meanwhile), so its INT2 lands on a later pump tick AFTER the
       * current ISR rte's. Re-arm to irq=2 so the per-access checkonly pump (settle / WD_SCSI_STATUS
       * ack) cannot deliver it inside the same ISR (our gate delivers any irq<=1 on checkonly). */
      if (amix_mmu_on && wc.status[0].irq <= 1)
         wc.status[0].irq = 2;
   } else {
      wc.queue_index = 0;
   }
}

static void dmac_reset(void) { dbg("[A3000SCSI] SCSI reset\n"); }
static void dmac_cint(void) { sd.dmac_istr = 0; sd.dmac_eop_delay = 0; a3000_recompute_irq(); }
static void scsi_dmac_start_dma(void) { sd.dmac_dma = 1; }
static void scsi_dmac_stop_dma(void) { sd.dmac_dma = 0; sd.dmac_istr &= ~ISTR_E_INT; sd.dmac_eop_delay = 0; }
static void set_dma_done(void) { sd.dmac_dma = -1; }
static int  is_dma_enabled(void) { return sd.dmac_dma > 0; }

static int canwddma(void)
{
   uae_u8 mode = wc.wdregs[WD_CONTROL] >> 5;
   return (mode == 4 || mode == 1) ? 1 : 0;   /* SDMAC: DBA or single-byte DMA */
}

/* Move the data phase. Block READ/WRITE go bulk via a3000_scsi_dma; synthesized
 * responses stream byte-wise from cur.buffer to guest RAM at ACR (faithful to
 * WinUAE do_dma_commodore, whose acr post-increments per byte). */
static int do_dma(void)
{
   if (cur.is_block_io) {
      uae_u32 bytes = cur.data_len;
      int ok = a3000_scsi_dma(cur.unit, cur.lba, bytes, sd.dmac_acr, cur.direction > 0 ? 1 : 0);
      cur.offset = cur.data_len;
      sd.dmac_acr += bytes;
      settc(0);
      if (!ok) { cur.status = 0x02; set_sense(0x04, 0x00, 0x00); }   /* HARDWARE ERROR */
      return 1;
   }
   if (cur.direction < 0) {                    /* target -> Amiga RAM */
      int run = 1;
      while (run) {
         uae_u8 v;
         int status = scsi_receive_data(&v);
         put_byte(sd.dmac_acr, v);
         sd.dmac_acr++;
         if (wc.wd_dataoffset < (int)sizeof wc.wd_data) wc.wd_data[wc.wd_dataoffset++] = v;
         if (decreasetc()) run = 0;
         if (status) run = 0;
      }
      return 1;
   } else if (cur.direction > 0) {             /* Amiga RAM -> target */
      int run = 1;
      while (run) {
         uae_u8 v = (uae_u8)get_byte(sd.dmac_acr);
         sd.dmac_acr++;
         if (wc.wd_dataoffset < (int)sizeof wc.wd_data) wc.wd_data[wc.wd_dataoffset++] = v;
         int status = scsi_send_data(v);
         if (decreasetc()) run = 0;
         if (status) run = 0;
      }
      return 1;
   }
   return 0;
}

/* ===================== WD33C93 command handlers ===================== */
static void wd_cmd_sel_xfer(int atn);

static int wd_do_transfer_out(void)
{
   if (wc.wdregs[WD_COMMAND_PHASE] < 0x20) {
      int msg = cur.buffer[0];
      setphase(0x20);
      wc.wd_phase = CSR_XFER_DONE | PHS_COMMAND;
      cur.status = 0;
      scsi_start_transfer();
      cur.message[0] = msg;
   } else if (wc.wdregs[WD_COMMAND_PHASE] == 0x30) {
      if (cur.offset < (int)cur.data_len) {
         wc.wd_phase = CSR_XFER_DONE | PHS_COMMAND;
         setphase(0x30 + cur.offset);
         set_status(wc.wd_phase, 1);
         return 0;
      }
      settc(0);
      scsi_start_transfer();
      scsi_emulate_analyze();
      if (cur.direction > 0) {
         if (cur.data_len == 0 || cur.direction == 0) {
            wc.wd_phase = CSR_XFER_DONE | PHS_STATUS; setphase(0x46);
         } else {
            wc.wd_phase = CSR_XFER_DONE | PHS_DATA_OUT; setphase(0x45);
         }
      } else {
         scsi_emulate_cmd();
         if ((int)cur.data_len <= 0 || cur.direction == 0) {
            wc.wd_phase = CSR_XFER_DONE | PHS_STATUS; setphase(0x46);
         } else {
            wc.wd_phase = CSR_XFER_DONE | PHS_DATA_IN; setphase(0x45);
         }
      }
   } else if (wc.wdregs[WD_COMMAND_PHASE] == 0x46 || wc.wdregs[WD_COMMAND_PHASE] == 0x45) {
      if (cur.offset < (int)cur.data_len) {
         wc.wd_phase = CSR_XFER_DONE | (cur.direction < 0 ? PHS_DATA_IN : PHS_DATA_OUT);
         set_status(wc.wd_phase, 10);
         return 0;
      }
      settc(0);
      if (cur.direction > 0) {
         scsi_emulate_cmd();
         cur.data_len = 0;
         wc.wd_phase = CSR_XFER_DONE | PHS_STATUS;
      }
      scsi_start_transfer();
      setphase(0x47);
   }
   wc.wd_dataoffset = 0;
   if (wc.wdregs[WD_COMMAND] == WD_CMD_SEL_ATN_XFER || wc.wdregs[WD_COMMAND] == WD_CMD_SEL_XFER) {
      wd_cmd_sel_xfer(0);
      return 1;
   }
   set_status(wc.wd_phase, cur.direction <= 0 ? 0 : 1);
   wc.wd_busy = 0;
   return 1;
}

static int wd_do_transfer_in(int message_in_transfer_info)
{
   wc.wd_dataoffset = 0;
   if (wc.wdregs[WD_COMMAND_PHASE] >= 0x36 && wc.wdregs[WD_COMMAND_PHASE] < 0x46) {
      if (cur.offset < (int)cur.data_len) {
         wc.wd_phase = CSR_XFER_DONE | (cur.direction < 0 ? PHS_DATA_IN : PHS_DATA_OUT);
         set_status(wc.wd_phase, 1);
         return 0;
      }
      if (gettc() != 0) {
         wc.wd_phase = CSR_UNEXP | PHS_STATUS; setphase(0x46);
      } else {
         wc.wd_phase = CSR_XFER_DONE | PHS_STATUS; setphase(0x46);
         if (wc.wdregs[WD_COMMAND] == WD_CMD_SEL_ATN_XFER || wc.wdregs[WD_COMMAND] == WD_CMD_SEL_XFER) {
            wd_cmd_sel_xfer(0);
            return 1;
         }
      }
      scsi_start_transfer();
   } else if (wc.wdregs[WD_COMMAND_PHASE] == 0x46 || wc.wdregs[WD_COMMAND_PHASE] == 0x47) {
      if (wc.wdregs[WD_COMMAND] == WD_CMD_SEL_ATN_XFER || wc.wdregs[WD_COMMAND] == WD_CMD_SEL_XFER) {
         wd_cmd_sel_xfer(0);
         return 1;
      }
      setphase(0x50);
      wc.wd_phase = CSR_XFER_DONE | PHS_MESS_IN;
      scsi_start_transfer();
   } else if (wc.wdregs[WD_COMMAND_PHASE] == 0x50) {
      if (!message_in_transfer_info) {
         wc.wd_phase = CSR_DISC;
         wc.wd_selected = 0;
         scsi_start_transfer();
         setphase(0x60);
      } else {
         wc.wd_phase = CSR_MSGIN;
      }
   }
   set_status(wc.wd_phase, 1);
   cur.direction = 0;
   return 1;
}

static void wd_cmd_sel_xfer(int atn)
{
   int tmp_tc;
   wd_delay_mode = 0;   /* autonomous ROM boot-load: immediate delivery (no inter-INT2 gap wanted) */
   if (wc.auxstatus & ASR_INT) amix_clobber_int++;   // TEMP: new cmd issued while a completion INT was still pending
   if (wc.queue_index > 0) amix_clobber_q += wc.queue_index;   // TEMP: queued statuses about to be stranded
   wc.auxstatus = 0;
   wc.wd_data_avail = 0;
   tmp_tc = gettc();
   cur.unit = wc.wdregs[WD_DESTINATION_ID] & 7;
   if (!target_present(cur.unit)) {
      set_status(CSR_TIMEOUT, 0);
      wc.wdregs[WD_COMMAND_PHASE] = 0x00;
      return;
   }
   if (!wc.wd_selected) {
      cur.message[0] = 0x80;
      wc.wd_selected = 1;
      wc.wdregs[WD_COMMAND_PHASE] = 0x10;
   }
   if (wc.wdregs[WD_COMMAND_PHASE] <= 0x30) {
      cur.buffer[0] = 0;
      cur.status = 0;
      memcpy(cur.cmd, &wc.wdregs[WD_CDB_1], 16);
      cur.data_len = tmp_tc;
      scsi_emulate_analyze();
      settc(cur.cmd_len);
      wc.wd_dataoffset = 0;
      scsi_start_transfer();
      cur.direction = 2;
      cur.data_len = cur.cmd_len;
      /* CDB already in cur.cmd via memcpy; just account for the bytes */
      for (uae_u32 i = 0; i < gettc(); i++) wc.wd_dataoffset++;
      cur.data_len = tmp_tc;
      if (!scsi_emulate_analyze()) {
         wc.wdregs[WD_COMMAND_PHASE] = 0x46;
         goto end;
      }
      wc.wdregs[WD_COMMAND_PHASE] = 0x30 + gettc();
      settc(0);
      if (cur.direction <= 0)
         scsi_emulate_cmd();
      scsi_start_transfer();
   }

   if (wc.wdregs[WD_COMMAND_PHASE] <= 0x41)
      wc.wdregs[WD_COMMAND_PHASE] = 0x44;
   if (wc.wdregs[WD_COMMAND_PHASE] == 0x44)
      wc.wdregs[WD_COMMAND_PHASE] = 0x45;

   if (wc.wdregs[WD_COMMAND_PHASE] == 0x45) {
      settc(tmp_tc);
      wc.wd_dataoffset = 0;
      setphase(0x45);
      if (gettc() == 0) {
         if (cur.direction != 0) {
            if (cur.direction < 0) {
               if (cur.data_len == 0 || cur.offset >= (int)cur.data_len) {
                  setphase(0x46);
                  goto end;
               }
            }
            wc.wd_phase = CSR_UNEXP | (cur.direction < 0 ? PHS_DATA_IN : PHS_DATA_OUT);
            set_status(wc.wd_phase, 1);
            return;
         }
      }
      if (cur.direction) {
         if (cur.direction < 0 && cur.data_len == 0 && gettc()) {
            setphase(0x46);
            wc.wd_phase = CSR_UNEXP | PHS_STATUS;
            set_status(wc.wd_phase, 1);
            return;
         }
         if (canwddma() > 0) {
            if (cur.direction <= 0) {
               do_dma();
               if (cur.offset < (int)cur.data_len) {
                  wc.wd_phase = CSR_UNEXP | PHS_DATA_IN;
                  set_status(wc.wd_phase, 1);
                  return;
               }
               if (gettc() > 0) {
                  setphase(0x46);
                  wc.wd_phase = CSR_UNEXP | PHS_STATUS;
                  set_status(wc.wd_phase, 1);
                  return;
               }
               setphase(0x46);
            } else {
               if (do_dma()) {
                  setphase(0x46);
                  if (cur.offset < (int)cur.data_len) {
                     wc.wd_phase = CSR_UNEXP | PHS_DATA_OUT;
                     set_status(wc.wd_phase, 1);
                     return;
                  }
                  scsi_emulate_cmd();
               }
            }
         } else {
            /* no DMA: request service so the guest does PIO via WD_DATA */
            wc.wd_phase = CSR_SRV_REQ | (cur.direction < 0 ? PHS_DATA_IN : PHS_DATA_OUT);
            set_status(wc.wd_phase, 1);
            return;
         }
      } else {
         if (gettc()) {
            wc.wd_phase = CSR_UNEXP | PHS_STATUS;
            set_status(wc.wd_phase, 1);
            return;
         }
         setphase(0x46);
      }
   }

end:
   if (wc.wdregs[WD_COMMAND_PHASE] == 0x46) {
      wc.wdregs[WD_COMMAND_PHASE] = 0x50;
      wc.wdregs[WD_TARGET_LUN] = cur.status;
      cur.buffer[0] = cur.status;
   }
   wc.wdregs[WD_COMMAND_PHASE] = 0x60;
   /* Command complete. If the driver did NOT enable the ending-disconnect
    * interrupt (CTL_EDI clear), the controller also reports the target's
    * disconnect: queue CSR_SEL_XFER_DONE then CSR_DISC as TWO interrupts the
    * guest acks with two separate WD_SCSI_STATUS reads (never collapse those).
    * If CTL_EDI is set, the host disconnects itself and WinUAE delivers only
    * SEL_XFER_DONE - queueing a trailing DISC then would be a spurious INT2.
    * Matches a2091.cpp:1257-1266. */
   wc.wd_phase = CSR_SEL_XFER_DONE;
   set_status(wc.wd_phase, 2);
   if (!(wc.wdregs[WD_CONTROL] & CTL_EDI)) {
      wc.wd_phase = CSR_DISC;
      set_status(wc.wd_phase, 0);
      amix_disc_q++;   // TEMP diag: how many trailing CSR_DISC are actually queued (is CTL_EDI clear here?)
   }
   wc.wd_selected = 0;
}

static void wd_cmd_trans_info(int pad)
{
   if (wc.wdregs[WD_COMMAND_PHASE] == 0x20) {
      wc.wdregs[WD_COMMAND_PHASE] = 0x30;
      cur.status = 0;
   }
   wc.wd_busy = 1;
   if (wc.wdregs[WD_COMMAND] & 0x80) settc(1);
   if (gettc() == 0) settc(1);
   wc.wd_dataoffset = 0;

   if (wc.wdregs[WD_COMMAND_PHASE] == 0x30) {
      cur.direction = 2;
      cur.cmd_len = cur.data_len = gettc();
   } else if (wc.wdregs[WD_COMMAND_PHASE] == 0x10) {
      cur.direction = 1;
      cur.data_len = gettc();
   } else if (wc.wdregs[WD_COMMAND_PHASE] == 0x45) {
      scsi_emulate_analyze();
   } else if (wc.wdregs[WD_COMMAND_PHASE] == 0x46 || wc.wdregs[WD_COMMAND_PHASE] == 0x47) {
      cur.buffer[0] = cur.status;
      wc.wdregs[WD_TARGET_LUN] = cur.status;
      cur.direction = -1;
      cur.data_len = 1;
   } else if (wc.wdregs[WD_COMMAND_PHASE] == 0x50) {
      cur.direction = -1;
      cur.data_len = gettc();
   }

   if (pad) {
      settc(0);
      if (cur.direction < 0) wd_do_transfer_in(0);
      else if (cur.direction > 0) wd_do_transfer_out();
      cur.direction = 0;
      wc.wd_data_avail = 0;
   } else {
      wc.wd_data_avail = (canwddma() > 0) ? -1 : 1;
   }
}

static void wd_cmd_sel(int atn)
{
   /* DELAYS DISABLED for now: the Kickstart ROM ALSO uses bare SEL + TRANS_INFO (step-by-step) for the
    * RDB reads during boot-load, and the interrupt-delay countdown stalls those reads (guest spins on
    * INTENAR after csr=1B). So all-immediate (the proven original behavior) to (a) confirm -Os boots and
    * (b) capture the root-mount EIO handshake via the trace, then design the real root-mount fix. */
   wd_delay_mode = 0;   /* was 1; step-by-step delay broke the ROM TRANS_INFO RDB reads */
   wc.wd_phase = 0;
   wc.wdregs[WD_COMMAND_PHASE] = 0;
   cur.unit = wc.wdregs[WD_DESTINATION_ID] & 7;
   if (!target_present(cur.unit) || (wc.wdregs[WD_DESTINATION_ID] & 7) == 7) {
      set_status(CSR_TIMEOUT, 1000);
      return;
   }
   scsi_start_transfer();
   wc.wd_selected = 1;
   cur.message[0] = 0x80;
   set_status(CSR_SELECT, 2);
   if (atn) {
      wc.wdregs[WD_COMMAND_PHASE] = 0x10;
      set_status(CSR_SRV_REQ | PHS_MESS_OUT, 4);
   } else {
      wc.wdregs[WD_COMMAND_PHASE] = 0x20;
      set_status(CSR_SRV_REQ | PHS_COMMAND, 4);
   }
}

static void wd_cmd_reset(int irq, int fast)
{
   for (int i = 1; i <= 0x16; i++) wc.wdregs[i] = 0;
   wc.wdregs[0x18] = 0;
   wc.sasr = 0;
   wc.wd_selected = 0;
   for (int j = 0; j < WD_STATUS_QUEUE; j++) { wc.status[j].status = 0; wc.status[j].irq = 0; }
   wc.queue_index = 0;
   wc.auxstatus = 0;
   wc.wd_data_avail = 0;
   wc.async_pending = 0;
   wc.resetnodelay_active = 0;
   sd.dmac_eop_delay = 0;   /* a WD/DMAC reset abandons any pending SDMAC end-of-process latency */
   if (irq) {
      uae_u8 status = (wc.wdregs[0] & 0x08) ? 1 : 0;
      if (fast) {
         wc.wdregs[WD_SCSI_STATUS] = status;
         wc.auxstatus |= ASR_INT;
         set_status0(status);
         wc.wd_busy = 0;
         wc.resetnodelay_active = 1;
         a3000_recompute_irq();
      } else {
         set_status(status, 50);
      }
   } else {
      wc.wd_busy = 0;
   }
}

static void wd_master_reset(int irq)
{
   memset(wc.wdregs, 0, sizeof wc.wdregs);
   wd_cmd_reset(0, 0);
   if (irq) {
      wc.wdregs[WD_SCSI_STATUS] = 0;
      wc.auxstatus |= ASR_INT;
      set_status0(0);
      wc.resetnodelay_active = 1;
      a3000_recompute_irq();
   }
}

static void wd_execute_cmd(int cmd)
{
   switch (cmd & 0x7f) {
   case WD_CMD_RESET:        wd_cmd_reset(1, 0); break;
   case WD_CMD_ABORT:        break;
   case WD_CMD_ASSERT_ATN:   wc.wdregs[WD_COMMAND_PHASE] = 0x10; break;
   case WD_CMD_SEL:          wd_cmd_sel(0); break;
   case WD_CMD_SEL_ATN:      wd_cmd_sel(1); break;
   case WD_CMD_SEL_ATN_XFER: wd_cmd_sel_xfer(1); break;
   case WD_CMD_SEL_XFER:     wd_cmd_sel_xfer(0); break;
   case WD_CMD_TRANS_INFO:   wd_cmd_trans_info(0); break;
   case WD_CMD_NEGATE_ACK:
      if (wc.wd_phase == CSR_MSGIN && wc.wd_selected) wd_do_transfer_in(0);
      break;
   case WD_CMD_TRANSFER_PAD: wd_cmd_trans_info(1); break;
   default:
      wc.wd_busy = 0;
      set_status(CSR_INVALID, 10);
      break;
   }
}

/* (amix_async_exec retired 2026-06-03 — the async-DMA defer is replaced by the per-status hsync
 * countdown in set_status/wd_check_interrupt, which spaces the completion statuses directly.) */

/* ===================== WD33C93 host register access ===================== */

static uae_u8 wdscsi_getauxstatus(void)
{
   /* Force-deliver a held completion ONLY to a genuine POLL of the AUX status (>=2 consecutive AUX reads,
    * e.g. the driver's initialize() busy-wait `while(!(reg(0x1f)&0x80))`), so a polled wait at high spl
    * still sees ASR_INT. Do NOT force on a SINGLE AUX read: a3091intr's entry cipwait() reads the AUX
    * exactly once (it tests ASR_CIP, which we never set, so it never loops) and is immediately followed by
    * a WD_SCSI_STATUS read that would ack-without-biodone any completion we leaked to it. The intmask hold
    * in wd_check_interrupt defers a3091intr's in-ISR re-issued completion until it rte's; forcing it out via
    * cipwait was the residual lost-completion race. amix_aux_streak resets on any non-AUX register touch. */
   amix_aux_streak++;
   /* WinUAE has NO force-ripen path: a polled AUX wait sees ASR_INT only once the steady hsync pump delivers
    * the status. The Z3660 force on a >=2 AUX-read streak can deliver the in-ISR re-issued completion early
    * for AMIX -> curunitp desync. Gate it out for amix_mmu_on (the hsync pump delivers it instead, like
    * Amiberry); keep it for the Kickstart ROM load (amix_mmu_on==0) where it is proven harmless. */
   if (amix_aux_streak >= 2 && !amix_mmu_on) {
      wd_check_interrupt(1, 1);
   }
   return (wc.auxstatus & ASR_INT) |
          ((wc.wd_busy || wc.wd_data_avail < 0) ? ASR_BSY : 0) |
          ((wc.wd_data_avail != 0) ? ASR_DBR : 0);
}

static void wdscsi_sasr(uae_u8 b) { wc.sasr = b; }

// TEMP: ring of the last WD33C93 commands the guest issued (to pair against the [SDMA] DMA ring).
extern "C" { volatile uae_u32 amix_wcmd_cmd[16], amix_wcmd_dest[16], amix_wcmd_ph[16]; volatile int amix_wcmd_head = 0; }

static void wdscsi_put(uae_u8 d)
{
   amix_aux_streak = 0;   /* any register WRITE breaks an AUX poll streak */
   if (!writeonlyreg(wc.sasr))
      writewdreg(wc.sasr, d);
   if (wc.sasr == WD_COMMAND_PHASE) {
      ;
   } else if (wc.sasr == WD_DATA) {
      if (!wc.wd_data_avail) return;
      if (wc.wd_dataoffset < (int)sizeof wc.wd_data) wc.wd_data[wc.wd_dataoffset] = wc.wdregs[wc.sasr];
      wc.wd_dataoffset++;
      decreasetc();
      wc.wd_data_avail = 1;
      if (scsi_send_data(wc.wdregs[wc.sasr]) || gettc() == 0) {
         wc.wd_data_avail = 0;
         wd_do_transfer_out();
      }
   } else if (wc.sasr == WD_COMMAND) {
      if (wd_trace_on()) dbg("[WDCMD] cmd=%02X destid=%X ph=%02X\n", d & 0x7f,
         wc.wdregs[WD_DESTINATION_ID] & 7, wc.wdregs[WD_COMMAND_PHASE]);  /* [SCSITRACE] rm */
      { int h=amix_wcmd_head&15; amix_wcmd_cmd[h]=d&0x7f; amix_wcmd_dest[h]=wc.wdregs[WD_DESTINATION_ID]&7;
        amix_wcmd_ph[h]=wc.wdregs[WD_COMMAND_PHASE]; amix_wcmd_head++; }   // TEMP command ring
      wc.wd_busy = 1;
      /* DESYNC DETECTOR: snapshot curunitp->4 (the request the guest dispatcher set just before this
       * WD_COMMAND write) so the WD_SCSI_STATUS completion ack can verify it didn't move. */
      if ((d & 0x7f) == WD_CMD_SEL_ATN_XFER && amix_mmu_on) amix_issued_req = amix_cpu_curunitp_req();
      /* Execute synchronously; the per-status hsync countdown (set_status / wd_check_interrupt) now
       * spaces the two completion statuses so a3091intr's in-ISR re-issued completion lands AFTER the
       * ISR rte's. (The async-DMA defer is retired -- it double-deferred the command without spacing
       * the 2-deep status queue, the actual bug.) */
      wd_execute_cmd(d);
   }
   incsasr(1);
}

static uae_u8 wdscsi_get(void)
{
   if (wc.sasr != WD_AUXILIARY_STATUS) amix_aux_streak = 0;   /* a non-AUX register read breaks an AUX poll streak */
   uae_u8 v = wc.wdregs[wc.sasr];
   if (wc.sasr == WD_DATA) {
      if (!wc.wd_data_avail) return 0;
      uae_u8 db = 0;
      int status = scsi_receive_data(&db);
      v = db;
      if (wc.wd_dataoffset < (int)sizeof wc.wd_data) wc.wd_data[wc.wd_dataoffset] = v;
      wc.wd_dataoffset++;
      decreasetc();
      wc.wdregs[wc.sasr] = v;
      wc.wd_data_avail = 1;
      if (status || gettc() == 0) {
         wc.wd_data_avail = 0;
         wd_do_transfer_in(1);
      }
   } else if (wc.sasr == WD_SCSI_STATUS) {
      { int h=amix_statrd_head&7; uae_u32 a6=amix_cpu_a6();   /* TEMP: which guest code reads the completion status? */
        amix_statrd_pc[h]=amix_cpu_pc(); amix_statrd_c1[h]=amix_cpu_kread(a6+4);
        amix_statrd_c2[h]=amix_cpu_kread(amix_cpu_kread(a6)+4); amix_statrd_head++; }
      if (wc.auxstatus & ASR_INT) {
         wc.auxstatus &= ~ASR_INT;
         /* DESYNC DETECTOR: at this completion ack, does curunitp->4 still equal the request issued by the
          * matching cmd=08? a3091intr's COMPLETE arm is about to biodone curunitp->4 -- if it moved, that
          * biodone hits the WRONG buf and the issued chunk is stranded (the intermittent lost-completion). */
         if (amix_mmu_on && amix_issued_req) {
            uae_u32 cmp = amix_cpu_curunitp_req();
            if (cmp != amix_issued_req) {
               int h = amix_dsy_h & 7; amix_dsy_iss[h] = amix_issued_req; amix_dsy_cmp[h] = cmp;
               amix_dsy_cu[h] = wc.wdregs[WD_SCSI_STATUS]; amix_dsy_h++; amix_desync_n++;
            } else amix_match_n++;
         }
         if (wc.resetnodelay_active) {
            /* resetnodelay models a WD reset clearing pending INTs — but it must NOT discard a real pending
             * completion (CSR_SEL_XFER_DONE) the driver is waiting on (that = a LOST completion -> orphaned buf
             * -> deadlock). Compact the queue: drop everything EXCEPT pending SEL_XFER_DONE completions. */
            amix_rnd_fire++; amix_rnd_disc += wc.queue_index;
            int keep = 0;
            for (int i = 0; i < wc.queue_index; i++) {
               if (wc.status[i].status == CSR_SEL_XFER_DONE) { wc.status[keep] = wc.status[i]; wc.status[keep].irq = 1; keep++; }
            }
            amix_rnd_compl += keep;
            amix_rnd_disc -= keep;
            wc.queue_index = keep;
         }
         wc.resetnodelay_active = 0;
      }
      sd.dmac_istr &= ~ISTR_INTS;
      wd_check_interrupt(1, 0);               /* pump the next queued status */
   } else if (wc.sasr == WD_AUXILIARY_STATUS) {
      v = wdscsi_getauxstatus();
   }
   incsasr(0);
   return v;
}

/* ===================== SuperDMAC register decode ($00DD page) ===================== */

static void mbdmac_write_word(uae_u32 addr, uae_u32 val)
{
   addr &= 0xfffe;
   switch (addr) {
   case 0x02: sd.dmac_dawr = val; break;
   case 0x04: sd.dmac_wtc = (sd.dmac_wtc & 0x0000ffff) | (val << 16); break;
   case 0x06: sd.dmac_wtc = (sd.dmac_wtc & 0xffff0000) | (val & 0xffff); break;
   case 0x0a:
      sd.dmac_cntr = val;
      if (sd.dmac_cntr & SCNTR_PREST) dmac_reset();
      // INT2 is a LEVEL function of (CNTR & INTEN) && ASR_INT (see file header). Writing CNTR changes
      // INTEN, so the line must be re-evaluated: without this, a SCSI completion that arrived while the
      // guest ISR had INTEN masked is never re-asserted when the ISR re-enables INTEN -> AMIX sleeps
      // forever on that I/O until an UNRELATED level-2 INT (e.g. a keyboard press) wakes it and its ISR
      // polls the WD status. That made the boot stall intermittently in gen_strategy disk waits.
      a3000_recompute_irq();
      break;
   case 0x0c: sd.dmac_acr = (sd.dmac_acr & 0x0000ffff) | (val << 16); break;
   case 0x0e: sd.dmac_acr = (sd.dmac_acr & 0xffff0000) | (val & 0xfffe); break;
   case 0x12: if (sd.dmac_dma <= 0) scsi_dmac_start_dma(); break;
   case 0x16: if (sd.dmac_dma) { sd.dmac_istr |= ISTR_FE_FLG; sd.dmac_dma = 0; } break;
   case 0x1a: dmac_cint(); break;
   case 0x1e: break;                          /* ISTR is read-only */
   case 0x3e: scsi_dmac_stop_dma(); break;
   case 0x40: case 0x48: wdscsi_sasr(val); break;
   case 0x42: case 0x46: wdscsi_put(val); break;
   }
}

static void mbdmac_write_byte(uae_u32 addr, uae_u32 val)
{
   addr &= 0xffff;
   switch (addr) {
   case 0x41: case 0x49: wdscsi_sasr(val); break;
   case 0x43: case 0x47: wdscsi_put(val); break;
   default:
      if (addr & 1) mbdmac_write_word(addr, val);
      else          mbdmac_write_word(addr, val << 8);
   }
}

static uae_u32 mbdmac_read_word(uae_u32 addr)
{
   uae_u32 v = 0xffffffff;
   addr &= 0xfffe;
   switch (addr) {
   case 0x02: v = sd.dmac_dawr; break;
   case 0x04: case 0x06: v = 0xffff; break;
   case 0x0a: v = sd.dmac_cntr; break;
   case 0x0c: v = sd.dmac_acr >> 16; break;
   case 0x0e: v = sd.dmac_acr; break;
   case 0x12: if (sd.dmac_dma <= 0) scsi_dmac_start_dma(); v = 0; break;
   case 0x1a: dmac_cint(); v = 0; break;
   case 0x1e:
      v = sd.dmac_istr;
      if (v & ISTR_INTS) v |= ISTR_INT_P;
      sd.dmac_istr &= ~15;
      if (!sd.dmac_dma) v |= ISTR_FE_FLG;
      break;
   case 0x3e:
      if (sd.dmac_dma) { scsi_dmac_stop_dma(); sd.dmac_istr |= ISTR_FE_FLG; }
      v = 0;
      break;
   case 0x40: case 0x48: v = wdscsi_getauxstatus(); break;
   case 0x42: case 0x46: v = wdscsi_get(); break;
   }
   return v;
}

static uae_u32 mbdmac_read_byte(uae_u32 addr)
{
   uae_u32 v = 0xffffffff;
   addr &= 0xffff;
   switch (addr) {
   case 0x41: case 0x49: v = wdscsi_getauxstatus(); break;
   case 0x43: case 0x47: v = wdscsi_get(); break;
   default:
      v = mbdmac_read_word(addr);
      if (!(addr & 1)) v >>= 8;
      break;
   }
   return v;
}

/* After every register access, settle: run a pending DMA (step-by-step TRANS_INFO
 * path) and pump the interrupt queue, then recompute the INT2 line. Collapses
 * WinUAE's hsync poll into synchronous execution. */
static void a3000_scsi_settle(void)
{
   if (wc.wd_data_avail < 0 && is_dma_enabled()) {
      int v;
      do_dma();
      if (cur.direction < 0)      v = wd_do_transfer_in(0);
      else if (cur.direction > 0) v = wd_do_transfer_out();
      else                        v = 1;
      if (v) { cur.direction = 0; wc.wd_data_avail = 0; }
      else   set_dma_done();
   }
   /* 2026-06-04 WinUAE-FAITHFUL DELIVERY: WinUAE delivers a queued status ONLY from scsi_hsync (decrement)
    * and the WD_SCSI_STATUS-read ack -- NEVER from a per-access settle on other registers. The Z3660 added
    * this per-access pump; for the AMIX runtime it can deliver the in-ISR re-issued completion EARLY (on a
    * register touch inside a3091intr, before it rte's) -> curunitp desync -> stranded page-ins. Gate it out
    * for amix_mmu_on so AMIX completions land only on the steady hsync pump or the status-read, exactly like
    * the working Amiberry. The Kickstart ROM load (amix_mmu_on==0) keeps the per-access pump unchanged. */
   if (!amix_mmu_on) wd_check_interrupt(1, 0);
   a3000_recompute_irq();
}

/* Steady "hsync" pump: advances the WD33C93 interrupt-delay countdown on a fixed time base
 * (called from the mmu030 CPU loop, ~every 64 instructions) so the guest cannot collapse the
 * inter-interrupt spacing by polling AUX_STATUS. This re-creates WinUAE's hsync-driven delivery
 * where the guest ISR runs between the SELECT and the service-request INT2s. */
extern "C" void a3000_scsi_hsync(void)
{
   wd_check_interrupt(0, 0);   /* the steady pump: decrement the countdown + deliver a ripe status (WinUAE scsi_hsync) */
   a3000_recompute_irq();
}

/* ============================ public bank entry points ============================ */

uint32_t a3000_scsi_read(uint32_t offset, int size)
{
   uint32_t v;
   if (size == 2)        /* long */
      v = (mbdmac_read_word(offset) << 16) | mbdmac_read_word(offset + 2);
   else if (size == 1)   /* word */
      v = mbdmac_read_word(offset);
   else                  /* byte */
      v = mbdmac_read_byte(offset);
   a3000_scsi_settle();
   return v;
}

void a3000_scsi_write(uint32_t offset, uint32_t data, int size)
{
   if (size == 2) {      /* long */
      if ((offset & 0xffff) == 0x40)
         mbdmac_write_byte(0x41, data);       /* long write to SASR == byte write */
      else {
         mbdmac_write_word(offset, data >> 16);
         mbdmac_write_word(offset + 2, data);
      }
   } else if (size == 1) /* word */
      mbdmac_write_word(offset, data);
   else                  /* byte */
      mbdmac_write_byte(offset, data);
   a3000_scsi_settle();
}

void a3000_scsi_init(void)
{
   memset(&wc, 0, sizeof wc);
   memset(&sd, 0, sizeof sd);
   memset(&cur, 0, sizeof cur);
   for (int i = 0; i < 8; i++) geo[i].valid = 0;
   amix_id6_drv = -1;   // re-probe the id-6 backend mapping after a (possibly reconfigured) reset
   sense_key = sense_asc = sense_ascq = 0;
   a3000_scsi_irq = 0;
   a3000_amix_mode = 1;
   wd_master_reset(0);
   a3000_recompute_irq();
   z3660_printf("[A3000SCSI] WD33C93+SuperDMAC emulation initialised ($00DD0000)\n");
}
