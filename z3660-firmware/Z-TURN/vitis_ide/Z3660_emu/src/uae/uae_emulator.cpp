/*
 * uae_emulator.cc
 *
 *  Created on: 2 ene. 2023
 *      Author: shanshe
 */
#include "uae/types.h"
#include "../main.h"
#include "sysconfig.h"
#include "xgpiops.h"
#include "maccess.h"
#include "memory.h"
#include "../memorymap.h"

LOCAL local;
#ifdef __cplusplus
extern "C" {
#endif
void cpu_emulator_reset_core0(void);
void cpu_set_fc(int fc);
void load_rom(void);
void load_romext(void);
void reset_autoconfig(void);
#ifdef __cplusplus
}
#endif

//cycle exact 68000
extern "C" void m68k_write_memory_32(unsigned int address, unsigned int value);
extern "C" void write_rtg_register(uint16_t zaddr,uint32_t zdata);
extern "C" void write_scsi_register(uint16_t zaddr,uint32_t zdata,int type);
extern "C" uint32_t read_scsi_register(uint16_t zaddr,int type);
extern "C" uint32_t read_rtg_register(uint16_t zaddr);
extern SHARED *shared;
/*
void put_long_ce000 (uaecptr addr, uae_u32 v)
{
   m68k_write_memory_32((unsigned int)addr,(unsigned int)v);
}
void put_word_ce000 (uaecptr addr, uae_u32 v)
{
   m68k_write_memory_16(addr,v);
}
void put_byte_ce000 (uaecptr addr, uae_u32 v)
{
   m68k_write_memory_8(addr,v);
}
*/
uae_u32 do_get_mem_long(uae_u32* a)
{
//    uint8_t* b = (uint8_t*)a;

//    return (*b << 24) | (*(b + 1) << 16) | (*(b + 2) << 8) | (*(b + 3));
   return((uae_u32)memory_get_long((uint32_t)a));
}

uint16_t do_get_mem_word(uint16_t* a)
{
//    uint8_t* b = (uint8_t*)a;

//    return (*b << 8) | (*(b + 1));
   return((uint16_t)memory_get_word((uint32_t)a));
}

uint8_t do_get_mem_byte(uint8_t* a)
{
//    return *a;
   return((uint8_t)memory_get_byte((uint32_t)a));
}

void do_put_mem_long(uae_u32* a, uae_u32 v)
{
//    uint8_t* b = (uint8_t*)a;

//    *b = v >> 24;
//    *(b + 1) = v >> 16;
//    *(b + 2) = v >> 8;
//    *(b + 3) = v;
   memory_put_long((unsigned int)a, (unsigned int)v);
}

void do_put_mem_word(uint16_t* a, uint16_t v)
{
//    uint8_t* b = (uint8_t*)a;

//    *b = v >> 8;
//    *(b + 1) = v;
   memory_put_word((unsigned int)a, (unsigned int)v);
}

void do_put_mem_byte(uint8_t* a, uint8_t v)
{
//    *a = v;
   memory_put_byte((unsigned int)a, (unsigned int)v);
}

#include "options.h"
#include "custom.h"
struct uae_prefs currprefs,changed_prefs;

#define write_log z3660_printf
//void write_log(const TCHAR *format, ...)
//{
//}

#include "uae_emulator.h"
#include "fpp.h"
#include "../main.h"
#include "newcpu.h"

void fill_prefetch_quick(void);
void m68k_reset_newcpu(bool hardreset);
void build_cpufunctbl (void);

bool is_cycle_ce(uaecptr address)
{
   return(false);
}
unsigned int direct_read_32(uaecptr add)
{
   return(swap32(*(uint32_t *)add));
}
unsigned int direct_read_16(uaecptr add)
{
   return(swap16(*(uint16_t *)add));
}
unsigned int direct_read_8(uaecptr add)
{
   return(*(uint8_t *)add);
}
void direct_write_32(uaecptr add, unsigned int data)
{
   *(uint32_t *)add=swap32(data);
}
void direct_write_16(uaecptr add, unsigned int data)
{
   *(uint16_t *)add=swap16(data);
}
void direct_write_8(uaecptr add, unsigned int data)
{
   *(uint8_t *)add=data;
}
extern uint32_t autoConfigBaseFastRam;
extern uint32_t autoConfigBaseRTG;
extern volatile uint8_t *Z3660_RTG_BASE;
extern volatile uint8_t *Z3660_Z3RAM_BASE;
extern uint32_t autoConfigBaseSCSI;
// AMIX A3000 onboard SCSI (WD33C93 + SuperDMAC) emulation, src/uae/a3000_scsi.cpp.
extern "C" uint32_t a3000_scsi_read(uint32_t offset, int size);
extern "C" void     a3000_scsi_write(uint32_t offset, uint32_t data, int size);
extern "C" void     a3000_scsi_init(void);

unsigned int z2scsi_read_32(uaecptr address)
{
   uint32_t add=address-autoConfigBaseSCSI;
   if(add<0x6000)
   {
      if(add<0x2000)
         return(0xFFFFFFFF);
      else
      {
         uint32_t data=read_scsi_register(add-0x2000,2);
//         printf("scsi read32 %08X %08X\n",add,data);
         return(data);
      }
   }
   if(add>=0x80000)
      return(swap32(*(uint32_t*)(Z3660_RTG_BASE+add)));
   else
   {
      uint32_t data=read_scsi_register(add-0x2000,2);
//      printf("scsi read32 %08X %08X\n",add,data);
      return(data);
   }
}
unsigned int z2scsi_read_16(uaecptr address)
{
   uint32_t add=address-autoConfigBaseSCSI;
   if(add<0x6000)
   {
      if(add<0x2000)
         return(0xFFFFFFFF);
      else
      {
         uint32_t data=read_scsi_register(add-0x2000,1);
//         printf("scsi read16 %08X %08X\n",add,data);
         return(data);
      }
   }
   if(add>=0x80000)
      return(swap16(*(uint16_t*)(Z3660_RTG_BASE+add)));
   else
   {
      uint32_t data=read_scsi_register(add-0x2000,1);
//      printf("scsi read16 %08X %08X\n",add,data);
      return(data);
   }
}
unsigned int z2scsi_read_8(uaecptr address)
{
   uint32_t add=address-autoConfigBaseSCSI;
   if(add<0x6000)
   {
      if(add<0x2000)
         return(0xFFFFFFFF);
      else
      {
         uint32_t data=read_scsi_register(add-0x2000,0);
//         printf("scsi read8 %08X %08X\n",add,data);
         return(data);
      }
   }
   if(add>=0x80000)
      return(Z3660_RTG_BASE[add]);
   else
   {
      uint32_t data=read_scsi_register(add-0x2000,0);
//      printf("scsi read8 %08X %08X\n",add,data);
      return(data);
   }
}
void z2scsi_write_32(uaecptr address, unsigned int data)
{
   uint32_t add=address-autoConfigBaseSCSI;
   *(uint32_t *)(Z3660_RTG_BASE+add)=swap32(data);
   if(add<0x6000)
   {
      if(add<0x2000)
         write_rtg_register(add,data);
      else
      {
//   	     printf("scsi write32 %08X %08X\n",add,data);
         write_scsi_register(add-0x2000,data,2);
      }
   }
}

