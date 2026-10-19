/*
 * Copyright (c) 2021 Apple Inc. All rights reserved.
 */

#include <stdint.h>
#include <arm64/proc_reg.h>
#include <kern/clock.h>
#include <mach/mach_time.h>
#include <machine/atomic.h>
#include <machine/machine_routines.h>
#include <pexpert/device_tree.h>
#include <pexpert/pexpert.h>
#include <pexpert/arm64/board_config.h>


#if HAS_GIC_V3
#define GICR_WAKE_TIMEOUT_NS (1000000000ULL) // timeout for redistributor wakeup (default 1s)
MACHINE_TIMEOUT(gicr_wake_timeout_ns, "gicr-wake-timeout", GICR_WAKE_TIMEOUT_NS, MACHINE_TIMEOUT_UNIT_NSEC, NULL);

static vm_offset_t gicd_base;
#if TULIP
uint32_t tulip_vtimer_intid = TULIP_VTIMER_INTID_DEFAULT;
uint64_t tulip_irq_count;
uint32_t tulip_last_intid;
#endif
static vm_offset_t gicr_base;
static vm_offset_t gicr_size;

static uint32_t
_gic_read32(vm_offset_t addr)
{
	return *((volatile uint32_t *) addr);
}

static uint64_t
_gic_read64(vm_offset_t addr)
{
	return *((volatile uint64_t *) addr);
}

static void
_gic_write32(vm_offset_t addr, uint32_t value)
{
	*((volatile uint32_t *) addr) = value;
}

#define gicd_read32(offset) (_gic_read32(gicd_base + (offset)))
#define gicd_write32(offset, data) (_gic_write32(gicd_base + (offset), (data)))
#define gicr_read32(offset) (_gic_read32(gicr_pe_base + (offset)))
#define gicr_write32(offset, data) (_gic_write32(gicr_pe_base + (offset), (data)))
#define gicr_read64(offset) (_gic_read64(gicr_pe_base + (offset)))

static vm_offset_t
find_gicr_pe_base()
{
	// We only care about aff1 and aff0
	uint32_t phys_id = __builtin_arm_rsr64("MPIDR_EL1") & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);

	for (vm_offset_t offset = 0; offset < gicr_size; offset += GICR_PE_SIZE) {
		vm_offset_t gicr_pe_base = gicr_base + offset;
		uint64_t gicr_typer = gicr_read64(GICR_TYPER);
		uint32_t aff_value = (uint32_t) (gicr_typer >> GICR_TYPER_AFFINITY_VALUE_SHIFT) & (MPIDR_AFF1_MASK | MPIDR_AFF0_MASK);

		if (phys_id == aff_value) {
			return gicr_pe_base;
		}

		if (gicr_typer & GICR_TYPER_LAST) {
			break;
		}
	}

	panic("%s: cannot find GICR base for core %u", __func__, ml_get_cpu_number(phys_id));
}

#if TULIP
void
tulip_gic_report(void)
{
	vm_offset_t gicr_pe_base = find_gicr_pe_base();
	uint32_t id = TULIP_VTIMER_INTID;
	uint32_t prio = gicr_read32(GICR_IPRIORITYR0 + (id & ~3u)) >> ((id & 3u) * 8);
	uint64_t cntv_ctl = __builtin_arm_rsr64("CNTV_CTL_EL0");
	uint64_t cval = __builtin_arm_rsr64("CNTV_CVAL_EL0");
	uint64_t now = __builtin_arm_rsr64("CNTVCT_EL0");

	kprintf("tick: irqs %llu last %u | cntv_ctl 0x%llx cval-now %lld\n",
	    tulip_irq_count, tulip_last_intid, cntv_ctl, (long long)(cval - now));
	kprintf("tick: intid %u pend %u en %u grp %u prio 0x%x | hppir1 0x%llx pmr 0x%llx igrpen1 0x%llx sre 0x%llx gicd_ctlr 0x%x\n",
	    id, (gicr_read32(GICR_ISPENDR0) >> id) & 1, (gicr_read32(GICR_ISENABLER0) >> id) & 1,
	    (gicr_read32(GICR_IGROUPR0) >> id) & 1, prio & 0xff,
	    __builtin_arm_rsr64("ICC_HPPIR1_EL1"), __builtin_arm_rsr64("ICC_PMR_EL1"),
	    __builtin_arm_rsr64("ICC_IGRPEN1_EL1"), __builtin_arm_rsr64("ICC_SRE_EL1"),
	    gicd_read32(GICD_CTLR));
}
#endif

