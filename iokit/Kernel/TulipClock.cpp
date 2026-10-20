/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <libkern/c++/OSData.h>
#include "TulipClock.h"

#define CBCR_ENABLE        (1u << 0)
#define CBCR_OFF           (1u << 31)
#define RCGR_UPDATE        (1u << 0)
#define RCGR_CFG           0x4
#define RCGR_M             0x8
#define RCGR_N             0xc
#define RCGR_D             0x10
#define GDSC_SW_COLLAPSE   (1u << 0)
#define GDSC_PWR_ON        (1u << 31)
#define WAIT_US            1500

static IOLock *gTulipClockLock;
static IOMemoryMap *gTulipClockMap[kTulipClockBlocks];
static vm_offset_t gTulipClockBase[kTulipClockBlocks];
static uint32_t gTulipClockSize[kTulipClockBlocks];

static volatile uint32_t *
clockReg(TulipClockBlock block, uint32_t offset)
{
	if (!gTulipClockLock) {
		IOLock *fresh = IOLockAlloc();
		if (!OSCompareAndSwapPtr(NULL, fresh, (void * volatile *)&gTulipClockLock)) {
			IOLockFree(fresh);
		}
	}
	IOLockLock(gTulipClockLock);
	if (!gTulipClockBase[kTulipGCC]) {
		IORegistryEntry *node = IORegistryEntry::fromPath("/arm-io/clocks", gIODTPlane);
		OSData *reg = node ? OSDynamicCast(OSData, node->getProperty("reg")) : NULL;
		const uint64_t *cells = reg ? (const uint64_t *)reg->getBytesNoCopy() : NULL;
		for (unsigned int i = 0; cells && i < kTulipClockBlocks && (i + 1) * 16 <= reg->getLength(); i++) {
			IOMemoryDescriptor *md = IOMemoryDescriptor::withPhysicalAddress(cells[2 * i], cells[2 * i + 1],
			    kIODirectionInOut);
			gTulipClockMap[i] = md ? md->map(kIOMapInhibitCache) : NULL;
			OSSafeReleaseNULL(md);
			if (gTulipClockMap[i]) {
				gTulipClockSize[i] = (uint32_t)cells[2 * i + 1];
				gTulipClockBase[i] = gTulipClockMap[i]->getVirtualAddress();
			}
		}
		OSSafeReleaseNULL(node);
	}
	IOLockUnlock(gTulipClockLock);
	if (block >= kTulipClockBlocks || !gTulipClockBase[block] || offset + 4 > gTulipClockSize[block]) {
		kprintf("TulipClock: no register 0x%x in block %d\n", offset, block);
		return NULL;
	}
	return (volatile uint32_t *)(gTulipClockBase[block] + offset);
}

static bool
waitBits(volatile uint32_t *reg, uint32_t mask, uint32_t value)
{
	for (int i = 0; i < WAIT_US; i++) {
		if ((*reg & mask) == value) {
			return true;
		}
		IODelay(1);
	}
	return false;
}

bool
tulip_clock_branch_enable(TulipClockBlock block, uint32_t cbcr)
{
	volatile uint32_t *reg = clockReg(block, cbcr);
	if (!reg) {
		return false;
	}
	IOLockLock(gTulipClockLock);
	*reg |= CBCR_ENABLE;
	bool ok = waitBits(reg, CBCR_OFF, 0);
	IOLockUnlock(gTulipClockLock);
	if (!ok) {
		kprintf("TulipClock: branch %d:0x%x stays off (0x%08x)\n", block, cbcr, *reg);
	}
	return ok;
}

/* Clocks shared with other processors are voted on; cbcr, if not 0, shows when the clock runs. */
bool
tulip_clock_vote(TulipClockBlock block, uint32_t voteReg, uint32_t bit, uint32_t cbcr)
{
	volatile uint32_t *vote = clockReg(block, voteReg);
	volatile uint32_t *status = cbcr ? clockReg(block, cbcr) : NULL;
	if (!vote || (cbcr && !status)) {
		return false;
	}
	IOLockLock(gTulipClockLock);
	*vote |= 1u << bit;
	bool ok = !status || waitBits(status, CBCR_OFF, 0);
	IOLockUnlock(gTulipClockLock);
	if (!ok) {
		kprintf("TulipClock: vote %d:0x%x bit %u: clock stays off (0x%08x)\n", block, voteReg, bit, *status);
	}
	return ok;
}

/* Sets a root clock generator: cfg holds the source and divider; m and n (n != 0) set the M/N counter. */
bool
tulip_clock_rcg_set(TulipClockBlock block, uint32_t cmdRcgr, uint32_t cfg, uint32_t m, uint32_t n)
{
	volatile uint32_t *cmd = clockReg(block, cmdRcgr);
	if (!cmd || !clockReg(block, cmdRcgr + RCGR_D)) {
		return false;
	}
	IOLockLock(gTulipClockLock);
	if (n) {
		cmd[RCGR_M / 4] = m;
		cmd[RCGR_N / 4] = ~(n - m);
		cmd[RCGR_D / 4] = ~n;
	}
	cmd[RCGR_CFG / 4] = cfg;
	*cmd |= RCGR_UPDATE;
	bool ok = waitBits(cmd, RCGR_UPDATE, 0);
	IOLockUnlock(gTulipClockLock);
	if (!ok) {
		kprintf("TulipClock: RCG %d:0x%x does not update (0x%08x)\n", block, cmdRcgr, *cmd);
	}
	return ok;
}

bool
tulip_clock_gdsc_enable(TulipClockBlock block, uint32_t gdscr)
{
	volatile uint32_t *reg = clockReg(block, gdscr);
	if (!reg) {
		return false;
	}
	IOLockLock(gTulipClockLock);
	*reg &= ~GDSC_SW_COLLAPSE;
	bool ok = waitBits(reg, GDSC_PWR_ON, GDSC_PWR_ON);
	IOLockUnlock(gTulipClockLock);
	if (!ok) {
		kprintf("TulipClock: power domain %d:0x%x stays off (0x%08x)\n", block, gdscr, *reg);
	}
	return ok;
}

uint32_t
tulip_clock_read(TulipClockBlock block, uint32_t offset)
{
	volatile uint32_t *reg = clockReg(block, offset);
	return reg ? *reg : 0xffffffffu;
}