void z2scsi_write_16(uaecptr address, unsigned int data)
{
   uint32_t add=address-autoConfigBaseSCSI;
   *(uint16_t *)(Z3660_RTG_BASE+add)=swap16(data);
   if(add<0x6000)
   {
      if(add<0x2000)
         write_rtg_register(add,data);
      else
      {
//	      printf("scsi write16 %08X %08X\n",add,data);
         write_scsi_register(add-0x2000,data,1);
      }
   }
}
void z2scsi_write_8(uaecptr address, unsigned int data)
{
   uint32_t add=address-autoConfigBaseSCSI;
   Z3660_RTG_BASE[add]=data;
   if(add<0x6000)
   {
      if(add<0x2000)
         write_rtg_register(add,data);
      else
      {
//	      printf("scsi write8 %08X %08X\n",add,data);
         write_scsi_register(add-0x2000,data,0);
      }
   }
}
unsigned int z3ram_read_32(uaecptr address)
{
   uint32_t add=address-autoConfigBaseFastRam;
   return(swap32(*(uint32_t *)(Z3660_Z3RAM_BASE+add)));
}
unsigned int z3ram_read_16(uaecptr address)
{
   uint32_t add=address-autoConfigBaseFastRam;
   return(swap16(*(uint16_t *)(Z3660_Z3RAM_BASE+add)));
}
unsigned int z3ram_read_8(uaecptr address)
{
   uint32_t add=address-autoConfigBaseFastRam;
   return(*(uint8_t *)(Z3660_Z3RAM_BASE+add));
}
void z3ram_write_32(uaecptr address, unsigned int data)
{
   uint32_t add=address-autoConfigBaseFastRam;
   *(uint32_t *)(Z3660_Z3RAM_BASE+add)=swap32(data);
}
void z3ram_write_16(uaecptr address, unsigned int data)
{
   uint32_t add=address-autoConfigBaseFastRam;
   *(uint16_t *)(Z3660_Z3RAM_BASE+add)=swap16(data);
}
void z3ram_write_8(uaecptr address, unsigned int data)
{
   uint32_t add=address-autoConfigBaseFastRam;
   *(uint8_t *)(Z3660_Z3RAM_BASE+add)=data&0xFF;
}
extern "C" uint32_t read_autoconfig_z2(uint32_t address);
extern "C" uint32_t read_autoconfig_z3(uint32_t address);
unsigned int auto_read_32_z2(uaecptr address)
{
//   z3660_printf("[Core1] Autoconfig Z2 BANK: Read32 LONG 0x%08lX\n",address);
#ifdef AUTOCONFIG_ENABLED
   if(((configured_z2|shutup_z2)&local.z2_enabled)!=local.z2_enabled)
      return(read_autoconfig_z2(address));
   else
      return(ps_read_32(address));
#else
   return(ps_read_32(address));
#endif
}
unsigned int auto_read_16_z2(uaecptr address)
{
//   z3660_printf("[Core1] Autoconfig Z2 BANK: Read16 WORD 0x%08lX\n",address);
#ifdef AUTOCONFIG_ENABLED
   if(((configured_z2|shutup_z2)&local.z2_enabled)!=local.z2_enabled)
      return(read_autoconfig_z2(address)>>16);
   else
      return(ps_read_16(address));
#else
   return(ps_read_16(address));
#endif
}
unsigned int auto_read_8_z2(uaecptr address)
{
//   z3660_printf("Autoconfig Z2 BANK: Read8 0x%08lX Z2\n",address);
#ifdef AUTOCONFIG_ENABLED
   if(((configured_z2|shutup_z2)&local.z2_enabled)!=local.z2_enabled)
      return(read_autoconfig_z2(address)>>24);
   else
      return(ps_read_8(address));
#else
   return(ps_read_8(address));
#endif
}
unsigned int auto_read_32_z3(uaecptr address)
{
//   z3660_printf("[Core1] Autoconfig Z3 BANK: Read32 LONG 0x%08lX\n",address);
#ifdef AUTOCONFIG_ENABLED
   if(((configured_z3|shutup_z3)&local.z3_enabled)!=local.z3_enabled)
      return(read_autoconfig_z3(address));
   else
      return(ps_read_32(address));
#else
   return(ps_read_32(address));
#endif
}
unsigned int auto_read_16_z3(uaecptr address)
{
//   z3660_printf("[Core1] Autoconfig Z3 BANK: Read16 WORD 0x%08lX\n",address);
#ifdef AUTOCONFIG_ENABLED
   if(((configured_z3|shutup_z3)&local.z3_enabled)!=local.z3_enabled)
      return(read_autoconfig_z3(address)>>16);
   else
      return(ps_read_16(address));
#else
   return(ps_read_16(address));
#endif
}
unsigned int auto_read_8_z3(uaecptr address)
{
//   z3660_printf("Autoconfig Z3 BANK: Read8 0x%08lX\n",address);
#ifdef AUTOCONFIG_ENABLED
   if(((configured_z3|shutup_z3)&local.z3_enabled)!=local.z3_enabled)
      return(read_autoconfig_z3(address)>>24);
   else
      return(ps_read_8(address));
#else
   return(ps_read_8(address));
#endif
}
extern "C" void write_autoconfig_z2(uint32_t address, uint32_t data);
extern "C" void write_autoconfig_z3(uint32_t address, uint32_t data);
void auto_write_32_z2(uaecptr address, unsigned int data)
{
   z3660_printf("[Core1] Autoconfig Z2 BANK: Write32 0x%08X 0x%08X\n",address,data);
#ifdef AUTOCONFIG_ENABLED
   if(((configured_z2|shutup_z2)&local.z2_enabled)!=local.z2_enabled)
      write_autoconfig_z2(address,data);
   else
      ps_write_32(address,data);
#else
   ps_write_32(address,data);
#endif
}
void auto_write_16_z2(uaecptr address, unsigned int data)
{
   z3660_printf("[Core1] Autoconfig Z2 BANK: Write16 0x%08X 0x%08X\n",address,data);
#ifdef AUTOCONFIG_ENABLED
   if(((configured_z2|shutup_z2)&local.z2_enabled)!=local.z2_enabled)
      write_autoconfig_z2(address,data<<16);
   else
      ps_write_16(address,data);
#else
   ps_write_16(address,data);
#endif
}
void auto_write_8_z2(uaecptr address, unsigned int data)
{
#ifdef AUTOCONFIG_ENABLED
   z3660_printf("[Core1] Autoconfig Z2 BANK: Write8 0x%08X 0x%08X\n",address,data);
   if(((configured_z2|shutup_z2)&local.z2_enabled)!=local.z2_enabled)
      write_autoconfig_z2(address,data<<24);
   else
      ps_write_8(address,data);
#else
   ps_write_8(address,data);
#endif
}
void auto_write_32_z3(uaecptr address, unsigned int data)
{
   z3660_printf("[Core1] Autoconfig Z3 BANK: Write32 0x%08X 0x%08X\n",address,data);
#ifdef AUTOCONFIG_ENABLED
   if(((configured_z3|shutup_z3)&local.z3_enabled)!=local.z3_enabled)
      write_autoconfig_z3(address,data);
   else
      ps_write_32(address,data);
#else
   ps_write_32(address,data);
#endif
}
void auto_write_16_z3(uaecptr address, unsigned int data)
{
   z3660_printf("[Core1] Autoconfig Z3 BANK: Write16 0x%08X 0x%08X\n",address,data);
#ifdef AUTOCONFIG_ENABLED
   if(((configured_z3|shutup_z3)&local.z3_enabled)!=local.z3_enabled)
      write_autoconfig_z3(address,data<<16);
   else
      ps_write_16(address,data);
#else
   ps_write_16(address,data);
#endif
}
void auto_write_8_z3(uaecptr address, unsigned int data)
{
#ifdef AUTOCONFIG_ENABLED
   z3660_printf("[Core1] Autoconfig Z3 BANK: Write8 0x%08X 0x%08X\n",address,data);
   if(((configured_z3|shutup_z3)&local.z3_enabled)!=local.z3_enabled)
      write_autoconfig_z3(address,data<<24);
   else
      ps_write_8(address,data);
#else
   ps_write_8(address,data);
#endif
}
unsigned int rtg_regs_read_32(uaecptr address)
{
   uint32_t add=address-autoConfigBaseRTG;
#define REG_ZZ_VBLANK_STATUS 0x17C
   if(add==REG_ZZ_VBLANK_STATUS)
   {
//         return(video_formatter_read(0));
#define VIDEO_FORMATTER_BASEADDR XPAR_PROCESSING_AV_SYSTEM_AUDIO_VIDEO_ENGINE_VIDEO_VIDEO_FORMATTER_0_BASEADDR
      return(*(uint32_t *)VIDEO_FORMATTER_BASEADDR);
   }
   if(add<0x6000)
   {
      if(add<0x2000)
         return(read_rtg_register(add));
      else
      {
         uint32_t data=read_scsi_register(add-0x2000,2);
//         printf("scsi read32 %08X %08X\n",add,data);
         return(data);
      }
   }
   if(add>=0x80000)
      return(swap32(*(uint32_t*)(Z3660_RTG_BASE+add)));
   else
   {
      uint32_t data=read_scsi_register(add-0x2000,2);
//      printf("scsi read32 %08X %08X\n",add,data);
      return(data);
   }
}
unsigned int rtg_regs_read_16(uaecptr address)
{
   uint32_t add=address-autoConfigBaseRTG;
   if(add<0x6000)
   {
      if(add<0x2000)
         return(read_rtg_register(add));
      else
      {
         uint32_t data=read_scsi_register(add-0x2000,1);
//         printf("scsi read16 %08X %08X\n",add,data);
         return(data);
      }
   }
   if(add>=0x80000)
      return(swap16(*(uint16_t*)(Z3660_RTG_BASE+add)));
   else
   {
      uint32_t data=read_scsi_register(add-0x2000,1);
//      printf("scsi read16 %08X %08X\n",add,data);
      return(data);
   }
}
unsigned int rtg_regs_read_8(uaecptr address)
{
   uint32_t add=address-autoConfigBaseRTG;
   if(add<0x6000)
   {
      if(add<0x2000)
      {
         uint32_t data=read_rtg_register(add&0x1FFFFC);
         switch(add&0x3)
         {
         case 0:
            return((data>>24)&0xFF);
         case 1:
            return((data>>16)&0xFF);
         case 2:
            return((data>>8 )&0xFF);
         case 3:
            return((data    )&0xFF);
         }
      }
      else
      {
         uint32_t data=read_scsi_register(add-0x2000,0);
//         printf("scsi read8 %08X %08X\n",add,data);
         return(data);
      }
   }
   if(add>=0x80000)
      return(Z3660_RTG_BASE[add]);
   else
   {
      uint32_t data=read_scsi_register(add-0x2000,0);
//      printf("scsi read8 %08X %08X\n",add,data);
      return(data);
   }
}
void rtg_regs_write_32(uaecptr address, unsigned int data)
{
   uint32_t add=address-autoConfigBaseRTG;
   *(uint32_t *)(Z3660_RTG_BASE+add)=swap32(data);
   if(add<0x6000)
   {
      if(add<0x2000)
         write_rtg_register(add,data);
      else
      {
//         printf("scsi write32 %08X %08X\n",add,data);
         write_scsi_register(add-0x2000,data,2);
      }
   }
}
void rtg_regs_write_16(uaecptr address, unsigned int data)
{
   uint32_t add=address-autoConfigBaseRTG;
   *(uint16_t *)(Z3660_RTG_BASE+add)=swap16(data);
   if(add<0x6000)
   {
      if(add<0x2000)
         write_rtg_register(add,data);
      else
      {
//         printf("scsi write16 %08X %08X\n",add,data);
         write_scsi_register(add-0x2000,data,1);
      }
   }
}
void rtg_regs_write_8(uaecptr address, unsigned int data)
{
   uint32_t add=address-autoConfigBaseRTG;
   Z3660_RTG_BASE[add]=data;
   if(add<0x6000)
   {
      if(add<0x2000)
         write_rtg_register(add,data<<24);
      else
      {
//         printf("scsi write8 %08X %08X\n",add,data);
         write_scsi_register(add-0x2000,data,0);
      }
   }
}
unsigned int rtg_read_32(uaecptr address)
{
   uint32_t add=address-autoConfigBaseRTG;
   uint32_t data=swap32(*(uint32_t *)(Z3660_RTG_BASE+add));
//   printf("read32 rtg (%08lX) = %08lX\n",add,data);
//   if(add&3)
//      printf("rtg_read32 unaligned to 0x%08X\n",add);
   return(data);
}
unsigned int rtg_read_16(uaecptr address)
{
   uint32_t add=address-autoConfigBaseRTG;
//   if(add&1)
//      printf("rtg_read16 unaligned to 0x%08X\n",add);
   uint32_t data=swap16(*(uint16_t *)(Z3660_RTG_BASE+add));
//   printf("read16 rtg (%08lX) = %08lX\n",add,data);
   return(data);
}
unsigned int rtg_read_8(uaecptr address)
{
   uint32_t add=address-autoConfigBaseRTG;
   uint32_t data=(uint32_t)(Z3660_RTG_BASE[add]&0xFF);
//   printf("read rtg (%08lX) = %08lX\n",add,data);
   return(data);
}
void rtg_write_32(uaecptr address, unsigned int data)
{
   uint32_t add=address-autoConfigBaseRTG;
   *(uint32_t *)(Z3660_RTG_BASE+add)=swap32(data);
//   printf("write32 rtg (%08lX) = %08lX\n",add,data);
/*
   if(add&3)
      printf("rtg_write32 unaligned to 0x%08X\n",add);
   switch(add&3)
   {
   case 0:
      *(uint32_t *)(Z3660_RTG_BASE+add)=swap32(data);
      break;
   case 1:
      Z3660_RTG_BASE[add]=data>>24;
      *(uint16_t *)(Z3660_RTG_BASE+add+1)=swap16(data>>8);
      Z3660_RTG_BASE[add+3]=data;
      break;
   case 2:
      *(uint16_t *)(Z3660_RTG_BASE+add)=swap16(data>>16);
      *(uint16_t *)(Z3660_RTG_BASE+add+2)=swap16(data);
      break;
   case 3:
      Z3660_RTG_BASE[add]=data>>24;
      *(uint16_t *)(Z3660_RTG_BASE+add+1)=swap16(data>>8);
      Z3660_RTG_BASE[add+3]=data;
      break;
   }
*/
}
void rtg_write_16(uaecptr address, unsigned int data)
{
   uint32_t add=address-autoConfigBaseRTG;
//   printf("write16 rtg (%08lX) = %08lX\n",add,data);
//   if(add&1)
//      printf("rtg_write16 unaligned to 0x%08X\n",add);
   *(uint16_t *)(Z3660_RTG_BASE+add)=swap16(data);
}
void rtg_write_8(uaecptr address, unsigned int data)
{
   uint32_t add=address-autoConfigBaseRTG;
//   printf("write8 rtg (%08lX) = %08lX\n",add,data);
   Z3660_RTG_BASE[add]=data&0xFF;
}
unsigned int dummy_read(uaecptr add)
{
   return(0xFFFFFFFF);
}
void dummy_write(uaecptr add, unsigned int data)
{
}
uae_u8 *dummy_xlate(uaecptr add)
{
   return((uae_u8 *)add);
}
int dummy_check(uaecptr add, uae_u32)
{
   return(0);
}
int slow_check(uaecptr add, uae_u32)
{
   return(1);
}
int dflt_check(uaecptr add, uae_u32)
{
   return(0);
}
int auto_check(uaecptr add, uae_u32)
{
   return(1);
}
int mobo_check(uaecptr add, uae_u32)
{
   return(1);
}
int drct_check(uaecptr add, uae_u32)
{
   return(1);
}
int romd_check(uaecptr add, uae_u32)
{
   return(1);
}
int z2scsi_check(uaecptr add, uae_u32)
{
   return(1);
}
int z3ram_check(uaecptr add, uae_u32)
{
   return(1);
}
int rtg_check(uaecptr add, uae_u32)
{
   return(1);
}
#define USE_MEM_BANKS
#ifdef USE_MEM_BANKS
#define MB_READ S_READ
#define MB_WRITE S_WRITE
#else
#define MB_READ 0
#define MB_WRITE 0
#endif
addrbank slow_bank = {
      read_long, read_word, read_byte,
      m68k_write_memory_32, m68k_write_memory_16, m68k_write_memory_8,
      dummy_xlate, slow_check, NULL, NULL, NULL,
      read_long, read_word,
      ABFLAG_NONE, MB_READ, MB_WRITE
};
/*
addrbank prom_bank = {
      prom_read_long, prom_read_word, prom_read_byte,
      prom_m68k_write_memory_32, prom_m68k_write_memory_16, prom_m68k_write_memory_8,
      dummy_xlate, dummy_check, NULL, NULL, NULL,
      prom_read_long, prom_read_word,
      ABFLAG_NONE, MB_READ, MB_WRITE
};
 */
