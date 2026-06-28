/* Host harness shadow of Xilinx <xscuwdt.h> — SCU watchdog. No-op on host. */
#ifndef HOST_STUB_XSCUWDT_H
#define HOST_STUB_XSCUWDT_H
#include <stdint.h>
typedef struct { uintptr_t BaseAddr; } XScuWdt_Config;
typedef struct { XScuWdt_Config Config; } XScuWdt;
#ifdef __cplusplus
extern "C" {
#endif
static inline XScuWdt_Config *XScuWdt_LookupConfig(uint16_t id){ (void)id; static XScuWdt_Config c = {0}; return &c; }
static inline int  XScuWdt_CfgInitialize(XScuWdt *i, XScuWdt_Config *c, uintptr_t base){ (void)i;(void)c;(void)base; return 0; }
static inline void XScuWdt_LoadWdt(XScuWdt *i, uint32_t v){ (void)i;(void)v; }
static inline void XScuWdt_Start(XScuWdt *i){ (void)i; }
static inline void XScuWdt_SetWdMode(XScuWdt *i){ (void)i; }
static inline void XScuWdt_RestartWdt(XScuWdt *i){ (void)i; }
#ifdef __cplusplus
}
#endif
#endif
