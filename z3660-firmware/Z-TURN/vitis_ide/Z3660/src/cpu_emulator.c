/*
 * cpu_emulator.c
 *
 *  Created on: 3 dic. 2022
 *      Author: shanshe
 *
 *  Just a simple wrapper for Musashi
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "main.h"
#include "cpu_emulator.h"
#include "config_file.h"
#include "rom_window.h"
#include "xscuwdt.h"
#include "scsi/scsi.h"
#include "floppy/floppy.h"
#include "core0_stats.h"

void write_rtg_register(uint16_t zaddr,uint32_t zdata);
uint32_t read_rtg_register(uint16_t zaddr);
void reset_run(int cpu_boot_mode,int counter, int counter_max,int long_int);
void reset_init(void);

SHARED *shared;
void init_shared(void)
{
   shared=(SHARED *)0xFFFF0000;
}

void other_tasks(void);
extern CONFIG config;
extern ENV_FILE_VARS env_file_vars_temp[9]; // really size 8
//extern int bm,sb,ar,cr,ks,ext_ks,*scsi_num;

int load_rom(void)
{
   //#define DUMP_ROM
#define MAP_ROM
#ifdef MAP_ROM
   static FIL fil;      /* File object */
   static FATFS fatfs;
   uint8_t *ROM=(uint8_t *)shared->load_rom_addr;
   TCHAR *Path = DEFAULT_ROOT;
   Xil_ExceptionDisable();
   int ret;

   retry:
   ret=f_clk_mount(&fatfs, Path, 1); // 1 mount immediately
   if(ret!=0)
   {
      printf("Error opening SD media\nRetry in 5 seconds\n");
      usleep(5000000);
      goto retry;
   }
   char *kickstart_pointer=0;
   switch(config.kickstart)
   {
   case 0:
      f_umount(Path);
      return(2);
      break;
#define CASE_KICKSTART(x) case x:\
      kickstart_pointer=&config.kickstart ## x[0];\
      break;
      CASE_KICKSTART(1)
      CASE_KICKSTART(2)
      CASE_KICKSTART(3)
      CASE_KICKSTART(4)
      CASE_KICKSTART(5)
      CASE_KICKSTART(6)
      CASE_KICKSTART(7)
      CASE_KICKSTART(8)
      CASE_KICKSTART(9)
   default:
      printf("Error: index %d -> kickstart is not loaded\n",config.kickstart);
      f_umount(Path);
      return(2);
   }
   if(config.kickstart!=0)
   {
      ret=f_open(&fil,kickstart_pointer, FA_OPEN_EXISTING | FA_READ);
      if(ret!=0)
      {
         printf("Error opening file \"%s\"\n",kickstart_pointer);
         f_umount(Path);
         return(2);
      }
      unsigned int NumBytesRead;
      int total_bytes_to_read=512*1024; // 512 KB
      total_bytes_to_read=f_size(&fil);
      /* Same bound as the extended window: the loop below is sized by the file,
       * so anything over 512 KB would run off the end of the MAPROM window. */
      ROM_LOAD_VERDICT verdict=rom_window_verdict(total_bytes_to_read,total_bytes_to_read);
      if(verdict!=ROM_LOAD_OK)
      {
         printf("Error: kickstart \"%s\": %s -> not mapped\n",
               kickstart_pointer,rom_load_verdict_name(verdict));
         f_close(&fil);
         f_umount(Path);
         Xil_ExceptionEnable();
         return(2);
      }
      if((total_bytes_to_read==512*1024) || (total_bytes_to_read==256*1024))
      {
         printf("ROM filesize %d bytes\n",total_bytes_to_read);
      }
      else
      {
         printf("ROM filesize %d is NOT 256KB or 512KB\n",total_bytes_to_read);
      }
      /* A ROM shorter than the window leaves the tail visible to the reset
       * vector fetch and the RomTagScanner; zero it rather than expose DDR. */
      memset(ROM,0,ROM_WINDOW_SIZE);
      printf("Loading Kickstart at address: 0x%08lX length %d KB\n",(uint32_t)ROM,total_bytes_to_read/1024);
      printf("Reading %s file:\n[----------------]\n\033[F",kickstart_pointer);
#define BYTES_TO_READ 128
      for(int i=0,j=0,k=2;i<total_bytes_to_read;i+=BYTES_TO_READ,j+=BYTES_TO_READ)
      {
         if(j==(total_bytes_to_read/16))
         {
            j=0;
            printf("%.*s\n\033[F",(int)++k,"[================]");
         }
         f_read(&fil, ROM+i, BYTES_TO_READ,&NumBytesRead);
         if(NumBytesRead!=BYTES_TO_READ)
         {
            printf("\nError reading at file offset 0x%08x\nHALT!!!",i);
            f_umount(Path);
            return(2);
         }
      }
      f_close(&fil);
      printf("\nFile read OK\n");
   }
   else
   {
      f_umount(Path);
      return(2);
   }
   Xil_L1DCacheFlush();
   Xil_L2CacheFlush();
   f_umount(Path);
   //   while(1);