addrbank auto_z2_bank = {
      auto_read_32_z2, auto_read_16_z2, auto_read_8_z2,
      auto_write_32_z2, auto_write_16_z2, auto_write_8_z2,
      dummy_xlate, auto_check, NULL, NULL, NULL,
      auto_read_32_z2, auto_read_16_z2,
      ABFLAG_NONE, MB_READ, MB_WRITE
};
addrbank auto_z3_bank = {
      auto_read_32_z3, auto_read_16_z3, auto_read_8_z3,
      auto_write_32_z3, auto_write_16_z3, auto_write_8_z3,
      dummy_xlate, auto_check, NULL, NULL, NULL,
      auto_read_32_z3, auto_read_16_z3,
      ABFLAG_NONE, MB_READ, MB_WRITE
};
/*
addrbank test_bank = {
      test_read_32, test_read_16, test_read_8,
      test_write_32, test_write_16, test_write_8,
      dummy_xlate, mobo_check, NULL, NULL, NULL,
      test_read_32, test_read_16,
      ABFLAG_IO, MB_READ, MB_WRITE
};
*/
addrbank mobo_bank = {
      ps_read_32, ps_read_16, ps_read_8,
      ps_write_32, ps_write_16, ps_write_8,
      dummy_xlate, mobo_check, NULL, NULL, NULL,
      ps_read_32, ps_read_16,
      ABFLAG_IO, MB_READ, MB_WRITE
};
addrbank chpr_bank = {
      ps_read_32, ps_read_16, ps_read_8,
      ps_write_32, ps_write_16, ps_write_8,
      dummy_xlate, mobo_check, NULL, NULL, NULL,
      ps_read_32, ps_read_16,
      ABFLAG_RAM | ABFLAG_CHIPRAM, MB_READ, MB_WRITE
};
addrbank mbrm_bank = {
      ps_read_32, ps_read_16, ps_read_8,
      ps_write_32, ps_write_16, ps_write_8,
      dummy_xlate, mobo_check, NULL, NULL, NULL,
      ps_read_32, ps_read_16,
      ABFLAG_RAM, MB_READ, MB_WRITE
};
addrbank drct_bank = {
      direct_read_32, direct_read_16, direct_read_8,
      direct_write_32, direct_write_16, direct_write_8,
      dummy_xlate, drct_check, NULL, NULL, NULL,
      direct_read_32, direct_read_16,
//      ABFLAG_NONE, MB_READ, MB_WRITE,
      ABFLAG_RAM | ABFLAG_DIRECTACCESS, 0, 0, // <--- Direct memory is faster if it's NOT "special_mem"
      NULL, // sub_banks
      0xFFFFFFFF, //mask
      0, // startmask
      0, // start
      0x08000000, // allocated_size 128 MByte
      0x08000000, // reserved_size 128 MByte
      (uae_u8*)0x08000000, // baseaddr_direct_r
      (uae_u8*)0x08000000, // baseaddr_direct_w
      0x08000000, // startaccessmask
};
addrbank romd_bank = {
      direct_read_32, direct_read_16, direct_read_8,
      dummy_write, dummy_write, dummy_write,
      dummy_xlate, romd_check, NULL, NULL, NULL,
      direct_read_32, direct_read_16,
      ABFLAG_ROM | ABFLAG_DIRECTACCESS, 0, 0, // <--- Direct memory is faster if it's NOT "special_mem"
      NULL, // sub_banks
      0xFFFFFFFF, //mask
      0, // startmask
      0, // start
      0x00080000, // allocated_size 512 KByte
      0x00080000, // reserved_size 512 KByte
      (uae_u8*)0x00F80000, // baseaddr_direct_r
      (uae_u8*)0, // baseaddr_direct_w
      0x00F80000, // startaccessmask
};
addrbank rome_bank = {
      direct_read_32, direct_read_16, direct_read_8,
      dummy_write, dummy_write, dummy_write,
      dummy_xlate, romd_check, NULL, NULL, NULL,
      direct_read_32, direct_read_16,
      ABFLAG_ROM | ABFLAG_DIRECTACCESS, 0, 0, // <--- Direct memory is faster if it's NOT "special_mem"
      NULL, // sub_banks
      0xFFFFFFFF, //mask
      0, // startmask
      0, // start
      0x00080000, // allocated_size 512 KByte
      0x00080000, // reserved_size 512 KByte
      (uae_u8*)0x00F00000, // baseaddr_direct_r
      (uae_u8*)0, // baseaddr_direct_w
      0x00F00000, // startaccessmask
};

