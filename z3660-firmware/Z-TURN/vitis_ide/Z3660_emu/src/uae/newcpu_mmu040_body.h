/* THE 68040 MMU RUN LOOP'S BODY, INCLUDED TWICE -- docs/WARP-ENGINE.md §0.17.1.
 *
 * WHY THIS FILE EXISTS. A dormant engine has to be free, and §0.16.2 measured that it was
 * not: the disarmed image paid -3.445 % against the clean interpreter lineage, and the
 * ablation kit attributed **+1.967 pp of it to `L-HOOK`** -- the run loop's
 * `if (z3660_warp_on) ... z3660_warp_step()` gate and the loop body restructured around it.
 * The A3/A4/A5 variants, which remove that arm at compile time, measured 8 064 / 8 140 /
 * 8 163 against a shipping lean of 7 903.
 *
 * SO THE DISARMED PATH MUST NOT RUN A CHEAPER GATE. It must run a loop that does not contain
 * one -- the same shape A3/A4/A5 compile, because that shape is what measured the rows this
 * fix has to reproduce. This file is the loop body; `newcpu.cpp` includes it twice:
 *
 *    Z3660_RUNLOOP_WARP 0  ->  m68k_run_mmu040_nowarp   no hook, no block-consumes-the-pass
 *                                                       branch, no `else` around the
 *                                                       interpreter body. Textually the loop
 *                                                       as it was before WARP existed.
 *    Z3660_RUNLOOP_WARP 1  ->  m68k_run_mmu040_warp     the hooked loop, unchanged.
 *
 * IT IS THE PREPROCESSOR AND NOT THE OPTIMISER, deliberately. The obvious alternative is one
 * `always_inline` body taking a `const int warp` and letting -Os fold the dead arm -- and
 * that would make "no hook compiled in" a claim about gcc's constant propagation. §0.15.2 is
 * this campaign's standing proof that a claim about what a build produces has to be readable
 * off the artifact: round 4 shipped an image its own source does not build, measured a
 * penalty on it, and scored a gate MISSED against it. A `#if` cannot be mis-optimised, and
 * `tools/warp-gate.sh` reads both loops off the ELF afterwards anyway.
 *
 * IT IS A `.h` AND NOT A `.cpp` FOR A MECHANICAL REASON: `Z3660_emu/Makefile` collects its
 * sources with `find ... -name '*.cpp'`, so a body named `.cpp` would also be compiled
 * standalone, as a third copy with neither macro defined.
 *
 * ONE CONSEQUENCE WORTH NAMING. The `Z3660_EMU_PROBES` diagnostics inside carry
 * function-scope statics (`prev_pc`, `rom_hits`); two instantiations means two independent
 * sets of them. They are capped, first-few-hits diagnostics and the probes build is not a
 * shipping variant, so per-variant counters are correct rather than merely harmless -- but
 * it is stated here rather than discovered in a trace.
 *
 * SELECTION HAPPENS ONCE, at the arm/disarm transition, in `m68k_go`'s existing `run_func`
 * choice; nothing in either loop tests the arm state per pass. See newcpu.cpp's
 * `m68k_run_mmu040()` selector and `z3660_knobs_poll()`'s SPCFLAG_MODE_CHANGE.
 */
#if !defined(Z3660_RUNLOOP_WARP) || !defined(Z3660_RUNLOOP_NAME)
#error "newcpu_mmu040_body.h needs Z3660_RUNLOOP_WARP (0/1) and Z3660_RUNLOOP_NAME"
#endif

/* noinline, noclone: each variant must keep its own symbol and exactly one body, or
 * tools/warp-gate.sh cannot read the disarmed loop's shape off the ELF and G-R6.6 is
 * unenforceable. -flto sees every call site of both and would otherwise be free to fold
 * them into the selector. */