#endif
#ifdef DUMP_ROM
   char Filename[]=DEFAULT_ROOT "DiagROM11.rom";
   static FIL fil;      /* File object */
   static FATFS fatfs;
   TCHAR *Path = DEFAULT_ROOT;
   f_clk_mount(&fatfs, Path, 1); // 1 mount immediately
   //   f_open(&fil,Filename, FA_OPEN_ALWAYS | FA_READ);
   f_open(&fil,Filename, FA_OPEN_ALWAYS | FA_WRITE);
   //   f_lseek(&fil, 4);
   //   UINT NumBytesRead;
   UINT NumBytesWritten;
   printf("Dumping %s file:\n[----------]\n\033[F",Filename);
   for(int i=0,j=0,k=0;i<0x80000;i+=4,j+=4)
   {
      if(j==512*1024/10)
      {
         j=0;
         printf("%.*s\n\033[F",(int)++k,"[==========]");
      }
      uint32_t data=arm_read_amiga_map(i,LONG);
      uint8_t buff[4];
      buff[0]=(data>>24)&0xFF;
      buff[1]=(data>>16)&0xFF;
      buff[2]=(data>> 8)&0xFF;
      buff[3]=(data    )&0xFF;
      f_write(&fil, buff, 4,&NumBytesWritten);
   }
   f_close(&fil);
   f_umount(Path);
   printf("\nFile written OK\n");
   while(1);
#endif
#ifdef SERIAL_DUMP_ROM
   for(int i=0x00F80000-0x00F80000;i<0x01000000-0x00F80000;)
   {
      printf("%08lX: ",i);
      for(int j=0;j<8;j++,i+=4)
         printf("%08lX ",arm_read_amiga_map(i,LONG));
      printf("\n");
   }
   while(1);