#if TULIP
void
tulip_timer_selftest(void)
{
	vm_offset_t gicr_pe_base = find_gicr_pe_base();
	uint64_t freq = gPEClockFrequencyInfo.timebase_frequency_hz;
	uint64_t old_cval = __builtin_arm_rsr64("CNTV_CVAL_EL0");
	uint64_t old_ctl = __builtin_arm_rsr64("CNTV_CTL_EL0");
	boolean_t istate = ml_set_interrupts_enabled(FALSE);
	uint64_t irqs0 = tulip_irq_count;

	kprintf("timertest: pend 0x%08x en 0x%08x grp 0x%08x gicd_ctlr 0x%x pmr 0x%llx igrpen1 0x%llx sre 0x%llx ctlr 0x%llx\n",
	    gicr_read32(GICR_ISPENDR0), gicr_read32(GICR_ISENABLER0), gicr_read32(GICR_IGROUPR0),
	    gicd_read32(GICD_CTLR), __builtin_arm_rsr64("ICC_PMR_EL1"), __builtin_arm_rsr64("ICC_IGRPEN1_EL1"),
	    __builtin_arm_rsr64("ICC_SRE_EL1"), __builtin_arm_rsr64("ICC_CTLR_EL1"));

	uint64_t gpar;
	__asm__ volatile ("at s1e1r, %1; isb; mrs %0, PAR_EL1" : "=r"(gpar) : "r"(gicr_pe_base));
	kprintf("timertest: PAR for gicr 0x%llx (attr 0x%02llx)\n", gpar, gpar >> 56);
	kprintf("timertest: frame 0x%lx typer 0x%llx waker 0x%x mpidr 0x%llx\n",
	    (unsigned long)(gicr_pe_base - gicr_base), gicr_read64(GICR_TYPER), gicr_read32(GICR_WAKER),
	    __builtin_arm_rsr64("MPIDR_EL1"));
	gicr_write32(GICR_ISENABLER0, 1u << TULIP_VTIMER_INTID);
	__builtin_arm_dsb(DSB_SY);
	kprintf("timertest: after enable: en 0x%08x\n", gicr_read32(GICR_ISENABLER0));

	__builtin_arm_wsr64("CNTV_CVAL_EL0", __builtin_arm_rsr64("CNTVCT_EL0") + freq / 1000);
	__builtin_arm_wsr64("CNTV_CTL_EL0", 1);
	__builtin_arm_isb(ISB_SY);
	uint64_t end = __builtin_arm_rsr64("CNTVCT_EL0") + freq / 500;
	while (__builtin_arm_rsr64("CNTVCT_EL0") < end) {
	}
	kprintf("timertest: masked: cntv_ctl 0x%llx pend 0x%08x hppir1 0x%llx rpr 0x%llx\n",
	    __builtin_arm_rsr64("CNTV_CTL_EL0"), gicr_read32(GICR_ISPENDR0),
	    __builtin_arm_rsr64("ICC_HPPIR1_EL1"), __builtin_arm_rsr64("ICC_RPR_EL1"));

	ml_set_interrupts_enabled(TRUE);
	end = __builtin_arm_rsr64("CNTVCT_EL0") + freq / 500;
	while (__builtin_arm_rsr64("CNTVCT_EL0") < end) {
	}
	ml_set_interrupts_enabled(FALSE);
	kprintf("timertest: unmasked: irqs %llu (+%llu) last %u cntv_ctl 0x%llx pend 0x%08x\n",
	    tulip_irq_count, tulip_irq_count - irqs0, tulip_last_intid,
	    __builtin_arm_rsr64("CNTV_CTL_EL0"), gicr_read32(GICR_ISPENDR0));

	__builtin_arm_wsr64("CNTV_CVAL_EL0", old_cval);
	__builtin_arm_wsr64("CNTV_CTL_EL0", old_ctl);
	__builtin_arm_isb(ISB_SY);
	ml_set_interrupts_enabled(istate);
}
#endif

