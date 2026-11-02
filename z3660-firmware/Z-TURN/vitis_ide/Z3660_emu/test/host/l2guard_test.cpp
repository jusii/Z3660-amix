/* Host tests for core 0's guarded whole-L2 flush and its abort record
 * (../../../Z3660/src/l2guard.h).
 *
 * Pins: the flush runs with IRQ and FIQ masked and the caller's CPSR comes back exactly;
 * the site, mode and depth bookkeeping, including a nested call; the record's seal and
 * its rejection of a torn or foreign record; the async-abort decode; the printed text; and,
 * by reading the core-0 sources, that no whole-L2 flush bypasses the wrapper and that each
 * site is used exactly once. Same pattern as rdb_scan_test.cpp. */
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>

static uint32_t g_cpsr;
static int g_flushes, g_masked_during, g_unmasked_during;
static void (*g_inside)(void);

#define L2G_GET_CPSR()   (g_cpsr)
#define L2G_SET_CPSR(v)  (g_cpsr = (v))
#define L2G_FLUSH()      fake_flush()
static void fake_flush(void);

extern "C" {
#include "../../../Z3660/src/l2guard.h"
}

static void fake_flush(void)
{
   g_flushes++;
   if ((g_cpsr & 0xC0U) == 0xC0U) g_masked_during++; else g_unmasked_during++;
   if (g_inside) { void (*f)(void) = g_inside; g_inside = 0; f(); }
}

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, ...) do { \
   if (cond) { g_pass++; } \
   else { g_fail++; printf("  FAIL: " __VA_ARGS__); printf("   [%s:%d]\n", __FILE__, __LINE__); } \
} while (0)

static volatile struct l2g_state g_st;
static uint32_t g_seen_depth, g_seen_site;

static void inner_from_isr(void)
{
   uint32_t saved = g_cpsr;
   g_cpsr = (g_cpsr & ~0x1FU) | 0x12U;        /* as if in IRQ mode */
   l2g_flush(&g_st, L2G_VIDEO_FLUSH);
   g_seen_depth = g_st.depth;                 /* back in the outer flush */
   g_seen_site = g_st.site;
   g_cpsr = saved;
}

static void t_mask_and_restore(void)
{
   static const uint32_t start[] = { 0x60000013U, 0x600000D3U, 0x20000053U, 0x00000092U, 0x1FU };
   for (unsigned i = 0; i < sizeof(start) / sizeof(start[0]); i++) {
      g_cpsr = start[i];
      g_flushes = g_masked_during = g_unmasked_during = 0;
      l2g_flush(&g_st, L2G_SCSI_SETFSH_NONE);
      CHECK(g_flushes == 1, "one flush per call (cpsr %08x)\n", start[i]);
      CHECK(g_masked_during == 1 && g_unmasked_during == 0, "IRQ+FIQ masked during the flush (cpsr %08x)\n", start[i]);
      CHECK(g_cpsr == start[i], "CPSR restored exactly: %08x, was %08x\n", g_cpsr, start[i]);
   }
}

static void t_bookkeeping(void)
{
   memset((void *)&g_st, 0, sizeof(g_st));
   g_cpsr = 0x60000013U;                      /* svc */
   l2g_flush(&g_st, L2G_SCSI_SETFSH);
   CHECK(g_st.depth == 0, "depth back to 0: %u\n", g_st.depth);
   CHECK(g_st.site == 0, "no site in progress after: %08x\n", g_st.site);
   CHECK(g_st.last == (L2G_SCSI_SETFSH | (0x13U << 16)), "last = site | svc<<16: %08x\n", g_st.last);
   CHECK(g_st.count == 1, "count 1: %u\n", g_st.count);
   CHECK(g_st.nested == 0, "nothing nested: %u\n", g_st.nested);

   /* a nested call (what an interrupt inside the flush would make) */
   g_inside = inner_from_isr;
   l2g_flush(&g_st, L2G_SCSI_INIT);
   CHECK(g_seen_depth == 1, "the outer flush is still in progress after the inner: depth %u\n", g_seen_depth);
   CHECK(g_seen_site == (L2G_SCSI_INIT | (0x13U << 16)), "the outer site is restored: %08x\n", g_seen_site);
   CHECK(g_st.depth == 0 && g_st.site == 0, "all done: depth %u site %08x\n", g_st.depth, g_st.site);
   CHECK(g_st.last == (L2G_VIDEO_FLUSH | (0x12U << 16)), "last = the inner, from irq: %08x\n", g_st.last);
   CHECK(g_st.count == 3, "count 3: %u\n", g_st.count);
   CHECK(g_st.nested == 1, "one nested entry counted: %u\n", g_st.nested);
}

static void fill(struct l2g_record *r)
{
   memset(r, 0, sizeof(*r));
   r->kind = L2G_KIND_DATA;
   r->fsr = 0x00001C06U;
   r->pc = 0x01214DBCU;
   r->spsr = 0x600001D2U;                      /* irq */
   r->depth = 1;
   r->site = L2G_VIDEO_FLUSH | (0x12U << 16);
   r->last = r->site;
   r->count = 4711;
   r->nested = 12;
   r->pl310_ris = L2G_PL310_SLVERR | L2G_PL310_ERRWD;
   r->pl310_way = 0xFFFF;
   r->pl310_dbg = 3;
   l2g_record_seal(r);
}