addrbank z2scsi_bank = {
      z2scsi_read_32, z2scsi_read_16, z2scsi_read_8,
      z2scsi_write_32, z2scsi_write_16, z2scsi_write_8,
      dummy_xlate, z2scsi_check, NULL, NULL, NULL,
      z2scsi_read_32, z2scsi_read_16,
      ABFLAG_NONE, MB_READ, MB_WRITE
};

// Emulated A3000 mainboard SCSI (WD33C93 + Commodore SuperDMAC) for AMIX boot.
// Fixed motherboard page $00DD0000; handlers pass the page-relative offset to
// a3000_scsi.cpp. ABFLAG_IO: register reads have side effects (SCSI_STATUS read
// clears the WD INT; ISTR read clears bits) so never direct/JIT-bypass them.
#define A3000_SCSI_BASE 0x00DD0000
unsigned int a3000scsi_read_32(uaecptr address) { return a3000_scsi_read(address - A3000_SCSI_BASE, 2); }
unsigned int a3000scsi_read_16(uaecptr address) { return a3000_scsi_read(address - A3000_SCSI_BASE, 1); }
unsigned int a3000scsi_read_8 (uaecptr address) { return a3000_scsi_read(address - A3000_SCSI_BASE, 0); }
void a3000scsi_write_32(uaecptr address, unsigned int data) { a3000_scsi_write(address - A3000_SCSI_BASE, data, 2); }
void a3000scsi_write_16(uaecptr address, unsigned int data) { a3000_scsi_write(address - A3000_SCSI_BASE, data, 1); }
void a3000scsi_write_8 (uaecptr address, unsigned int data) { a3000_scsi_write(address - A3000_SCSI_BASE, data, 0); }
int a3000scsi_check(uaecptr add, uae_u32) { return 1; }
addrbank a3000_scsi_bank = {
      a3000scsi_read_32, a3000scsi_read_16, a3000scsi_read_8,
      a3000scsi_write_32, a3000scsi_write_16, a3000scsi_write_8,
      dummy_xlate, a3000scsi_check, NULL, NULL, NULL,
      a3000scsi_read_32, a3000scsi_read_16,
      ABFLAG_IO, MB_READ, MB_WRITE
};

