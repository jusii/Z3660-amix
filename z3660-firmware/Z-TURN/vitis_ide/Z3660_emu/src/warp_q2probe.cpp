/*
 * warp_q2probe.cpp -- Plan WARP ENGINE, milestone M0, question Q2.
 *
 * THE QUESTION.  docs/WARP-ENGINE.md Sec 2e derives, but does not measure, that a C++
 * `throw` raised in a helper called FROM generated code cannot be unwound: generated
 * code lives in malloc'd DDR, is in no linked object, and therefore has no .ARM.exidx
 * entry for the ARM EHABI unwinder to find.  The unwinder's documented response to a PC
 * it cannot index is failure, and failure becomes std::terminate.  The whole fault
 * chapter -- bail-before-fault, the state-sync discipline, the write-back exclusion --
 * follows from that being true.  A derivation is not a measurement, so this probe
 * measures it, on the target, for the price of one boot.
 *
 * IT MUST NOT SHIP.  It emits code into the heap and then deliberately provokes an
 * unwind failure.  Two independent gates keep it out of every shipping image:
 *   1. the Makefile filters this file out of the source list unless Z3660_Q2PROBE=1,
 *   2. the whole translation unit is empty unless Z3660_WARP_Q2_PROBE is defined,
 * and its one call site in main.cpp is inside the same #if.  BOOT.BIN, BOOT-lean.BIN and
 * BOOT-prof.BIN are byte-identical with this file present and with it absent.
 *
 * WHAT IT PRINTS, and why the printing is designed rather than incidental.  One of the
 * three outcomes -- termination -- destroys the process that would have reported it, so
 * "no output" has to be a readable answer and not an ambiguity.  Every experiment
 * therefore prints a SENTINEL line before the call that could kill it, and names in that
 * line what silence afterwards would mean.  A capture of one boot is then decidable
 * whatever happens:
 *
 *   [WARP-Q2] A: RESULT = CONTAINED ...     the helper's own TRY/CATCH holds a throw that
 *                                           never reaches generated code   (Sec 2e opt. 1)
 *   [WARP-Q2] A: <silence after SENTINEL>   generated code did not even run -- an I-cache
 *                                           or execute-permission failure, not an unwind one
 *   [WARP-Q2] B: RESULT = CROSSED ...       the throw unwound THROUGH generated code
 *   [WARP-Q2] B: RESULT = TERMINATED ...    std::terminate was reached: the unwinder
 *                                           refused the generated frame (the predicted answer)
 *   [WARP-Q2] B: <silence after SENTINEL>   died harder than terminate -- abort/fault/reboot;
 *                                           read this as TERMINATED, and note it was worse
 *
 * ORDER IS DELIBERATE.  The containment experiment (A) runs FIRST because it is the safe
 * one, so its answer is already on the wire before the experiment that can end the boot.
 *
 * FREE EVIDENCE FOR Q3.  Both stubs are made visible to the I-cache with the sequence the
 * firmware actually ships (arm_flush_cache + arm_icache_invalidate, exactly as
 * flush_cpu_icache() composes them) -- stub A with that and nothing else, stub B with the
 * ISB and branch-predictor invalidate Q3 proposes adding.  Two stubs is far too small a
 * volume to settle Q3; a stub that fails to execute at all is still worth knowing about
 * before a codegen session is booked.
 */

#if defined(Z3660_WARP_Q2_PROBE)

#include <stdint.h>
#include <stdlib.h>
#include <setjmp.h>
#include <exception>

/* TRY / CATCH / THROW come from mmu_common.h -- the SAME macros the MMU engine and the
 * 040 run loop use, not a local re-spelling of them. That is the point: a copy and the
 * original can drift, and this probe is only worth booting if it exercises the exception
 * mechanism the emulator actually raises faults with. mmu_common.h needs the standard
 * UAE prologue below to compile -- it defines the phys_get_ and phys_put_ helpers inline
 * over the memory-access API -- so the prologue is mirrored from newcpu_mmu_glue.cpp. */
#include "sysconfig.h"
#include "sysdeps.h"
#include "options.h"
#include "memory.h"
#include "newcpu.h"
#include "mmu_common.h"

extern "C" void z3660_printf(const char *format, ...);

/* Defined in jit/compemu_midfunc_arm.cpp (textually included into
 * jit/compemu_support_arm.cpp).  Declared here with the identical spelling so the C++
 * mangling matches whatever those typedefs resolve to.  These two ARE the shipped
 * sequence: DCCIMVAC per 32-byte line then DSB, ICIMVAU per line then DSB. */
void arm_flush_cache(uintptr_t addr, uint32_t length);
void arm_icache_invalidate(uintptr_t addr, uint32_t length);