__attribute__((noinline, noclone)) void Z3660_RUNLOOP_NAME(void)
{
   struct flag_struct f;
   int halt = 0;

   Z3660_PROF_LOOP_RESET();   // C1: do_specialties() can return out of this loop mid-stage
   z3660_dopc_ext_pc = Z3660_DOPC_NO_PC;   // C2: no window survives a loop re-entry
   while (!halt) {
      TRY(prb) {
         for (;;) {
            f = regs.ccrflags;               // snapshot flags for instruction restart
            mmu_restart = true;
            regs.instruction_pc = m68k_getpc();

#if Z3660_EMU_PROBES
            // ROM-ENTRY PROBE (diagnostic, AMIX-040 only, first few hits then silent).
            // Gated by z3660_probes_on (the overhead switches). gcc evaluates the volatile
            // amix_mmu_on arm first, so a non-AMIX boot pays nothing at all and an
            // AMIX boot pays 4 instructions to skip 44. It matters more than it looks
            // -- the amix_mmu_on arm below is TRUE for every instruction of an AMIX-040
            // boot, so the pc masks, the two compares and the prev_pc store ran on every
            // guest instruction for the whole run, long after rom_hits was exhausted.
            // The F87F6FA7 trace put the failure at 0x00F80B4C -- the `rts` at the tail of
            // AmigaOS exec's CPU-exception dispatcher in KS 2.04, which jumps to
            // ExecBase->ThisTask->tc_TrapCode read from ABSOLUTE ADDRESS 4. The kernel's own
            // vector table (M68Kvec, .text+0x1378 == the observed vbr 0x08001378) relocates
            // entirely to kernel symbols -- no ROM address anywhere -- and the trace saw zero
            // interrupts, so NO exception vectored there. Control was transferred by a
            // pointer. This names the source pc and the pushed return address, which is the
            // one fact the kernel binary cannot supply and the emulator can.
            if (z3660_probes_on && amix_mmu_on && currprefs.mmu_model == 68040) {
               static uae_u32 prev_pc = 0; static int rom_hits = 0;
               uae_u32 pc_now = (uae_u32)regs.instruction_pc;
               int in_rom  = (pc_now  & 0xFFF80000u) == 0x00F80000u;
               int was_rom = (prev_pc & 0xFFF80000u) == 0x00F80000u;
               if (in_rom && !was_rom && rom_hits < 4) {
                  rom_hits++;
                  uae_u32 sp = m68k_areg(regs, 7);
                  z3660_printf("[ROMENT%d] pc=%08lX from=%08lX a7=%08lX sp@=%08lX sp@4=%08lX execbase@4=%08lX\r\n",
                               rom_hits, (unsigned long)pc_now, (unsigned long)prev_pc,
                               (unsigned long)sp, (unsigned long)get_long(sp),
                               (unsigned long)get_long(sp + 4), (unsigned long)get_long(4));
               }
               prev_pc = pc_now;
            }
#endif /* Z3660_EMU_PROBES */

            do_cycles(cpu_cycles);

            /* ================================================================= WARP =====
             * THE BLOCK ENGINE'S HOOK, and it sits here for the same reason the decoded-op
             * cache's lookup sits twelve lines below: this is the last moment at which the
             * loop still has a choice about paying for the fetch and the dispatch.
             *
             * DISARMED IT IS ONE LOAD, ONE TEST AND A PERFECTLY PREDICTED BRANCH -- three
             * ARM instructions, pre-registered in §0.8.4 G-R2.8 and read off the lean
             * objdump, never estimated. Round 1 measured FIVE, because the two argument
             * loads for an out-of-line call were hoisted above the test; the fast path is
             * inlined now, so nothing is loaded until the arm is known to be live. It is a
             * SHIPPED cost until the M5 gate rather than a free one, because §30's pricing
             * law says work prepended to the path arrives in full and the one thing this
             * round must not do is charge itself nothing.
             *
             * THE PASS-CONSUMING ARM LANDS HERE, and it is the other half of the
             * no-empty-pass invariant. A block entry either retires >= 1 guest instruction
             * and CONSUMES this pass -- the interpreter body below is skipped entirely and
             * the tail runs once -- or retires 0 and falls through inside the same pass, in
             * which case the loop runs exactly as it always has. There is no third outcome
             * and therefore no re-entry guard: progress is guaranteed by construction.
             *
             * CYCLES ARE PRESERVED BY CONSTRUCTION, not by approximation. Every handler in
             * the cpuemu_31 table this loop dispatches returns the same constant, so a
             * block that retired n instructions is worth exactly n of them and the harness
             * pins that constant against a real handler's return value.
             *
             * THE CADENCE FEED IS CALLED FROM THE CALL SITE AND ONLY WHEN IT HAS WORK.
             * Round 1 called it unconditionally with 0 and paid ten ARM instructions -- a
             * call, a prologue, an early-out and a return -- to do nothing; §0.7.3 named
             * that as one of the four shapes that were "not the engine". n <= 1 needs no
             * feed at all, because the pass's own tail supplies that one tick (Q10, frozen).
             * =========================================================================== */
#if Z3660_RUNLOOP_WARP
            unsigned z3660_warp_n = 0;
#endif
#if Z3660_EMU_PROF
            /* Hoisted above the WARP arm because the tail's sampler reads it and a block
             * pass skips the fetch that would otherwise set it. A block pass therefore
             * samples opcode 0, which is declared rather than discovered: the WARP verdict
             * rows are read from COUNTERS, and the family histogram of an armed profiling
             * capture is not one of them. */
            uae_u16 z3660_prof_op = 0;
#endif
            /* THE HOOK, AND THE WHOLE POINT OF THE TWO-INSTANTIATION SPLIT (§0.17.1). With
             * Z3660_RUNLOOP_WARP 0 this arm is not compiled: no gate, no
             * block-consumes-the-pass branch, and no `else` around the interpreter body --
             * the loop is textually what it was before WARP existed. That instantiation is
             * the one a DISARMED image runs, and it is the shape the A3/A4/A5 ablation
             * variants measured at 8 064 / 8 140 / 8 163 (§0.16.2). The engine is still
             * compiled and still linked; only the hook is absent, and only from this loop.
             * (This guard used to read `#if !Z3660_WARP_ABL_NO_HOOK` -- the ablation kit's
             * L-HOOK layer, §0.14.1. The kit measured the cost; the split removes it.) */
#if Z3660_RUNLOOP_WARP
            if (z3660_warp_on)
               z3660_warp_n = z3660_warp_step((unsigned)regs.instruction_pc);

            if (z3660_warp_n != 0) {
               cpu_cycles = (int)(z3660_warp_n * (unsigned)Z3660_WARP_CYC_PER_INSN);
               if (z3660_warp_n > 1u)
                  z3660_warp_cadence_feed(z3660_warp_n);
            } else {
#endif
            mmu_opcode = -1;
            /* Interpreter speed-up rung 1. The decoded-op cache is consulted HERE, before
             * x_prefetch, because this is the last moment at which the loop still has a
             * choice about paying for the fetch. A hit supplies the opcode word, the
             * resolved handler, rung 0's idiom verdict and one extension word; a miss is
             * byte-for-byte the fetch this loop has always done, plus one lookahead read
             * to fill the extension slot. See the long comment above the table.
             *
             * C2 rung 1d: the lookup carries its OWN bucket (DOPCFIND) rather than falling
             * to LOOP, because map v3 SS15.4's re-attribution hypothesis -- that LOOP's
             * 26.4 % now CONTAINS the hit path rung 1 added -- is exactly the claim this
             * bracket settles. It closes on BOTH arms before anything else runs: on the
             * miss arm before x_prefetch(), so the opcode fetch stays in FETCHOP where
             * every earlier capture has it, and so the bucket measures the LOOKUP rather
             * than the lookup-plus-whatever-the-miss-costs.
             *
             * THE ONE PLACE RUNG 1d MOVED LEAN SOURCE, and it is declared rather than
             * hoped over. z3660_prof.h's idiom is that the non-profiling expansion is
             * TEXTUALLY the original code; the two extension-slot locals below used to be
             * declared AFTER the lookup, and bracketing the lookup would have put their
             * two zero-stores inside DOPCFIND -- roughly a sixth of a bucket whose whole
             * content is a generation compare and four tag compares. So they are hoisted
             * two lines instead, and the deviation was MEASURED rather than argued:
             * objdump of the lean newcpu.o before and against after differs by exactly
             * those two stores changing position and by nothing else at all. Neither
             * local is readable by z3660_dopc_find(), so the reorder cannot be observed. */
            const uaecptr z3660_dopc_pc = (uaecptr)regs.instruction_pc;
            int     z3660_dopc_way    = 0;
            int     z3660_dopc_ext_ok = 0;
            uae_u16 z3660_dopc_ext0   = 0;
            Z3660_PROF_ENTER(Z3660_PROF_B_DOPCFIND);
            struct z3660_dopc_set *z3660_dopc_e = z3660_dopc_find(z3660_dopc_pc, &z3660_dopc_way);
            if (z3660_dopc_e) {
               mmu_opcode = regs.opcode = z3660_dopc_e->opcode[z3660_dopc_way];
               if (z3660_dopc_e->meta[z3660_dopc_way] & Z3660_DOPC_M_EXT0) {
                  z3660_dopc_ext_val = z3660_dopc_e->ext0[z3660_dopc_way];
                  z3660_dopc_ext_pc  = (unsigned int)(z3660_dopc_pc + 2u);
               } else {
                  z3660_dopc_ext_pc  = Z3660_DOPC_NO_PC;
               }
               Z3660_PROF_EXIT();
            } else {
               /* Disarm BEFORE the fetch: the previous instruction's window must not be
                * allowed to answer this one's opcode, and on the fill path below the
                * lookahead read has to be a real fetch or the slot would cache itself. */
               z3660_dopc_ext_pc = Z3660_DOPC_NO_PC;
               Z3660_PROF_EXIT();
               Z3660_PROF_MARK_OPFETCH();   // C1: this fetch is the OPCODE, not an extension word
               mmu_opcode = regs.opcode = x_prefetch(0);   // MMU-translated opcode fetch
               if (z3660_dopc_on && z3660_dopc_ext_reachable(z3660_dopc_pc)) {
                  z3660_dopc_ext0   = (uae_u16)get_iword_mmu040(2);
                  z3660_dopc_ext_ok = 1;
               }
            }
            const uae_u16 z3660_dopc_op = (uae_u16)regs.opcode;
            count_instr(regs.opcode);
#if Z3660_EMU_PROF
            /* Snapshot the opcode for the sampler. regs.opcode is fair game for a handler
             * to overwrite, and mmu_opcode is not reset on this loop's path -- neither is
             * a promise, and a sample record that names the wrong instruction is worse
             * than no sample at all. One store per instruction, profiling build only.
             * Declared above the WARP arm, assigned here. */
            z3660_prof_op = (uae_u16)regs.opcode;
#endif

#if Z3660_EMU_PROBES
            /* Overhead switches: the three post-fetch ISSUE-52 probes collapsed behind ONE
             * gate -- 3 ARM instructions for the whole group instead of a per-probe
             * compare chain, and the branch is perfectly predicted. [USTEP]
             * is the reason this is worth doing: its first term is `!regs.s`, which is
             * TRUE for every user-mode instruction, so an AMIX-040 user-space workload
             * paid the full four-term chain on the majority of its instructions with the
             * cap long since exhausted. Nothing is deleted -- PROBES/emu_probes turns
             * the whole set back on, and Z3660_EMU_PROBES=0 compiles it out for a lean
             * build. */
            if (z3660_probes_on) {
            /* ISSUE-52 RTE PROBE (diagnostic). The harness proves RTE delivers a
             * table-walked frame's PC exactly (test_mmu040_rte_delivers_pc), so
             * "fed 0x80800000, delivered 0x80000012" cannot happen inside the core --
             * which means the frame already holds the wrong PC when RTE reads it.
             * This shows what RTE actually consumes. Cost: one opcode compare per
             * instruction, then a range test on a7; the frame is only read for RTEs
             * whose stack is in the u-block window (0x40000000-0x4000FFFF), which is
             * exactly the frame class _start builds, and those are rare. Capped at 8
             * lines. Reading the frame here cannot introduce a new fault class: RTE is
             * about to read the same three locations. */
            if (regs.opcode == 0x4E73 && regs.s && amix_mmu_on
                && currprefs.mmu_model == 68040 && amix_rte_probe_n < 8) {
               uaecptr _a = m68k_areg(regs, 7);
               if ((_a & 0xFFFF0000u) == 0x40000000u) {
                  amix_rte_probe_n++;
                  z3660_printf("[RTE%d] a7=%08lX sr=%04lX pc=%08lX fmt=%04lX usp=%08lX\r\n",
                               amix_rte_probe_n, (unsigned long)_a,
                               (unsigned long)get_word_mmu040(_a),
                               (unsigned long)get_long_mmu040(_a + 2),
                               (unsigned long)get_word_mmu040(_a + 6),
                               (unsigned long)regs.usp);
               }
            }

            /* ISSUE-52 USER-TRANSITION RTE PROBE (diagnostic). Fires on ANY RTE (not just
             * u-block-stacked) whose frame drops to USER mode -- the frame SR word at a7+0
             * has the S bit (0x2000) CLEAR. This catches proc 1's launch RTE (firing #1) and
             * the syscall RETURN from icode's trap #0 (firing #2), which the plain [RTE]
             * u-block gate can conflate. The decisive field is `usp`: the value regs.usp
             * holds NOW is what MakeFromSR will install into A7 when the S bit clears. A
             * clean #1 usp (valid user stack) with a corrupt #2 usp (the kernel SSP
             * 0x40001FC0, or garbage/odd like 0xCB7C0002) against an otherwise-correct
             * kernel-built frame means the emulator's RTE-from-syscall isn't restoring USP
             * (family #7). If instead the FRAME (pc/sr) is wrong, it is kernel-side.
             * `ssp` is a7 before the pop; `usp` is read before MakeFromSR runs. Same reads
             * RTE is about to do; capped, one line per slot. */
            if (regs.opcode == 0x4E73 && regs.s && amix_mmu_on
                && currprefs.mmu_model == 68040 && amix_urte_probe_n < 8) {
               uaecptr _a = m68k_areg(regs, 7);
               uae_u16 _fsr = (uae_u16)get_word_mmu040(_a);
               if ((_fsr & 0x2000u) == 0) {   /* frame SR: S clear -> RTE drops to user */
                  amix_urte_probe_n++;
                  z3660_printf("[URTE%d] pc=%08lX sr=%04lX ssp=%08lX usp=%08lX fmt=%04lX\r\n",
                               amix_urte_probe_n,
                               (unsigned long)get_long_mmu040(_a + 2),
                               (unsigned long)_fsr,
                               (unsigned long)_a,
                               (unsigned long)regs.usp,
                               (unsigned long)get_word_mmu040(_a + 6));
               }
            }

            /* ISSUE-52 round-5 USER SINGLE-STEP PROBE (diagnostic). Prints the pre-state of
             * the first up-to-8 USER-mode instructions executed under AMIX-040 -- i.e. proc 1's
             * icode. regs.opcode / regs.instruction_pc are already the instruction about to run;
             * a7 and d0 are the register state BEFORE it. So step 0 shows the icode's lea about
             * to run (pc=0x80800000 op=0x4ffb a7=<launch stack>), and step 1 shows the state
             * AFTER the lea -- its a7 is the lea's computed EA. The decisive cells:
             *   step 0 op must be 0x4ffb; step 1 a7 must be 0x8080002A (base 0x80800002 + bd
             *   0x28) and pc must be 0x80800008 (opword 2 + ext 2 + bd 4). A wrong step-1 a7
             *   means the full-format PC-relative EA is mis-computed; a step-1 pc != 0x80800008
             *   means the extension length was mis-parsed and the stream (incl. trap #0) is
             *   skewed. Capped, one line per step, gated to the AMIX-040 user path only. */
            if (!regs.s && amix_mmu_on && currprefs.mmu_model == 68040 && amix_ustep_n < 8) {
               z3660_printf("[USTEP%d] pc=%08lX op=%04lX a7=%08lX d0=%08lX\r\n",
                            amix_ustep_n,
                            (unsigned long)regs.instruction_pc,
                            (unsigned long)(regs.opcode & 0xFFFF),
                            (unsigned long)m68k_areg(regs, 7),
                            (unsigned long)m68k_dreg(regs, 0));
               amix_ustep_n++;

               /* ISSUE-52 round-6 I-vs-D TRANSLATION COMPARE (one-shot). The [USTEP] trace
                * showed the user instruction FETCH returning garbage for offsets > 0 of a
                * page whose bytes the kernel wrote via the DATA path -- so the INSTRUCTION
                * translation of a user VA resolves to a different physical frame than the
                * DATA translation, or the fetched frame's content is wrong. This reads the
                * known-wrong spot (pc+4, the lea's 32-bit base displacement) BOTH ways and
                * prints each path's physical translation and value. Decisive:
                *   iphys != dphys           -> INSTRUCTION and DATA resolve different frames
                *                               (I-vs-D ATC / user-page translation coherence)
                *   iphys == dphys, iw==dw   -> same frame, content wrong (the write never
                *     both 0x4AFC0000...        landed there: data-write / MOVE16 / kernel)
                *   iphys == dphys, iw!=dw   -> same frame, the PHYSICAL long read itself
                *                               diverges I-vs-D (bus/accessor)
                * Fires once, for the first user instruction. Wrapped so a fault cannot
                * derail the boot; each mmu_translate/read is exactly what the fetch does. */
               if (amix_ustep_n == 1) {
                  uaecptr _t = (uaecptr)regs.instruction_pc + 4;
                  uae_u32 iphys = 0xFFFFFFFF, dphys = 0xFFFFFFFF, iw = 0xFFFFFFFF, dw = 0xFFFFFFFF;
                  TRY(pi) { iphys = mmu_translate(_t, 0, false, false, false, sz_long); } CATCH(pi) { (void)pi; } ENDTRY
                  TRY(pd) { dphys = mmu_translate(_t, 0, false, true,  false, sz_long); } CATCH(pd) { (void)pd; } ENDTRY
                  TRY(pw) { iw = uae_mmu040_get_ilong(_t); } CATCH(pw) { (void)pw; } ENDTRY
                  TRY(pv) { dw = get_long_mmu040(_t);      } CATCH(pv) { (void)pv; } ENDTRY
                  z3660_printf("[IDCMP] at=%08lX iphys=%08lX dphys=%08lX iword=%08lX dword=%08lX srp=%08lX urp=%08lX\r\n",
                               (unsigned long)_t, (unsigned long)iphys, (unsigned long)dphys,
                               (unsigned long)iw, (unsigned long)dw,
                               (unsigned long)regs.srp, (unsigned long)regs.urp);
               }
            }
            }   /* if (z3660_probes_on) -- post-fetch probe group */
#endif /* Z3660_EMU_PROBES */
            /* Interpreter speed-up rung 0. The block-idiom recognizer sits exactly here -- after
             * the opcode is in hand, before the dispatch -- because that is the only place
             * that knows both what the instruction is and that the run loop has not yet
             * committed to executing one of it.
             *
             * It is deliberately NOT inside the HANDLER bucket. HANDLER's per-event cost
             * is what the map divides by an instruction count to price a single guest
             * instruction, and a chunk of 256 landing in it would corrupt exactly that
             * figure. The guest stores it performs still pass through the WRITE bracket
             * normally.
             *
             * IT USED TO FALL TO LOOP, and the argument for that was "LOOP is 0.51 %
             * after probe subtraction today, so a post-change capture in which LOOP has
             * grown is unambiguously this and nothing else". Both halves of that are dead:
             * per-bucket spans put LOOP at 26.4 % (map v3 SS15), so it is no longer a quiet
             * place where one new arrival is unambiguous. Rung 1d gives the recognizer its
             * own bucket (BLKREC) instead, which is the same intent implemented against a
             * bucket that is now the largest on the page rather than the smallest. */
            {
               int z3660_blk_cyc = 0, z3660_blk_shape = 1, z3660_blk_did;

               /* THE IDIOM-FLAG INTEGRATION. On a hit whose cached verdict says "not an
                * idiom", the recognizer is not called -- so rung 0's per-dispatch test,
                * which metal priced at about 1 % of dhrystone, costs nothing on the 88 %+
                * of dispatches this cache expects to serve. On a miss the recognizer runs
                * as it always has and reports the verdict the fill will store. */
               z3660_blk_did =
                  (z3660_dopc_e == NULL
                   || (z3660_dopc_e->meta[z3660_dopc_way] & Z3660_DOPC_M_IDIOM))
                  && Z3660_PROF_BLK_RUN(z3660_dopc_op, &z3660_blk_cyc, &z3660_blk_shape);

               /* Cache maintenance BEFORE the dispatch, so nothing here depends on state
                * a handler may have changed -- regs.opcode is documented as fair game for
                * a handler to overwrite, and z3660_blk_run() moves regs.instruction_pc. */
               if (z3660_dopc_e != NULL) {
                  /* Self-correcting downgrade. The verdict is stored conservative-true,
                   * so an entry filled while the machine could not decide (tracing, say)
                   * says "may be an idiom"; the first pass that CAN decide, and decides
                   * no, writes the cheaper answer back. It only ever moves toward the
                   * static truth, and only ever after the recognizer positively
                   * established it. Left in LOOPRES deliberately: it is one conditional
                   * byte store, and bracketing it would charge the HIT path two more
                   * transitions to measure something smaller than the probe. */
                  if (!z3660_blk_shape)
                     z3660_dopc_e->meta[z3660_dopc_way] &= (uae_u8)~Z3660_DOPC_M_IDIOM;
               } else {
                  /* C2 rung 1d: the FILL gets its own bucket, and it is the one the map's
                   * SS17.3 BAR is about -- "a physical index costs a TRANSLATION ON THE
                   * FILL PATH ... this study must pre-register its cost site and measure
                   * the fill-path price, not assume it". This is that price, on the
                   * current fill, measured before anything is built on top of it. The
                   * bracket is inside the miss arm, so a hit pays nothing for it. */
                  Z3660_PROF_ENTER(Z3660_PROF_B_DOPCFILL);
                  z3660_dopc_fill(z3660_dopc_pc, z3660_dopc_op, z3660_blk_shape,
                                  z3660_dopc_ext_ok, z3660_dopc_ext0);
                  Z3660_PROF_EXIT();
               }

               if (z3660_blk_did) {
                  cpu_cycles = z3660_blk_cyc;
               } else {
                  Z3660_PROF_ENTER(Z3660_PROF_B_HANDLER);
                  cpu_cycles = z3660_dopc_e
                        ? (*z3660_dopc_e->handler[z3660_dopc_way])(regs.opcode)
                        : (*cpufunctbl[regs.opcode])(regs.opcode);
                  Z3660_PROF_EXIT();
               }
            }

            // Same IPL/cross-core poll throttle the 030 loop has, and for the same reason:
            // check_uae_int_request() runs z3660_tasks() + ipl_main_read() + (under AMIX)
            // the WD33C93 hsync pump on EVERY instruction, which is real per-instruction
            // overhead during long guest polling loops. do_specialties() stays
            // per-instruction (STOP/trace/mode-change correctness). cadence 1 -- the
            // default and what the env sets today -- is byte-for-byte the old behaviour;
            // this only makes the existing SERV knob reach the 040 loop as well.
            //
#if Z3660_RUNLOOP_WARP
            }   /* end of the interpreter arm -- a block that retired something skips it */
#endif

            // C2-prep: the bucket opens BEFORE adjust_cycles() and the sampler hook, so
            // the tail's own accounting work is inside it. In v1 both sat outside and
            // were charged to LOOP, which is part of why LOOP looked like the largest
            // bucket in the whole profile.
            Z3660_PROF_ENTER(Z3660_PROF_B_TAIL);
            cpu_cycles = adjust_cycles(cpu_cycles);
            Z3660_PROF_RUNLOOP_SAMPLE(regs.instruction_pc, z3660_prof_op, 1);
            Z3660_PROF_TAIL_POLL_040();
            Z3660_PROF_TAIL_SPECIALTIES(cpu_cycles);
            Z3660_PROF_EXIT();
         }
      } CATCH(prb) {
         /* C2 rung 1: the fault unwound past the rest of the instruction that armed the
          * extension-word window, so nothing is left to consume it and Exception() is
          * about to run with the pc somewhere else entirely. Disarm. */
         z3660_dopc_ext_pc = Z3660_DOPC_NO_PC;
         Z3660_PROF_UNWOUND();
         Z3660_PROF_CNT(Z3660_PROF_C_FAULTS);
         if (mmu_restart) {
            // instruction restart: undo partial effects by rewinding to its start
            regs.ccrflags = f;
            m68k_setpci(regs.instruction_pc);
         }
         cpu_restore_fixup();
         TRY(prb2) {
            Exception(prb);
         } CATCH(prb2) {
            // fault while building the frame == double fault
            halt = CPU_HALT_BUS_ERROR_DOUBLE_FAULT;
         } ENDTRY
      } ENDTRY
   }
   cpu_halt(halt);
}