// AMIX A3000 motherboard fast RAM (a3000mem) at GUEST $07000000 (where a real
// A3000 has its motherboard RAM and where AMIX maps/runs its kernel), backed by
// FREE host DDR at $09000000. Host $07000000 itself is owned by core0 (video
// DECODED buffer), so we can't be 1:1 there; instead get_real_address() maps the
// $07xxxxxx window to host $09xxxxxx for the MMU/direct paths, and these functions
// + a3000mem_xlate do the same for the normal access paths.
// Big-endian-swapped like z3ram so byte-wise DMA stores + longword reads agree.
#define A3000MEM_HOST 0x09000000u
// AMIX a3000mem ($07000000 guest -> host DDR $09000000) byte-swapped like z3ram/drct so byte-wise
// DMA stores + longword reads agree. (Vestigial under the $08000000-DDR boot path; see the RANGE_MAP
// comment in uae_emulator() -- AMIX runs from $08000000, this window is mapped but not in the memlist.)
unsigned int a3000mem_read_32(uaecptr address){ return(swap32(*(uint32_t*)(A3000MEM_HOST+(address-0x07000000)))); }
unsigned int a3000mem_read_16(uaecptr address){ return(swap16(*(uint16_t*)(A3000MEM_HOST+(address-0x07000000)))); }
unsigned int a3000mem_read_8 (uaecptr address){ return(*(uint8_t*)(A3000MEM_HOST+(address-0x07000000))); }
void a3000mem_write_32(uaecptr address, unsigned int data){ *(uint32_t*)(A3000MEM_HOST+(address-0x07000000))=swap32(data); }
void a3000mem_write_16(uaecptr address, unsigned int data){ *(uint16_t*)(A3000MEM_HOST+(address-0x07000000))=swap16(data); }
void a3000mem_write_8 (uaecptr address, unsigned int data){ *(uint8_t*)(A3000MEM_HOST+(address-0x07000000))=data&0xFF; }
int a3000mem_check(uaecptr add, uae_u32){ return(1); }
uae_u8 *a3000mem_xlate(uaecptr add){ return((uae_u8*)(A3000MEM_HOST+(add-0x07000000))); }
// baseaddr = host_base - guest_base = $09000000 - $07000000 = $02000000, so any bulk
// path that does `baseaddr + guest_addr` (e.g. the loader/AmigaOS) lands on host
// $09xxxxxx. (drct has host==guest so its baseaddr is NULL; ours can't be.)
// Full direct-access bank like drct_bank, but mapping guest $07000000 -> host $09000000.
// baseaddr = host-guest offset ($02000000); baseaddr_direct_r/w = host base ($09000000);
// mask = 0xFFFFFFFF; allocated_size = 8MB. This lets a3000mem be the SOLE RAM (the boot's
// executing code needs real direct-access RAM - function-based gave illegal-instruction Gurus).
addrbank a3000mem_bank = {
      a3000mem_read_32, a3000mem_read_16, a3000mem_read_8,
      a3000mem_write_32, a3000mem_write_16, a3000mem_write_8,
      a3000mem_xlate, a3000mem_check, (uae_u8*)0x09000000u, NULL, NULL,   // baseaddr = ABSOLUTE host base $09000000 (WinUAE convention). map_banks (memory.h:515) sets the CPU fast-path ptr baseaddr[idx] = baseaddr - realstart = $09000000 - $07000000 = $02000000, so a direct/exec/RAM-probe access to guest $07xxxxxx = $02000000 + $07xxxxxx = host $09xxxxxx (correct). (NULL forced the slow function path which the A3000 ROM's downward RAM-probe doesn't use; $02000000 offset made baseaddr[] negative -> host $02xxxxxx = core0 firmware.)
      a3000mem_read_32, a3000mem_read_16,
      ABFLAG_RAM | ABFLAG_DIRECTACCESS, 0, 0,   // direct-access: the AMIX kernel executes from this window
      NULL,                      // sub_banks
      0xFFFFFFFF,                // mask
      0,                         // startmask
      0,                         // start
      0x01000000,                // allocated_size = 16MB (was stale 8MB; match AMIX_A3000MEM_MB + the RANGE_MAP)
      0x01000000,                // reserved_size = 16MB
      (uae_u8*)0x09000000u,      // baseaddr_direct_r
      (uae_u8*)0x09000000u,      // baseaddr_direct_w
      0x01000000,                // startaccessmask = 16MB (= allocated_size, like drct_bank)
};

