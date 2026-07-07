/* scsi_cd_test.cpp - host unit test for the emulated A3000 WD33C93 SCSI target's
 * CD-ROM (peripheral device type 0x05) CDB synthesis in a3000_scsi.cpp.
 *
 * This is the BYTE-LEVEL REFERENCE for the Path A driver work (z3660.c, T2.P3):
 * the exact INQUIRY / READ CAPACITY / MODE SENSE / write-gating bytes asserted
 * here are the contract both the emulated (Path B) and native (Path A) SCSI
 * targets must present to the stock Amix kernel + cdfs.
 *
 * The CDB decoders (scsi_emulate_analyze / scsi_emulate_cmd) and the geometry
 * cache geo[] are file-static in a3000_scsi.cpp, so we compile that translation
 * unit directly (#include) to reach them, and stub the board seams (cross-core
 * PISCSI registers, cache ops, guest-CPU probes) it references. The tests
 * pre-populate geo[] (valid=1 so fetch_geometry() takes no cross-core reads) and
 * feed CDBs straight into the decoders, then assert the synthesized bytes. */

#include <cstdio>
#include <cstring>
#include <cstdint>

/* Pull in the production SCSI target emulation verbatim (static decoders + geo[]). */
#include "a3000_scsi.cpp"

/* ---- board seams referenced by a3000_scsi.cpp, stubbed inert on the host ----
 * (real defs live in main.cc / newcpu.cpp / the Xilinx BSP, none linked here). */
extern "C" void     write_scsi_register(uint16_t, uint32_t, int) { }
extern "C" uint32_t read_scsi_register(uint16_t, int) { return 0; }
extern "C" void     z3660_printf(const TCHAR *, ...) { }
extern "C" int      amix_cpu_intmask(void) { return 0; }
extern "C" uae_u32  amix_cpu_pc(void)  { return 0; }
extern "C" uae_u32  amix_cpu_a6(void)  { return 0; }
extern "C" uae_u32  amix_cpu_kread(uae_u32) { return 0; }
uae_u32 memory_get_byte(uaecptr) { return 0; }
void    memory_put_byte(uaecptr, uae_u32) { }
volatile int a3000_scsi_irq = 0;

/* ---------------------------------------------------------------- */
static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
   if (cond) { g_pass++; } \
   else { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("   [%s:%d]\n", __FILE__, __LINE__); } \
} while (0)

/* Populate the geometry cache for `unit`. valid=1 => fetch_geometry() is a no-op,
 * so read_scsi_register (which returns 0 on the host) never clobbers pdt/bsize. */
static void setup_unit(int unit, uae_u32 pdt, uae_u32 bsize, uae_u32 nblocks)
{
   memset(&geo[unit], 0, sizeof geo[unit]);
   geo[unit].valid   = 1;
   geo[unit].present = 1;
   geo[unit].bsize   = bsize;
   geo[unit].nblocks = nblocks;
   geo[unit].cyls    = 1000;   /* benign non-zero CHS (mirrors the CD firmware path) */
   geo[unit].heads   = 16;
   geo[unit].secs    = 63;
   geo[unit].pdt     = pdt;
}

/* Feed one CDB through the decoders exactly as wd_cmd_sel_xfer does: analyze,
 * and (only if understood) synthesize the response. Returns the analyze result. */
static int run_cdb(int unit, const uae_u8 *cdb, int cdblen)
{
   memset(&cur, 0, sizeof cur);
   cur.unit = unit;
   memcpy(cur.cmd, cdb, cdblen);
   int ok = scsi_emulate_analyze();
   if (ok)
      scsi_emulate_cmd();
   return ok;
}

static void dump(const char *tag, const uae_u8 *b, int n)
{
   printf("       %s:", tag);
   for (int i = 0; i < n; i++) printf(" %02X", b[i]);
   printf("\n");
}

