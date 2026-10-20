/*
 * ethernet.c
 *
 *  Created on: 23 sept. 2022
 *      Author: shanshe
 */

/*
 * MNT ZZ9000 Amiga Graphics and Coprocessor Card Operating System (ZZ9000OS)
 *
 * Copyright (C) 2019, Lukas F. Hartmann <lukas@mntre.com>
 *                     MNT Research GmbH, Berlin
 *                     https://mntre.com
 *
 * More Info: https://mntre.com/zz9000
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 * GNU General Public License v3.0 or later
 *
 * https://spdx.org/licenses/GPL-3.0-or-later.html
 *
 * Some portions (from EMACPS example code, weird custom license) Copyright (C) 2010 - 2015 Xilinx, Inc.
 *
 */

#include <stdio.h>
#include "platform.h"
#include <xil_cache.h>
#include <xil_mmu.h>
//#include "xparameters.h"
#include "xil_types.h"
#include <xemacps.h>
#include <xscugic.h>
#include "ethernet.h"
#include "interrupt.h"
#include "memorymap.h"
#include "main.h"
#include "debug_console.h"
extern DEBUG_CONSOLE debug_console;

void SetUpSLCRDivisors_z3660(UINTPTR mac_baseaddr, int32_t speed);

static XEmacPs EmacPsInstance;
volatile int semaphore=0;
void DEBUG_ETHERNET(const char *format, ...)
{
   if(debug_console.debug_ethernet==0)
      return;
   while(semaphore==1);
   semaphore=1;
   va_list args;
   va_start(args, format);
   vprintf(format,args);
   va_end(args);
   semaphore=0;
}

#define FRAME_MAX_BACKLOG 64

/* ETH_RXRING N (build flag, default 0): N RX descriptors instead of one. A 1514 B frame lasts 121 us on the wire at
   100 Mbit and the ISR + copy takes about 26 us, so one descriptor only loses frames when the ISR is held off; N of
   them bridge N frame times of IRQ latency. N buffers of FRAME_SIZE must fit RX_FRAME_ADDRESS..USB_DATA (64 KiB). */
/* ETH_ADV1000 (build flag, default 0): the OS-time PHY setup advertises 1000BASE-T as well as 10/100 (the boot menu's
   lwIP path links at 1000 on the same RGMII); step2 then sets the GEM speed and clocks from the resolved link. */
#ifndef ETH_ADV1000
#define ETH_ADV1000 0
#endif
#ifndef ETH_RXRING
#define ETH_RXRING 0
#endif
#if ETH_RXRING
#define RXBD_CNT       ETH_RXRING
#if RXBD_CNT * FRAME_SIZE > 0x10000
#error "ETH_RXRING: the RX buffers overrun RX_FRAME_ADDRESS's 64 KiB"
#endif
// The backlog region ends where TX_FRAME_ADDRESS begins (32 slots). 64 slots overran it into the TX frame (slots
// 32-47) and the RX buffers (48-63); one RxBD hid that, the ring fills it under AmigaOS. Full -> reset, as before.
#undef FRAME_MAX_BACKLOG
#define FRAME_MAX_BACKLOG 32
#if FRAME_MAX_BACKLOG * FRAME_SIZE > TX_FRAME_ADDRESS - RX_BACKLOG_ADDRESS
#error "ETH_RXRING: FRAME_MAX_BACKLOG overruns the RX backlog region"
#endif
#else
#define RXBD_CNT       1   /* Number of RxBDs to use */
#endif
#define TXBD_CNT       4   /* Number of TxBDs to use */

// could also be 55, 77 (eth1), see interrupts.pdf last page
// XPS_GEM0_INT_ID == 54
#define EMACPS_IRPT_INTR         XPS_GEM0_INT_ID

#define SLCR_LOCK_ADDR           (XPS_SYS_CTRL_BASEADDR + 0x4)
#define SLCR_UNLOCK_ADDR         (XPS_SYS_CTRL_BASEADDR + 0x8)
#define SLCR_GEM0_CLK_CTRL_ADDR  (XPS_SYS_CTRL_BASEADDR + 0x140)
#define SLCR_GEM1_CLK_CTRL_ADDR  (XPS_SYS_CTRL_BASEADDR + 0x144)

#define SLCR_LOCK_KEY_VALUE      0x767B
#define SLCR_UNLOCK_KEY_VALUE    0xDF0D
#define SLCR_ADDR_GEM_RST_CTRL   (XPS_SYS_CTRL_BASEADDR + 0x214)

#define EMACPS_PHY_DELAY_SEC     4 //Amount of time to delay waiting on PHY to reset
#define EMACPS_SLCR_DIV_MASK     0xFC0FC0FF

static int32_t GemVersion;
uint8_t EmacPsMAC[6] = {0x00,0x80,0x10,0x00,0x01,0x00};

static volatile int32_t DeviceErrors = 0;
static volatile uint32_t FramesTx = 0;
static volatile uint32_t FramesRx = 0;
static volatile uint16_t frame_serial = 0;
static volatile uint32_t frames_received = 0;
static volatile uint16_t frames_backlog = 0;
int frames_received_from_backlog = 0;

#if ETH_PROF
static void ep_release(int slot);
static void ep_tx_end(int rc, XTime t0, XTime t3, uint32_t sleeps);
#endif

#define ETH_PHY_TYPE_MICREL    0
#define ETH_PHY_TYPE_MOTORCOMM 1
static int eth_phy_type_ = ETH_PHY_TYPE_MICREL;

uint32_t PhyAddr=0;

typedef char EthernetFrame[XEMACPS_MAX_VLAN_FRAME_SIZE_JUMBO] __attribute__ ((aligned(64)));

volatile char* TxFrame = (char*)(RTG_BASE+TX_FRAME_ADDRESS);      /* Transmit buffer */
volatile char* RxFrame = (char*)(RTG_BASE+RX_FRAME_ADDRESS);      /* Receive buffer */

/*
 * Buffer descriptors are allocated in uncached memory. The memory is made
 * uncached by setting the attributes appropriately in the MMU table.
 */
#define RXBD_SPACE_BYTES XEmacPs_BdRingMemCalc(XEMACPS_BD_ALIGNMENT, RXBD_CNT)
#define TXBD_SPACE_BYTES XEmacPs_BdRingMemCalc(XEMACPS_BD_ALIGNMENT, TXBD_CNT)

#define PHY_DETECT_REG1 2
#define PHY_DETECT_REG2 3
#define PHY_ID_MARVELL        0x141
#define PHY_ID_MICREL_KSZ9031 0x22

static void xEmacPsSendHandler(void *Callback);
static void xEmacPsRecvHandler(void *Callback);
static void xEmacPsErrorHandler(void *Callback, uint8_t direction, uint32_t word);
LONG setup_phy(XEmacPs * EmacPsInstancePtr);
static LONG emacPsSetupIntrSystem(XEmacPs *EmacPsInstancePtr, uint16_t EmacPsIntrId);

void micrel_auto_negotiate(XEmacPs *xemacpsp, uint32_t phy_addr, int eth_phy_type);
uint32_t micrel_auto_negotiate_step2(XEmacPs *xemacpsp, uint32_t phy_addr, int eth_phy_type);

enum {
   ETH_TASK_SETUP,
   ETH_TASK_NEGOTIATE,
   ETH_TASK_INIT,
   ETH_TASK_READY
};

int ethernet_task_state = ETH_TASK_SETUP;

void xEmacPsClkSetup(XEmacPs *EmacPsInstancePtr, uint16_t EmacPsIntrId, int link_speed)
{
   (void)EmacPsInstancePtr;
   uint32_t SlcrTxClkCntrl;
   //uint32_t CrlApbClkCntrl;

   // Check and correct divisors before configuring them
   FixEthernetPLLDivisors_z3660();

   if (GemVersion == 2)
   {
      // SLCR unlock
      *(volatile unsigned int *)(SLCR_UNLOCK_ADDR) = SLCR_UNLOCK_KEY_VALUE;
      if (EmacPsIntrId == XPS_GEM0_INT_ID) {
         // GEM0 clock configuration
         SlcrTxClkCntrl = *(volatile unsigned int *)(SLCR_GEM0_CLK_CTRL_ADDR);

         SlcrTxClkCntrl &= EMACPS_SLCR_DIV_MASK;
         if (link_speed == 100) {
//            SlcrTxClkCntrl |= (XPAR_PS7_ETHERNET_0_ENET_SLCR_100MBPS_DIV1 << 20);
//            SlcrTxClkCntrl |= (XPAR_PS7_ETHERNET_0_ENET_SLCR_100MBPS_DIV0 << 8);
            SetUpSLCRDivisors_z3660(EmacPsInstancePtr->Config.BaseAddress,100);
//            SlcrTxClkCntrl |= (10 << 20);
//            SlcrTxClkCntrl |= (10 << 8);
         } else if (link_speed == 1000) {
//            SlcrTxClkCntrl |= (XPAR_PS7_ETHERNET_0_ENET_SLCR_1000MBPS_DIV1 << 20);
//            SlcrTxClkCntrl |= (XPAR_PS7_ETHERNET_0_ENET_SLCR_1000MBPS_DIV0 << 8);
            SetUpSLCRDivisors_z3660(EmacPsInstancePtr->Config.BaseAddress,1000);
//            SlcrTxClkCntrl |= (1 << 20);
//            SlcrTxClkCntrl |= (10 << 8);
         } else if (link_speed == 10) {
//            SlcrTxClkCntrl |= (XPAR_PS7_ETHERNET_0_ENET_SLCR_10MBPS_DIV1 << 20);
//            SlcrTxClkCntrl |= (XPAR_PS7_ETHERNET_0_ENET_SLCR_10MBPS_DIV0 << 8);
            SetUpSLCRDivisors_z3660(EmacPsInstancePtr->Config.BaseAddress,100);
//            SlcrTxClkCntrl |= (50 << 20);
//            SlcrTxClkCntrl |= (20 << 8);
         } else {
            printf("XEmacPsClkSetup: invalid link speed %d\n", link_speed);
         }
         *(volatile unsigned int *)(SLCR_GEM0_CLK_CTRL_ADDR) = SlcrTxClkCntrl;
      } else if (EmacPsIntrId == XPS_GEM1_INT_ID) {
      }
      // SLCR lock
      *(unsigned int *)(SLCR_LOCK_ADDR) = SLCR_LOCK_KEY_VALUE;
   }
}

