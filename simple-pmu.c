/*
 * Copyright (C) 2010, 2011 Intel Corporation
 * Author: Andi Kleen
 *
 * This software may be redistributed and/or modified under the terms of
 * the GNU General Public License ("GPL") version 2 only as published by the
 * Free Software Foundation.
 *
 * Simple PMU driver for no overhead self-monitoring.
 * Enable fixed counters on Intel CPUs and let them be read by RDPMC in ring 3.
 */
#include <linux/version.h>
#include <linux/moduleparam.h>
#include <linux/notifier.h>
#include <linux/sysdev.h>
#include <linux/kernel.h>
#include <linux/bitops.h>
#include <linux/module.h>
#include <linux/cpu.h>
#include <asm/cpufeature.h>
#include <asm/msr-index.h>
#include <asm/processor.h>
#include <asm/msr.h>
#include <asm/nmi.h>

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,27)
#define SMP_CALL_ARG 1, 1
#else
#define SMP_CALL_ARG 1
#endif

static unsigned long counter_mask;
static int num_counter;

static int ring = 3;
static int rdpmc_fixed = 1;

enum rflags {
	R_UNINIT =  1 << 0,
	R_RESERVE = 1 << 1,
};

static const struct fixed_ctr {
	int cpuid;
	unsigned msr;
} fixed_ctr[] = {
	{ 1, MSR_CORE_PERF_FIXED_CTR0 },	/* INST RETIRED.ANY */
	{ 0, MSR_CORE_PERF_FIXED_CTR1 },	/* CLK_UNHALTED_CORE */
	{ 2, MSR_CORE_PERF_FIXED_CTR2 },	/* CLK_UNHALTED_REF */
};

/* Enable or disable per CPU PMU state */
static void simple_pmu_cpu_init(void *arg)
{
	int enable = arg != NULL;
	int i;
	u64 gc, fc;
	u32 cr4;
	unsigned r = (ring & 0x3);
	int err;

	printk("simple pmu cpu init cpu %d %d\n", smp_processor_id(), enable);

	err = rdmsrl_safe(MSR_CORE_PERF_FIXED_CTR_CTRL, &fc);
	err |= rdmsrl_safe(MSR_CORE_PERF_GLOBAL_CTRL, &gc);
	for (i = 0; i < num_counter; i++)
		if (test_bit(i, &counter_mask)) {
			fc &= ~(0xfUL << (4*i));
			if (enable) {
				fc |= r << (4*i);
				gc |= 1UL << (32 + i);
			} else {
				gc &= ~(1UL << (32 + i));
			}
			wrmsr_safe(fixed_ctr[i].msr, 0, 0);
		}
	err |= wrmsr_safe(MSR_CORE_PERF_FIXED_CTR_CTRL, (u32)fc, fc >> 32);
	err |= wrmsr_safe(MSR_CORE_PERF_GLOBAL_CTRL, (u32)gc, gc >> 32);

	if (err) {
		pr_err("CPU %d: simple PMU msr access failed\n",
				smp_processor_id());
		return;
	}

	cr4 = read_cr4();
	if (enable)
		cr4 |= X86_CR4_PCE;
	else
		cr4 &= ~X86_CR4_PCE;
	write_cr4(cr4);
}

static int
simple_pmu_cpuhandler(struct notifier_block *nb, unsigned long action, void *v)
{
	unsigned long cpu = (unsigned long)v;
	void *enable;

	switch (action) {
	case CPU_ONLINE:
	case CPU_ONLINE_FROZEN:
	case CPU_DOWN_FAILED:
	case CPU_DOWN_FAILED_FROZEN:
		enable = (void *)1L;
		break;
	case CPU_DOWN_PREPARE_FROZEN:
	case CPU_DOWN_PREPARE:
		enable = NULL;
		break;
	default:
		return NOTIFY_OK;
	}
	smp_call_function_single(cpu, simple_pmu_cpu_init,
				 enable, SMP_CALL_ARG);
	return NOTIFY_DONE;
}

static struct notifier_block cpu_notifier = {
	.notifier_call = simple_pmu_cpuhandler,
};

struct a_ebx {
	unsigned version : 8;
	unsigned num_counter : 8;
	unsigned width : 8;
	unsigned mask_bitlength : 8;
};

static void query_cpu(void)
{
	union {
		u32 val;
		struct a_ebx f;
	} eax;
	u32 ebx, edx, tmp;
	int i;
	u32 mask;

	cpuid(0xa, &eax.val, &ebx, &tmp, &edx);

	num_counter = min_t(unsigned, ARRAY_SIZE(fixed_ctr), edx & 0xf);
	mask = (~ebx) & ((1UL << eax.f.mask_bitlength)-1);

	counter_mask = 0;
	for (i = 0; i < num_counter; i++)
		if ((1U << fixed_ctr[i].cpuid) & mask)
			__set_bit(i, &counter_mask);
}

static void reserve_counters(void)
{
	int i;
	int other;

	query_cpu();

	other = 0;
	for (i = 0; i < num_counter; i++)
		if (test_bit(i, &counter_mask)) {
			if (!reserve_perfctr_nmi(fixed_ctr[i].msr)) {
				__clear_bit(i, &counter_mask);
				other++;
			}
		}

	pr_info("Simple-PMU: %d fixed counters used, CPU has %d total\n",
		num_counter - other, num_counter);
}

