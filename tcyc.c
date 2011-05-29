#include <stdio.h>
#include <stdlib.h>
#include "cycles.h"

volatile float f;

static inline void kernel(void)
{
	asm volatile("nop ; nop ; nop ; nop ; nop");
	asm volatile("nop ; nop ; nop ; nop ; nop");
}

int main(void)
{
	unsigned long long a, b;

	if (perfmon_available() <= 0) {
		printf("no fixed perfmon available\n");
		exit(1);
	}
	sync_core();
	a = unhalted_core();
	kernel();
	b = unhalted_core();
	sync_core();
	printf("unhalted cycles %llu\n", b-a);	

	sync_core();
	a = unhalted_ref();
	kernel();
	b = unhalted_ref();
	sync_core();
	printf("reference cycles %llu\n", b-a);	

	sync_core();
	a = insn_retired();
	kernel();
	b = insn_retired();
	sync_core();
	printf("instructions retired %llu\n", b-a);	

	return 0;	

}