static XEmacPs_Bd *BdRxPtr;

int init_ethernet_buffers() {
   XEmacPs* EmacPsInstancePtr = &EmacPsInstance;
   XEmacPs_Bd BdTemplate;

   XEmacPs_Stop(EmacPsInstancePtr);

   XEmacPs_BdClear(&BdTemplate);

   int Status = XEmacPs_BdRingCreate(&(XEmacPs_GetRxRing
         (EmacPsInstancePtr)),
         RTG_BASE+RX_BD_LIST_START_ADDRESS,
         RTG_BASE+RX_BD_LIST_START_ADDRESS,
         XEMACPS_BD_ALIGNMENT,
         RXBD_CNT);
   if (Status != XST_SUCCESS) {
      printf("EMAC: Error setting up RxBD space, BdRingCreate\n");
      return(XST_FAILURE);
   }

   Status = XEmacPs_BdRingClone(&(XEmacPs_GetRxRing(EmacPsInstancePtr)),
         &BdTemplate, XEMACPS_RECV);
   if (Status != XST_SUCCESS) {
      printf("EMAC: Error setting up RxBD space, BdRingClone\n");
      return(XST_FAILURE);
   }

   XEmacPs_BdClear(&BdTemplate);
   XEmacPs_BdSetStatus(&BdTemplate, XEMACPS_TXBUF_USED_MASK);

   // Create the TxBD ring
   Status = XEmacPs_BdRingCreate(&(XEmacPs_GetTxRing
         (EmacPsInstancePtr)),
         RTG_BASE+TX_BD_LIST_START_ADDRESS,
         RTG_BASE+TX_BD_LIST_START_ADDRESS,
         XEMACPS_BD_ALIGNMENT,
         TXBD_CNT);
   if (Status != XST_SUCCESS) {
      printf("Error setting up TxBD space, BdRingCreate\n");
      return(XST_FAILURE);
   }
   Status = XEmacPs_BdRingClone(&(XEmacPs_GetTxRing(EmacPsInstancePtr)), &BdTemplate, XEMACPS_SEND);
   if (Status != XST_SUCCESS) {
      printf("Error setting up TxBD space, BdRingClone\n");
      return(XST_FAILURE);
   }

   Status = XEmacPs_BdRingAlloc(&
         (XEmacPs_GetRxRing(EmacPsInstancePtr)),
         RXBD_CNT, &BdRxPtr);
   if (Status != XST_SUCCESS) {
      printf("EMAC: Error allocating RxBDs\n");
      return(XST_FAILURE);
   }
   for (int i=0; i<RXBD_CNT; i++) {
      XEmacPs_BdSetAddressRx(BdRxPtr, RxFrame + FRAME_SIZE*i);
      BdRxPtr = XEmacPs_BdRingNext(&(XEmacPs_GetRxRing(EmacPsInstancePtr)), BdRxPtr);
   }
   Status = XEmacPs_BdRingToHw(&(XEmacPs_GetRxRing(EmacPsInstancePtr)), RXBD_CNT, BdRxPtr);
   if (Status != XST_SUCCESS) {
      printf("EMAC: Error committing RxBD to HW\n");
      return(XST_FAILURE);
   }

   XEmacPs_Start(EmacPsInstancePtr);
   printf("EMAC: XEmacPs_Start done.\n");

   return(XST_SUCCESS);
}

int ethernet_init() {
   XEmacPs_Config *Config;
   long Status;
   XEmacPs* EmacPsInstancePtr = &EmacPsInstance;

   ethernet_task_state = ETH_TASK_SETUP;
   frames_backlog = 0;
   frames_received_from_backlog = 0;

   DeviceErrors = 0;
   FramesTx = 0;
   FramesRx = 0;
   frame_serial = 0;
   frames_received = 0;

   Config = XEmacPs_LookupConfig(XPAR_XEMACPS_0_DEVICE_ID);
   Status = XEmacPs_CfgInitialize(EmacPsInstancePtr, Config, Config->BaseAddress);

   if (Status != XST_SUCCESS) {
      printf("EMAC: Error in initialize\n");
      return(XST_FAILURE);
   }

   GemVersion = ((Xil_In32(Config->BaseAddress + 0xFC)) >> 16) & 0xFFF;
   printf("EMAC: GemVersion: %ld\n", GemVersion);

   Status = XEmacPs_SetMacAddress(EmacPsInstancePtr, EmacPsMAC, 1);
   if (Status != XST_SUCCESS) {
      printf("EMAC: Error setting MAC address\n");
      return(XST_FAILURE);
   }

   Status = XEmacPs_SetHandler(EmacPsInstancePtr, XEMACPS_HANDLER_DMASEND,
         (void *) xEmacPsSendHandler,
         EmacPsInstancePtr);
   Status |=
         XEmacPs_SetHandler(EmacPsInstancePtr, XEMACPS_HANDLER_DMARECV,
               (void *) xEmacPsRecvHandler,
               EmacPsInstancePtr);
   Status |=
         XEmacPs_SetHandler(EmacPsInstancePtr, XEMACPS_HANDLER_ERROR,
               (void *) xEmacPsErrorHandler,
               EmacPsInstancePtr);

   if (Status != XST_SUCCESS) {
      printf("EMAC: Error assigning handlers\n");
      return(XST_FAILURE);
   }

   // FIXME address space?
   /*
    * The BDs need to be allocated in uncached memory. Hence the 1 MB
    * address range that starts at address 0x0FF00000 is made uncached.
    */

   //Xil_SetTlbAttributes(RTG_BASE+TX_BD_LIST_START_ADDRESS, STRONG_ORDERED);
   //Xil_SetTlbAttributes(RTG_BASE+TX_FRAME_ADDRESS, STRONG_ORDERED);
   //Xil_SetTlbAttributes(RTG_BASE+RX_BACKLOG_ADDRESS, STRONG_ORDERED);
   //Xil_SetTlbAttributes(RTG_BASE+RX_FRAME_ADDRESS, 0xc02);
   //Xil_SetTlbAttributes(RTG_BASE+TX_FRAME_ADDRESS, 0xc02);

   XEmacPs_SetMdioDivisor(EmacPsInstancePtr, MDC_DIV_224);

   setup_phy(EmacPsInstancePtr);


   ethernet_update_mac_address();

   return(XST_SUCCESS);
}

void SetUpSLCRDivisors_z3660(UINTPTR mac_baseaddr, int32_t speed)
{
	volatile UINTPTR slcrBaseAddress;
	uint32_t SlcrDiv0 = 0;
	uint32_t SlcrDiv1 = 0;
	uint32_t SlcrTxClkCntrl;
	uint32_t gigeversion;

	gigeversion = ((Xil_In32(mac_baseaddr + 0xFC)) >> 16) & 0xFFF;
	if (gigeversion == 2) {

		Xil_Out32(SLCR_UNLOCK_ADDR, SLCR_UNLOCK_KEY_VALUE);

		if (mac_baseaddr == XPAR_XEMACPS_0_BASEADDR) {
			slcrBaseAddress = SLCR_GEM0_CLK_CTRL_ADDR;
		} else {
			slcrBaseAddress = SLCR_GEM1_CLK_CTRL_ADDR;
		}
#define SLCR_GEM_SRCSEL_EMIO 0x40

		if(Xil_In32(slcrBaseAddress) &
			SLCR_GEM_SRCSEL_EMIO) {
				return;
		}

		if (speed == 1000) {
			if (mac_baseaddr == XPAR_XEMACPS_0_BASEADDR) {
				// Use corrected values to generate 125 MHz
				// Originally: DIV0=16, DIV1=1 -> 62.5 MHz (incorrect)
				// Corrected: DIV0=8, DIV1=1 -> 125 MHz (correct)
				SlcrDiv0 = 8;  // Corrected value
				SlcrDiv1 = 1;  // Corrected value
//				printf("Setting corrected divisors for 1000 Mbps: DIV0=8, DIV1=1\n");
			}
		} else if (speed == 100) {
			if (mac_baseaddr == XPAR_XEMACPS_0_BASEADDR) {
				// Use corrected values to generate 25 MHz
				SlcrDiv0 = 8;  // Corrected value
				SlcrDiv1 = 5;  // Corrected value
//				printf("Setting corrected divisors for 100 Mbps: DIV0=8, DIV1=5\n");
			}
		} else {
			if (mac_baseaddr == XPAR_XEMACPS_0_BASEADDR) {
				// Use corrected values to generate 2.5 MHz
				SlcrDiv0 = 8;   // Corrected value
				SlcrDiv1 = 50;  // Corrected value
//				printf("Setting corrected divisors for 10 Mbps: DIV0=8, DIV1=50\n");
			}
		}

		if (SlcrDiv0 != 0 && SlcrDiv1 != 0) {
			SlcrTxClkCntrl = Xil_In32(slcrBaseAddress);
			SlcrTxClkCntrl &= EMACPS_SLCR_DIV_MASK;
			SlcrTxClkCntrl |= (SlcrDiv1 << 20);
			SlcrTxClkCntrl |= (SlcrDiv0 << 8);
			Xil_Out32(slcrBaseAddress, SlcrTxClkCntrl);
			Xil_Out32(SLCR_LOCK_ADDR, SLCR_LOCK_KEY_VALUE);
		} else {
			printf("Clock Divisors incorrect - Please check\n");
		}
	}
	else
   {
      printf("Unsupported GEM version %ld for SLCR clock setup\n", gigeversion);
   }
	return;
}