#endif
   Xil_ExceptionEnable();
   return(1);
}
void dumphex(const char *label, const uint8_t *data, size_t size)
{
   printf("%s:\n", label);
   for (size_t i = 0; i < size; i++) {
      if (i % 16 == 0)
         printf("%08zx: ", i);
      printf("%02X ", data[i]);
      if ((i + 1) % 16 == 0 || i + 1 == size)
         printf("\n");
   }
}
int load_romext(void)
{
   //#define DUMP_ROMEXT
#define MAP_ROMEXT
#ifdef MAP_ROMEXT
   static FIL fil;      /* File object */
   static FATFS fatfs;
   uint8_t *EXT_ROM=(uint8_t *)shared->load_ext_rom_addr;
   TCHAR *Path = DEFAULT_ROOT;
   Xil_ExceptionDisable();
   int ret;

   retry:
   ret=f_clk_mount(&fatfs, Path, 1); // 1 mount immediately
   if(ret!=0)
   {
      printf("Error opening SD media\nRetry in 5 seconds\n");
      usleep(5000000);
      goto retry;
   }
   char *ext_kickstart_pointer=0;
   switch(config.ext_kickstart)
   {
   case 0:
      f_umount(Path);
      return(2);
      break;
#define CASE_EXT_KICKSTART(x) case x:\
      ext_kickstart_pointer=&config.ext_kickstart ## x[0];\
      break;
      CASE_EXT_KICKSTART(1)
      CASE_EXT_KICKSTART(2)
      CASE_EXT_KICKSTART(3)
      CASE_EXT_KICKSTART(4)
      CASE_EXT_KICKSTART(5)
      CASE_EXT_KICKSTART(6)
      CASE_EXT_KICKSTART(7)
      CASE_EXT_KICKSTART(8)
      CASE_EXT_KICKSTART(9)
   default:
      printf("Error: index %d -> extended kickstart is not loaded\n",config.kickstart);
      f_umount(Path);
      return(2);
   }
   if(config.ext_kickstart!=0)
   {
      ret=f_open(&fil,ext_kickstart_pointer, FA_OPEN_EXISTING | FA_READ);
      if(ret!=0)
      {
         printf("Error opening file \"%s\"\n",ext_kickstart_pointer);
         f_umount(Path);
         return(2);
      }
      printf("Loading Extended Kickstart at address: 0x%08lX\n",(uint32_t)EXT_ROM);
      unsigned int NumBytesRead=0;
      unsigned int size=fil.obj.objsize;
      printf("Extended Kickstart file size: 0x%08lX\n",(uint32_t)size);
      /* Bound BEFORE reading: the read below is sized by the file, not by the
       * window, so an oversized image would overrun $00F00000+512KB. */
      ROM_LOAD_VERDICT verdict=rom_window_verdict(size,size);
      if(verdict!=ROM_LOAD_OK)
      {
         printf("Error: extended kickstart \"%s\": %s -> not mapped\n",
               ext_kickstart_pointer,rom_load_verdict_name(verdict));
         f_close(&fil);
         f_umount(Path);
         Xil_ExceptionEnable();
         return(2);
      }
      /* Kickstart SCANS the whole window for $4AFC resident matchwords, so the
       * tail behind a short image must not be left as uninitialised DDR. */
      memset(EXT_ROM,0,ROM_WINDOW_SIZE);
      f_read(&fil, EXT_ROM, size,&NumBytesRead);
      f_close(&fil);
      verdict=ext_rom_verdict(size,NumBytesRead,((unsigned int)EXT_ROM[0]<<8)|EXT_ROM[1]);
      if(verdict!=ROM_LOAD_OK)
      {
         printf("Error: extended kickstart \"%s\": %s -> not mapped\n",
               ext_kickstart_pointer,rom_load_verdict_name(verdict));
         memset(EXT_ROM,0,ROM_WINDOW_SIZE);
         f_umount(Path);
         Xil_ExceptionEnable();
         return(2);
      }
      printf("File read OK\n");
   }
   Xil_L1DCacheFlush();
   Xil_L2CacheFlush();
   f_umount(Path);
//   while(1);
#endif
   Xil_ExceptionEnable();
   return(1);
}
int last_irq=-1;
#define IRQ_FIFO_MAX 100
int irq_fifo[IRQ_FIFO_MAX];
unsigned int irq_rd=0;
unsigned int irq_wr=0;
void cpu_emulator_reset(void)
{
//   mux.nbr_arm=0;
   CPLD_RESET_ARM(0);
//   usleep(1000);
//   CPLD_RESET_ARM(1);
//   printf("Resetting...\n");
}

