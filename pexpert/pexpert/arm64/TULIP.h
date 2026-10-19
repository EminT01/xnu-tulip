/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#ifndef _PEXPERT_ARM64_TULIP_H
#define _PEXPERT_ARM64_TULIP_H

/* Xiaomi Redmi Note 6 Pro: Qualcomm SDM636, XNU runs on the Kryo 260 Gold (Cortex-A73) cluster. */

#define NO_MONITOR                1
#define NO_ECORE                  1
#define TULIP                     1

#define __ARM_ARCH__              8
#define __ARM_VMSA__              8
#define __ARM_VFP__               4
#define __ARM_COHERENT_CACHE__    1
#define __ARM_IC_NOALIAS_ICACHE__ 1
#define __ARM_DEBUG__             7
#define __ARM_ENABLE_SWAP__       1
#define __ARM_V8_CRYPTO_EXTENSIONS__ 1
#define __ARM_16K_PG__            1
#define __ARM_MIXED_PAGE_SIZE__   1
#define ARM_PARAMETERIZED_PMAP    1
#define ARM_ARCH_TIMER
#define HAS_GIC_V3                1

#define CPU_VERSION_UNKNOWN       0xff

#define GIC_SPURIOUS_IRQ          1023
#define GICR_PE_SIZE              0x20000
#define GICD_CTLR                 0x0
#define GICD_CTLR_ENABLEGRP0      0x1
#define GICD_CTLR_NS_ENABLEGRP1   0x1
#define GICD_CTLR_NS_ENABLEGRP1A  0x2
#define GICD_CTLR_ARE_NS          0x10
#define GICR_TYPER                0x08
#define GICR_WAKER                0x14
#define GICR_IGROUPR0             0x10080
#define GICR_ISENABLER0           0x10100
#define GICR_ICENABLER0           0x10180
#define GICR_IPRIORITYR0          0x10400
#define GICR_ISPENDR0             0x10200
#define GICR_TYPER_AFFINITY_VALUE_SHIFT 32
#define GICR_TYPER_LAST           0x10
#define GICR_WAKER_PROCESSORSLEEP 0x2
#define GICR_WAKER_CHILDRENASLEEP 0x4
#define ICC_CTLR_EOIMODE          0x1
#define ICC_SRE_SRE               0x1

#define TULIP_VTIMER_INTID_DEFAULT 19

#ifndef ASSEMBLER
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
extern uint32_t tulip_vtimer_intid;
extern uint64_t tulip_irq_count;
extern uint32_t tulip_last_intid;
void tulip_gic_report(void);
#ifdef __cplusplus
}
#endif
#define TULIP_VTIMER_INTID        tulip_vtimer_intid
#define TULIP_FB_CONSOLE
#define PLATFORM_PANIC_LOG_DISABLED
#endif

#endif /* ! _PEXPERT_ARM64_TULIP_H */