void ethernet_task() {
   XEmacPs* EmacPsInstancePtr = &EmacPsInstance;

   switch (ethernet_task_state)
   {
   case ETH_TASK_SETUP:
      // FIXME
      emacPsSetupIntrSystem(EmacPsInstancePtr, EMACPS_IRPT_INTR);

      ethernet_task_state = ETH_TASK_NEGOTIATE;
      break;


   case ETH_TASK_NEGOTIATE: {
      int complete = micrel_auto_negotiate_step2(EmacPsInstancePtr, PhyAddr, eth_phy_type_);

      if (complete!=XST_FAILURE) {
         ethernet_task_state = ETH_TASK_INIT;
      }
      break;


   }
   case ETH_TASK_INIT: {
      // init_ethernet_buffers() also starts EmacPS
      uint16_t status = init_ethernet_buffers();
      if (status != XST_SUCCESS) {
         printf("EMAC: init_ethernet_buffers() error\n");
      }

      ethernet_task_state = ETH_TASK_READY;
      break;
   }
   case ETH_TASK_READY:
      break;
   default:
      ethernet_task_state = ETH_TASK_READY;
   }
}


static void xEmacPsSendHandler(void *Callback)
{
   (void)Callback;
   XEmacPs_Bd *BdTxPtr;
   //XEmacPs *EmacPsInstancePtr = (XEmacPs *) Callback;
   XEmacPs* EmacPsInstancePtr = &EmacPsInstance;
   EP_T(ep_ta);

   uint32_t status = XEmacPs_ReadReg(EmacPsInstancePtr->Config.BaseAddress, XEMACPS_TXSR_OFFSET);
   XEmacPs_WriteReg(EmacPsInstancePtr->Config.BaseAddress, XEMACPS_TXSR_OFFSET, status);

   DEBUG_ETHERNET("XEMACPS_TXSR status: %lu\n", status);

   int bds_sent = XEmacPs_BdRingFromHwTx(&(XEmacPs_GetTxRing(EmacPsInstancePtr)), 1, &BdTxPtr);

   if (bds_sent == 1) {
      status = XEmacPs_BdGetStatus(BdTxPtr);
      if(debug_console.debug_ethernet)
      {
         DEBUG_ETHERNET("BD status: ");
         if (status&XEMACPS_TXBUF_USED_MASK) DEBUG_ETHERNET("USED ");
         if (status&XEMACPS_TXBUF_WRAP_MASK) DEBUG_ETHERNET("WRAP ");
         if (status&XEMACPS_TXBUF_RETRY_MASK) DEBUG_ETHERNET("RETRY "); // retry limit exceeded
         if (status&XEMACPS_TXBUF_URUN_MASK) DEBUG_ETHERNET("URUN"); // tx underrun
         if (status&XEMACPS_TXBUF_EXH_MASK) DEBUG_ETHERNET("EXH "); // buffers exhausted
         if (status&XEMACPS_TXBUF_TCP_MASK) DEBUG_ETHERNET("TCP "); // late collision
         if (status&XEMACPS_TXBUF_NOCRC_MASK) DEBUG_ETHERNET("NOCRC "); // no crc
         if (status&XEMACPS_TXBUF_LAST_MASK) DEBUG_ETHERNET("LAST ");
         if (status&XEMACPS_TXBUF_LEN_MASK) DEBUG_ETHERNET("LEN ");
         DEBUG_ETHERNET("\n");
      }
      status = XEmacPs_BdRingFree(&(XEmacPs_GetTxRing(EmacPsInstancePtr)), 1, BdTxPtr);

      if (status != XST_SUCCESS) {
         printf("XEmacPs_BdRingFree error: %lu\n",status);
         return;
      }

      XEmacPs_BdSetStatus(BdTxPtr, XEMACPS_TXBUF_USED_MASK); // XEMACPS_TXBUF_WRAP_MASK

      EP_DO(if(eth_prof.t_xmit && eth_prof_on) ep_acc(EP_TX_DONE, ep_now()-eth_prof.t_xmit); eth_prof.t_xmit=0);
      FramesTx++;
   }
   EP_END(EP_TX_ISR,ep_ta);
   EP_DO(ep_src_done(EP_SRC_GEMTX,ep_ta));
}

#define XEMACPS_BD_TO_INDEX(ringptr, bdptr) \
      (((uint32_t)bdptr - (uint32_t)(ringptr)->BaseBdAddr) / (ringptr)->Separation)

void ethernet_alloc_rx_frames() {
   XEmacPs* EmacPsInstancePtr = &EmacPsInstance;

   XEmacPs_BdRing* rxring = &(XEmacPs_GetRxRing(EmacPsInstancePtr));
   XEmacPs_Bd* rxbd;

   int free_bds = XEmacPs_BdRingGetFreeCnt(rxring);

   for (int i=0; i<free_bds; i++) {

      int Status = XEmacPs_BdRingAlloc(rxring, 1, &rxbd);
      if (Status != XST_SUCCESS) {
         printf("EMAC: Error allocating RxBD\n");
      } else {

         int bd_index=XEMACPS_BD_TO_INDEX(rxring, rxbd);
         XEmacPs_BdClearRxNew(rxbd);
         XEmacPs_BdSetAddressRx(rxbd, RxFrame+bd_index*FRAME_SIZE); // FIXME redundant?

         Status = XEmacPs_BdRingToHw(rxring, 1, rxbd);
         if (Status != XST_SUCCESS) {
            printf("EMAC: Error committing RxBD to HW\n");

            XEmacPs_BdRingUnAlloc(rxring, 1, rxbd); // FIXME double check
         }
      }
   }

#if ETH_RXRING
   __asm__ volatile ("dsb sy" ::: "memory");   // the cleared RxNew bits are out before the GEM looks again
#endif
   if (!free_bds) {
      printf("EMAC: no BDs free for allocation\n");
   }
}
int num_rx_bufs=0;
extern int interrupt_enabled_ethernet;
static void xEmacPsRecvHandler(void *Callback)
{
   (void)Callback;
   uint32_t status;
//   XEmacPs* EmacPsInstancePtr = (XEmacPs *) Callback;
   XEmacPs* EmacPsInstancePtr = &EmacPsInstance;

   XEmacPs_BdRing* rxring = &(XEmacPs_GetRxRing(EmacPsInstancePtr));
   XEmacPs_Bd* rxbdset, *cur_bd_ptr;
   EP_T(ep_ra);

#if ETH_RXRING
   // While the ring is full the BSP returns one BD per call (HwTail == HwHead), so drain until it returns none;
   // each pass frees and re-arms what it took, as lwIP's xemacpsif does.
   int isr_frames = 0;
   while ((num_rx_bufs = XEmacPs_BdRingFromHwRx(rxring, RXBD_CNT, &rxbdset)) > 0) {
   isr_frames += num_rx_bufs;
#else
   num_rx_bufs = XEmacPs_BdRingFromHwRx(rxring, RXBD_CNT, &rxbdset);
#endif
   EP_DO(eth_prof.rx_calls++; if(num_rx_bufs<=0) eth_prof.rx_bufs0++; else eth_prof.rx_frames+=num_rx_bufs;
         if(num_rx_bufs>(int)eth_prof.rx_bufs_max) eth_prof.rx_bufs_max=num_rx_bufs);

   // we immediately process the incoming frame.
   // main task will then signal the Amiga via interrupt
   // driver on Amiga side will call ethernet_receive_frame after copying the frame
   // and this will free up the EmacPS receive buffer again.

   if (num_rx_bufs > 0) {
      DEBUG_ETHERNET("EMAC: num_rx_bufs %d\n", num_rx_bufs);

      cur_bd_ptr = rxbdset;

      for (int i=0; i<num_rx_bufs; i++) {

         frame_serial++;
//         printf("f_s %d\n",frame_serial);

         DEBUG_ETHERNET("RX ser: %d\n",frame_serial);

         uint32_t bd_idx = XEMACPS_BD_TO_INDEX(rxring, cur_bd_ptr);
         int rx_bytes = XEmacPs_BdGetLength(cur_bd_ptr);

         uint8_t* frame_ptr = (uint8_t*)(RxFrame + bd_idx*FRAME_SIZE);

         DEBUG_ETHERNET("EMAC: RX: %d [%d] bd_idx: %d\n", frame_serial, rx_bytes, bd_idx);

//         Xil_DCacheInvalidateRange((UINTPTR)frame_ptr, FRAME_SIZE);
//         Xil_L1DCacheFlushRange((UINTPTR)frame_ptr, FRAME_SIZE);
//         Xil_L2CacheFlushRange((UINTPTR)frame_ptr, FRAME_SIZE);

         if (frames_backlog<FRAME_MAX_BACKLOG) {
            // copy the frame to the backlog (frames that amiga hasn't fetched yet)
            uint8_t* frame_bl_ptr = (uint8_t*)(RTG_BASE+RX_BACKLOG_ADDRESS+frames_backlog*FRAME_SIZE);
            EP_T(ep_ca);
            memcpy(frame_bl_ptr+RX_FRAME_PAD, frame_ptr, rx_bytes);
            EP_END(EP_RX_COPY,ep_ca);
            if(rx_bytes>FRAME_SIZE)
               printf("rx_bytes>FRAME_SIZE!!! <--------------------");
//            printf("f_s %d rx_b %d fb %d\n",frame_serial,rx_bytes,frames_backlog);

            *(frame_bl_ptr)   = (rx_bytes&0xff00)>>8;
            *(frame_bl_ptr+1) = (rx_bytes&0xff);
            *(frame_bl_ptr+2) = (frame_serial&0xff00)>>8;
            *(frame_bl_ptr+3) = (frame_serial&0xff);

            EP_DO(eth_prof.t_slot[frames_backlog] = eth_prof_on ? ep_now() : 0);
            frames_backlog++;
            EP_DO(if(frames_backlog>eth_prof.bl_max) eth_prof.bl_max=frames_backlog);

            DEBUG_ETHERNET("bd %ld [%d] copied from %p to %p\n", bd_idx, rx_bytes, frame_ptr, frame_bl_ptr);
         } else {
            if(interrupt_enabled_ethernet)
               DEBUG_ETHERNET("EMAC: frames_backlog full\n");
            EP_DO(eth_prof.bl_full++; eth_prof.bl_full_lost += frames_backlog - frames_received_from_backlog + 1);
            frames_backlog = 0;
            frames_received_from_backlog = 0;

         }

#if !ETH_RXRING
         XEmacPs_BdClearRxNew(cur_bd_ptr);
#endif
         // (ring: the BD goes back to the GEM only in ethernet_alloc_rx_frames, after BdRingFree; clearing RxNew
         // here would let the GEM refill it before the re-arm clears RxNew again and drops that frame)
         cur_bd_ptr = XEmacPs_BdRingNext(rxring, cur_bd_ptr);

         frames_received++;
      }

      int Status = XEmacPs_BdRingFree(rxring, num_rx_bufs, rxbdset);
      if (Status != XST_SUCCESS) {
         DEBUG_ETHERNET("EMAC: Error freeing RxBDs\n");
      } else {
         DEBUG_ETHERNET("EMAC: freed %d RxBDs\n", num_rx_bufs);
      }

      ethernet_alloc_rx_frames();

      DEBUG_ETHERNET("EMAC: backlog %d fetched %d\n", frames_backlog, frames_received_from_backlog);
   }
#if ETH_RXRING
   }
   (void)isr_frames;
   EP_DO(eth_prof.rx_isr++; if(!isr_frames) eth_prof.rx_isr0++; if(isr_frames>(int)eth_prof.rx_isr_max) eth_prof.rx_isr_max=isr_frames);
   EP_DO(ep_rx_window(isr_frames,ep_ra));
#endif

   status = XEmacPs_ReadReg(EmacPsInstancePtr->Config.BaseAddress, XEMACPS_RXSR_OFFSET);
   EP_DO(eth_prof.rxsr_or |= status; if(status&XEMACPS_RXSR_BUFFNA_MASK) eth_prof.rxsr_buffna++;
         if(status&XEMACPS_RXSR_RXOVR_MASK) eth_prof.rxsr_rxovr++; if(status&XEMACPS_RXSR_HRESPNOK_MASK) eth_prof.rxsr_hresp++;
         if(status&XEMACPS_RXSR_FRAMERX_MASK) eth_prof.rxsr_framerx++);
   XEmacPs_WriteReg(EmacPsInstancePtr->Config.BaseAddress, XEMACPS_RXSR_OFFSET, status);
   EP_END(EP_RX_ISR,ep_ra);
}

