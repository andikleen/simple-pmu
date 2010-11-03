/*
 * Simple PMU driver.
 * Enable fixed counters on Intel CPUs and let them be read
 * by RDPMC in ring 3.
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

static unsigned long counter_mask;
static int num_counter;

static int reset;
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
	smp_call_function_single(cpu, simple_pmu_cpu_init, enable, 1);
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

	/* Work around Yonah bug */
	if (boot_cpu_data.x86_vendor == X86_VENDOR_INTEL &&
		boot_cpu_data.x86 == 6 &&
		boot_cpu_data.x86_model == 15)
		eax.f.version = 2;
	if (eax.f.version < 2)
		return;

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
		on_each_cpu(simple_pmu_cpu_init, NULL, 1);
		if (rflags & R_RESERVE)
			unreserve_counters();
	}
	if (enable) {
		if (rflags & R_RESERVE)
			reserve_counters();
		on_each_cpu(simple_pmu_cpu_init, (void *)1L, 1);
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

/* A kingdom for a linux OO system that actually works! */

struct spmu_attr {
	struct sysdev_class_attribute attr;
	int *var;
};

static ssize_t
spmu_attr_store(struct sysdev_class *c, struct sysdev_class_attribute *a,
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

static ssize_t spmu_attr_show(struct sysdev_class *c,
			      struct sysdev_class_attribute *a,
			      char *buf)
{
	struct spmu_attr *sa = container_of(a, struct spmu_attr, attr);
	return snprintf(buf, PAGE_SIZE, "%d", *(sa->var));
}

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,34)
#define SPMU_ATTR(name) \
static struct spmu_attr name##_attr;					\
static ssize_t name##_store(struct sysdev_class *c, const char *buf,	\
			   size_t size)					\
{									\
	return spmu_attr_store(c, &name##_attr.attr, buf, size);	\
}									\
									\
static ssize_t name##_show(struct sysdev_class *c, char *buf)		\
{									\
	return spmu_attr_show(c, &name##_attr.attr, buf);		\
}									\
static struct spmu_attr name##_attr = { 				\
	_SYSDEV_CLASS_ATTR(name, 0644, name##_show, name##_store),	\
	&name								\
};
#else
#define SPMU_ATTR(name) 						\
static struct spmu_attr name##_attr = { 				\
	_SYSDEV_CLASS_ATTR(name, 0644, spmu_attr_show, spmu_attr_store),\
	&name,								\
}
#endif

SPMU_ATTR(rdpmc_fixed);
SPMU_ATTR(ring);
SPMU_ATTR(reset);

static struct sysdev_class_attribute *spmu_attr[] = {
	&ring_attr.attr,
	&reset_attr.attr,
	&rdpmc_fixed_attr.attr,
	NULL
};

static struct sysdev_class spmu_sysdev_class = {
	.name = "simple-pmu",
	.suspend = simple_pmu_suspend,
	.resume = simple_pmu_resume
};

static int simple_pmu_init(void)
{
	int err;
	int i;
	struct kobject *ko;

	if (!boot_cpu_has(X86_FEATURE_ARCH_PERFMON) || cpuid_eax(0) < 0xa)
		return -ENODEV;

	err = sysdev_class_register(&spmu_sysdev_class);
	if (err)
		return err;

	/* Convert to sysdev_class->attrs link in 2.6.34 */
	ko = &spmu_sysdev_class.kset.kobj;
	for (i = 0; spmu_attr[i] && !err; i++)
		err = sysfs_create_file(ko, &spmu_attr[i]->attr);
	if (err) {
		while (--i >= 0)
			sysfs_remove_file(ko, &spmu_attr[i]->attr);
		sysdev_class_unregister(&spmu_sysdev_class);
		return err;
	}

	restart(R_RESERVE);
	register_cpu_notifier(&cpu_notifier);
	return 0;
}

static void simple_pmu_exit(void)
{
	int i;

	for (i = 0; spmu_attr[i]; i++)
		sysfs_remove_file(&spmu_sysdev_class.kset.kobj,
				  &spmu_attr[i]->attr);
	sysdev_class_unregister(&spmu_sysdev_class);
	unregister_cpu_notifier(&cpu_notifier);
	rdpmc_fixed = 0;
	restart(R_UNINIT|R_RESERVE);
}

module_init(simple_pmu_init);
module_exit(simple_pmu_exit);
MODULE_LICENSE("GPL");
