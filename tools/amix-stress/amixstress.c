/* amixstress.c -- maximize the 68030 MMU "multi-fault continuation" trigger on AMIX.
 *
 * Deep recursion with a big per-frame buffer forces each function prologue's
 * MOVEM.L <regs>,-(SP) (+ the buffer) to grow the USER STACK downward across many
 * not-yet-resident pages. Each crossing page-faults; the kernel grows the stack and
 * the 030 continuation resumes the faulted instruction -- exactly the path that, when
 * a resumed MOVEM faulted AGAIN, used to frame a stale kernel PC and return the
 * process to a wild PC ("User BUS ERROR"). Touch the buffer so it isn't optimized out.
 *
 * Build on AMIX:  cc -O amixstress.c -o amixstress
 * Run:            amixstress [depth]      (default 160 frames; ~depth*2KB of stack)
 */
#include <stdio.h>
#include <stdlib.h>

static long recurse(int depth, long sum)
{
    volatile char buf[2048];
    int i;
    for (i = 0; i < (int)sizeof(buf); i += 64)
        buf[i] = (char)(depth + i);
    if (depth <= 0)
        return sum + buf[0];
    return recurse(depth - 1, sum + buf[(depth * 37) & (sizeof(buf) - 1)]);
}

int main(argc, argv)
int argc;
char **argv;
{
    int depth = (argc > 1) ? atoi(argv[1]) : 160;
    int iter, n = (argc > 2) ? atoi(argv[2]) : 1;
    long r = 0;
    for (iter = 0; iter < n; iter++)
        r += recurse(depth, 0);
    printf("amixstress depth=%d iters=%d ok r=%ld\n", depth, n, r);
    return 0;
}