static void unreserve_counters(void)
{
	int i;

	for (i = 0; i < num_counter; i++)
		if (test_bit(i, &counter_mask))
			release_perfctr_nmi(fixed_ctr[i].msr);
}

static void restart(enum rflags rflags)
{
	static DEFINE_MUTEX(restart_lock);
	static int prev;
	int enable;

	mutex_lock(&restart_lock);
	enable = rdpmc_fixed;
	if ((rflags & R_UNINIT) && ((prev && enable) || !enable)) {
		on_each_cpu(simple_pmu_cpu_init, NULL, SMP_CALL_ARG);
		if (rflags & R_RESERVE)
			unreserve_counters();
	}
	if (enable) {
		if (rflags & R_RESERVE)
			reserve_counters();
		on_each_cpu(simple_pmu_cpu_init, (void *)1L, SMP_CALL_ARG);
	}
	prev = enable;
	mutex_unlock(&restart_lock);
}

static int old_state;

static int simple_pmu_suspend(struct sys_device *dev, pm_message_t state)
{
	printk("simple_pmu_suspend\n");
	old_state = rdpmc_fixed;
	rdpmc_fixed = 0;
	restart(R_UNINIT);
	return 0;
}

static int simple_pmu_resume(struct sys_device *dev)
{
	printk("simple_pmu_resume\n");
	rdpmc_fixed = old_state;
	restart(0);
	return 0;
}

struct spmu_attr {
	struct sysdev_attribute attr;
	int *var;
};

static ssize_t
spmu_attr_store(struct sys_device *c, struct sysdev_attribute *a,
		const char *buf, size_t size)
{
	struct spmu_attr *sa = container_of(a, struct spmu_attr, attr);
	char *end;
	long new = simple_strtol(buf, &end, 0);
	if (end == buf || new > INT_MAX || new < INT_MIN)
		return -EINVAL;
	*(int *)(sa->var) = new;
	restart(R_RESERVE|R_UNINIT);
	return size;
}

static ssize_t 
spmu_attr_show(struct sys_device *c, struct sysdev_attribute *a, char *buf)
{
	struct spmu_attr *sa = container_of(a, struct spmu_attr, attr);
	return snprintf(buf, PAGE_SIZE, "%d", *(sa->var));
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,34)
#define SPMU_ATTR(name) \
static struct spmu_attr name##_attr;					\
static ssize_t name##_store(struct sys_device *c, const char *buf,	\
			   size_t size)					\
{									\
	return spmu_attr_store(c, &name##_attr.attr, buf, size);	\
}									\
									\
static ssize_t name##_show(struct sys_device *c, char *buf)			\
{									\
	return spmu_attr_show(c, &name##_attr.attr, buf);		\
}									\
static struct spmu_attr name##_attr = { 				\
	_SYSDEV_ATTR(name, 0644, name##_show, name##_store),		\
	&name								\
};
#else
#define SPMU_ATTR(name) 						\
static struct spmu_attr name##_attr = { 				\
	_SYSDEV_ATTR(name, 0644, spmu_attr_show, spmu_attr_store),	\
	&name,								\
}
#endif

SPMU_ATTR(rdpmc_fixed);
SPMU_ATTR(ring);

static struct sysdev_attribute *spmu_attr[] = {
	&ring_attr.attr,
	&rdpmc_fixed_attr.attr,
	NULL
};

static struct sysdev_class spmu_sysdev_class = {
	.name = "simple-pmu",
};

static struct sysdev_driver spmu_sysdev_driver = {
	.suspend = simple_pmu_suspend,
	.resume = simple_pmu_resume
};

static struct sys_device spmu_sysdev = {
	.cls = &spmu_sysdev_class,
};

static int simple_pmu_init(void)
{
	int err;
	int i;

	if (!boot_cpu_has(X86_FEATURE_ARCH_PERFMON) || cpuid_eax(0) < 0xa)
		return -ENODEV;

	err = sysdev_class_register(&spmu_sysdev_class);
	if (err)
		return err;

	err = sysdev_register(&spmu_sysdev);
	if (err)
		goto err_class;

	err = sysdev_driver_register(&spmu_sysdev_class, &spmu_sysdev_driver);
	if (err)
		goto err_sysdev;

	for (i = 0; spmu_attr[i] && !err; i++)
		err = sysdev_create_file(&spmu_sysdev, spmu_attr[i]);
	if (err)
		goto error_file;

	restart(R_RESERVE);
	register_cpu_notifier(&cpu_notifier);
	return 0;

error_file:
	while (--i >= 0)
		sysdev_remove_file(&spmu_sysdev, spmu_attr[i]);
	sysdev_driver_unregister(&spmu_sysdev_class, &spmu_sysdev_driver);
err_sysdev:
	sysdev_unregister(&spmu_sysdev);
err_class:
	sysdev_class_unregister(&spmu_sysdev_class);
	return err;
}

static void simple_pmu_exit(void)
{
	int i;

	for (i = 0; spmu_attr[i]; i++)
		sysdev_remove_file(&spmu_sysdev, spmu_attr[i]);
	sysdev_unregister(&spmu_sysdev);
	sysdev_driver_unregister(&spmu_sysdev_class, &spmu_sysdev_driver);
	sysdev_class_unregister(&spmu_sysdev_class);
	unregister_cpu_notifier(&cpu_notifier);
	rdpmc_fixed = 0;
	restart(R_UNINIT|R_RESERVE);
}

module_init(simple_pmu_init);
module_exit(simple_pmu_exit);
MODULE_LICENSE("GPL");
