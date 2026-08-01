/* Host harness shadow of Xilinx <xil_cache.h> — cache ops are no-ops on host,
 * and INTPTR (normally from <xil_types.h>) is the host pointer-int type. Included
 * by a3000_scsi.cpp when compiled into the SCSI CDB unit test (scsi_cd_test.cpp). */
#ifndef HOST_STUB_XIL_CACHE_H
#define HOST_STUB_XIL_CACHE_H

#include <stdint.h>

#ifndef INTPTR
typedef intptr_t INTPTR;
#endif

#ifdef __cplusplus
extern "C" {
#endif
static inline void Xil_DCacheFlush(void){}
static inline void Xil_DCacheFlushRange(INTPTR a, unsigned l){ (void)a; (void)l; }
static inline void Xil_DCacheInvalidateRange(INTPTR a, unsigned l){ (void)a; (void)l; }
static inline void Xil_DCacheEnable(void){}
static inline void Xil_ICacheEnable(void){}
#ifdef __cplusplus
}
#endif
#endif