void
pe_init_fiq()
{
	int error;
	DTEntry entry;

	// gicd_base is 0x0 only before it's initialized by the boot processor.
	// Avoid calling SecureDT* routines on secondary processors to avoid
	// race conditions because they are not thread-safe.
	if (!gicd_base) {
		// Find GIC DT node
		error = SecureDTLookupEntry(NULL, "/arm-io/gic", &entry);
		if (error != kSuccess) {
			panic("%s: cannot find GIC node in DT", __func__);
		}

		// Find "reg" property
		void const *prop;
		unsigned int prop_size;
		error = SecureDTGetProperty(entry, "reg", &prop, &prop_size);
		if (error != kSuccess) {
			panic("%s: cannot find GIC MMIO regions in DT", __func__);
		}

#if TULIP
		void const *intid_prop;
		unsigned int intid_size;
		if (SecureDTGetProperty(entry, "vtimer-intid", &intid_prop, &intid_size) == kSuccess &&
		    intid_size == sizeof(uint32_t)) {
			tulip_vtimer_intid = *(uint32_t const *)intid_prop;
		}
#endif

		// Need at least GICD base, GICD size, GICR base and GICR size
		if (prop_size < 4 * sizeof(uint64_t)) {
			panic("%s: incorrect reg property size in GIC DT node; expecting 32 bytes but got %u bytes", __func__, prop_size);
		}

		vm_offset_t soc_base_phys = pe_arm_get_soc_base_phys();

		uint64_t const gicd_base_prop = ((uint64_t const *) prop)[0];
		uint64_t const gicd_size_prop = ((uint64_t const *) prop)[1];
		uint64_t const gicr_base_prop = ((uint64_t const *) prop)[2];
		uint64_t const gicr_size_prop = ((uint64_t const *) prop)[3];

		// Find GICD base address
		gicd_base = ml_io_map(soc_base_phys + gicd_base_prop, gicd_size_prop);

		if (!gicd_base) {
			panic("%s: cannot map GICD region", __func__);
		}

		// Find GICR base address
		gicr_base = ml_io_map(soc_base_phys + gicr_base_prop, gicr_size_prop);

		if (!gicr_base) {
			panic("%s: cannot map GICR region", __func__);
		}

		gicr_size = gicr_size_prop;
	}

	// Find the redistributor for this processor
	vm_offset_t gicr_pe_base = find_gicr_pe_base();

	// Mark this PE to be awake
	uint32_t gicr_waker = gicr_read32(GICR_WAKER);
	if (gicr_waker & GICR_WAKER_CHILDRENASLEEP) {
		gicr_waker &= ~GICR_WAKER_PROCESSORSLEEP;

		gicr_write32(GICR_WAKER, gicr_waker);

		uint64_t gicr_wake_deadline;
		nanoseconds_to_deadline(os_atomic_load(&gicr_wake_timeout_ns, relaxed), &gicr_wake_deadline);
		while (gicr_read32(GICR_WAKER) & GICR_WAKER_CHILDRENASLEEP) {
			// Spin
			if (gicr_wake_timeout_ns > 0 && mach_absolute_time() > gicr_wake_deadline) {
				panic("%s: core %u timed out waiting for redistributor to wake up",
				    __func__, ml_get_cpu_number_local());
			}
		}
	}

#if TULIP
	// Group 0 belongs to the secure world here, so the vtimer is a Group 1 IRQ.
	gicr_write32(GICR_IGROUPR0, gicr_read32(GICR_IGROUPR0) | (1u << TULIP_VTIMER_INTID));
	vm_offset_t prio = GICR_IPRIORITYR0 + (TULIP_VTIMER_INTID & ~3u);
	uint32_t shift = (TULIP_VTIMER_INTID & 3u) * 8;
	gicr_write32(prio, (gicr_read32(prio) & ~(0xffu << shift)) | (0xa0u << shift));
	gicr_write32(GICR_ISENABLER0, (1u << TULIP_VTIMER_INTID));
#else
	// Configure timers and legacy FIQ to be group 0
	gicr_write32(GICR_IGROUPR0, 0x81FFFFFF);

	// Enable PPI 27
	gicr_write32(GICR_ISENABLER0, (1 << 27));
#endif

	// Enable system register access
	uint64_t icc_sre = __builtin_arm_rsr64("ICC_SRE_EL1");
	icc_sre |= ICC_SRE_SRE;
	__builtin_arm_wsr64("ICC_SRE_EL1", icc_sre);
	__builtin_arm_isb(ISB_SY);

#if TULIP
	__builtin_arm_wsr64("ICC_BPR1_EL1", 0);
#else
	// Set priority masks and binary point for group 0
	__builtin_arm_wsr64("ICC_BPR0_EL1", 0);
#endif
	__builtin_arm_wsr64("ICC_PMR_EL1", 0xFF);

	// Set EOI mode of this processor
	uint64_t icc_ctlr = __builtin_arm_rsr64("ICC_CTLR_EL1");
	icc_ctlr &= ~ICC_CTLR_EOIMODE;
	__builtin_arm_wsr64("ICC_CTLR_EL1", icc_ctlr);

	// Enable the forwarding of the vtimer interrupt
	uint32_t gicd_ctlr = gicd_read32(GICD_CTLR);
#if TULIP
	// ARE_NS may only change while the groups are disabled; the boot loader may have run without it.
	if (!(gicd_ctlr & GICD_CTLR_ARE_NS)) {
		gicd_write32(GICD_CTLR, 0);
		while (gicd_read32(GICD_CTLR) & GICD_CTLR_RWP) {
		}
		gicd_write32(GICD_CTLR, GICD_CTLR_ARE_NS);
		while (gicd_read32(GICD_CTLR) & GICD_CTLR_RWP) {
		}
		gicd_ctlr = GICD_CTLR_ARE_NS;
	}
	gicd_ctlr |= GICD_CTLR_ARE_NS | GICD_CTLR_NS_ENABLEGRP1A | GICD_CTLR_NS_ENABLEGRP1;
	gicd_write32(GICD_CTLR, gicd_ctlr);
	while (gicd_read32(GICD_CTLR) & GICD_CTLR_RWP) {
	}
	__builtin_arm_wsr64("ICC_IGRPEN1_EL1", 1);
#else
	gicd_ctlr |= GICD_CTLR_ENABLEGRP0;
	gicd_write32(GICD_CTLR, gicd_ctlr);
	__builtin_arm_wsr64("ICC_IGRPEN0_EL1", 1);
#endif
	__builtin_arm_isb(ISB_SY);
}
#else
void
pe_init_fiq()
{
}
#endif