volatile uint8_t* ethernet_current_receive_ptr() {
   return((volatile uint8_t*)(RX_BACKLOG_ADDRESS+frames_received_from_backlog*FRAME_SIZE));
}

int ethernet_get_backlog() {
   return(frames_backlog);
}

int ethernet_receive_frame() {
   volatile uint8_t* frm = ethernet_current_receive_ptr()+RTG_BASE;
   //   printf("ethernet_receive_frame: %d bytes: %d serial: %d frames_backlog %d\n", frames_received_from_backlog, frm[0]<<8|frm[1], frm[2]<<8|frm[3],frames_backlog);
   /*
   int frame_size=frm[0]<<8|frm[1];
   volatile uint8_t* frm2=frm+4;
   for (int y=0; y<frame_size; y++) {
      printf("%02x",frm2[y]);
      if (y%4==3) printf(" ");
      if (y%32==31) printf("\n");
   }
   printf("\n==========================================\n");
    */
   if (frames_backlog>0) {
      EP_DO(ep_release(frames_received_from_backlog));
      frames_received_from_backlog++;
      if (frames_received_from_backlog >= frames_backlog) {
         // start over
         frames_backlog = 0;
         frames_received_from_backlog = 0;

         // fake the frame serial (so Amiga doesn't check this frame again)
         uint8_t old_serial_0=frm[2];
         uint8_t old_serial_1=frm[3];
         frm = ethernet_current_receive_ptr()+RTG_BASE;
         frm[2]=old_serial_0;
         frm[3]=old_serial_1;

         DEBUG_ETHERNET("EMAC: caught up with backlog\n");
      }
   } else {
      // this is NOT an error, Amiga wants data and there is no data on RX buffers
      DEBUG_ETHERNET("EMAC: ethernet_receive_frame() called but backlog is empty\n");
      frames_received_from_backlog=0;
   }
   return(frames_received_from_backlog);
}

uint32_t get_frames_received() {
   return(frames_received);
}

static int frames_dropped = 0;

uint8_t* ethernet_get_mac_address_ptr() {
   return((uint8_t*)&EmacPsMAC);
}

void ethernet_update_mac_address() {
   XEmacPs* EmacPsInstancePtr = &EmacPsInstance;

   if (ethernet_task_state != ETH_TASK_READY) {
      return;
   }

   printf("Ethernet: New MAC address %02x:%02x:%02x:%02x:%02x:%02x\n",
         EmacPsMAC[0],EmacPsMAC[1],EmacPsMAC[2],EmacPsMAC[3],EmacPsMAC[4],EmacPsMAC[5]);

   XEmacPs_Stop(EmacPsInstancePtr);

   int Status = XEmacPs_SetMacAddress(EmacPsInstancePtr, EmacPsMAC, 1);
   if (Status != XST_SUCCESS) {
      printf("EMAC: Error setting MAC address\n");
   }

   XEmacPs_Start(EmacPsInstancePtr);
}

static void xEmacPsErrorHandler(void *Callback, uint8_t Direction, uint32_t ErrorWord)
{
   (void)Callback;
   //XEmacPs *EmacPsInstancePtr = (XEmacPs *) Callback;
   EP_T(ep_ea);

   DeviceErrors++;

   switch (Direction) {
   case XEMACPS_RECV:
      if (ErrorWord & XEMACPS_RXSR_HRESPNOK_MASK) {
         EP_CNT(hrespnok);
         printf("EMAC: Receive DMA error\n");
      }
      if (ErrorWord & XEMACPS_RXSR_RXOVR_MASK) {
         EP_CNT(rxovr);
         DEBUG_ETHERNET("EMAC: Receive over run\n");
      }
      if (ErrorWord & XEMACPS_RXSR_BUFFNA_MASK) {
         EP_CNT(buffna);
         printf("EMAC: Receive buffer not available\n");
         // signal to host that frames are available
         frames_dropped++;
         frames_received++;
         if (frames_dropped%10 == 0) {
            DEBUG_ETHERNET("ETHDROP: %d\n",frames_dropped);
            ethernet_alloc_rx_frames();
         }
      }
      break;
   case XEMACPS_SEND:
      if (ErrorWord & XEMACPS_TXSR_HRESPNOK_MASK) {
         printf("EMAC: Transmit DMA error\n");
      }
      if (ErrorWord & XEMACPS_TXSR_URUN_MASK) {
         printf("EMAC: Transmit under run\n");
      }
      if (ErrorWord & XEMACPS_TXSR_BUFEXH_MASK) {
         printf("EMAC: Transmit buffer exhausted\n");
      }
      if (ErrorWord & XEMACPS_TXSR_RXOVR_MASK) {
         printf("EMAC: Transmit retry excessed limits\n");
      }
      if (ErrorWord & XEMACPS_TXSR_FRAMERX_MASK) {
         printf("EMAC: Transmit collision\n");
      }
      if (ErrorWord & XEMACPS_TXSR_USEDREAD_MASK) {
         printf("EMAC: Transmit buffer not available\n");
      }
      break;
   }
   /*
    * Bypassing the reset functionality as the default tx status for q0 is
    * USED BIT READ. so, the first interrupt will be tx used bit and it resets
    * the core always.
    */
   if (GemVersion == 2) {
      //EmacPsResetDevice(EmacPsInstancePtr);
   }
   EP_END(EP_ERR_ISR,ep_ea);
   EP_DO(ep_src_done(EP_SRC_GEMERR,ep_ea));
}

uint32_t xEmacPsDetectPHY(XEmacPs * EmacPsInstancePtr)
{
   uint32_t PhyAddr;
   uint32_t Status;
   uint16_t PhyReg1;
   uint16_t PhyReg2;

   for (PhyAddr = 0; PhyAddr <= 31; PhyAddr++) {
      Status = XEmacPs_PhyRead(EmacPsInstancePtr, PhyAddr,
            PHY_DETECT_REG1, &PhyReg1);

      Status |= XEmacPs_PhyRead(EmacPsInstancePtr, PhyAddr,
            PHY_DETECT_REG2, &PhyReg2);

      if ((Status == XST_SUCCESS) &&
            (PhyReg1 > 0x0000) && (PhyReg1 < 0xffff) &&
            (PhyReg2 > 0x0000) && (PhyReg2 < 0xffff)) {
         /* Found a valid PHY address */
         return(PhyAddr);
      }
   }

   return(PhyAddr);      /* default to 32(max of iteration) */
}