/* The stub.  Four ARM instructions, hand-encoded, no assembler involved:
 *
 *   e92d4010   push {r4, lr}   preserve lr across the call; two registers keeps SP
 *                              8-byte aligned as AAPCS requires
 *   e12fff30   blx  r0         call the helper whose address arrived in r0.  blx-register
 *                              honours the low bit, so it is correct whether the helper
 *                              was compiled ARM or Thumb
 *   e8bd4010   pop  {r4, lr}
 *   e12fff1e   bx   lr         r0 is untouched, so the helper's return value is the stub's
 *
 * This is the smallest thing that is genuinely a frame of generated code with a call in
 * it, which is exactly the shape Sec 2e is about. */
static const uint32_t warp_q2_stub_words[4] = {
   0xe92d4010u, 0xe12fff30u, 0xe8bd4010u, 0xe12fff1eu
};

typedef uint32_t (*warp_q2_stub_t)(uint32_t (*helper)(void));

#define WARP_Q2_STUB_BYTES ((uint32_t)sizeof(warp_q2_stub_words))
#define WARP_Q2_CONTAINED_MAGIC 0x57415221u   /* 'WAR!' */
#define WARP_Q2_THROW_CODE      0x5750u       /* what B throws, to prove identity outside */

static volatile int warp_q2_helper_entered;

/* --- experiment A's helper: throws, and contains its own throw ----------------------
 * This is Sec 2e option 1 in miniature.  The throw is raised and caught inside one C++
 * frame; generated code sees only an ordinary return value.  Nothing crosses. */
static uint32_t warp_q2_helper_contained(void)
{
   volatile uint32_t status = 0;
   warp_q2_helper_entered = 1;
   TRY(prb) {
      THROW(WARP_Q2_THROW_CODE);
   } CATCH(prb) {
      status = WARP_Q2_CONTAINED_MAGIC | ((uint32_t)(int)prb << 16);
   } ENDTRY
   return status;
}

/* --- experiment B's helper: throws, and does NOT contain it ------------------------- */
static uint32_t warp_q2_helper_throwing(void)
{
   warp_q2_helper_entered = 1;
   THROW(WARP_Q2_THROW_CODE);
   return 0;   /* not reached */
}

/* --- the terminate handler ---------------------------------------------------------
 * std::terminate calls this BEFORE abort(), so it is the one place the terminated
 * outcome can still speak.  It then longjmps back to the probe so the rest of the boot
 * survives; if that does not work the answer has already been printed, which is the
 * whole reason the print comes first. */
static jmp_buf warp_q2_terminate_jmp;
static volatile int warp_q2_terminate_armed;
static std::terminate_handler warp_q2_prev_terminate;

static void warp_q2_on_terminate(void)
{
   z3660_printf("[WARP-Q2] B: RESULT = TERMINATED -- std::terminate reached; the ARM EHABI\n");
   z3660_printf("[WARP-Q2] B:          unwinder refused the generated frame (no .ARM.exidx entry).\n");
   if (warp_q2_terminate_armed) {
      warp_q2_terminate_armed = 0;
      longjmp(warp_q2_terminate_jmp, 1);
   }
   if (warp_q2_prev_terminate)
      warp_q2_prev_terminate();
   for (;;) { }
}

/* Emit the stub into heap memory and make it visible to the I-cache.
 * malloc() is not a shortcut here: cache_alloc() in jit/compemu_support_arm.cpp is
 * itself malloc() (its mmap arm is commented out), so the probe's memory has exactly
 * the attributes the real code cache has -- core1's DDR heap, which the section-table
 * loop in main.cpp leaves at the BSP default: normal write-back cacheable, executable. */
static uint32_t *warp_q2_emit_stub(const char *tag, int with_isb_and_bpiall)
{
   uint32_t *stub = (uint32_t *)malloc(WARP_Q2_STUB_BYTES);
   if (stub == 0) {
      z3660_printf("[WARP-Q2] %s: ABORTED -- malloc(%lu) failed, no probe possible\n",
                   tag, (unsigned long)WARP_Q2_STUB_BYTES);
      return 0;
   }
   for (unsigned i = 0; i < WARP_Q2_STUB_BYTES / 4; i++)
      stub[i] = warp_q2_stub_words[i];

   /* The shipped sequence, composed exactly as flush_cpu_icache() composes it. */
   arm_flush_cache((uintptr_t)stub, WARP_Q2_STUB_BYTES);
   arm_icache_invalidate((uintptr_t)stub, WARP_Q2_STUB_BYTES);

   if (with_isb_and_bpiall) {
      /* What Q3 proposes adding before trusting the sequence at JIT volumes:
       * BPIALL (branch-predictor invalidate all) + DSB + ISB. */
      __asm__ __volatile__("mcr p15, 0, %0, c7, c5, 6" :: "r"(0) : "memory"); /* BPIALL */
      __asm__ __volatile__("dsb" ::: "memory");
      __asm__ __volatile__("isb" ::: "memory");
   }

   z3660_printf("[WARP-Q2] %s: stub @ 0x%08lx = %08lx %08lx %08lx %08lx  (icache seq: %s)\n",
                tag, (unsigned long)(uintptr_t)stub,
                (unsigned long)stub[0], (unsigned long)stub[1],
                (unsigned long)stub[2], (unsigned long)stub[3],
                with_isb_and_bpiall ? "shipped + BPIALL/DSB/ISB" : "shipped, as-is");
   return stub;
}

