/* Host harness shadow of the Xilinx BSP <xil_io.h>.
 * Provides no-op MMIO accessors so the UAE core compiles on x86 Linux.
 * NOT the real BSP — see test/host/README.md. */
#ifndef HOST_STUB_XIL_IO_H
#define HOST_STUB_XIL_IO_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
static inline uint32_t Xil_In32(uintptr_t a){ (void)a; return 0; }
static inline uint16_t Xil_In16(uintptr_t a){ (void)a; return 0; }
static inline uint8_t  Xil_In8 (uintptr_t a){ (void)a; return 0; }
static inline void Xil_Out32(uintptr_t a, uint32_t v){ (void)a; (void)v; }
static inline void Xil_Out16(uintptr_t a, uint16_t v){ (void)a; (void)v; }
static inline void Xil_Out8 (uintptr_t a, uint8_t  v){ (void)a; (void)v; }
#ifdef __cplusplus
}
#endif
#endif
