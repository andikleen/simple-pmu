#ifndef CYCLES_H
#define CYCLES_H 1

typedef unsigned long long counter_t;

static inline void p_cpuid(unsigned in, 
	unsigned *a, unsigned *b, unsigned *c, unsigned *d)
{
	asm("cpuid" : "=a" (*a), "=b" (*b), "=c" (*c), "=d" (*d)
		    : "0" (in));
}

static inline unsigned p_cpuid_a(unsigned in)
{
	unsigned a, b, c, d;
	p_cpuid(in, &a, &b, &c, &d);
	return a;
}

static inline void sync_core(void)
{
	unsigned a, b, c, d;
	asm volatile("cpuid" : "=a" (a), "=b" (b), "=c" (c), "=d" (d)
		    	     : "0" (0) : "memory");
}

static inline unsigned long long p_rdpmc(unsigned in)
{
	unsigned d, a;

	asm volatile("rdpmc" : "=d" (d), "=a" (a) : "c" (in) : "memory");
	return ((unsigned long long)d << 32) | a;
}

static inline int perfmon_available(void)
{
	unsigned eax;
	if (p_cpuid_a(0) < 10)
		return 0;
	eax = p_cpuid_a(10);
	if ((eax & 0xff) == 0)
		return 0;
	return (eax >> 8) & 0xff;
}

enum {
	FIXED_SELECT = (1U << 30),
	FIXED_INST_RETIRED_ANY = 0,
	FIXED_CPU_CLK_UNHALTED_CORE = 1,
	FIXED_CPU_CLK_UNHALTED_REF = 2,
};

static inline counter_t unhalted_core(void)
{
	return p_rdpmc(FIXED_SELECT|FIXED_CPU_CLK_UNHALTED_CORE);
}

static inline counter_t unhalted_ref(void)
{
	return p_rdpmc(FIXED_SELECT|FIXED_CPU_CLK_UNHALTED_REF);
}

static inline counter_t insn_retired(void)
{
	return p_rdpmc(FIXED_SELECT|FIXED_INST_RETIRED_ANY);
}

#endif