LONG setup_phy(XEmacPs * EmacPsInstancePtr)
{
   uint16_t PhyIdentity;

   PhyAddr = xEmacPsDetectPHY(EmacPsInstancePtr);

   if (PhyAddr >= 32) {
      printf("EMAC: Error detecting PHY\n");
      return(XST_FAILURE);
   }

   XEmacPs_PhyRead(EmacPsInstancePtr, PhyAddr, PHY_DETECT_REG1, &PhyIdentity);

   if (PhyIdentity == PHY_ID_MICREL_KSZ9031) {
      eth_phy_type_ = ETH_PHY_TYPE_MICREL;
      printf("EMAC: MICROCHIP KSZ9031 PHY detected\n");
      micrel_auto_negotiate(EmacPsInstancePtr, PhyAddr,eth_phy_type_);
      return(XST_SUCCESS);
   }
   else if (PhyIdentity == 0x4f51) {
      eth_phy_type_ = ETH_PHY_TYPE_MOTORCOMM;
      printf("EMAC: MOTORCOMM YT8531S PHY detected\n");
      micrel_auto_negotiate(EmacPsInstancePtr, PhyAddr,eth_phy_type_);
      return(XST_SUCCESS);
   }
   else {
      printf("EMAC: Unsupported PHY with id 0x%x\n",PhyIdentity);
   }

   return(XST_FAILURE);
}

#define ADVERTISE_10HALF        0x0020  /* Try for 10mbps half-duplex  */
#define ADVERTISE_1000XFULL     0x0020  /* Try for 1000BASE-X full-duplex */
#define ADVERTISE_10FULL        0x0040  /* Try for 10mbps full-duplex  */
#define ADVERTISE_1000XHALF     0x0040  /* Try for 1000BASE-X half-duplex */
#define ADVERTISE_100HALF       0x0080  /* Try for 100mbps half-duplex */
#define ADVERTISE_1000XPAUSE    0x0080  /* Try for 1000BASE-X pause    */
#define ADVERTISE_100FULL       0x0100  /* Try for 100mbps full-duplex */
#define ADVERTISE_1000XPSE_ASYM 0x0100  /* Try for 1000BASE-X asym pause */
#define ADVERTISE_100BASE4      0x0200  /* Try for 100mbps 4k packets  */

#define ADVERTISE_100_AND_10   (ADVERTISE_10FULL  | ADVERTISE_100FULL | ADVERTISE_10HALF | ADVERTISE_100HALF)
#define ADVERTISE_100          (ADVERTISE_100FULL | ADVERTISE_100HALF )
#define ADVERTISE_10           (ADVERTISE_10FULL  | ADVERTISE_10HALF  )

#define ADVERTISE_1000      0x0300

#define IEEE_ASYMMETRIC_PAUSE_MASK            0x0800
#define IEEE_PAUSE_MASK                       0x0400
#define IEEE_AUTONEG_ERROR_MASK               0x8000

#define IEEE_CONTROL_REG_OFFSET              0
#define IEEE_STATUS_REG_OFFSET               1
#define IEEE_AUTONEGO_ADVERTISE_REG          4
#define IEEE_PARTNER_ABILITIES_1_REG_OFFSET  5
#define IEEE_PARTNER_ABILITIES_2_REG_OFFSET  8
#define IEEE_1000BASET_CONTROL_REG  9
#define IEEE_1000BASET_STATUS_REG   10
#define IEEE_COPPER_SPECIFIC_CONTROL_REG     16
#define IEEE_SPECIFIC_STATUS_REG             17
#define IEEE_COPPER_SPECIFIC_STATUS_REG_2    19
#define IEEE_EXT_PHY_SPECIFIC_CONTROL_REG    20
#define IEEE_CONTROL_REG_MAC                 21
#define IEEE_PAGE_ADDRESS_REGISTER           22

#define IEEE_CTRL_1GBPS_LINKSPEED_MASK       0x2040
#define IEEE_CTRL_LINKSPEED_MASK             0x0040
#define IEEE_CTRL_LINKSPEED_1000M            0x0040
#define IEEE_CTRL_LINKSPEED_100M             0x2000
#define IEEE_CTRL_LINKSPEED_10M              0x0000
#define IEEE_CTRL_RESET_MASK                 0x8000
#define IEEE_CTRL_AUTONEGOTIATE_ENABLE       0x1000
#define IEEE_STAT_AUTONEGOTIATE_CAPABLE      0x0008
#define IEEE_STAT_AUTONEGOTIATE_COMPLETE     0x0020
#define IEEE_STAT_AUTONEGOTIATE_RESTART      0x0200
#define IEEE_STAT_1GBPS_EXTENSIONS           0x0100
#define IEEE_AN1_ABILITY_MASK                0x1FE0
#define IEEE_AN3_ABILITY_MASK_1GBPS          0x0C00
#define IEEE_AN1_ABILITY_MASK_100MBPS        0x0380
#define IEEE_AN1_ABILITY_MASK_10MBPS         0x0060
#define IEEE_RGMII_TXRX_CLOCK_DELAYED_MASK   0x0030

#define IEEE_1000BASE_STATUS_REG 0x0a

void micrel_auto_negotiate(XEmacPs *xemacpsp, uint32_t phy_addr, int eth_phy_type)
{
   uint16_t control;
   uint16_t status;
   int link_speed = 100;

   if(eth_phy_type == ETH_PHY_TYPE_MOTORCOMM) {
      printf("PHY: Start Ethernet PHY auto negotiation (MotorCom YT8531S)\n"); //MotorComm
      // access IEEE MII regs
      //      XEmacPs_PhyWrite(xemacpsp, phy_addr, 0x100, 0x6);
   } else {
      printf("PHY: Start Ethernet PHY auto negotiation (Microchip KSZ9031)\n"); //Micrel
   }

   XEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_PAGE_ADDRESS_REGISTER, 2);
   XEmacPs_PhyRead(xemacpsp, phy_addr, IEEE_CONTROL_REG_MAC, &control);
   control |= IEEE_RGMII_TXRX_CLOCK_DELAYED_MASK;
   XEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_CONTROL_REG_MAC, control);
   XEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_PAGE_ADDRESS_REGISTER, 0);

   XEmacPs_PhyRead(xemacpsp, phy_addr, IEEE_AUTONEGO_ADVERTISE_REG, &control);  //reg 0x04
   control |= IEEE_ASYMMETRIC_PAUSE_MASK;   //0x0800
   control |= IEEE_PAUSE_MASK;
   control |= ADVERTISE_100;
   control |= ADVERTISE_10;
   XEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_AUTONEGO_ADVERTISE_REG, control);

   if (link_speed == 100) {
      // register 0: 100 mbit
      XEmacPs_PhyRead(xemacpsp, phy_addr, IEEE_CONTROL_REG_OFFSET, &control);
      control &= ~(1<<6);
      // FIXME: why is this disabled?
      //control |= (1<<13);
      XEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_CONTROL_REG_OFFSET, control);

      // register 9, disable 1000base-t
      XEmacPs_PhyRead(xemacpsp, phy_addr, IEEE_1000BASET_CONTROL_REG, &control);
#if ETH_ADV1000
      control |= ADVERTISE_1000;   // advertise 1000 as well: 10/100 stay advertised and autonegotiation picks
#else
      control &= ~(1<<9 | 1<<8);
#endif
      XEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_1000BASET_CONTROL_REG,control);
   } else if (link_speed == 1000) {
      // register 0: 1000 mbit
      /*XEmacPs_PhyRead(xemacpsp, phy_addr, IEEE_CONTROL_REG_OFFSET, &control);
      control |= (1<<6);
      control &= ~(1<<13);
      XEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_CONTROL_REG_OFFSET, control);*/

      // register 9
      XEmacPs_PhyRead(xemacpsp, phy_addr, IEEE_1000BASET_CONTROL_REG, &control);
      control |= ADVERTISE_1000;
      XEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_1000BASET_CONTROL_REG, control);

      // this is "reserved" according to manual?!
      // page 0, register 10h
      XEmacPs_PhyRead(xemacpsp, phy_addr, IEEE_COPPER_SPECIFIC_CONTROL_REG,&control);
      control |= (7 << 12); // max number of gigabit attempts
      control |= (1 << 11); // enable downshift
      XEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_COPPER_SPECIFIC_CONTROL_REG,control);
   }

   // register 0
   XEmacPs_PhyRead(xemacpsp, phy_addr, IEEE_CONTROL_REG_OFFSET, &control);
   if (link_speed == 1000) {
      control |= IEEE_CTRL_AUTONEGOTIATE_ENABLE;
   }
   control |= IEEE_STAT_AUTONEGOTIATE_RESTART;
   XEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_CONTROL_REG_OFFSET, control);

   if (link_speed == 1000) {
      XEmacPs_PhyRead(xemacpsp, phy_addr, IEEE_CONTROL_REG_OFFSET, &control);
      control |= IEEE_CTRL_RESET_MASK;
      XEmacPs_PhyWrite(xemacpsp, phy_addr, IEEE_CONTROL_REG_OFFSET, control);

      while (1) {
         XEmacPs_PhyRead(xemacpsp, phy_addr, IEEE_CONTROL_REG_OFFSET, &control);
         if (control & IEEE_CTRL_RESET_MASK) continue; // ??? weird
         else break;
      }
   }

   XEmacPs_PhyRead(xemacpsp, phy_addr, IEEE_STATUS_REG_OFFSET, &status);

   printf("PHY: Waiting for PHY to complete auto negotiation. (status: %x).\n", status);
}

