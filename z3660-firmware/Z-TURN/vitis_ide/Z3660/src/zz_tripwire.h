/*
 * zz_tripwire.h -- fork-only diagnostic for the register-capture phantoms (S3-46, 2026-10-26).
 *
 * Build flag ZZ_TRIPWIRE (default 0): with it off both macros are empty and the image is the release build. With it
 * on, every REG1 event's raw word is kept (ZZT_EVENT) and every write the 68k cannot have meant -- an unknown or
 * read-only ZZ register, a MODE or ACC_OP value out of range, a piscsi offset no handler takes -- is counted and
 * printed (ZZT) with this event's and the previous event's REG1 words. An address the FPGA captured as
 * (previous & current) shows as: this address has no bit the previous one lacks ("sub 1").
 */
#ifndef ZZ_TRIPWIRE_H
#define ZZ_TRIPWIRE_H

#include <stdint.h>

#ifndef ZZ_TRIPWIRE
#define ZZ_TRIPWIRE 0
#endif

#if ZZ_TRIPWIRE
extern uint32_t zzt_prev_gpio, zzt_cur_gpio;
void zz_tripwire(const char *what, uint32_t zaddr, uint32_t zdata);
#define ZZT_EVENT(g)    do { zzt_prev_gpio = zzt_cur_gpio; zzt_cur_gpio = (g); } while (0)
#define ZZT(what,a,d)   zz_tripwire((what), (a), (d))
#else
#define ZZT_EVENT(g)    do { } while (0)
#define ZZT(what,a,d)   do { } while (0)
#endif

#endif