/* ================================================================== */
static void test_cd_inquiry(void)
{
   printf("[test] CD-ROM INQUIRY (pdt=0x05)\n");
   setup_unit(6, 0x05, 2048, 0x00010000);
   const uae_u8 cdb[6] = { 0x12, 0x00, 0x00, 0x00, 0x24, 0x00 };
   int ok = run_cdb(6, cdb, 6);
   dump("INQUIRY", cur.buffer, 8);
   printf("       product-id: '%.16s'\n", (const char *)(cur.buffer + 16));
   CHECK(ok, "INQUIRY understood\n");
   CHECK(cur.buffer[0] == 0x05, "INQUIRY b[0]==0x05 (CD-ROM), got 0x%02X\n", cur.buffer[0]);
   CHECK(cur.buffer[1] == 0x80, "INQUIRY b[1]==0x80 (RMB), got 0x%02X\n", cur.buffer[1]);
   CHECK(memcmp(cur.buffer + 16, "AMIX CD-ROM     ", 16) == 0, "INQUIRY product-id == 'AMIX CD-ROM     '\n");
}

static void test_cd_read_capacity(void)
{
   printf("[test] CD-ROM READ CAPACITY (2048-byte blocks)\n");
   setup_unit(6, 0x05, 2048, 0x00010000);   /* 65536 blocks -> last LBA 0x0000FFFF */
   const uae_u8 cdb[10] = { 0x25, 0,0,0,0,0,0,0,0,0 };
   int ok = run_cdb(6, cdb, 10);
   dump("READ CAPACITY", cur.buffer, 8);
   uae_u32 last_lba = ((uae_u32)cur.buffer[0] << 24) | (cur.buffer[1] << 16) | (cur.buffer[2] << 8) | cur.buffer[3];
   uae_u32 blklen   = ((uae_u32)cur.buffer[4] << 24) | (cur.buffer[5] << 16) | (cur.buffer[6] << 8) | cur.buffer[7];
   CHECK(ok, "READ CAPACITY understood\n");
   CHECK(last_lba == 0x0000FFFF, "READ CAPACITY last LBA == nblocks-1 (0x0000FFFF), got 0x%08X\n", last_lba);
   CHECK(blklen == 2048, "READ CAPACITY block length == 2048, got %u\n", blklen);
}

static void test_cd_mode_sense6(void)
{
   printf("[test] CD-ROM MODE SENSE(6) write-protect + no rigid geometry\n");
   setup_unit(6, 0x05, 2048, 0x00010000);
   const uae_u8 cdb[6] = { 0x1a, 0x00, 0x3f, 0x00, 0x40, 0x00 };   /* page 0x3f, alloc 64 */
   int ok = run_cdb(6, cdb, 6);
   dump("MODE SENSE6", cur.buffer, 12);
   CHECK(ok, "MODE SENSE(6) understood\n");
   CHECK((cur.buffer[2] & 0x80) != 0, "MODE SENSE(6) WP bit set in devspec b[2], got 0x%02X\n", cur.buffer[2]);
   /* mode data length b[0] = hdr(4)+bd(8)+pages(0)-1 = 0x0B -> geometry pages dropped */
   CHECK(cur.buffer[0] == 0x0B, "MODE SENSE(6) no rigid-geometry pages (len==0x0B), got 0x%02X\n", cur.buffer[0]);
}

static void test_cd_mode_sense10(void)
{
   printf("[test] CD-ROM MODE SENSE(10) write-protect + no rigid geometry\n");
   setup_unit(6, 0x05, 2048, 0x00010000);
   const uae_u8 cdb[10] = { 0x5a, 0x00, 0x3f, 0,0,0,0, 0x00, 0x40, 0x00 };   /* page 0x3f, alloc 64 */
   int ok = run_cdb(6, cdb, 10);
   dump("MODE SENSE10", cur.buffer, 12);
   CHECK(ok, "MODE SENSE(10) understood\n");
   CHECK((cur.buffer[3] & 0x80) != 0, "MODE SENSE(10) WP bit set in devspec b[3], got 0x%02X\n", cur.buffer[3]);
   /* mode data length b[0..1] = hdr(8)+bd(8)+pages(0)-2 = 0x000E */
   CHECK(cur.buffer[0] == 0x00 && cur.buffer[1] == 0x0E,
         "MODE SENSE(10) no rigid-geometry pages (len==0x000E), got 0x%02X%02X\n", cur.buffer[0], cur.buffer[1]);
}

