/* Layer-1 host MMU test harness entry point.
 * For now: a placeholder main so the subset can link. Real tests land once the
 * flat-buffer machine boots and (later) cpummu030.cpp is imported. */
#include <cstdio>

int main(int argc, char **argv)
{
	(void)argc; (void)argv;
	printf("[harness] Z3660 UAE host MMU harness — skeleton link OK\n");
	return 0;
}
