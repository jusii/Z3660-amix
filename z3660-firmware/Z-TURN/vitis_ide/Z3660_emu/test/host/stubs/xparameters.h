/* Host harness shadow of Xilinx <xparameters.h>. Minimal device-ID/base defines. */
#ifndef HOST_STUB_XPARAMETERS_H
#define HOST_STUB_XPARAMETERS_H
#define XPAR_SCUWDT_0_DEVICE_ID   0
#define XPAR_XGPIOPS_0_DEVICE_ID  0
#define XPAR_PS7_DDR_0_S_AXI_BASEADDR 0x00000000
#define XPAR_PS7_DDR_0_S_AXI_HIGHADDR 0x3FFFFFFF
/* Bases the board reset/IPL paths read via volatile pointers. Dummy on host —
 * those paths (hard_reboot, GPIO IPL poll) are not exercised by the harness.
 * newcpu.cpp derives PS_RST_CTRL_REG/PS_RST_MASK itself from XPS_SYS_CTRL_BASEADDR,
 * so we must NOT predefine those here (would clash). */
#define XPS_SYS_CTRL_BASEADDR     0
#define XPAR_PS7_GPIO_0_BASEADDR  0
#endif
