/* hz.c -- print the system clock-tick rate (HZ) for Dhrystone's -DHZ=.
 * Used by runbench.sh when getconf(1) is unavailable (e.g. SVR4.0 / AMIX).
 * Prints the POSIX _SC_CLK_TCK value, falling back to <sys/param.h> HZ, then 100. */
#include <stdio.h>
#ifdef __unix__
#include <unistd.h>
#endif
#include <unistd.h>
#include <sys/param.h>

int main()
{
    long t = 0;
#ifdef _SC_CLK_TCK
    t = sysconf(_SC_CLK_TCK);
#endif
#ifdef HZ
    if (t <= 0) t = HZ;
#endif
    if (t <= 0) t = 100;
    printf("%ld\n", t);
    return 0;
}