addrbank z3ram_bank = {
      z3ram_read_32, z3ram_read_16, z3ram_read_8,
      z3ram_write_32, z3ram_write_16, z3ram_write_8,
      dummy_xlate, z3ram_check, NULL, NULL, NULL,
      z3ram_read_32, z3ram_read_16,
      ABFLAG_RAM | ABFLAG_DIRECTACCESS, 0, 0, // <--- Direct memory is faster if it's NOT "special_mem"
      NULL, // sub_banks
      0xFFFFFFFF, //mask
      0, // startmask
      0, // start
      0x10000000, // allocated_size 256 MByte
      0x10000000, // reserved_size 256 MByte
      (uae_u8*)0x20000000, // baseaddr_direct_r
      (uae_u8*)0x20000000, // baseaddr_direct_w
      0x20000000, // startaccessmask
};
addrbank rtg_regs_bank = {
      rtg_regs_read_32, rtg_regs_read_16, rtg_regs_read_8,
      rtg_regs_write_32, rtg_regs_write_16, rtg_regs_write_8,
      dummy_xlate, rtg_check, NULL, NULL, NULL,
      rtg_regs_read_32, rtg_regs_read_16,
      ABFLAG_NONE, MB_READ, MB_WRITE
};
addrbank rtg_bank = {
      rtg_read_32, rtg_read_16, rtg_read_8,
      rtg_write_32, rtg_write_16, rtg_write_8,
      dummy_xlate, rtg_check, NULL, NULL, NULL,
      rtg_read_32, rtg_read_16,
//      ABFLAG_NONE, MB_READ, MB_WRITE,
      ABFLAG_RAM | ABFLAG_DIRECTACCESS, 0, 0, // <--- Direct memory is faster if it's NOT "special_mem"
      NULL, // sub_banks
      0xFFFFFFFF, //mask
      0, // startmask
      0, // start
      0x08000000, // allocated_size 128 MByte
      0x08000000, // reserved_size 128 MByte
      (uae_u8*)RTG_BASE, // baseaddr_direct_r
      (uae_u8*)RTG_BASE, // baseaddr_direct_w
      RTG_BASE, // startaccessmask
};
addrbank dmmy_bank = {
      dummy_read, dummy_read, dummy_read,
      dummy_write, dummy_write, dummy_write,
      dummy_xlate, dummy_check, NULL, NULL, NULL,
      dummy_read, dummy_read,
      ABFLAG_NONE, MB_READ, MB_WRITE
};
addrbank dflt_bank = {
      read_long, read_word, read_byte,
      m68k_write_memory_32, m68k_write_memory_16, m68k_write_memory_8,
      dummy_xlate, dflt_check, NULL, NULL, NULL,
      read_long, read_word,
      ABFLAG_NONE, MB_READ, MB_WRITE
};
#define RANGE_MAP(A,B,bank) do{for(unsigned int i=(A);i<(B);i++)\
      mem_banks[bankindex(i << 16)]=&bank;}while(0)
