/* Host harness shadow of Xilinx <xgpiops.h> — GPIO. No-op on host. */
#ifndef HOST_STUB_XGPIOPS_H
#define HOST_STUB_XGPIOPS_H
#include <stdint.h>
/* GPIO register offsets read by the IPL poll path in newcpu.cpp (dummy on host) */
#define XGPIOPS_DATA_RO_OFFSET    0x00000060
#define XGPIOPS_DATA_OFFSET       0x00000040
typedef struct { uintptr_t BaseAddr; } XGpioPs_Config;
typedef struct { XGpioPs_Config GpioConfig; } XGpioPs;
#ifdef __cplusplus
extern "C" {
#endif
static inline XGpioPs_Config *XGpioPs_LookupConfig(uint16_t id){ (void)id; static XGpioPs_Config c={0}; return &c; }
static inline int XGpioPs_CfgInitialize(XGpioPs *i, XGpioPs_Config *c, uintptr_t base){ (void)i;(void)c;(void)base; return 0; }
static inline void XGpioPs_SetDirectionPin(XGpioPs *i, uint32_t pin, uint32_t dir){ (void)i;(void)pin;(void)dir; }
static inline void XGpioPs_SetOutputEnablePin(XGpioPs *i, uint32_t pin, uint32_t en){ (void)i;(void)pin;(void)en; }
static inline void XGpioPs_WritePin(XGpioPs *i, uint32_t pin, uint32_t v){ (void)i;(void)pin;(void)v; }
static inline uint32_t XGpioPs_ReadPin(XGpioPs *i, uint32_t pin){ (void)i;(void)pin; return 0; }
#ifdef __cplusplus
}
#endif
#endif