static void test_cd_write_gating(void)
{
   printf("[test] CD-ROM WRITE gating -> CHECK CONDITION / DATA PROTECT\n");
   /* WRITE(10) */
   setup_unit(6, 0x05, 2048, 0x00010000);
   const uae_u8 w10[10] = { 0x2a, 0x00, 0,0,0,0, 0x00, 0x00, 0x01, 0x00 };   /* 1 block @ lba 0 */
   int ok10 = run_cdb(6, w10, 10);
   printf("       WRITE(10): analyze=%d status=0x%02X sense=%02X/%02X/%02X is_block_io=%d dir=%d\n",
          ok10, cur.status, sense_key, sense_asc, sense_ascq, cur.is_block_io, cur.direction);
   CHECK(ok10 == 0, "WRITE(10) to CD rejected by analyze (returns 0)\n");
   CHECK(cur.status == 0x02, "WRITE(10) -> CHECK CONDITION (0x02), got 0x%02X\n", cur.status);
   CHECK(sense_key == 0x07 && sense_asc == 0x27 && sense_ascq == 0x00,
         "WRITE(10) sense == DATA PROTECT 0x07/0x27/0x00, got %02X/%02X/%02X\n", sense_key, sense_asc, sense_ascq);
   CHECK(cur.is_block_io == 0 && cur.direction == 0, "WRITE(10) not turned into a block-io transfer\n");

   /* WRITE(6) */
   setup_unit(6, 0x05, 2048, 0x00010000);
   const uae_u8 w6[6] = { 0x0a, 0x00, 0x00, 0x00, 0x01, 0x00 };
   int ok6 = run_cdb(6, w6, 6);
   printf("       WRITE(6):  analyze=%d status=0x%02X sense=%02X/%02X/%02X\n",
          ok6, cur.status, sense_key, sense_asc, sense_ascq);
   CHECK(ok6 == 0, "WRITE(6) to CD rejected by analyze (returns 0)\n");
   CHECK(cur.status == 0x02, "WRITE(6) -> CHECK CONDITION (0x02), got 0x%02X\n", cur.status);
   CHECK(sense_key == 0x07 && sense_asc == 0x27 && sense_ascq == 0x00,
         "WRITE(6) sense == DATA PROTECT 0x07/0x27/0x00, got %02X/%02X/%02X\n", sense_key, sense_asc, sense_ascq);
}

/* ==================================================================
 * Disk-path (pdt==0x00) regression: must be byte-identical to before the
 * CD-ROM support was added, and writes must still be permitted.
 * ================================================================== */
static void test_disk_inquiry_unchanged(void)
{
   printf("[test] DISK INQUIRY unchanged (pdt=0x00)\n");
   setup_unit(6, 0x00, 512, 0x00010000);
   const uae_u8 cdb[6] = { 0x12, 0x00, 0x00, 0x00, 0x24, 0x00 };
   int ok = run_cdb(6, cdb, 6);
   dump("INQUIRY", cur.buffer, 8);
   printf("       product-id: '%.16s'\n", (const char *)(cur.buffer + 16));
   CHECK(ok, "INQUIRY understood\n");
   CHECK(cur.buffer[0] == 0x00, "DISK INQUIRY b[0]==0x00 (direct-access), got 0x%02X\n", cur.buffer[0]);
   CHECK(cur.buffer[1] == 0x00, "DISK INQUIRY b[1]==0x00 (RMB clear), got 0x%02X\n", cur.buffer[1]);
   CHECK(memcmp(cur.buffer + 16, "AMIX SCSI Disk  ", 16) == 0, "DISK INQUIRY product-id == 'AMIX SCSI Disk  '\n");
}