void hard_reboot(void)
{
   usleep(10000);
   //   cpu_emulator_reset();
   CPLD_RESET_ARM(0);
#define PS_RST_CTRL_REG  (XPS_SYS_CTRL_BASEADDR + 0x200)
#define PS_RST_MASK      0x1  /**< PS software reset */
   Xil_Out32(PS_RST_CTRL_REG, PS_RST_MASK);
   usleep(100);
   Xil_Out32(PS_RST_CTRL_REG, PS_RST_MASK);
   XScuWdt_Config *config=XScuWdt_LookupConfig(XPAR_SCUWDT_0_DEVICE_ID);
   XScuWdt instance;
   XScuWdt_CfgInitialize(&instance,config,config->BaseAddr);
   XScuWdt_LoadWdt(&instance,0xFF);
   XScuWdt_Start(&instance);
   XScuWdt_SetWdMode(&instance);
   while(1);
}
extern int no_init;
#include "pt/pt.h"
int emulator_thread(struct pt *pt)
{
   PT_BEGIN(pt);

   while(1)
   {
      PT_WAIT_UNTIL(pt,shared->shared_data==1);
      shared->shared_data=0;
      /* Nested C0STAT spans: the enclosing C0T_EMULATOR stamp in main.c cannot
       * tell an RTG blit from an SD transfer, and the whole point of this
       * instrumentation is to name the offender. Each span brackets only the
       * handler call, so the flag/dsb bookkeeping stays with the parent. */
      if(shared->write_rtg==1)
      {
         uint32_t addr=shared->write_rtg_addr;
         uint32_t data=shared->write_rtg_data;
         C0_SPAN(sp);
         write_rtg_register(addr,data);
         C0_SPAN_END(sp,C0T_EMU_RTG_WR);
         dsb();
         shared->write_rtg=0;
      }
      else if(shared->read_rtg==1)
      {
         uint32_t addr=shared->read_rtg_addr;
         uint32_t data;
         C0_SPAN(sp);
         data=read_rtg_register(addr);
         C0_SPAN_END(sp,C0T_EMU_RTG_RD);
         shared->read_rtg_data=data;
         dsb();
         shared->read_rtg=0;
      }
      else if(shared->write_scsi==1)
      {
         int type=shared->write_scsi_type;
         uint32_t addr=shared->write_scsi_addr;
         uint32_t data=shared->write_scsi_data;
         C0_SPAN(sp);
         handle_piscsi_reg_write(addr,data,type);
         C0_SPAN_END(sp,C0T_EMU_SCSI_WR);
         dsb();
         shared->write_scsi=0;
//         shared->write_scsi_in_progress=1;
//         handle_piscsi_write(addr,data,type);
//         shared->write_scsi_in_progress=0;
//         shared->write_scsi=0;
      }
      else if(shared->read_scsi==1)
      {
         int type=shared->read_scsi_type;
         uint32_t addr=shared->read_scsi_addr;
         C0_SPAN(sp);
         shared->read_scsi_data=handle_piscsi_read(addr,type);
         C0_SPAN_END(sp,C0T_EMU_SCSI_RD);
         dsb();
         shared->read_scsi=0;
      }
      else if(shared->write_floppy==1)
      {
         int type=shared->write_floppy_type;
         uint32_t addr=shared->write_floppy_addr;
         uint32_t data=shared->write_floppy_data;
         C0_SPAN(sp);
         handle_pifloppy_reg_write(addr,data,type);
         C0_SPAN_END(sp,C0T_EMU_FLOP_WR);
         dsb();
         shared->write_floppy=0;
      }
      else if(shared->read_floppy==1)
      {
         int type=shared->read_floppy_type;
         uint32_t addr=shared->read_floppy_addr;
         C0_SPAN(sp);
         shared->read_floppy_data=handle_pifloppy_read(addr,type);
         C0_SPAN_END(sp,C0T_EMU_FLOP_RD);
         dsb();
         shared->read_floppy=0;
      }
      else if(shared->reset_emulator==1)
      {
         C0_SPAN(sp);
         cpu_emulator_reset();
         C0_SPAN_END(sp,C0T_EMU_RSTE);
         dsb();
         shared->reset_emulator=0;
      }
      else if(shared->core0_hold==1)
      {
         C0_SPAN(sp);
         shared->core0_hold_ack=1;
         do
         {
            usleep(1000);
         }while(shared->core0_hold==1);
         C0_SPAN_END(sp,C0T_EMU_HOLD);
         dsb();
         shared->core0_hold_ack=0;
      }
   }
   PT_END(pt);
}
extern volatile int read_reset;
int emulator_reset_thread(struct pt *pt)
{
   PT_BEGIN(pt);
   while(1)
   {
//      PT_WAIT_UNTIL(pt,read_reset);
//      PT_WAIT_UNTIL(pt,XGpioPs_ReadPin(&GpioPs, n040RSTI)==0);
      PT_WAIT_UNTIL(pt,(((*(volatile uint32_t*)(XPAR_PS7_GPIO_0_BASEADDR+XGPIOPS_DATA_RO_OFFSET))>>(n040RSTI))&1)==0);
      {
//         piscsi_shutdown();
         printf("[Core0] Reset active (DOWN)...\n");
         reset_init();
         CPLD_RESET_ARM(1);
         int reset_time_counter=0;
         int reset_time_counter_max=60*4; // 4 seconds
         int long_reset=0;
         while(XGpioPs_ReadPin(&GpioPs, n040RSTI)==0)
         {
            reset_run(env_file_vars_temp[preset_selected].boot_mode,reset_time_counter,reset_time_counter_max,long_reset);
            reset_time_counter++;
            if(reset_time_counter==60) // 60 -> 1 sec
               no_init=1;
            if(reset_time_counter==reset_time_counter_max) // 60 -> reset_run waits for vblank;
            {
               //               no_init=1;
               if(long_reset==0)
               {
                  long_reset=1;
                  reset_time_counter=0;
                  reset_time_counter_max=60*4; // 4 seconds
                  env_file_vars_temp[preset_selected].boot_mode++;
                  if(env_file_vars_temp[preset_selected].boot_mode>=BOOTMODE_NUM)
                     env_file_vars_temp[preset_selected].boot_mode=0;
                  write_env_files(&env_file_vars_temp[preset_selected]);
                  for(int i=0;i<env_file_vars_temp[preset_selected].boot_mode+1;i++)
                  {
                     DiscreteSet(REG0,FPGA_BP);
                     usleep(10000);
                     DiscreteClear(REG0,FPGA_BP);
                     usleep(100000);
                  }
               }
               else // long_reset==1
               {
                  //delete_env_files();
                  remove_preset_file();
                  for(int i=0;i<5+1;i++)
                  {
                     DiscreteSet(REG0,FPGA_BP); // beep
                     usleep(10000);
                     DiscreteClear(REG0,FPGA_BP);
                     usleep(100000);
                  }
                  DiscreteSet(REG0,FPGA_BP); // beep
                  usleep(30000);
                  DiscreteClear(REG0,FPGA_BP);
                  hard_reboot(); //
               }
            }
         }
//         printf("[Core0] Reset inactive (UP)...\n");
//         hard_reboot();
//         piscsi_refresh_drives();
//         printf("[PISCSI] Refreshed drives\n");
         CPLD_RESET_ARM(0);
         video_reset();
         audio_reset();
         usleep(100000);
         ethernet_init();

         /* Re-load the PISCSI boot ROM + re-map the HDFs, exactly as cold boot
          * (main.c piscsi_init()) does. Idempotent (forces piscsi_rom_ptr=NULL,
          * re-mounts the SD, re-opens the HDFs into the static FIL slots) and
          * touches only the SCSI ROM/device tables -- NOT DDR/GIC/FPGA-clocks/
          * ethernet/REG0 latches. Needed so the rebooted guest sees the boot
          * device; the 68k is still held in reset here. */
         piscsi_init();

         /* Clear any pending Z3660-board interrupt left asserted across the guest
          * reset.  amiga_interrupt_set() drives FPGA_INT6 (REG0) HIGH to raise the
          * Amiga's level-6/EXTER line for board events (USB/audio); the bit (and
          * thus INT6) is only deasserted once the Amiga's Z3660 INT6 handler reads
          * the cause and the firmware calls amiga_interrupt_clear().  If any source
          * is still pending when AMIX soft-reboots (e.g. fsck's reboot), FPGA_INT6
          * stays HIGH -- and the freshly-restarted Kickstart has no Z3660 INT6
          * handler installed yet, so it can never clear the source.  The level-6
          * interrupt re-fires forever -> the 68k storms in Kickstart's autovector
          * handler and never progresses (observed stuck at 0x00F81212, the INT
          * handler's adda/rte exit, s=1 msk=2).  Cold boot starts with
          * amiga_interrupts==0 so it never happens.  Force it clear here (also
          * deasserts FPGA_INT6 since the mask goes to 0) before releasing the 68k. */
         amiga_interrupt_clear(0xFFFFFFFF);

         DiscreteSet(REG0, FPGA_RESET);
         usleep(1000);
         CPLD_RESET_ARM(1);
         DiscreteClear(REG0, FPGA_RESET);
//         read_reset=0;

      }
   }
   PT_END(pt);
}

