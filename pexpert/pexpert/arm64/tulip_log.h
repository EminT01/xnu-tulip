/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#ifndef _ARM64_TULIP_LOG_H
#define _ARM64_TULIP_LOG_H

#include <stdint.h>
#include <mach/boolean.h>

__BEGIN_DECLS

extern uint64_t tulip_crumbs[64];
extern char tulip_log[262144];
extern uint64_t tulip_log_pos;

void tulip_crumb(unsigned int idx, uint64_t val);
void tulip_log_putc(char c);

/* Serialize console output across CPUs; returns the interrupt state for the unlock. */
boolean_t tulip_console_lock(void);
void tulip_console_unlock(boolean_t istate);

/*
 * Crumb indices (tulip_crumbs[]):
 *  0  "TULIPCRB" magic
 *  1  CNTVCT_EL0 at arm_init entry
 *  2  arm_init entry reached
 *  3  after arm_vm_init
 *  4  serial_init entry
 *  5  tulip fb console map result (1 = mapped)
 *  6  tulip fb console mapped vaddr (ml_io_map_wcomb return)
 *  7  serial_init return value (serial_exists)
 *  8  PE_init_platform(TRUE) reached
 *  9  kernel_bootstrap reached
 * 10  machine_startup reached
 * 11  first sleh_synchronous ESR_EL1
 * 12  first sleh_synchronous FAR_EL1
 * 13  first sleh_synchronous ELR (saved pc)
 * 14  panic() entry
 */
#define TULIP_CRUMB_ARM_INIT_TSC    1
#define TULIP_CRUMB_ARM_INIT        2
#define TULIP_CRUMB_ARM_VM_INIT     3
#define TULIP_CRUMB_SERIAL_INIT     4
#define TULIP_CRUMB_FB_MAP_RESULT   5
#define TULIP_CRUMB_FB_MAP_VADDR    6
#define TULIP_CRUMB_SERIAL_EXISTS   7
#define TULIP_CRUMB_PE_PLATFORM     8
#define TULIP_CRUMB_KERN_BOOTSTRAP  9
#define TULIP_CRUMB_MACHINE_STARTUP 10
#define TULIP_CRUMB_SLEH_ESR        11
#define TULIP_CRUMB_SLEH_FAR        12
#define TULIP_CRUMB_SLEH_PC         13
#define TULIP_CRUMB_PANIC           14

__END_DECLS

#endif /* _ARM64_TULIP_LOG_H */