static void test_disk_read_capacity_512(void)
{
   printf("[test] DISK READ CAPACITY reports 512-byte blocks\n");
   setup_unit(6, 0x00, 512, 0x00010000);
   const uae_u8 cdb[10] = { 0x25, 0,0,0,0,0,0,0,0,0 };
   int ok = run_cdb(6, cdb, 10);
   dump("READ CAPACITY", cur.buffer, 8);
   uae_u32 blklen = ((uae_u32)cur.buffer[4] << 24) | (cur.buffer[5] << 16) | (cur.buffer[6] << 8) | cur.buffer[7];
   CHECK(ok, "READ CAPACITY understood\n");
   CHECK(blklen == 512, "DISK READ CAPACITY block length == 512, got %u\n", blklen);
}

static void test_disk_mode_sense6_geometry(void)
{
   printf("[test] DISK MODE SENSE(6) WP clear + rigid geometry present\n");
   setup_unit(6, 0x00, 512, 0x00010000);
   const uae_u8 cdb[6] = { 0x1a, 0x00, 0x3f, 0x00, 0x40, 0x00 };
   int ok = run_cdb(6, cdb, 6);
   dump("MODE SENSE6", cur.buffer, 16);
   CHECK(ok, "MODE SENSE(6) understood\n");
   CHECK((cur.buffer[2] & 0x80) == 0, "DISK MODE SENSE(6) WP bit CLEAR in b[2], got 0x%02X\n", cur.buffer[2]);
   /* block descriptor is 8 bytes at b[4..11]; first mode page starts at b[12] */
   CHECK(cur.buffer[12] == 0x03, "DISK MODE SENSE(6) Format Device page 0x03 present at b[12], got 0x%02X\n", cur.buffer[12]);
   /* len = hdr(4)+bd(8)+page03(0x18)+page04(0x18)-1 = 0x3B */
   CHECK(cur.buffer[0] == 0x3B, "DISK MODE SENSE(6) geometry pages present (len==0x3B), got 0x%02X\n", cur.buffer[0]);
}

static void test_disk_write_allowed(void)
{
   printf("[test] DISK WRITE(10) still permitted (pdt=0x00)\n");
   setup_unit(6, 0x00, 512, 0x00010000);
   const uae_u8 w10[10] = { 0x2a, 0x00, 0,0,0,0, 0x00, 0x00, 0x01, 0x00 };
   int ok = run_cdb(6, w10, 10);
   printf("       WRITE(10): analyze=%d is_block_io=%d dir=%d status=0x%02X\n",
          ok, cur.is_block_io, cur.direction, cur.status);
   CHECK(ok, "DISK WRITE(10) understood (analyze returns 1)\n");
   CHECK(cur.is_block_io == 1, "DISK WRITE(10) is a block-io transfer\n");
   CHECK(cur.direction == 1, "DISK WRITE(10) direction == 1 (host->target write)\n");
   CHECK(cur.status == 0x00, "DISK WRITE(10) status GOOD (0x00), got 0x%02X\n", cur.status);
}

/* ================================================================== */
int main(void)
{
   printf("=== Z3660 A3000-SCSI CD-ROM CDB unit test (Path B, T2.P2) ===\n");
   printf("PISCSI_CMD_PDT mirror = 0x%02X\n", PISCSI_CMD_PDT);

   test_cd_inquiry();
   test_cd_read_capacity();
   test_cd_mode_sense6();
   test_cd_mode_sense10();
   test_cd_write_gating();

   test_disk_inquiry_unchanged();
   test_disk_read_capacity_512();
   test_disk_mode_sense6_geometry();
   test_disk_write_allowed();

   printf("\n=== %d passed, %d failed ===\n", g_pass, g_fail);
   return g_fail ? 1 : 0;
}