uint32_t micrel_auto_negotiate_step2(XEmacPs *xemacpsp, uint32_t phy_addr, int eth_phy_type) {
   uint16_t status;
   uint16_t status_speed;
   uint16_t link_speed = 1; // XST_FAILURE

   XEmacPs_PhyRead(xemacpsp, phy_addr, IEEE_STATUS_REG_OFFSET, &status);

   //   printf("PHY status: %x\n", status);

   if (status & IEEE_STAT_AUTONEGOTIATE_COMPLETE) {
      if(eth_phy_type == ETH_PHY_TYPE_MOTORCOMM) {
         // http://www.fpgadeveloper.com/2018/05/board-bring-up-myir-myd-y7z010-dev-board.html
         XEmacPs_PhyRead(xemacpsp, phy_addr, 0x11, &status_speed);

         if ((status_speed & 0xc000) == 0)
            link_speed = 10;
         else if((status_speed & 0xc000) == 0x4000)
            link_speed = 100;
         else if((status_speed & 0xc000) == 0x8000)
            link_speed = 1000;

//         printf("PHY (MotorComm): Link speed: %d mbit (status: %x)\n", link_speed, status_speed);
//         printf("PHY (MotorComm): Link up: %x\n",(status_speed&(1<<10))>>10);
      } else {
         // http://www.fpgadeveloper.com/2018/05/board-bring-up-myir-myd-y7z010-dev-board.html

         XEmacPs_PhyRead(xemacpsp, phy_addr, 0x1F, &status_speed);

         if (status_speed & 0x040)
            link_speed = 1000;
         else if(status_speed & 0x020)
            link_speed = 100;
         else if(status_speed & 0x010)
            link_speed = 10;

         printf("PHY (Micrel): Link speed: %d mbit\n",link_speed);
      }

      XEmacPs_SetOperatingSpeed(xemacpsp, link_speed);
      xEmacPsClkSetup(xemacpsp, EMACPS_IRPT_INTR, link_speed);

      return(link_speed);
   } else {
      uint16_t temp;

      // TODO: why?
      XEmacPs_PhyRead(xemacpsp, phy_addr, IEEE_COPPER_SPECIFIC_STATUS_REG_2, &temp);
      //timeout_counter++;

      //if (timeout_counter > 2) {
      //   printf("PHY: Auto negotiation timeout\n");
      //   break;
      //}

      return(XST_FAILURE);
   }
}

/****************************************************************************/
/**
 *
 * This function setups the interrupt system so interrupts can occur for the
 * EMACPS.
 * @param   EmacPsInstancePtr is a pointer to the instance of the EmacPs
 *      driver.
 * @param   EmacPsIntrId is the Interrupt ID and is typically
 *      XPAR_<EMACPS_instance>_INTR value from xparameters.h.
 *
 * @return   XST_SUCCESS if successful, otherwise XST_FAILURE.
 *
 * @note      None.
 *
 *****************************************************************************/
static LONG emacPsSetupIntrSystem(XEmacPs *EmacPsInstancePtr, uint16_t EmacPsIntrId)
{
   LONG Status;
   XScuGic *IntcInstancePtr = interrupt_get_intc();

   /*
    * Connect a device driver handler that will be called when an
    * interrupt for the device occurs, the device driver handler performs
    * the specific interrupt processing for the device.
    */
   Status = XScuGic_Connect(IntcInstancePtr, EmacPsIntrId,
         (Xil_InterruptHandler) XEmacPs_IntrHandler,
         (void *) EmacPsInstancePtr);
   if (Status != XST_SUCCESS) {
      printf("GIC: Unable to connect ISR to interrupt controller\n");
      return(XST_FAILURE);
   }

   /*
    * Enable interrupts from the hardware
    */
   XScuGic_Enable(IntcInstancePtr, EmacPsIntrId);
   printf("GIC: SCU GIC enabled\n");

   /*
    * Enable interrupts in the processor
    */
   //   Xil_ExceptionEnable();

   printf("GIC: Interrupts enabled\n");
   return(XST_SUCCESS);
}

uint16_t ethernet_send_frame(uint16_t frame_size) {
   XEmacPs* EmacPsInstancePtr = &EmacPsInstance;
   XEmacPs_Bd *BdTxPtr;
   EP_T(ep_t0);

   if (ethernet_task_state != ETH_TASK_READY) {
      EP_DO(ep_tx_end(1,ep_t0,0,0));
      return(1);
   }

   uint32_t old_frames_tx = FramesTx;

//   Xil_DCacheInvalidateRange((UINTPTR)TxFrame, sizeof(EthernetFrame));
   Xil_L1DCacheFlushRange((UINTPTR)TxFrame, sizeof(EthernetFrame));
   Xil_L2CacheFlushRange((UINTPTR)TxFrame, sizeof(EthernetFrame));
   EP_T(ep_t1);
   EP_SPAN(EP_TX_FLUSH,ep_t0,ep_t1);
   EP_DO(ep_src_done(EP_SRC_TXFLUSH,ep_t0));

   //   printf("ethernet_send_frame: %lu %d\n",old_frames_tx,frame_size);
   /*
   for (int y=0; y<frame_size; y++) {
      printf("%02x",TxFrame[y]);
      if (y%4==3) printf(" ");
      if (y%32==31) printf("\n");
   }
   printf("\n==========================================\n");
    */
   LONG Status = XEmacPs_BdRingAlloc(&(XEmacPs_GetTxRing(EmacPsInstancePtr)), 1, &BdTxPtr);

   if (Status != XST_SUCCESS) {
      printf("ERROR: BdRingAlloc error: %ld\n",Status);

      // lets unstick this
      //init_ethernet_buffers();
      EP_DO(ep_tx_end(2,ep_t0,0,0));
      return(2);
   }

   XEmacPs_BdSetAddressTx(BdTxPtr, TxFrame);
   XEmacPs_BdSetLength(BdTxPtr, frame_size);
   XEmacPs_BdClearTxUsed(BdTxPtr);
   XEmacPs_BdSetLast(BdTxPtr);

   Status = XEmacPs_BdRingToHw(&(XEmacPs_GetTxRing(EmacPsInstancePtr)), 1, BdTxPtr);

   if (Status != XST_SUCCESS) {
      printf("ERROR: BdRingToHw error: %ld\n",Status);
      EP_DO(ep_tx_end(3,ep_t0,0,0));
      return(3);
   }

   //   Xil_L1DCacheFlushRange((UINTPTR)BdTxPtr, 128);
   //   Xil_L2CacheFlushRange((UINTPTR)BdTxPtr, 128);

   EP_DO(eth_prof.t_xmit = eth_prof_on ? ep_now() : 0);
   XEmacPs_Transmit(EmacPsInstancePtr);
   EP_T(ep_t2);
   EP_SPAN(EP_TX_QUEUE,ep_t1,ep_t2);

   DEBUG_ETHERNET("FramesTx:%ld\n",FramesTx);
   DEBUG_ETHERNET("BdTxPtr:0x%08lx\n",(uint32_t)BdTxPtr);
   uint32_t counter = 0;
   while (old_frames_tx == FramesTx) {
      usleep(100);
      counter++;
      // 1ms
      if (counter>10) {
         printf("ERROR: timeout in ethernet_send_frame waiting for tx!\n");
         //init_ethernet_buffers();
         EP_DO(ep_tx_end(4,ep_t0,0,counter));
         return(4);
      }
   }
   EP_T(ep_t3);
   EP_SPAN(EP_TX_WAIT,ep_t2,ep_t3);
   DEBUG_ETHERNET("frame %ld transmitted!\n",FramesTx);

   // all good
   EP_DO(ep_tx_end(0,ep_t0,ep_t3,counter));
   return(0);
}

void PrintEthernetPLLFrequencies_z3660()
{
//   printf("=== Ethernet PLL Frequencies ===\n");
   
   // Base frequency of the IO PLL is typically 1000 MHz for Zynq
   // According to Xilinx documentation, the base frequency is 1000 MHz
   const uint32_t IO_PLL_FREQ_HZ = 1000000000; // 1000 MHz
   
//   printf("IO PLL base frequency: %lu MHz\n", IO_PLL_FREQ_HZ / 1000000);
   
   // Calculate frequency for 1000 Mbps
   uint32_t div0_1000 = XPAR_PS7_ETHERNET_0_ENET_SLCR_1000MBPS_DIV0;
   uint32_t div1_1000 = XPAR_PS7_ETHERNET_0_ENET_SLCR_1000MBPS_DIV1;
   uint32_t freq_1000 = IO_PLL_FREQ_HZ / (div0_1000 * div1_1000);
   
//   printf("1000 Mbps - Divisors: DIV0=%lu, DIV1=%lu\n", div0_1000, div1_1000);
//   printf("Resulting frequency: %lu MHz (expected: 125 MHz)\n", freq_1000 / 1000000);
//   printf("Difference: %d %%\n", (int)(((float)freq_1000 / 125000000.0 - 1.0) * 100.0));
   
   // Calculate frequency for 100 Mbps
//   uint32_t div0_100 = XPAR_PS7_ETHERNET_0_ENET_SLCR_100MBPS_DIV0;
//   uint32_t div1_100 = XPAR_PS7_ETHERNET_0_ENET_SLCR_100MBPS_DIV1;
//   uint32_t freq_100 = IO_PLL_FREQ_HZ / (div0_100 * div1_100);
   
//   printf("100 Mbps - Divisors: DIV0=%lu, DIV1=%lu\n", div0_100, div1_100);
//   printf("Resulting frequency: %lu MHz (expected: 25 MHz)\n", freq_100 / 1000000);
//   printf("Difference: %d %%\n", (int)(((float)freq_100 / 25000000.0 - 1.0) * 100.0));
   
   // Calculate frequency for 10 Mbps
//   uint32_t div0_10 = XPAR_PS7_ETHERNET_0_ENET_SLCR_10MBPS_DIV0;
//   uint32_t div1_10 = XPAR_PS7_ETHERNET_0_ENET_SLCR_10MBPS_DIV1;
//   uint32_t freq_10 = IO_PLL_FREQ_HZ / (div0_10 * div1_10);
   
//   printf("10 Mbps - Divisors: DIV0=%lu, DIV1=%lu\n", div0_10, div1_10);
//   printf("Resulting frequency: %lu MHz (expected: 2.5 MHz)\n", freq_10 / 1000000);
//   printf("Difference: %d %%\n", (int)(((float)freq_10 / 2500000.0 - 1.0) * 100.0));
   
//   printf("====================================\n");
   
   // Verify if the values match those defined in xparameters.h
   uint32_t expected_freq = XPAR_PS7_ETHERNET_0_ENET_CLK_FREQ_HZ;
//   printf("Expected frequency defined in xparameters.h: %lu MHz\n", expected_freq / 1000000);
   
   if (freq_1000 == expected_freq) {
//      printf("✓ Divisors for 1000 Mbps generate the expected frequency (125 MHz)\n");
   } else {
      printf("✗ ERROR: Divisors for 1000 Mbps do NOT generate the expected frequency\n");
      printf("  Calculated: %lu Hz, Expected: %lu Hz\n", freq_1000, expected_freq);
   }
}

