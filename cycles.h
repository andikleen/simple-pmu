/*
 * Copyright (c) 2011 Intel Corporation
 * Author: Andi Kleen
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that: (1) source code distributions
 * retain the above copyright notice and this paragraph in its entirety, (2)
 * distributions including binary code include the above copyright notice and
 * this paragraph in its entirety in the documentation or other materials
 * provided with the distribution
 *
 * THIS SOFTWARE IS PROVIDED ``AS IS'' AND WITHOUT ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, WITHOUT LIMITATION, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE.
 * 
 * Library for efficient self monitoring on modern Intel CPUs using the 
 * simple-pmu driver.
 */
#ifndef CYCLES_H
#define CYCLES_H 1

#if defined(_SCHED_H) && !defined(__USE_GNU)
#error "Add #define _GNU_SOURCE 1 at beginning of source file"
#endif

#define _GNU_SOURCE 1
#include <sched.h>

#define force_inline __attribute__((always_inline))

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

static inline force_inline void sync_core(void)
{
	unsigned a, b, c, d;
	asm volatile("cpuid" : "=a" (a), "=b" (b), "=c" (c), "=d" (d)
		    	     : "0" (0) : "memory");
}

static inline force_inline unsigned long long p_rdpmc(unsigned in)
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

static inline force_inline counter_t unhalted_core(void)
{
	return p_rdpmc(FIXED_SELECT|FIXED_CPU_CLK_UNHALTED_CORE);
}

static inline force_inline counter_t unhalted_ref(void)
{
	return p_rdpmc(FIXED_SELECT|FIXED_CPU_CLK_UNHALTED_REF);
}

static inline force_inline counter_t insn_retired(void)
{
	return p_rdpmc(FIXED_SELECT|FIXED_INST_RETIRED_ANY);
}

/* Lots of caveats when you use that */

static inline force_inline unsigned long long rdtsc(void)
{
#ifdef __i386__
	unsigned long long s;
	asm volatile("rdtsc" : "=A" (s) :: "memory");
	return s;
#else
	unsigned low, high;
	asm volatile("rdtsc" : "=a" (low), "=d" (high) :: "memory");
	return ((unsigned long long)high << 32) | low;
#endif
}

static inline force_inline unsigned long long rdtscp(void)
{
#ifdef __i386__
	unsigned long long s;
	asm volatile("rdtscp" : "=A" (s) :: "ecx", "memory");
	return s;
#else
	unsigned low, high;
	asm volatile("rdtscp" : "=a" (low), "=d" (high) :: "ecx", "memory");
	return ((unsigned long long)high << 32) | low;
#endif
}

static inline int pin_cpu(cpu_set_t *oldcpus)
{
	int cpu = sched_getcpu();
	cpu_set_t cpus;
	CPU_ZERO(&cpus);
	CPU_SET(cpu, &cpus);
	if (oldcpus)
		sched_getaffinity(0, sizeof(cpu_set_t), oldcpus);
	return sched_setaffinity(0, sizeof(cpu_set_t), &cpus);
}

static inline void unpin_cpu(cpu_set_t *oldcpus)
{
	sched_setaffinity(0, sizeof(cpu_set_t), oldcpus);
}

#endif
