/* Host harness shadow of Xilinx <sleep.h>. On host we just use libc usleep.
 * newcpu.cpp renames usleep->usleep2 around its sleep.h include and then calls
 * usleep2() (backed in cpu_emulator.cpp on the firmware). The harness excludes
 * cpu_emulator.cpp, so declare usleep2 here and back it in board_stubs.cpp. */
#ifndef HOST_STUB_SLEEP_H
#define HOST_STUB_SLEEP_H
#include <unistd.h>
#ifdef __cplusplus
extern "C"
#endif
void usleep2(unsigned long useconds);
#endif