void FixEthernetPLLDivisors_z3660()
{
//   printf("=== Automatic Ethernet PLL Divisors Correction ===\n");
   
   // Check current divisors
   uint32_t div0_1000 = XPAR_PS7_ETHERNET_0_ENET_SLCR_1000MBPS_DIV0;
   uint32_t div1_1000 = XPAR_PS7_ETHERNET_0_ENET_SLCR_1000MBPS_DIV1;
   uint32_t div0_100 = XPAR_PS7_ETHERNET_0_ENET_SLCR_100MBPS_DIV0;
   uint32_t div1_100 = XPAR_PS7_ETHERNET_0_ENET_SLCR_100MBPS_DIV1;
   uint32_t div0_10 = XPAR_PS7_ETHERNET_0_ENET_SLCR_10MBPS_DIV0;
   uint32_t div1_10 = XPAR_PS7_ETHERNET_0_ENET_SLCR_10MBPS_DIV1;
   
//   printf("Current divisors:\n");
//   printf("  1000 Mbps: DIV0=%lu, DIV1=%lu\n", div0_1000, div1_1000);
//   printf("  100 Mbps:  DIV0=%lu, DIV1=%lu\n", div0_100, div1_100);
//   printf("  10 Mbps:   DIV0=%lu, DIV1=%lu\n", div0_10, div1_10);
   
   // Correct values to generate 125 MHz: DIV0=8, DIV1=1
   const uint32_t CORRECT_DIV0_1000 = 8;
   const uint32_t CORRECT_DIV1_1000 = 1;
   const uint32_t CORRECT_DIV0_100 = 8;
   const uint32_t CORRECT_DIV1_100 = 5;
   const uint32_t CORRECT_DIV0_10 = 8;
   const uint32_t CORRECT_DIV1_10 = 50;
   
   // Redefine the correct values if they are wrong
   if (div0_1000 != CORRECT_DIV0_1000 || div1_1000 != CORRECT_DIV1_1000) {
      printf("✓ Correcting divisors for 1000 Mbps: DIV0=8, DIV1=1\n");
      
      // In embedded systems we cannot redefine macros, so we'll use direct values
      // in the SetUpSLCRDivisors_z3660 function when called
      // For calculation purposes, we'll update our temporary variables
      div0_1000 = CORRECT_DIV0_1000;
      div1_1000 = CORRECT_DIV1_1000;
   }
   
   if (div0_100 != CORRECT_DIV0_100 || div1_100 != CORRECT_DIV1_100) {
      printf("✓ Correcting divisors for 100 Mbps: DIV0=8, DIV1=5\n");
      div0_100 = CORRECT_DIV0_100;
      div1_100 = CORRECT_DIV1_100;
   }
   
   if (div0_10 != CORRECT_DIV0_10 || div1_10 != CORRECT_DIV1_10) {
      printf("✓ Correcting divisors for 10 Mbps: DIV0=8, DIV1=50\n");
      div0_10 = CORRECT_DIV0_10;
      div1_10 = CORRECT_DIV1_10;
   }
   
//   printf("Corrected divisors:\n");
//   printf("  1000 Mbps: DIV0=%lu, DIV1=%lu\n", div0_1000, div1_1000);
//   printf("  100 Mbps:  DIV0=%lu, DIV1=%lu\n", div0_100, div1_100);
//   printf("  10 Mbps:   DIV0=%lu, DIV1=%lu\n", div0_10, div1_10);
   
   // Verify the resulting frequency
   const uint32_t IO_PLL_FREQ_HZ = 1000000000;
   uint32_t freq_1000 = IO_PLL_FREQ_HZ / (div0_1000 * div1_1000);
   
   if (freq_1000 == XPAR_PS7_ETHERNET_0_ENET_CLK_FREQ_HZ) {
//      printf("Corrected divisors generate 125 MHz correctly\n");
   } else {
      printf("ERROR: Corrected divisors do NOT generate 125 MHz\n");
   }
   
//   printf("=== End of correction ===\n");
}



#if ETH_PROF
/* ---- the ethernet frame-path profile (eth_prof.h) ---------------------------------------------------------------- */
#ifndef ETH_PROF_REF
#define ETH_PROF_REF 0   /* the build's git commit, short hash as a number (CDEFS -DETH_PROF_REF=0x...) */
#endif
extern uint32_t counts_per_second;   /* the live ARM clock / 2 (main.c) */
#include "video.h"
#include "config_file.h"
extern ZZ_VIDEO_STATE vs;
extern int interrupt_enabled_audio, interrupt_enabled_audio_fake;
ETH_PROF_T eth_prof;
int eth_prof_on = 1;

static const char *const ep_names[EP_NUM] = {
   "TX_TOTAL", "TX_FLUSH", "TX_QUEUE", "TX_WAIT", "TX_TAIL", "TX_DONE", "TX_ISR",
   "RX_ISR", "RX_COPY", "RX_RES", "ERR_ISR",
   "H_TX_W", "H_TX_R", "H_RX_W", "H_RXA_R",
   "A_TX_W_SVC", "A_TX_W_ACK", "A_TX_R_SVC", "A_TX_R_ACK", "A_RX_W_SVC", "A_RX_W_ACK", "A_RXA_R_SVC", "A_RXA_R_ACK",
   "A_OTHER", "LOOP_PASS", "V_ISR", "V_FLUSH_L1", "V_FLUSH_L2", "AUD_ISR", "SCSI_INV", "SCSI_FLUSH",
};
static const char *const ep_src_names[EP_SRC_N] = { "video", "audio", "gemtx", "gemerr", "txflush", "scsi-inv", "scsi-flush" };
/* the first 8 are the "gem" line as before; the rest print as "gemx" */
static const uint32_t ep_gem_off[EP_GEM_N] = {
   XEMACPS_TXCNT_OFFSET, XEMACPS_RXCNT_OFFSET, XEMACPS_RXFCSCNT_OFFSET, XEMACPS_RXRESERRCNT_OFFSET,
   XEMACPS_RXORCNT_OFFSET, XEMACPS_RXALIGNCNT_OFFSET, XEMACPS_RXUNDRCNT_OFFSET, XEMACPS_RXOVRCNT_OFFSET,
   XEMACPS_RXIPCCNT_OFFSET, XEMACPS_RXTCPCCNT_OFFSET, XEMACPS_RXUDPCCNT_OFFSET, XEMACPS_RXSYMBCNT_OFFSET,
   XEMACPS_RXLENGTHCNT_OFFSET, XEMACPS_RXJABCNT_OFFSET, XEMACPS_RXPAUSECNT_OFFSET, XEMACPS_TXPAUSECNT_OFFSET,
   XEMACPS_TXURUNCNT_OFFSET, XEMACPS_SNGLCOLLCNT_OFFSET, XEMACPS_MULTICOLLCNT_OFFSET, XEMACPS_EXCESSCOLLCNT_OFFSET,
   XEMACPS_LATECOLLCNT_OFFSET, XEMACPS_TXDEFERCNT_OFFSET, XEMACPS_TXCSENSECNT_OFFSET,
};
static const char *const ep_gem_names[EP_GEM_N] = { "tx", "rx", "fcs", "reserr", "overrun", "align", "undersize", "oversize",
   "ipcsum", "tcpcsum", "udpcsum", "symbol", "length", "jabber", "rxpause", "txpause",
   "txurun", "coll1", "collm", "collx", "colllate", "defer", "csense" };

