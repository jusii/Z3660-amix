/* Host harness shadow of Xilinx <xil_cache_l.h> — cache ops are no-ops on host. */
#ifndef HOST_STUB_XIL_CACHE_L_H
#define HOST_STUB_XIL_CACHE_L_H
#ifdef __cplusplus
extern "C" {
#endif
static inline void Xil_L1DCacheFlush(void){}
static inline void Xil_L1DCacheInvalidate(void){}
static inline void Xil_L2CacheFlush(void){}
static inline void Xil_L2CacheInvalidate(void){}
static inline void Xil_DCacheFlush(void){}
static inline void Xil_DCacheFlushRange(unsigned long a, unsigned long l){ (void)a; (void)l; }
static inline void Xil_DCacheInvalidateRange(unsigned long a, unsigned long l){ (void)a; (void)l; }
static inline void Xil_ICacheEnable(void){}
static inline void Xil_DCacheEnable(void){}
static inline void Xil_SetTlbAttributes(unsigned long a, unsigned int attr){ (void)a; (void)attr; }
#ifdef __cplusplus
}
#endif
#endif
