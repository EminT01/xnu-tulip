/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#ifndef _IOKIT_TULIPCLOCK_H
#define _IOKIT_TULIPCLOCK_H

#include <stdint.h>

/* Clock controllers, in the order of the "reg" entries of /arm-io/clocks. */
enum TulipClockBlock {
	kTulipGCC,
	kTulipMMCC,
	kTulipClockBlocks,
};

/* Branch clocks, root clock generators and power domains of the Qualcomm clock controllers (Linux drivers/clk/qcom). */
bool tulip_clock_branch_enable(TulipClockBlock block, uint32_t cbcr);
bool tulip_clock_vote(TulipClockBlock block, uint32_t voteReg, uint32_t bit, uint32_t cbcr);
bool tulip_clock_rcg_set(TulipClockBlock block, uint32_t cmdRcgr, uint32_t cfg, uint32_t m, uint32_t n);
bool tulip_clock_gdsc_enable(TulipClockBlock block, uint32_t gdscr);
uint32_t tulip_clock_read(TulipClockBlock block, uint32_t offset);

#endif /* _IOKIT_TULIPCLOCK_H */