void warp_q2_probe(void)
{
   z3660_printf("[WARP-Q2] ==== WARP M0 / Q2 unwind probe ====\n");
   z3660_printf("[WARP-Q2] question: can a C++ throw cross a frame of generated code?\n");

   /* ---------------- experiment A: containment (the safe one) ---------------- */
   uint32_t *stub_a = warp_q2_emit_stub("A", 0);
   if (stub_a) {
      warp_q2_helper_entered = 0;
      z3660_printf("[WARP-Q2] A: SENTINEL -- calling generated code; the helper CONTAINS its own\n");
      z3660_printf("[WARP-Q2] A:            throw. Silence after this line means generated code did\n");
      z3660_printf("[WARP-Q2] A:            not execute at all (I-cache or execute-permission), which\n");
      z3660_printf("[WARP-Q2] A:            is a DIFFERENT failure from the unwind question.\n");
      uint32_t st = ((warp_q2_stub_t)(void *)stub_a)(warp_q2_helper_contained);
      if (!warp_q2_helper_entered) {
         z3660_printf("[WARP-Q2] A: RESULT = NO-CALL -- the stub returned without reaching the helper\n");
      } else if ((st & 0xffffu) == (WARP_Q2_CONTAINED_MAGIC & 0xffffu)) {
         z3660_printf("[WARP-Q2] A: RESULT = CONTAINED (status=0x%08lx, prb=%lu) -- generated code ran,\n",
                      (unsigned long)st, (unsigned long)(st >> 16));
         z3660_printf("[WARP-Q2] A:          called C++, and the throw was caught inside the helper.\n");
      } else {
         z3660_printf("[WARP-Q2] A: RESULT = WRONG-STATUS 0x%08lx -- helper ran but returned garbage\n",
                      (unsigned long)st);
      }
   }

   /* ---------------- experiment B: crossing (the one that can kill the boot) -------- */
   uint32_t *stub_b = warp_q2_emit_stub("B", 1);
   if (stub_b) {
      warp_q2_helper_entered = 0;
      warp_q2_prev_terminate = std::set_terminate(warp_q2_on_terminate);
      z3660_printf("[WARP-Q2] B: SENTINEL -- calling generated code; the helper THROWS ACROSS it.\n");
      z3660_printf("[WARP-Q2] B:            If no further [WARP-Q2] line appears, read the answer as\n");
      z3660_printf("[WARP-Q2] B:            TERMINATED-HARD (abort/fault/reboot before terminate could\n");
      z3660_printf("[WARP-Q2] B:            speak). Exactly one of CROSSED / TERMINATED / silence follows.\n");

      if (setjmp(warp_q2_terminate_jmp) == 0) {
         warp_q2_terminate_armed = 1;
         TRY(prb) {
            ((warp_q2_stub_t)(void *)stub_b)(warp_q2_helper_throwing);
            z3660_printf("[WARP-Q2] B: RESULT = NO-THROW -- the stub returned normally; the helper did\n");
            z3660_printf("[WARP-Q2] B:          not throw, so this boot answered nothing.\n");
         } CATCH(prb) {
            z3660_printf("[WARP-Q2] B: RESULT = CROSSED (caught outside, prb=%d, helper_entered=%d) --\n",
                         (int)prb, warp_q2_helper_entered);
            z3660_printf("[WARP-Q2] B:          the throw unwound THROUGH the generated frame.\n");
         } ENDTRY
         warp_q2_terminate_armed = 0;
      } else {
         z3660_printf("[WARP-Q2] B: (recovered from terminate via longjmp; boot continues)\n");
      }

      std::set_terminate(warp_q2_prev_terminate);
   }

   z3660_printf("[WARP-Q2] ==== probe end ====\n");
}

#endif /* Z3660_WARP_Q2_PROBE */
