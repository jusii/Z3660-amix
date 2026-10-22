; ztest.s -- Z3660 timing-scanner bus test program (P-27 D8/D9).
;
; A 512 KB image for a z3660cfg kickstart slot. The firmware maps it at $F80000 (MAPROM on the
; real 68060, the emulator's ROM bank under UAE 040 JIT). No Kickstart, interrupts masked,
; data cache off. Each pass walks the enabled paths, counts errors per path into the mailbox
; at MB in card RAM, bumps the heartbeat and republishes the record with a CRC and a mirror.
; The ARM writes run parameters at MB+$200 (WM console command). Layout: TESTPROG.md.
;
; Assemble: vasmm68k_mot -Fbin -m68060 -o ztest.raw ztest.s ; then fixsum.py (Kickstart-style
; sum-with-carry fixed to $FFFFFFFF, the self-check of path 8).
;
; Instruction subset: 68040 and 68060 common, no MOVEP/CHK2/CMP2/CAS2/64-bit MUL/DIV, no FPU.
; MOVEC PCR is executed once under a trap to tell the real 060 from the emulated 040.

		machine	68060

BUILD		equ	2			; bump on every change; reported in MB_VER

MB		equ	$0FFF0000		; mailbox page, top 64 KB of CPU RAM
STACKTOP	equ	$0FFFF000

; mailbox (68k big-endian; the ARM byte-swaps longwords)
MB_MAGIC	equ	$000			; 'ZTMB'
MB_VER		equ	$004			; $0001<<16 | BUILD
MB_HB		equ	$008			; completed passes
MB_WHERE	equ	$00C			; path<<16 | substep, or a W_ code
MB_EPOCH	equ	$010			; params epoch the counters belong to
MB_STAT		equ	$014			; ST_ bits, last exception vector in 23:16
MB_RS		equ	$018			; entries at 'start' since the page was valid
MB_ID		equ	$01C			; 68060 PCR, 0 when emulated
MB_REC		equ	$020			; 8 path records of 32 bytes
MB_SZ		equ	$120			; sizing report: present map, mobo mask, rom sum, card acfg hash, mobo acfg hash
MB_XADDR	equ	$134			; last exception: frame long at +8 (060 fault address)
MB_XPC		equ	$138			; last exception: stacked PC
MB_XCNT		equ	$13C			; exceptions taken
MB_CRC		equ	$1FC			; CRC32 of $000-$1FB
MB_MIRROR	equ	$800

; path record
REC_ERR		equ	$00
REC_OPS		equ	$04
REC_BERR	equ	$08
REC_FA		equ	$0C			; first failing address (0 = none yet)
REC_FX		equ	$10			; OR of (expected XOR read)
REC_LAST	equ	$14			; last wrong value read
REC_PRES	equ	$18			; 0 NP, 1 run, 2 NX (emulated), 3 disabled
NPATH		equ	8

; parameters, written by the ARM; zero means "default"
PA_MAGIC	equ	$200			; 'ZTPA'
PA_EPOCH	equ	$204
PA_CMD		equ	$208			; 0 RUN, 1 PARK, 2 SIZE
PA_EN		equ	$20C			; path enable mask (bit p-1)
PA_PRES		equ	$210			; present mask from the baseline; 0 = use own sizing
PA_MODE		equ	$214			; 0 auto, 1 real 060, 2 emulated
PA_CHLO		equ	$218
PA_CHHI		equ	$21C
PA_MOBO		equ	$220			; 1 MB blocks at $07000000 (bit n); 0 = sizing
PA_CLO		equ	$224
PA_CHI		equ	$228
PA_Z3B		equ	$22C
PA_Z3O		equ	$230
PA_Z3L		equ	$234
PA_ROMB		equ	$238			; socket ROM bytes summed through the overlay
PA_ROMS		equ	$23C			; reserved (per-chunk sums come from sizing)
PA_ACFG		equ	$240			; expected mobo autoconfig hash; 0 = sizing
PA_CHUNK	equ	$244			; longwords per RAM path per pass
PA_CACR		equ	$248			; bit31 set: use bits 30:0 as CACR; 0: I-cache on
PA_CIA		equ	$24C			; CIA iterations per pass
PA_FLAGS	equ	$250			; bit0: no MOVE16 burst on paths 3/4; bit1: Gary BERR probe (path 6);
					; bit2: the probe also sets TOENB ($DE0001)
PA_BPROBE	equ	$254			; BERR probe address; 0 = $00F00000

VBRT		equ	$400			; vector table, 1 KB
; variables
V_REC		equ	$A00			; recovery address for the exception handler
V_SSP		equ	$A04			; stack pointer to restore
V_PATH		equ	$A08			; current path record, or 0
V_CUR		equ	$A0C			; current path index
V_MODE		equ	$A10			; 0 real, 1 emulated
V_SIZED		equ	$A14
V_PRES		equ	$A18			; sized present mask
V_MOBO		equ	$A1C			; sized mobo block mask
V_ACFG		equ	$A20			; sized mobo autoconfig hash
V_Z3OK		equ	$A24			; bit0 card RAM board configured, bit1 RTG configured
V_MOBI		equ	$A28			; mobo block cursor
V_OFF		equ	$A2C			; 8 longs: per-path window offset
V_LFSR		equ	$A4C
V_SPIN		equ	$A50
V_CHASH		equ	$A54			; card autoconfig nibble hash
V_CLEAN		equ	$A58			; params were valid this pass
; effective parameters, recomputed every pass
E_CMD		equ	$A80
E_EN		equ	$A84
E_PRES		equ	$A88
E_CHLO		equ	$A8C
E_CHHI		equ	$A90
E_MOBO		equ	$A94
E_CLO		equ	$A98
E_CHI		equ	$A9C
E_Z3B		equ	$AA0
E_Z3O		equ	$AA4
E_Z3L		equ	$AA8
E_ROMB		equ	$AAC
E_ACFG		equ	$AB0
E_CHUNK		equ	$AB4
E_CIA		equ	$AB8
E_FLAGS		equ	$ABC
CRCTAB		equ	$C00			; 256 longs
ROMSUMS		equ	$1000			; 128 x 4 KB chunk sums of the socket ROM (sizing)
SELFSUMS	equ	$1200			; 128 x 4 KB chunk sums of this image (sizing)

ST_SIZED	equ	0
ST_OVL		equ	1
ST_ICACHE	equ	2
ST_EMU		equ	3
ST_Z3RAM	equ	4
ST_RTG		equ	5
ST_PARAMS	equ	6

W_START		equ	$FFFE0000
W_PARKED	equ	$FFFF0000
W_IDLE		equ	$FFFC0000
W_SIZE		equ	$FFFD0000

NX_EMU		equ	%11011100		; paths 3,4,5,7,8 do not reach the bus when emulated

CIAA		equ	$BFE001
CIAB		equ	$BFD000
CUSTOM		equ	$DFF000
MOBO_RAM	equ	$07000000
ROMBASE		equ	$00F80000
ROMSIZE		equ	$00080000
Z3CFG		equ	$FF000000
Z2CFG		equ	$00E80000
Z3660_MFR	equ	$144B
GARY_TO		equ	$DE0000			; Gary: bit 7 set = a cycle nobody answers ends in BERR
GARY_TOEN	equ	$DE0001			; Gary TOENB: bit 7 = the bus timeout enable (METAL-2: $DE0000 alone did nothing on the D)
BPROBE_DEF	equ	$00F00000		; empty on the A4000D: Gary's timeout ends the cycle

; TRY <label>: an exception from here on resumes at <label> with SP as it is now.
TRY		macro
		lea	\1(pc),a0
		move.l	a0,MB+V_REC
		move.l	sp,MB+V_SSP
		endm

		org	ROMBASE
rom:
		dc.w	$1114,$4EF9		; like a 512 KB Kickstart: SSP/PC for the emulator
		dc.l	ROMBASE+$D2
		dcb.b	$D2-8,0
		bra.w	start			; $D2: Kickstart socket ROMs reset here
		bra.w	start			; $D6: DiagROM socket ROMs reset here
		dc.b	"Z3660 ztest timing-scanner bus test",0
		even

; ---------------------------------------------------------------------------------------
start:
		move.w	#$2700,sr
		lea	STACKTOP,sp
		lea	MB,a5
		lea	exc(pc),a1		; every vector to one handler
		lea	VBRT(a5),a0
		move.w	#255,d0
.vec:		move.l	a1,(a0)+
		dbra	d0,.vec
		lea	VBRT(a5),a0
		movec	a0,vbr
		lea	start_recover(pc),a0	; a fault before the first TRY restarts the setup
		move.l	a0,V_REC(a5)
		move.l	sp,V_SSP(a5)
		clr.l	V_PATH(a5)
		moveq	#0,d0
		movec	d0,cacr
		cinva	bc
		bsr	mmu_off
		bsr	build_crctab
		; page: keep the restart count if the page was valid, otherwise clear it
		cmp.l	#'ZTMB',MB_MAGIC(a5)
		bne.s	.fresh
		addq.l	#1,MB_RS(a5)
		bra.s	.page
.fresh:		lea	(a5),a0
		move.w	#($200/4)-1,d0
.clr:		clr.l	(a0)+
		dbra	d0,.clr
		move.l	#1,MB_RS(a5)
.page:		move.l	#'ZTMB',MB_MAGIC(a5)
		move.l	#$00010000+BUILD,MB_VER(a5)
		clr.l	MB_HB(a5)
		move.l	#W_START,MB_WHERE(a5)
		move.l	#-1,MB_EPOCH(a5)	; forces a counter reset at the first pass
		clr.l	MB_STAT(a5)
		bsr	detect_cpu		; after the page init, which clears MB_STAT and MB_ID
		clr.l	V_SIZED(a5)
		clr.l	V_Z3OK(a5)
		clr.l	V_MOBI(a5)
		lea	V_OFF(a5),a0
		moveq	#NPATH-1,d0
.off:		clr.l	(a0)+
		dbra	d0,.off
		move.l	#$12345678,V_LFSR(a5)
		bsr	custom_off
		bsr	eff_params
		tst.l	V_MODE(a5)
		bne.s	.noacfg
		bsr	acfg_card		; configure the card's own Z3 boards (real 060 only)
.noacfg:	bsr	set_cacr
		bsr	publish
		bra	loop
start_recover:
		lea	MB,a5
		bra	loop

; ---------------------------------------------------------------------------------------
; exception handler: count against the current path, then unwind to the TRY point
exc:
		move.w	#$2700,sr
		lea	MB,a5
		addq.l	#1,MB_XCNT(a5)
		move.l	2(sp),MB_XPC(a5)
		move.l	8(sp),MB_XADDR(a5)
		move.w	6(sp),d0
		and.l	#$0FFF,d0
		lsr.w	#2,d0			; vector number
		swap	d0
		move.l	MB_STAT(a5),d1
		and.l	#$FF00FFFF,d1
		or.l	d0,d1
		move.l	d1,MB_STAT(a5)
		move.l	V_PATH(a5),d1
		beq.s	.np
		movea.l	d1,a6
		addq.l	#1,REC_BERR(a6)
.np:		movea.l	V_SSP(a5),sp
		movea.l	V_REC(a5),a0
		jmp	(a0)

; ---------------------------------------------------------------------------------------
mmu_off:
		TRY	.done
		moveq	#0,d0
		movec	d0,tc
		movec	d0,itt0
		movec	d0,itt1
		movec	d0,dtt0
		movec	d0,dtt1
		pflusha
.done:		lea	MB,a5
		rts

; PCR exists only on the 68060: a trap means the emulated 040
detect_cpu:
		clr.l	MB_ID(a5)
		move.l	#1,V_MODE(a5)
		TRY	.emu
		movec	pcr,d0
		lea	MB,a5
		move.l	d0,MB_ID(a5)
		clr.l	V_MODE(a5)
.emu:		lea	MB,a5
		move.l	PA_MODE(a5),d0		; a valid parameter block may override
		cmp.l	#'ZTPA',PA_MAGIC(a5)
		bne.s	.st
		cmp.l	#1,d0
		bne.s	.m2
		clr.l	V_MODE(a5)
.m2:		cmp.l	#2,d0
		bne.s	.st
		move.l	#1,V_MODE(a5)
.st:		bclr	#ST_EMU,MB_STAT+3(a5)
		tst.l	V_MODE(a5)
		beq.s	.r
		bset	#ST_EMU,MB_STAT+3(a5)
.r:		rts

custom_off:
		TRY	.done
		move.w	#$7FFF,CUSTOM+$09A	; INTENA
		move.w	#$7FFF,CUSTOM+$09C	; INTREQ
		move.w	#$7FFF,CUSTOM+$096	; DMACON
		move.b	#$03,CIAA+$200		; DDRA: OVL, LED out
		move.b	#$02,CIAA		; OVL=0: chip RAM at $000000 (the emulator drops chip writes under OVL too)
.done:		lea	MB,a5
		rts

set_cacr:
		move.l	PA_CACR(a5),d0
		cmp.l	#'ZTPA',PA_MAGIC(a5)
		beq.s	.p
		moveq	#0,d0
.p:		btst	#31,d0
		bne.s	.explicit
		move.l	#$00008000,d0		; instruction cache only
		bra.s	.set
.explicit:	and.l	#$5FFFFFFF,d0		; never the data cache (bit 31) or the 060 store buffer (bit 29)
.set:		cinva	bc
		movec	d0,cacr
		bclr	#ST_ICACHE,MB_STAT+3(a5)
		btst	#15,d0
		beq.s	.r
		bset	#ST_ICACHE,MB_STAT+3(a5)
.r:		rts

; ---------------------------------------------------------------------------------------
; eff_params: defaults where the block is absent or a field is zero
eff_params:
		clr.l	V_CLEAN(a5)
		bclr	#ST_PARAMS,MB_STAT+3(a5)
		lea	PA_MAGIC(a5),a1
		cmp.l	#'ZTPA',(a1)
		bne	.defaults
		move.l	#1,V_CLEAN(a5)
		bset	#ST_PARAMS,MB_STAT+3(a5)
		move.l	PA_CMD(a5),E_CMD(a5)
		move.l	PA_EN(a5),d0
		bne.s	.en
		move.l	#$FF,d0
.en:		move.l	d0,E_EN(a5)
		move.l	PA_PRES(a5),d0
		bne.s	.pr
		move.l	V_PRES(a5),d0
.pr:		move.l	d0,E_PRES(a5)
		lea	.tab(pc),a2
.fld:		move.w	(a2)+,d1		; param offset
		beq.s	.done
		move.w	(a2)+,d2		; effective offset
		move.l	(a2)+,d3		; default
		move.l	(a5,d1.w),d0
		bne.s	.set
		move.l	d3,d0
.set:		move.l	d0,(a5,d2.w)
		bra.s	.fld
.done:		move.l	PA_MOBO(a5),d0
		bne.s	.mb
		move.l	V_MOBO(a5),d0
.mb:		move.l	d0,E_MOBO(a5)
		move.l	PA_ACFG(a5),d0
		bne.s	.ac
		move.l	V_ACFG(a5),d0
.ac:		move.l	d0,E_ACFG(a5)
		move.l	PA_FLAGS(a5),E_FLAGS(a5)
		rts
.defaults:	tst.l	V_SIZED(a5)
		seq	d0
		ext.w	d0
		ext.l	d0
		neg.l	d0
		add.l	d0,d0			; not sized: SIZE (2), sized: RUN (0)
		move.l	d0,E_CMD(a5)
		move.l	#$FF,E_EN(a5)
		move.l	V_PRES(a5),E_PRES(a5)
		lea	.tab(pc),a2
.dfl:		move.w	(a2)+,d1
		beq.s	.dd
		move.w	(a2)+,d2
		move.l	(a2)+,(a5,d2.w)
		bra.s	.dfl
.dd:		move.l	V_MOBO(a5),E_MOBO(a5)
		move.l	V_ACFG(a5),E_ACFG(a5)
		clr.l	E_FLAGS(a5)
		rts
.tab:		dc.w	PA_CHLO,E_CHLO
		dc.l	$00002000
		dc.w	PA_CHHI,E_CHHI
		dc.l	$001FFFFF
		dc.w	PA_CLO,E_CLO
		dc.l	$08100000
		dc.w	PA_CHI,E_CHI
		dc.l	$082FFFFF
		dc.w	PA_Z3B,E_Z3B
		dc.l	$40000000
		dc.w	PA_Z3O,E_Z3O
		dc.l	$00100000
		dc.w	PA_Z3L,E_Z3L
		dc.l	$00100000
		dc.w	PA_ROMB,E_ROMB
		dc.l	ROMSIZE
		dc.w	PA_CHUNK,E_CHUNK
		dc.l	1024
		dc.w	PA_CIA,E_CIA
		dc.l	32
		dc.w	0

; ---------------------------------------------------------------------------------------
loop:
		lea	MB,a5
		lea	STACKTOP,sp
		clr.l	V_PATH(a5)
		move.l	#W_IDLE,MB_WHERE(a5)
		bsr	eff_params
		move.l	PA_EPOCH(a5),d0
		tst.l	V_CLEAN(a5)
		bne.s	.ep
		moveq	#0,d0
.ep:		cmp.l	MB_EPOCH(a5),d0
		beq.s	.same
		move.l	d0,MB_EPOCH(a5)
		bsr	zero_counters
		bsr	set_cacr
.same:		move.l	E_CMD(a5),d0
		cmp.l	#1,d0
		beq	park
		cmp.l	#2,d0
		beq.s	.size
		tst.l	V_SIZED(a5)
		bne.s	.run
		tst.l	PA_PRES(a5)		; RUN without a frozen mask needs our own sizing
		bne.s	.run
.size:		bsr	sizing
		bra.s	.pub
.run:		bsr	run_paths
.pub:		lea	MB,a5
		lea	STACKTOP,sp
		addq.l	#1,MB_HB(a5)
		bsr	publish
		bra	loop

park:
		move.l	#W_PARKED,MB_WHERE(a5)
		bsr	publish
.spin:		move.l	#20000,d1		; cache-resident delay between parameter reads
.d:		subq.l	#1,d1
		bne.s	.d
		lea	MB,a5
		cmp.l	#1,PA_CMD(a5)
		beq.s	.spin
		bra	loop

zero_counters:
		lea	MB_REC(a5),a0
		moveq	#NPATH-1,d0
.z:		clr.l	REC_ERR(a0)
		clr.l	REC_OPS(a0)
		clr.l	REC_BERR(a0)
		clr.l	REC_FA(a0)
		clr.l	REC_FX(a0)
		clr.l	REC_LAST(a0)
		lea	32(a0),a0
		dbra	d0,.z
		clr.l	MB_XCNT(a5)
		rts

; publish: CRC over the record, then the mirror copy
publish:
		lea	MB,a5
		lea	(a5),a0
		move.w	#MB_CRC-1,d1
		moveq	#-1,d0
		lea	CRCTAB(a5),a1
.c:		moveq	#0,d2
		move.b	(a0)+,d2
		eor.b	d0,d2
		lsr.l	#8,d0
		lsl.w	#2,d2
		move.l	(a1,d2.w),d3
		eor.l	d3,d0
		dbra	d1,.c
		not.l	d0
		move.l	d0,MB_CRC(a5)
		lea	(a5),a0
		lea	MB_MIRROR(a5),a1
		move.w	#($200/4)-1,d1
.m:		move.l	(a0)+,(a1)+
		dbra	d1,.m
		rts

build_crctab:
		lea	CRCTAB(a5),a0
		moveq	#0,d1			; n
.n:		move.l	d1,d0
		moveq	#7,d2
.k:		lsr.l	#1,d0
		bcc.s	.nx
		eor.l	#$EDB88320,d0
.nx:		dbra	d2,.k
		move.l	d0,(a0)+
		addq.w	#1,d1
		cmp.w	#256,d1
		bne.s	.n
		rts

; ---------------------------------------------------------------------------------------
; run_paths: each enabled and present path once; a fault abandons only that path's chunk
run_paths:
		clr.l	V_CUR(a5)
.next:		lea	MB,a5
		move.l	V_CUR(a5),d6
		cmp.l	#NPATH,d6
		bge	.end
		move.l	d6,d0
		lsl.l	#5,d0
		lea	MB_REC(a5,d0.l),a6
		; status of this path
		moveq	#2,d1
		tst.l	V_MODE(a5)
		beq.s	.real
		move.l	#NX_EMU,d0
		btst	d6,d0
		bne.s	.mark
.real:		moveq	#3,d1
		move.l	E_EN(a5),d0
		btst	d6,d0
		beq.s	.mark
		moveq	#0,d1
		move.l	E_PRES(a5),d0
		btst	d6,d0
		beq.s	.mark
		moveq	#1,d1
.mark:		move.l	d1,REC_PRES(a6)
		cmp.l	#1,d1
		bne.s	.skip
		move.l	a6,V_PATH(a5)
		move.l	d6,d0
		addq.l	#1,d0
		swap	d0
		move.l	d0,MB_WHERE(a5)
		TRY	.aborted
		lea	MB,a5
		move.l	V_CUR(a5),d0
		lsl.w	#2,d0
		jsr	ptab(pc,d0.w)
.aborted:	lea	MB,a5
		clr.l	V_PATH(a5)
		bsr	ovl_off_safe
.skip:		lea	MB,a5
		addq.l	#1,V_CUR(a5)
		bra	.next
.end:		rts

ptab:		bra.w	path_chip
		bra.w	path_mobo
		bra.w	path_card
		bra.w	path_z3
		bra.w	path_rom
		bra.w	path_cia
		bra.w	path_acfg
		bra.w	path_self

; ---------------------------------------------------------------------------------------
; error bookkeeping: d0 read, d1 expected, a1 address, a6 record
bad:
		addq.l	#1,REC_ERR(a6)
		tst.l	REC_FA(a6)
		bne.s	.fa
		move.l	a1,REC_FA(a6)
.fa:		move.l	d0,REC_LAST(a6)
		eor.l	d0,d1
		or.l	d1,REC_FX(a6)
		rts

; chunk: d2 lo, d3 hi (inclusive), a3 -> offset variable; out a0 base, d7 longs (>=16)
chunk:
		move.l	E_CHUNK(a5),d7
		lsl.l	#2,d7			; bytes
		move.l	(a3),d4
		move.l	d2,d0
		add.l	d4,d0			; base
		move.l	d3,d1
		addq.l	#1,d1
		sub.l	d0,d1			; bytes left in the window
		cmp.l	d7,d1
		bge.s	.fits
		moveq	#0,d4
		move.l	d2,d0
		move.l	d3,d1
		addq.l	#1,d1
		sub.l	d0,d1
		cmp.l	d7,d1
		bge.s	.fits
		move.l	d1,d7
.fits:		movea.l	d0,a0
		add.l	d7,d4
		move.l	d4,(a3)
		lsr.l	#2,d7
		and.l	#$FFFFFFF0,d7		; whole 16-long groups
		rts

; memtest: a0 base, d7 longs (multiple of 16), a6 record. Four patterns plus byte/word lanes.
memtest:
		move.l	a0,a4
		move.l	d7,d5
		; 1. address in address
		movea.l	a4,a1
		move.l	d5,d6
		subq.l	#1,d6
.w1:		move.l	a1,(a1)+
		subq.l	#1,d6
		bpl.s	.w1
		movea.l	a4,a1
		move.l	d5,d6
		subq.l	#1,d6
.r1:		move.l	(a1),d0
		move.l	a1,d1
		cmp.l	d1,d0
		beq.s	.o1
		bsr	bad
.o1:		addq.l	#4,a1
		subq.l	#1,d6
		bpl.s	.r1
		; 2. inverted address
		movea.l	a4,a1
		move.l	d5,d6
		subq.l	#1,d6
.w2:		move.l	a1,d0
		not.l	d0
		move.l	d0,(a1)+
		subq.l	#1,d6
		bpl.s	.w2
		movea.l	a4,a1
		move.l	d5,d6
		subq.l	#1,d6
.r2:		move.l	(a1),d0
		move.l	a1,d1
		not.l	d1
		cmp.l	d1,d0
		beq.s	.o2
		bsr	bad
.o2:		addq.l	#4,a1
		subq.l	#1,d6
		bpl.s	.r2
		; 3. LFSR fill, regenerate and compare
		lea	MB,a5
		move.l	V_LFSR(a5),d2
		add.l	MB_HB(a5),d2
		bne.s	.sd
		moveq	#1,d2
.sd:		move.l	d2,d3			; seed kept for the compare
		movea.l	a4,a1
		move.l	d5,d6
		subq.l	#1,d6
.w3:		lsr.l	#1,d2
		bcc.s	.w3n
		eor.l	#$80200003,d2
.w3n:		move.l	d2,(a1)+
		subq.l	#1,d6
		bpl.s	.w3
		move.l	d2,V_LFSR(a5)
		move.l	d3,d2
		movea.l	a4,a1
		move.l	d5,d6
		subq.l	#1,d6
.r3:		lsr.l	#1,d2
		bcc.s	.r3n
		eor.l	#$80200003,d2
.r3n:		move.l	(a1),d0
		cmp.l	d2,d0
		beq.s	.o3
		move.l	d2,d1
		bsr	bad
.o3:		addq.l	#4,a1
		subq.l	#1,d6
		bpl.s	.r3
		; 4. walking one and walking zero across the 32 data lines
		movea.l	a4,a1
		moveq	#1,d2
		moveq	#31,d6
.w4:		move.l	d2,(a1)+
		move.l	d2,d0
		not.l	d0
		move.l	d0,(a1)+
		rol.l	#1,d2
		dbra	d6,.w4
		movea.l	a4,a1
		moveq	#1,d2
		moveq	#31,d6
.r4:		move.l	(a1),d0
		move.l	d2,d1
		cmp.l	d1,d0
		beq.s	.o4a
		bsr	bad
.o4a:		addq.l	#4,a1
		move.l	(a1),d0
		move.l	d2,d1
		not.l	d1
		cmp.l	d1,d0
		beq.s	.o4b
		bsr	bad
.o4b:		addq.l	#4,a1
		rol.l	#1,d2
		dbra	d6,.r4
		; 5. byte and word lanes (SIZ/A1:0 translation)
		movea.l	a4,a1
		move.b	#$11,(a1)
		move.b	#$22,1(a1)
		move.b	#$33,2(a1)
		move.b	#$44,3(a1)
		move.w	#$5566,4(a1)
		move.w	#$7788,6(a1)
		move.l	(a1),d0
		move.l	#$11223344,d1
		cmp.l	d1,d0
		beq.s	.o5
		bsr	bad
.o5:		addq.l	#4,a1
		move.l	(a1),d0
		move.l	#$55667788,d1
		cmp.l	d1,d0
		beq.s	.o6
		bsr	bad
.o6:		move.w	2(a4),d0		; word read of a long written as bytes
		cmp.w	#$3344,d0
		beq.s	.o7
		movea.l	a4,a1
		addq.l	#2,a1
		and.l	#$FFFF,d0
		move.l	#$3344,d1
		bsr	bad
.o7:		move.b	1(a4),d0
		cmp.b	#$22,d0
		beq.s	.o8
		movea.l	a4,a1
		addq.l	#1,a1
		and.l	#$FF,d0
		moveq	#$22,d1
		bsr	bad
.o8:		move.l	d5,d0
		mulu.l	#6,d0
		add.l	#64*2+4,d0
		add.l	d0,REC_OPS(a6)
		rts

; burst: MOVE16 lines from the first half of the chunk to the second, compare. a0 base, d7 longs
burst:
		lea	MB,a5
		btst	#0,E_FLAGS+3(a5)
		bne.s	.r
		move.l	d7,d5
		lsr.l	#1,d5			; longs per half
		and.l	#$FFFFFFFC,d5
		beq.s	.r
		movea.l	a0,a1
		move.l	d5,d0
		lsl.l	#2,d0
		movea.l	a0,a2
		adda.l	d0,a2			; destination
		movea.l	a2,a3
		move.l	d5,d6
		lsr.l	#2,d6			; lines
		subq.l	#1,d6
.mv:		move16	(a1)+,(a2)+
		subq.l	#1,d6
		bpl.s	.mv
		movea.l	a0,a4
		move.l	d5,d6
		subq.l	#1,d6
.cmp:		move.l	(a3)+,d0
		move.l	(a4)+,d1
		cmp.l	d1,d0
		beq.s	.ok
		lea	-4(a3),a1
		bsr	bad
.ok:		subq.l	#1,d6
		bpl.s	.cmp
		add.l	d5,REC_OPS(a6)
.r:		rts

; ---------------------------------------------------------------------------------------
path_chip:
		move.l	E_CHLO(a5),d2
		move.l	E_CHHI(a5),d3
		lea	V_OFF+0(a5),a3
		bsr	chunk
		bra	memtest

path_mobo:
		move.l	E_MOBO(a5),d4
		beq.s	.r
		move.l	V_MOBI(a5),d5
		moveq	#15,d6
.find:		addq.l	#1,d5
		and.l	#15,d5
		btst	d5,d4
		dbne	d6,.find
		move.l	V_MOBI(a5),d0
		move.l	d5,V_MOBI(a5)
		cmp.l	d0,d5			; wrapped to a lower block: next offset
		bgt.s	.same
		move.l	E_CHUNK(a5),d0
		lsl.l	#2,d0
		add.l	d0,V_OFF+4(a5)
		and.l	#$000FFFFF,V_OFF+4(a5)
.same:		move.l	d5,d2
		swap	d2
		lsl.l	#4,d2			; block << 20
		add.l	#MOBO_RAM,d2
		move.l	d2,d3
		add.l	#$000FFFFF,d3
		lea	V_OFF+4(a5),a3
		move.l	(a3),-(sp)		; chunk() advances it; this path advances per wrap
		bsr	chunk
		move.l	(sp)+,V_OFF+4(a5)
		bra	memtest
.r:		rts

path_card:
		move.l	E_CLO(a5),d2
		move.l	E_CHI(a5),d3
		lea	V_OFF+8(a5),a3
		bsr	chunk
		movem.l	a0/d7,-(sp)
		bsr	memtest
		movem.l	(sp)+,a0/d7
		bra	burst

path_z3:
		btst	#0,V_Z3OK+3(a5)
		beq.s	.r
		move.l	E_Z3B(a5),d2
		add.l	E_Z3O(a5),d2
		move.l	d2,d3
		add.l	E_Z3L(a5),d3
		subq.l	#1,d3
		lea	V_OFF+12(a5),a3
		bsr	chunk
		movem.l	a0/d7,-(sp)
		bsr	memtest
		movem.l	(sp)+,a0/d7
		bra	burst
.r:		rts

; socket ROM through the overlay at $000000: one 4 KB chunk per pass against its sizing sum
path_rom:
		lea	V_OFF+16(a5),a3
		move.l	(a3),d4
		move.l	E_ROMB(a5),d3
		cmp.l	d3,d4
		blt.s	.in
		moveq	#0,d4
.in:		bsr	ovl_on
		movea.l	d4,a1
		bsr	sum4k			; d0 = sum of 1024 longs at a1
		bsr	ovl_off
		lea	MB,a5
		move.l	d4,d1
		moveq	#12,d2
		lsr.l	d2,d1
		lsl.l	#2,d1
		move.l	ROMSUMS(a5,d1.l),d1
		cmp.l	d1,d0
		beq.s	.ok
		movea.l	d4,a1
		bsr	bad
.ok:		add.l	#1024,REC_OPS(a6)
		add.l	#4096,d4
		move.l	d4,V_OFF+16(a5)
		rts

; sum4k: Kickstart-style add-with-carry over 1024 longs at a1 -> d0
sum4k:
		moveq	#0,d0
		moveq	#0,d2
		move.w	#1023,d1
.s:		add.l	(a1)+,d0
		addx.l	d2,d0
		dbra	d1,.s
		rts

ovl_on:
		move.b	#$03,CIAA+$200		; DDRA: OVL, LED out
		move.b	#$03,CIAA		; OVL=1
		bset	#ST_OVL,MB+MB_STAT+3
		rts
ovl_off:
		move.b	#$02,CIAA		; OVL=0
		bclr	#ST_OVL,MB+MB_STAT+3
		rts
; after an aborted path: clear OVL if it was left on, never twice through a fault
ovl_off_safe:
		lea	MB,a5
		tst.l	V_MODE(a5)
		bne.s	.r
		btst	#ST_OVL,MB_STAT+3(a5)
		beq.s	.r
		bclr	#ST_OVL,MB_STAT+3(a5)
		TRY	.r
		move.b	#$02,CIAA
.r:		lea	MB,a5
		rts

; CIA: stopped-timer latch write/read on both chips (both byte lanes), CIA-A DDRB
path_cia:
		move.l	E_CIA(a5),d5
		subq.l	#1,d5
		move.l	MB_HB(a5),d4
		lea	CIAA,a2
		lea	CIAB,a3
		clr.b	$E00(a2)		; CRA: stop
		clr.b	$F00(a2)		; CRB: stop
		clr.b	$E00(a3)
		clr.b	$F00(a3)
.it:		addq.l	#1,d4
		move.l	d4,d3
		mulu.w	#$9E37,d3		; spread the pattern over 16 bits
		lea	MB,a5
		movea.l	a2,a4
		bsr	cia_timer
		movea.l	a3,a4
		bsr	cia_timer
		move.b	d3,$300(a2)		; CIA-A DDRB (register only)
		move.b	$300(a2),d0
		cmp.b	d3,d0
		beq.s	.dd
		lea	$300(a2),a1
		and.l	#$FF,d0
		moveq	#0,d1
		move.b	d3,d1
		bsr	bad
.dd:		addq.l	#2,REC_OPS(a6)
		subq.l	#1,d5
		bpl.s	.it
		clr.b	$300(a2)
		btst	#1,E_FLAGS+3(a5)
		beq.s	.nobp
		tst.l	V_MODE(a5)		; real 060 only: the emulator's bus does not take a BERR
		bne.s	.nobp
		bsr	berr_probe
.nobp:		rts

; one read of an empty motherboard address with Gary in BERR mode. The access fault is
; the expected result (berr +1 through the handler); a read that completes is an error.
; Each probe costs Gary's BERR timeout (~0.28 s on the A4000T), so it is a diagnostic only.
berr_probe:
		move.l	PA_BPROBE(a5),d0
		bne.s	.a
		move.l	#BPROBE_DEF,d0
.a:		movea.l	d0,a1
		addq.l	#1,REC_OPS(a6)
		btst	#2,E_FLAGS+3(a5)
		beq.s	.to
		move.b	#$80,GARY_TOEN
.to:		move.b	#$80,GARY_TO
		TRY	.fault
		move.l	(a1),d0
		nop				; the access completes inside the TRY
		lea	MB,a5
		moveq	#0,d1
		bsr	bad
.fault:		lea	MB,a5
		clr.b	GARY_TO
		clr.b	GARY_TOEN		; Kickstart's own setting: both clear
		rts
; one CIA at a4: timer A then timer B latch, d3 low word = value
cia_timer:
		moveq	#1,d2			; 1: timer A at $400/$500, 0: timer B at $600/$700
		movea.l	a4,a1
		lea	$400(a1),a1
.t:		move.b	d3,(a1)			; TxLO
		move.w	d3,d0
		lsr.w	#8,d0
		move.b	d0,$100(a1)		; TxHI: loads the stopped counter
		tst.b	(a1)			; the 8520 load lands an E cycle after the write
		moveq	#0,d0
		move.b	$100(a1),d0
		lsl.w	#8,d0
		move.b	(a1),d0
		and.l	#$FFFF,d0
		moveq	#0,d1
		move.w	d3,d1
		cmp.l	d1,d0
		beq.s	.ok
		bsr	bad
.ok:		addq.l	#4,REC_OPS(a6)
		lea	$200(a1),a1
		dbra	d2,.t
		rts

; autoconfig: the motherboard chain behind the card's configured boards, hashed
path_acfg:
		bsr	acfg_hash		; d4
		lea	MB,a5
		move.l	E_ACFG(a5),d1
		move.l	d4,d0
		cmp.l	d1,d0
		beq.s	.ok
		lea	Z3CFG,a1
		bsr	bad
.ok:		add.l	#18,REC_OPS(a6)
		rts

acfg_hash:
		moveq	#0,d4
		lea	.z3(pc),a2
		moveq	#9,d5
.l3:		movea.l	(a2)+,a1
		TRY	.f3
		move.l	(a1),d0
		and.l	#$F000F000,d0
		bra.s	.h3
.f3:		move.l	#$DEAD0003,d0
.h3:		rol.l	#5,d4
		eor.l	d0,d4
		dbra	d5,.l3
		lea	.z2(pc),a2
		moveq	#7,d5
.l2:		movea.l	(a2)+,a1
		TRY	.f2
		moveq	#0,d0
		move.w	(a1),d0
		and.l	#$F000,d0
		bra.s	.h2
.f2:		move.l	#$DEAD0002,d0
.h2:		rol.l	#5,d4
		eor.l	d0,d4
		dbra	d5,.l2
		lea	MB,a5
		rts
.z3:		dc.l	Z3CFG+$000,Z3CFG+$100,Z3CFG+$004,Z3CFG+$104,Z3CFG+$008
		dc.l	Z3CFG+$108,Z3CFG+$010,Z3CFG+$110,Z3CFG+$014,Z3CFG+$114
.z2:		dc.l	Z2CFG+$00,Z2CFG+$02,Z2CFG+$04,Z2CFG+$06,Z2CFG+$10,Z2CFG+$12,Z2CFG+$14,Z2CFG+$16

; this image through the MAPROM decode: one 4 KB chunk per pass against its sizing sum
path_self:
		lea	V_OFF+28(a5),a3
		move.l	(a3),d4
		cmp.l	#ROMSIZE,d4
		blt.s	.in
		moveq	#0,d4
.in:		movea.l	#ROMBASE,a1
		adda.l	d4,a1
		bsr	sum4k
		lea	MB,a5
		move.l	d4,d1
		moveq	#12,d2
		lsr.l	d2,d1
		lsl.l	#2,d1
		move.l	SELFSUMS(a5,d1.l),d1
		cmp.l	d1,d0
		beq.s	.ok
		movea.l	#ROMBASE,a1
		adda.l	d4,a1
		bsr	bad
.ok:		add.l	#1024,REC_OPS(a6)
		add.l	#4096,d4
		move.l	d4,V_OFF+28(a5)
		rts

; ---------------------------------------------------------------------------------------
; acfg_card: configure the Z3660's own Z3 boards (manufacturer $144B) so the motherboard
; chain becomes visible at $FF000000. RAM board at E_Z3B, RTG 256 MB above it.
acfg_card:
		clr.l	V_Z3OK(a5)
		clr.l	V_CHASH(a5)
		moveq	#1,d7
.b:		TRY	.done
		move.l	Z3CFG+$000,d0
		move.l	Z3CFG+$100,d1
		rol.l	#4,d0
		and.w	#$F,d0
		rol.l	#4,d1
		and.w	#$F,d1
		lsl.w	#4,d0
		or.w	d1,d0			; er_Type
		move.w	d0,d6
		and.w	#$C0,d0
		cmp.w	#$80,d0			; Zorro III
		bne.s	.done
		lea	.mfr(pc),a2
		moveq	#0,d5
		moveq	#3,d4
.m:		movea.l	(a2)+,a1
		move.l	(a1),d0
		rol.l	#4,d0
		and.w	#$F,d0
		not.w	d0
		and.w	#$F,d0
		lsl.w	#4,d5
		or.w	d0,d5
		dbra	d4,.m
		cmp.w	#Z3660_MFR,d5
		bne.s	.done
		lea	MB,a5
		move.l	V_CHASH(a5),d0
		rol.l	#8,d0
		eor.w	d6,d0
		move.l	d0,V_CHASH(a5)
		move.l	E_Z3B(a5),d0
		btst	#5,d6			; memory list: the RAM board
		bne.s	.ram
		add.l	#$10000000,d0
		move.l	d0,Z3CFG+$044
		bset	#1,V_Z3OK+3(a5)
		bset	#ST_RTG,MB_STAT+3(a5)
		bra.s	.nx
.ram:		move.l	d0,Z3CFG+$044
		bset	#0,V_Z3OK+3(a5)
		bset	#ST_Z3RAM,MB_STAT+3(a5)
.nx:		dbra	d7,.b
.done:		lea	MB,a5
		rts
.mfr:		dc.l	Z3CFG+$010,Z3CFG+$110,Z3CFG+$014,Z3CFG+$114

; ---------------------------------------------------------------------------------------
; sizing: decides what is present at the card's own (known-good) timings. The host freezes
; the result into PA_PRES, so a later timing failure is never read as "absent".
sizing:
		lea	MB,a5
		move.l	#W_SIZE,MB_WHERE(a5)
		moveq	#0,d7			; present mask
		; 1 chip RAM: write, disturb, read back
		bsr	probe_win
		; 2 motherboard RAM: 1 MB blocks at $07000000
		bsr	size_mobo
		tst.l	V_MOBO(a5)
		beq.s	.nomb
		bset	#1,d7
.nomb:		tst.l	V_MODE(a5)
		bne	.emu
		; 3 card RAM, CPU decode
		move.l	E_CLO(a5),a1
		bsr	probe_at
		beq.s	.n3
		bset	#2,d7
.n3:		; 4 card RAM, Z3 window
		btst	#0,V_Z3OK+3(a5)
		beq.s	.n4
		move.l	E_Z3B(a5),a1
		adda.l	E_Z3O(a5),a1
		bsr	probe_at
		beq.s	.n4
		bset	#3,d7
.n4:		; 5 socket ROM: per-chunk sums
		TRY	.n5
		bsr	ovl_on
		suba.l	a1,a1
		lea	MB+ROMSUMS,a3
		moveq	#0,d3			; whole-ROM sum
		move.w	#127,d5
.rs:		bsr	sum4k
		move.l	d0,(a3)+
		add.l	d0,d3
		moveq	#0,d0
		addx.l	d0,d3
		dbra	d5,.rs
		bsr	ovl_off
		lea	MB,a5
		move.l	d3,MB_SZ+8(a5)
		bset	#4,d7
.n5:		bsr	ovl_off_safe
		; 6 CIA
		TRY	.n6
		move.b	#$5A,CIAA+$300
		move.b	CIAA+$300,d0
		clr.b	CIAA+$300
		cmp.b	#$5A,d0
		bne.s	.n6
		bset	#5,d7
.n6:		lea	MB,a5
		; 7 autoconfig: always comparable (faults are part of the hash)
		bsr	acfg_hash
		move.l	d4,V_ACFG(a5)
		move.l	d4,MB_SZ+16(a5)
		move.l	V_CHASH(a5),MB_SZ+12(a5)
		bset	#6,d7
		; 8 this image
		lea	MB+SELFSUMS,a3
		movea.l	#ROMBASE,a1
		move.w	#127,d5
.ss:		bsr	sum4k
		move.l	d0,(a3)+
		dbra	d5,.ss
		bset	#7,d7
		bra.s	.fin
.emu:		bset	#5,d7			; CIA reaches the bus under the emulator
		TRY	.fin
		move.b	#$5A,CIAA+$300
		move.b	CIAA+$300,d0
		clr.b	CIAA+$300
		cmp.b	#$5A,d0
		beq.s	.fin
		bclr	#5,d7
.fin:		lea	MB,a5
		move.l	d7,V_PRES(a5)
		move.l	d7,MB_SZ(a5)
		move.l	V_MOBO(a5),MB_SZ+4(a5)
		move.l	#1,V_SIZED(a5)
		bset	#ST_SIZED,MB_STAT+3(a5)
		; records show what was found
		lea	MB_REC(a5),a6
		moveq	#0,d6
.pr:		moveq	#0,d1
		btst	d6,d7
		beq.s	.pn
		moveq	#1,d1
.pn:		tst.l	V_MODE(a5)
		beq.s	.pw
		move.l	#NX_EMU,d0
		btst	d6,d0
		beq.s	.pw
		moveq	#2,d1
.pw:		move.l	d1,REC_PRES(a6)
		lea	32(a6),a6
		addq.l	#1,d6
		cmp.l	#NPATH,d6
		blt.s	.pr
		rts

; probe_win: chip window start (path 1); sets bit 0 of d7 when write/read-back works
probe_win:
		move.l	E_CHLO(a5),a1
		bsr	probe_at
		beq.s	.r
		bset	#0,d7
.r:		rts

; probe_at: a1 address; Z clear (ne) when a pattern and its complement read back
probe_at:
		TRY	.f
		move.l	(a1),d2			; keep the old contents
		move.l	#$A5C3F00F,(a1)
		move.l	#$5A3C0FF0,4(a1)	; drive the bus with something else
		move.l	(a1),d0
		move.l	d2,(a1)
		lea	MB,a5
		cmp.l	#$A5C3F00F,d0
		bne.s	.f
		moveq	#1,d0
		rts
.f:		lea	MB,a5
		moveq	#0,d0
		rts

; size_mobo: tag each 1 MB block, then read the tags back; a block that holds its own
; tag is present (catches floating bus, echo and aliasing)
size_mobo:
		clr.l	V_MOBO(a5)
		moveq	#0,d5
.w:		move.l	d5,d0
		swap	d0
		lsl.l	#4,d0
		add.l	#MOBO_RAM,d0
		movea.l	d0,a1
		TRY	.wn
		move.l	d5,d0
		or.l	#$7A600000,d0
		move.l	d0,(a1)
		not.l	d0
		move.l	d0,4(a1)
.wn:		lea	MB,a5
		addq.l	#1,d5
		cmp.l	#16,d5
		blt.s	.w
		moveq	#0,d5
.r:		move.l	d5,d0
		swap	d0
		lsl.l	#4,d0
		add.l	#MOBO_RAM,d0
		movea.l	d0,a1
		TRY	.rn
		move.l	(a1),d0
		move.l	d5,d1
		or.l	#$7A600000,d1
		cmp.l	d1,d0
		bne.s	.rn
		lea	MB,a5
		move.l	V_MOBO(a5),d0
		bset	d5,d0
		move.l	d0,V_MOBO(a5)
.rn:		lea	MB,a5
		addq.l	#1,d5
		cmp.l	#16,d5
		blt.s	.r
		rts

		; checksum longword fixed by fixsum.py: the whole image sums to $FFFFFFFF
		org	ROMBASE+ROMSIZE-$18
		dc.l	0
		dcb.b	$14,0