extern "C" void init_ovl_chip_ram_bank(void)
{
   RANGE_MAP(0x0000,0x0008,chpr_bank);
}
extern "C" void init_z2_scsi_bank(unsigned int ini)
{
   uint32_t dir=ini;
   RANGE_MAP(dir,dir+0x0001,z2scsi_bank); // 2 MB
}
extern "C" void init_z3_ram_bank(unsigned int ini)
{
   uint32_t dir=ini<<4;
   RANGE_MAP(dir,dir+0x1000,z3ram_bank); // 256 MB Z3 RAM
}
extern "C" void init_rtg_bank(unsigned int ini)
{
   uint32_t dir=ini<<4;
   RANGE_MAP(dir+0x0000,dir+0x0020,rtg_regs_bank); // RTG Registers
   RANGE_MAP(dir+0x0020,dir+0x0800,rtg_bank); // RTG RAM
}
int maxcycles = 64*512; //256*512
extern void finish_Attributes(void);
extern uint32_t MMUTable;
extern uint32_t MMUL2Table[];
extern "C" void make_dummy_address_bank(uint32_t address)
{
   int add=address>>16;
   RANGE_MAP(add,add,dmmy_bank); // dummy
}
void uae_emulator(int enable_jit, int cpu_model, int enable_mmu)
{
   z3660_printf("[Core1] Starting UAE%s_%s%s emulator\n",enable_jit?"JIT":"",cpu_model==68030?"030":"040",enable_mmu?"_MMU":"");
   currprefs.cpu_model              = changed_prefs.cpu_model=cpu_model;
   currprefs.fpu_model              = changed_prefs.fpu_model=cpu_model==68030?68882:68040;
   // UAE_030_MMU: real 68030 PMMU. enable_mmu and JIT are mutually exclusive (the
   // JIT inlines direct pointers and cannot restart on faults); callers pass
   // enable_jit=0 for MMU mode, so cachesize stays 0 below. cpu_compatible stays
   // false (AMIX kernel-panics with "More Compatible" on).
   currprefs.mmu_model              = changed_prefs.mmu_model=enable_mmu?cpu_model:0;
   currprefs.cpu_compatible         = changed_prefs.cpu_compatible=false;
   currprefs.address_space_24       = changed_prefs.address_space_24=false;
   currprefs.cpu_cycle_exact        = changed_prefs.cpu_cycle_exact=false;
   currprefs.cpu_memory_cycle_exact = changed_prefs.cpu_memory_cycle_exact=false;
   currprefs.int_no_unimplemented   = changed_prefs.int_no_unimplemented=false;
   // 68881/68882 implement ALL FPU opcodes (incl transcendentals) in hardware -> fpu_no_unimplemented=1
   // so they are NOT treated as 68040/060-style unimplemented (which fires Line-F). Matches Amiberry's
   // AmigaUnix.uae (fpu_no_unimplemented=true for its 68882). Keep 0 for the 68040 (AmigaOS emulates).
   currprefs.fpu_no_unimplemented   = changed_prefs.fpu_no_unimplemented=(changed_prefs.fpu_model==68881 || changed_prefs.fpu_model==68882);
   currprefs.crash_auto_reset       = changed_prefs.crash_auto_reset=true;
//   currprefs.blitter_cycle_exact    = changed_prefs.blitter_cycle_exact=false;
   currprefs.m68k_speed             = changed_prefs.m68k_speed=-1;//M68K_SPEED_25MHZ_CYCLES;
   currprefs.comptrustbyte          = changed_prefs.comptrustbyte=1;
   if(enable_jit)
   {
      currprefs.cachesize              = changed_prefs.cachesize=32*1024;
      currprefs.compfpu                = changed_prefs.compfpu=true;
   }
   else
   {
      currprefs.cachesize              = changed_prefs.cachesize=0;
      currprefs.compfpu                = changed_prefs.compfpu=false;
   }
   currprefs.fpu_strict             = changed_prefs.fpu_strict=true;

   regs.natmem_offset=(uae_u8*)0;//0x08000000;
   for(int i=0;i<MEMORY_BANKS;i++) // zero all memory banks vars
   {
      memset(&mem_banks[i],0,sizeof(addrbank));
   }
   RANGE_MAP(0x0000,MEMORY_BANKS,dflt_bank); // default all memory space
   if(local.load_rom_emu==1)
   {
      RANGE_MAP(0x0000,0x0008,slow_bank); // Slow bank ( ovl=1 -> ROM )
   }
   else
   {
      RANGE_MAP(0x0000,0x0008,chpr_bank); // Mother Board bank ( ovl=1 -> ROM )
   }
   RANGE_MAP(0x0008,0x00B8,chpr_bank); // Mother Board bank ( Chip RAM and Zorro II Expansion Space )
   RANGE_MAP(0x00BF,0x00C0,slow_bank); // Slow bank ( CIA ports & Timers ) <----- Amiga crashes with mobo_bank
   RANGE_MAP(0x00DC,0x00DD,mobo_bank); // Mother Board bank ( RTC )
   // AMIX (UAE_030_MMU) only: intercept $00DD0000 with the emulated A3000 SCSI
   // (WD33C93+SuperDMAC). Other emulator modes keep slow_bank so a normal
   // A3000-Kickstart AmigaOS boot is unaffected (its scsi.device sees no change).
   if(enable_mmu)
      RANGE_MAP(0x00DD,0x00DE,a3000_scsi_bank); // emulated A3000 SCSI @ $00DD0000
   else
      RANGE_MAP(0x00DD,0x00DE,slow_bank);
   RANGE_MAP(0x00DE,0x00E0,mobo_bank); // Mother Board bank ( Mobo Resources & Custom chips)
   RANGE_MAP(0x00E8,0x00E9,auto_z2_bank); // Z2 Autoconfig bank ( Zorro II AutoConfig )
   RANGE_MAP(0x00E9,0x00EA,mobo_bank); // Mother Board bank ( Zorro II Autoconfig )
//   RANGE_MAP(0x00EA,0x00EB,test_bank); // Z2 Autoconfig bank ( Zorro II AutoConfig )
   RANGE_MAP(0x00EA,0x00F0,mobo_bank); // Mother Board bank ( Zorro II Autoconfig )
   if(local.load_rom_emu==1)
   {
      for(int i=128;i<256;i++)
         MMUL2Table[i]=(0x00F00000+(i<<12))|0x5BA;
      RANGE_MAP(0x00F8,0x0100,romd_bank); // ROM direct bank ( ROM mapped )
   }
   else
   {
      for(int i=128;i<256;i++)
         MMUL2Table[i]=0;
      RANGE_MAP(0x00F8,0x0100,slow_bank);//mobo_bank); // Mother Board bank ( Mobo ROM )
   }
   if(local.load_romext_emu==1)
   {
      for(int i=0;i<128;i++)
         MMUL2Table[i]=(0x00F00000+(i<<12))|0x5BA;
      RANGE_MAP(0x00F0,0x00F8,rome_bank); // ROM direct bank ( ROM mapped )
   }
   else
   {
      for(int i=0;i<128;i++)
         MMUL2Table[i]=0;
      RANGE_MAP(0x00F0,0x00F8,slow_bank);//mobo_bank); // Mother Board bank ( Mobo ROM )
   }
   // AMIX (UAE_030_MMU): confirmed from the AMIX kernel source (sys/immu.h + amiga/kernel/
   // support.c). The m68k kernel maps physical main RAM into ONE section, SCN1 = kernel VA
   // 0x40000000-0x7FFFFFFF. config() sizes that single window [MAINSTORE, MAINSTORE+VSIZOFMEM)
   // from the AmigaOS memory list (copied verbatim into bootinfo.memory[]); its QUICK_KLUDGE
   // forces MAINSTORE=$07000000, VSIZOFMEM=16MB (window [$07000000,$08000000)) for a high-
   // loaded kernel. EVERY RAM region AmigaOS lists must lie INSIDE that one window, else the
   // kernel adds the stray pages to its pool, touches one outside SCN1, and panics
   // vatosde() ("address not in SCN1"). Two adjacent banks ($07+$08) COALESCE into a >16MB
   // window (also fatal); a split leaves a region outside SCN1 (fatal). So AMIX must see exactly
   // ONE <=16MB Fast-RAM window in its memlist -- we give it 16MB of DDR CPU RAM @ $08000000 (the
   // per-bank map below). Result: AMIX runs on fast Zynq DDR, ~3.4x faster than the mobo SIMMs.
#define AMIX_A3000MEM_MB 16
   if(enable_mmu)
   {
      (void)((AMIX_A3000MEM_MB * 1024 * 1024) >> 16);            // (a3000mem page count)
      // AMIX on Zynq-local DDR (NOT the slow motherboard SIMMs) -- per-bank map:
      //  $0100-$0700 slow_bank : real bus; no RAM lives below $07000000 on the A4000 (unchanged from PATH B).
      //  $0700-$0800 a3000mem  : 16MB DDR (host $09000000). VESTIGIAL -- NOT in the AmigaOS memlist (the
      //                          Kickstart's mobo-RAM detection never CPU-probes it; proven via the now-removed
      //                          [A3KMEM] diag = 0 hits even function-path). AMIX never uses it; left mapped for safety.
      //  $0800-$0900 drct_bank : 16MB DDR CPU RAM (host 1:1 $08000000) = AMIX's ACTUAL main RAM. The ext-kickstart/
      //                          autoconfig adds $08000000 to the AmigaOS memlist, the AMIX loader AllocMem(MEMF_FAST)s
      //                          the kernel there, and THIS kernel brings up its 2KB-page MMU IN PLACE at $08000000
      //                          (serial "MMU enabled ... PC=08000fe6") and RUNS there -- it does NOT relocate to $07000000.
      //  $0900-$1000 dmmy_bank : nothing above, so the memlist holds exactly one 16MB window (no >16MB SCN1 coalesce).
      // NOTE: AMIX_HIDE08 (mmu_common.h) is DISABLED -- it was built for an older kernel that relocated to $07000000;
      //       hiding $08000000 under THIS in-place kernel would yank its own running code -> reboot loop.
      RANGE_MAP(0x0100, 0x0700, slow_bank);                       // real bus, $01000000-$06FFFFFF (no RAM here)
      RANGE_MAP(0x0700, 0x0800, a3000mem_bank);                   // 16MB DDR AMIX main RAM @ guest $07000000 -> host $09000000
      RANGE_MAP(0x0800, 0x0900, drct_bank);                       // 16MB DDR CPU RAM @ $08000000 (loader memlist scratch; hidden post-MMU)
      RANGE_MAP(0x0900, 0x1000, dmmy_bank);                       // nothing above $09000000
   }
   else
   {
      RANGE_MAP(0x0100,0x0800,slow_bank); // Mother Board bank ( Mother board RAM )
      RANGE_MAP(0x0800,0x1000,drct_bank); // Direct bank ( CPU RAM, full 128 MB )
   }
   RANGE_MAP(0x1000,0x1800,dmmy_bank); // dummy
//   RANGE_MAP(0x1000,0x1800,dflt_bank); // Direct bank ( extended CPU RAM )
   RANGE_MAP(0x1800,0x4000,dmmy_bank); // dummy
   RANGE_MAP(0x4000,0x8000,slow_bank); // Slow bank ( Z3 Expansion space )
   RANGE_MAP(0xFF00,0xFF01,auto_z3_bank); // Z3 Autoconfig bank ( Z3660 and Z3 AutoConfig )
   RANGE_MAP(0xFFFF,0xFFFF,dmmy_bank); // dummy
   uint32_t *ptr;
   ptr = &MMUTable;
   ptr[0x00F]=((uint32_t)&MMUL2Table)|0x1E1;

   finish_Attributes();
#ifdef SHOW_MMU_TABLES
   {
      printf("Core 1 MMUTable 0x%08lX\n",(uint32_t)(&MMUTable));
      for(uint32_t i=0;i<0x100;i++)
      {
         printf("0x%03lX ",i*8);
         for(uint32_t j=0;j<8;j++)
         {
            uint32_t data=*((uint32_t *)(&MMUTable+(i*8+j)));
            printf("%08lX ",data);
         }
         printf("\n");
      }
      printf("\n");
      printf("Core 1 MMUL2Table 0x%08lX\n",(uint32_t)(&MMUL2Table));
      for(uint32_t i=0;i<0x20;i++)
      {
         printf("0x%03lX ",i*8);
         for(uint32_t j=0;j<8;j++)
         {
            uint32_t data=*((uint32_t *)((&MMUL2Table[0])+(i*8+j)));
            printf("%08lX ",data);
         }
         printf("\n");
      }
   }
#endif

   m68k_reset_newcpu(1);
   reset_autoconfig();
   if(enable_mmu)
   {
      memset((void*)A3000MEM_HOST, 0, AMIX_A3000MEM_MB * 1024 * 1024); // clear a3000mem host backing ($09000000)
      a3000_scsi_init(); // AMIX: reset emulated A3000 WD33C93+SuperDMAC state
   }

   init_m68k();
   build_cpufunctbl();
   m68k_setpc_normal (regs.pc);
   doint();
   fill_prefetch_quick();
   set_cycles (start_cycles);
   regs.stopped=false;
   set_special(0); //

   while(1)
   {
      m68k_go(1);
   }

}
extern SHARED *shared;
int jit_enabled_last=-1;
void print_histogram_dataabort(void);

void z3660_tasks(void)
{
   static long int count=10000000;
   if(--count>0) return;
   count=10000000;
   if(shared->printhist_dataabort)
   {
      shared->printhist_dataabort=0;
      print_histogram_dataabort();
   }

   int jit_enabled=shared->jit_enabled;
   if(jit_enabled!=jit_enabled_last)
   {
      jit_enabled_last=jit_enabled;
      if(jit_enabled)
      {
         printf("[Core1] JIT enabled\n");
         currprefs.cachesize = changed_prefs.cachesize=32*1024;
         currprefs.compfpu   = changed_prefs.compfpu=true;
      }
      else
      {
         printf("[Core1] JIT disabled\n");
         currprefs.cachesize = changed_prefs.cachesize=0;
         currprefs.compfpu   = changed_prefs.compfpu=false;
      }
      set_special(SPCFLAG_MODE_CHANGE);
   }
}