static void t_record(void)
{
   struct l2g_record r;
   fill(&r);
   CHECK(l2g_record_valid(&r), "a sealed record is valid\n");
   for (unsigned w = 0; w < sizeof(r) / 4; w++) {
      struct l2g_record c = r;
      ((uint32_t *)&c)[w] ^= 0x00000100U;
      CHECK(!l2g_record_valid(&c), "one flipped bit in word %u is rejected\n", w);
   }
   struct l2g_record z;
   memset(&z, 0, sizeof(z));
   CHECK(!l2g_record_valid(&z), "zeroed DDR is not a record\n");
   memset(&z, 0xFF, sizeof(z));
   CHECK(!l2g_record_valid(&z), "all-ones DDR is not a record\n");
   struct l2g_record k = r;
   k.kind = 7;
   l2g_record_seal(&k);
   CHECK(!l2g_record_valid(&k), "an unknown kind is rejected even when sealed\n");
   CHECK(sizeof(struct l2g_record) == 64, "record size 64: %u\n", (unsigned)sizeof(struct l2g_record));
}

static void t_decode_and_text(void)
{
   CHECK(l2g_fs(0x00001C06U) == 0x16U, "FS of 1C06 is 0b10110\n");
   CHECK(l2g_fs_async(0x00001C06U), "1C06 is asynchronous\n");
   CHECK(!l2g_fs_async(0x00000805U), "a translation fault is not\n");
   CHECK(!l2g_fs_async(0x00000008U), "a synchronous external abort is not\n");
   CHECK(l2g_fs_async(0x00000408U), "an asynchronous parity error (0b11000) is\n");

   struct l2g_record r;
   char buf[320];
   fill(&r);
   int n = l2g_record_format(&r, buf, sizeof(buf));
   std::string s(buf);
   CHECK(n > 0 && n < (int)sizeof(buf), "fits in 320 bytes: %d\n", n);
   CHECK(s.find("data abort FSR 00001C06 (async") != std::string::npos, "async marked: %s\n", buf);
   CHECK(s.find("from irq") != std::string::npos, "SPSR mode named: %s\n", buf);
   CHECK(s.find("site video-flush/irq") != std::string::npos, "site and mode named: %s\n", buf);
   CHECK(s.find("count 4711 nested 12;") != std::string::npos, "counts printed: %s\n", buf);
   CHECK(s.find(" SLVERR ERRWD") != std::string::npos, "PL310 bits named: %s\n", buf);
   CHECK(s.find("way FFFF dbg 3") != std::string::npos, "PL310 state: %s\n", buf);
   CHECK(std::string(l2g_site_name(0xFFFFU)) == "?", "an out-of-range site prints ?\n");
   for (unsigned i = 0; i < L2G_SITES; i++)
      CHECK(l2g_site_names[i] != 0 && l2g_site_names[i][0], "site %u has a name\n", i);
}

/* Every core-0 whole-L2 flush goes through the wrapper; every site is used exactly once */
static void t_sources(void)
{
   std::string cmd = std::string("grep -rn --include=*.c -E 'Xil_L2CacheFlush[[:space:]]*\\(|z3660_l2_flush[[:space:]]*\\(' ") +
                     Z3660_CORE0_SRC_PATH;
   FILE *p = popen(cmd.c_str(), "r");
   CHECK(p != 0, "grep over %s\n", Z3660_CORE0_SRC_PATH);
   if (!p) return;
   char line[1024];
   int raw = 0, uses[L2G_SITES] = { 0 }, lines = 0;
   while (fgets(line, sizeof(line), p)) {
      lines++;
      std::string l(line);
      size_t c = l.find(':');
      c = l.find(':', c + 1);
      std::string code = l.substr(c + 1);
      size_t first = code.find_first_not_of(" \t");
      if (first != std::string::npos && code.compare(first, 2, "//") == 0)
         continue;                                   /* a commented-out call */
      bool wrapper = l.find("/l2guard.c:") != std::string::npos;   /* defines the wrapper */
      if (!wrapper && code.find("Xil_L2CacheFlush") != std::string::npos &&
          code.find("Xil_L2CacheFlushRange") == std::string::npos) {
         raw++;
         printf("  raw whole-L2 flush: %s", line);
      }
      size_t z = code.find("z3660_l2_flush(");
      if (z != std::string::npos) {
         std::string arg = code.substr(z + 15);
         arg = arg.substr(0, arg.find(')'));
         for (unsigned i = 1; i < L2G_SITES; i++) {
            static const char *const ids[L2G_SITES] = { "L2G_NONE", "L2G_VIDEO_FLUSH",
               "L2G_VIDEO_PLAY", "L2G_MAIN_INIT", "L2G_LOAD_ROM", "L2G_LOAD_ROMEXT",
               "L2G_CPU_FREQ", "L2G_FLOPPY_INIT", "L2G_FLOPPY_REG", "L2G_SCSI_INIT",
               "L2G_SCSI_FSHD", "L2G_SCSI_DRIVER", "L2G_SCSI_SETFSH_NONE", "L2G_SCSI_SETFSH",
               "L2G_SCSI_LOADFS", "L2G_STRESS" };
            if (arg == ids[i]) uses[i]++;
         }
      }
   }
   pclose(p);
   CHECK(lines > 0, "the grep found the core-0 sources\n");
   CHECK(raw == 0, "no raw Xil_L2CacheFlush() call outside l2guard.c: %d\n", raw);
   for (unsigned i = 1; i < L2G_SITES; i++)
      CHECK(uses[i] == 1, "site %s used once: %d\n", l2g_site_names[i], uses[i]);
}

int main(void)
{
   printf("l2guard_test\n");
   t_mask_and_restore();
   t_bookkeeping();
   t_record();
   t_decode_and_text();
   t_sources();
   printf("l2guard_test: %d passed, %d failed\n", g_pass, g_fail);
   return g_fail ? 1 : 0;
}