static uint32_t ep_ns(uint64_t t) { return (uint32_t)(t * 1000000000ull / counts_per_second); }
/* ETH_RX released backlog slot `slot`: its residence since the frame was copied in */
static void ep_release(int slot)
{
   if(!eth_prof_on || slot < 0 || slot >= EP_SLOTS || !eth_prof.t_slot[slot]) return;
   XTime d = ep_now() - eth_prof.t_slot[slot];
   eth_prof.t_slot[slot] = 0;
   ep_acc(EP_RX_RES, d);
   uint32_t ms = (uint32_t)(d * 1000ull / counts_per_second);
   eth_prof.res_ms[ms < EP_RES_MS ? ms : EP_RES_MS - 1]++;
}
static void ep_tx_end(int rc, XTime t0, XTime t3, uint32_t sleeps)
{
   eth_prof.tx_rc[rc < EP_TX_RCS ? rc : EP_TX_RCS - 1]++;
   if(rc == 0 || rc == 4) eth_prof.tx_sleeps[sleeps < EP_TX_SLEEPS ? sleeps : EP_TX_SLEEPS - 1]++;
   if(!eth_prof_on || !t0) return;
   XTime t4 = ep_now();
   if(t3) ep_acc(EP_TX_TAIL, t4 - t3);
   ep_acc(EP_TX_TOTAL, t4 - t0);
}
static uint32_t ep_stamp_cost(void)   /* one XTime_GetTime, best of 64 */
{
   XTime a, b, best = ~0ull;
   for(int i = 0; i < 64; i++) { XTime_GetTime(&a); XTime_GetTime(&b); if(b - a < best) best = b - a; }
   return ep_ns(best);
}
static void ep_gem_read(void)
{
   for(int i = 0; i < EP_GEM_N; i++) eth_prof.gem[i] += XEmacPs_ReadReg(EmacPsInstance.Config.BaseAddress, ep_gem_off[i]);
}
void eth_prof_zero(void)
{
   memset(&eth_prof, 0, sizeof eth_prof);
   ep_gem_read();
   memset(eth_prof.gem, 0, sizeof eth_prof.gem);   /* the GEM counters clear on read: this read started the window */
   eth_prof.t_zero = ep_now();
   printf("[ESTAT] zeroed; stamps %s\n", eth_prof_on ? "ON" : "OFF");
}
void eth_prof_toggle(void)
{
   eth_prof_on = !eth_prof_on;
   printf("[ESTAT] stamps %s (EPROF); counts that need no timer read keep counting\n", eth_prof_on ? "ON" : "OFF");
}
void eth_prof_dump(void)
{
   XTime now = ep_now();
   ep_gem_read();
   printf("[ESTAT] eth-profile ref %07lx cps %lu stamp %lu ns window %lu ms stamps %s ien %d ien_writes %lu\n",
          (unsigned long)ETH_PROF_REF, (unsigned long)counts_per_second, (unsigned long)ep_stamp_cost(),
          (unsigned long)((now - eth_prof.t_zero) * 1000ull / counts_per_second), eth_prof_on ? "ON" : "OFF",
          interrupt_enabled_ethernet, (unsigned long)eth_prof.ien_writes);
   printf("[ESTAT] row n avg_ns max_ns sum_us max_counts\n");
   for(int i = 0; i < EP_NUM; i++) {
      const EP_ACC *a = &eth_prof.acc[i];
      printf("[ESTAT] %-12s %lu %lu %lu %lu %lu\n", ep_names[i], (unsigned long)a->n,
             (unsigned long)(a->n ? ep_ns(a->sum / a->n) : 0), (unsigned long)ep_ns(a->max),
             (unsigned long)(a->sum * 1000000ull / counts_per_second), (unsigned long)a->max);
   }
   printf("[ESTAT] tx_sleeps");
   for(int i = 0; i < EP_TX_SLEEPS; i++) printf(" %lu", (unsigned long)eth_prof.tx_sleeps[i]);
   printf("\n[ESTAT] tx_rc");
   for(int i = 0; i < EP_TX_RCS; i++) printf(" %lu", (unsigned long)eth_prof.tx_rc[i]);
   printf("\n[ESTAT] rx_res_ms");
   for(int i = 0; i < EP_RES_MS; i++) printf(" %lu", (unsigned long)eth_prof.res_ms[i]);
   printf("\n[ESTAT] loop_pass_us<1,2,4..1024,>=1024");
   for(int i = 0; i < EP_PASS_BUCKETS; i++) printf(" %lu", (unsigned long)eth_prof.pass_hist[i]);
#if ETH_RXRING
   printf("\n[ESTAT] ring %d isr %lu isr0 %lu isr_frames_max %lu",
          RXBD_CNT, (unsigned long)eth_prof.rx_isr, (unsigned long)eth_prof.rx_isr0, (unsigned long)eth_prof.rx_isr_max);
#endif
   printf("\n[ESTAT] rx calls %lu frames %lu bufs0 %lu bufs_max %lu backlog_max %lu backlog_full %lu lost_by_full %lu\n",
          (unsigned long)eth_prof.rx_calls, (unsigned long)eth_prof.rx_frames, (unsigned long)eth_prof.rx_bufs0,
          (unsigned long)eth_prof.rx_bufs_max, (unsigned long)eth_prof.bl_max, (unsigned long)eth_prof.bl_full,
          (unsigned long)eth_prof.bl_full_lost);
   printf("[ESTAT] err buffna %lu rxovr %lu hrespnok %lu; rxsr_or %02lx buffna %lu rxovr %lu hresp %lu framerx %lu; ack_spins_max %lu\n",
          (unsigned long)eth_prof.buffna, (unsigned long)eth_prof.rxovr, (unsigned long)eth_prof.hrespnok,
          (unsigned long)eth_prof.rxsr_or, (unsigned long)eth_prof.rxsr_buffna, (unsigned long)eth_prof.rxsr_rxovr,
          (unsigned long)eth_prof.rxsr_hresp, (unsigned long)eth_prof.rxsr_framerx, (unsigned long)eth_prof.ack_spins_max);
   printf("[ESTAT] gem");
   for(int i = 0; i < 8; i++) printf(" %s %lu", ep_gem_names[i], (unsigned long)eth_prof.gem[i]);
   printf("\n[ESTAT] gemx");
   for(int i = 8; i < EP_GEM_N; i++) printf(" %s %lu", ep_gem_names[i], (unsigned long)eth_prof.gem[i]);
   printf("\n[ESTAT] scsi inv_len_max %lu flush_len_max %lu", (unsigned long)eth_prof.scsi_inv_len_max,
          (unsigned long)eth_prof.scsi_flush_len_max);
   printf("\n[ESTAT] isr_hist_us<50,200,500,1000,2000,>=2000");
   for(int s = 0; s < EP_SRC_N; s++) {
      printf(" %s", ep_src_names[s]);
      for(int b = 0; b < EP_ISR_BUCKETS; b++) printf(" %lu", (unsigned long)eth_prof.isr_hist[s][b]);
   }
   printf("\n[ESTAT] bunch>=%d n %lu unattr %lu longest_max %lu us gap_max %lu us by_longest", EP_BUNCH,
          (unsigned long)eth_prof.bunch_n, (unsigned long)eth_prof.bunch_unattr,
          (unsigned long)(ep_ns(eth_prof.bunch_longest_max) / 1000), (unsigned long)(ep_ns(eth_prof.bunch_gap_max) / 1000));
   for(int s = 0; s < EP_SRC_N; s++) printf(" %s %lu", ep_src_names[s], (unsigned long)eth_prof.bunch_by_longest[s]);
   printf(" by_last");
   for(int s = 0; s < EP_SRC_N; s++) printf(" %s %lu", ep_src_names[s], (unsigned long)eth_prof.bunch_by_last[s]);
   {
      volatile uint8_t *pri = (volatile uint8_t *)0xF8F01400, *tgt = (volatile uint8_t *)0xF8F01800;   /* GIC ICDIPR / ICDIPTR */
      uint32_t nwcfg = XEmacPs_ReadReg(EmacPsInstance.Config.BaseAddress, XEMACPS_NWCFG_OFFSET);
      printf("\n[ESTAT] state boot_mode %d video_mode %d colormode %d fb %lux%lu size %lu audio_ien %d audio_fake %d eth_ien %d"
             " nwcfg %08lx fden %lu rxcsum %lu pauseen %lu gic pri/tgt gem %02x/%02x video %02x/%02x audio %02x/%02x",
             config.boot_mode, (int)vs.video_mode, (int)vs.colormode, (unsigned long)vs.vmode_hsize, (unsigned long)vs.vmode_vsize,
             (unsigned long)vs.framebuffer_size, interrupt_enabled_audio, interrupt_enabled_audio_fake, interrupt_enabled_ethernet,
             (unsigned long)nwcfg, (unsigned long)((nwcfg & XEMACPS_NWCFG_FDEN_MASK) != 0),
             (unsigned long)((nwcfg & XEMACPS_NWCFG_RXCHKSUMEN_MASK) != 0), (unsigned long)((nwcfg & XEMACPS_NWCFG_PAUSEEN_MASK) != 0),
             pri[EMACPS_IRPT_INTR], tgt[EMACPS_IRPT_INTR], pri[INT_INTERRUPT_ID_0], tgt[INT_INTERRUPT_ID_0],
             pri[INT_INTERRUPT_ID_1], tgt[INT_INTERRUPT_ID_1]);
   }
   if(PhyAddr < 32) {
      /* the PHY over MDIO (console context; no ISR uses MDIO). YT8531S 0x11: speed 15:14, duplex 13, resolved 11,
         link 10; 0x15: its RX error counter. BMSR (1) is read twice: latched, then current. */
      static const uint8_t regs[] = { 0, 1, 1, 4, 5, 6, 9, 10, 0x11, 0x15 };
      uint16_t v[sizeof regs];
      for(unsigned i = 0; i < sizeof regs; i++) XEmacPs_PhyRead(&EmacPsInstance, PhyAddr, regs[i], &v[i]);
      printf("\n[ESTAT] phy addr %lu", (unsigned long)PhyAddr);
      for(unsigned i = 0; i < sizeof regs; i++) printf(" r%02x %04x", regs[i], v[i]);
      uint16_t s = v[8], sp = (s >> 14) & 3;
      printf(" | speed %s duplex %s resolved %d link %d rxerr %u | lp %04x aner %04x lp_an %d pdf %d",
             sp == 2 ? "1000" : sp == 1 ? "100" : sp == 0 ? "10" : "?", (s & (1 << 13)) ? "full" : "half",
             (s >> 11) & 1, (s >> 10) & 1, v[9], v[4], v[5], v[5] & 1, (v[5] >> 4) & 1);
   }
   printf("\n[ESTAT] fw FramesTx %lu frames_received %lu frame_serial %u backlog %u fetched %d DeviceErrors %ld frames_dropped %d\n",
          (unsigned long)FramesTx, (unsigned long)frames_received, (unsigned)frame_serial, (unsigned)frames_backlog,
          frames_received_from_backlog, (long)DeviceErrors, frames_dropped);
}
#endif /* ETH_PROF */
