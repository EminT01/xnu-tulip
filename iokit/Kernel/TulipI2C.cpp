/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOLib.h>
#include "TulipClock.h"
#include "TulipI2C.h"
#include "TulipTLMM.h"

extern "C" {
#include <kern/clock.h>
#include <kern/sched_prim.h>
}

/* Qualcomm QUP v2 in I2C mode, FIFO mode with v2 tags (Linux drivers/i2c/busses/i2c-qup.c). */
#define QUP_CONFIG              0x000
#define QUP_STATE               0x004
#define QUP_IO_MODE             0x008
#define QUP_SW_RESET            0x00c
#define QUP_OPERATIONAL         0x018
#define QUP_ERROR_FLAGS         0x01c
#define QUP_HW_VERSION          0x030
#define QUP_MX_OUTPUT_CNT       0x100
#define QUP_OUT_FIFO_BASE       0x110
#define QUP_MX_WRITE_CNT        0x150
#define QUP_MX_INPUT_CNT        0x200
#define QUP_MX_READ_CNT         0x208
#define QUP_IN_FIFO_BASE        0x218
#define QUP_I2C_CLK_CTL         0x400
#define QUP_I2C_STATUS          0x404
#define QUP_I2C_MASTER_GEN      0x408

#define QUP_STATE_RESET         0
#define QUP_STATE_RUN           1
#define QUP_STATE_PAUSE         3
#define QUP_STATE_MASK          3
#define QUP_STATE_VALID         (1u << 2)
#define QUP_STATE_I2C_MAST_GEN  (1u << 4)
#define QUP_OP_OUT_NOT_EMPTY    (1u << 4)
#define QUP_OP_IN_NOT_EMPTY     (1u << 5)
#define QUP_OP_OUT_SVC          (1u << 8)
#define QUP_OP_IN_SVC           (1u << 9)
#define QUP_CONFIG_I2C          (2u << 8 | 7)
#define QUP_IO_MODE_REPACK      (3u << 14)
#define QUP_V2_TAGS_EN          1
#define QUP_ERROR_MASK          0x7cu
#define I2C_STATUS_ERROR_MASK   0x38000fcu
#define I2C_STATUS_NACK         (1u << 3)
#define I2C_STATUS_BUS_ACTIVE   (1u << 8)

#define TAG_START               0x81
#define TAG_DATA_WRITE          0x82
#define TAG_DATA_WRITE_STOP     0x83
#define TAG_DATA_READ_STOP      0x87
#define RX_TAG_BYTES            2

/* 400 kHz from the 19.2 MHz XO, with Linux's fast-mode formula: fs = (19.2 MHz / 400 kHz - 6) * 2 / 3. */
#define QUP_CLK_CTL_400K        (14u << 16 | 3u << 8 | 28u)

#define TRANSFER_MS             20
/* Longest sleep between checks, in case an interrupt does not come. */
#define SLEEP_US                2000

OSDefineMetaClassAndStructors(TulipI2C, IOService);

static bool
propertyU32(IOService *service, const char *key, uint32_t *values, unsigned int count)
{
	OSData *data = OSDynamicCast(OSData, service->getProperty(key));
	if (!data || data->getLength() != count * sizeof(uint32_t)) {
		return false;
	}
	memcpy(values, data->getBytesNoCopy(), count * sizeof(uint32_t));
	return true;
}

TulipI2C *
TulipI2C::forDevice(IOService *device)
{
	OSData *bus = OSDynamicCast(OSData, device->getProperty("i2c-bus"));
	if (!bus) {
		return NULL;
	}
	char name[32] = {};
	memcpy(name, bus->getBytesNoCopy(), bus->getLength() < sizeof(name) ? bus->getLength() : sizeof(name) - 1);
	OSDictionary *matching = serviceMatching("TulipI2C");
	if (!matching) {
		return NULL;
	}
	nameMatching(name, matching);
	IOService *service = waitForMatchingService(matching, 5 * NSEC_PER_SEC);
	matching->release();
	TulipI2C *i2c = OSDynamicCast(TulipI2C, service);
	if (!i2c) {
		OSSafeReleaseNULL(service);
	}
	return i2c;
}

volatile uint32_t *
TulipI2C::reg(uint32_t offset)
{
	return (volatile uint32_t *)(fMap->getVirtualAddress() + offset);
}

bool
TulipI2C::qupState(uint32_t state)
{
	for (int i = 0; i < 100; i++) {
		uint32_t s = *reg(QUP_STATE);
		if ((s & QUP_STATE_VALID) && (s & QUP_STATE_MASK) == state) {
			return true;
		}
		IODelay(1);
	}
	return false;
}

bool
TulipI2C::setState(uint32_t state)
{
	for (int i = 0; i < 100 && !(*reg(QUP_STATE) & QUP_STATE_VALID); i++) {
		IODelay(1);
	}
	*reg(QUP_STATE) = state;
	return qupState(state);
}

bool
TulipI2C::transfer(uint8_t addr, const uint8_t *wbuf, uint32_t wlen, uint8_t *rbuf, uint32_t rlen)
{
	uint8_t tags[3 + TULIP_I2C_MAX_WRITE + 4];
	uint32_t ntags = 0;
	if ((wlen == 0 && rlen == 0) || wlen > TULIP_I2C_MAX_WRITE || rlen > TULIP_I2C_MAX_READ) {
		return false;
	}
	if (wlen) {
		tags[ntags++] = TAG_START;
		tags[ntags++] = (uint8_t)(addr << 1);
		tags[ntags++] = rlen ? TAG_DATA_WRITE : TAG_DATA_WRITE_STOP;
		tags[ntags++] = (uint8_t)wlen;
		memcpy(tags + ntags, wbuf, wlen);
		ntags += wlen;
	}
	if (rlen) {
		tags[ntags++] = TAG_START;
		tags[ntags++] = (uint8_t)(addr << 1 | 1);
		tags[ntags++] = TAG_DATA_READ_STOP;
		tags[ntags++] = (uint8_t)rlen;
	}
	uint32_t rx = rlen ? RX_TAG_BYTES + rlen : 0, got = 0;

	IOLockLock(fLock);
	*reg(QUP_SW_RESET) = 1;
	bool ok = qupState(QUP_STATE_RESET);
	if (ok) {
		*reg(QUP_CONFIG) = QUP_CONFIG_I2C;
		*reg(QUP_I2C_MASTER_GEN) = QUP_V2_TAGS_EN;
		for (int i = 0; i < 100 && !(*reg(QUP_STATE) & QUP_STATE_I2C_MAST_GEN); i++) {
			IODelay(1);
		}
		*reg(QUP_IO_MODE) = QUP_IO_MODE_REPACK;
		*reg(QUP_MX_OUTPUT_CNT) = 0;
		*reg(QUP_MX_INPUT_CNT) = 0;
		*reg(QUP_MX_WRITE_CNT) = ntags;
		*reg(QUP_MX_READ_CNT) = rx;
		ok = setState(QUP_STATE_RUN);
	}
	if (ok) {
		*reg(QUP_I2C_CLK_CTL) = QUP_CLK_CTL_400K;
		ok = setState(QUP_STATE_PAUSE);
	}
	if (ok) {
		for (uint32_t i = 0; i < ntags; i += 4) {
			uint32_t word = 0;
			for (uint32_t j = 0; j < 4 && i + j < ntags; j++) {
				word |= (uint32_t)tags[i + j] << (8 * j);
			}
			*reg(QUP_OUT_FIFO_BASE) = word;
		}
		ok = setState(QUP_STATE_RUN);
	}
	uint64_t deadline;
	clock_interval_to_deadline(TRANSFER_MS, NSEC_PER_MSEC, &deadline);
	while (ok) {
		uint32_t i2c = *reg(QUP_I2C_STATUS) & I2C_STATUS_ERROR_MASK;
		uint32_t qup = *reg(QUP_ERROR_FLAGS) & QUP_ERROR_MASK;
		if (got == rx && !(*reg(QUP_OPERATIONAL) & QUP_OP_OUT_NOT_EMPTY)) {
			break;
		}
		if (i2c || qup || mach_absolute_time() > deadline) {
			if (!(i2c & I2C_STATUS_NACK)) {
				kprintf("TulipI2C: 0x%02x failed: i2c 0x%x qup 0x%x, %u of %u bytes\n", addr, i2c, qup, got, rx);
			}
			*reg(QUP_I2C_STATUS) = i2c;
			*reg(QUP_ERROR_FLAGS) = qup;
			ok = false;
			break;
		}
		if (got < rx && (*reg(QUP_OPERATIONAL) & QUP_OP_IN_NOT_EMPTY)) {
			uint32_t word = *reg(QUP_IN_FIFO_BASE);
			for (int j = 0; j < 4 && got < rx; j++, word >>= 8) {
				fRx[got++] = (uint8_t)word;
			}
		} else {
			waitInterrupt(got == rx);
		}
	}
	for (int i = 0; ok && i < 1000 && (*reg(QUP_I2C_STATUS) & I2C_STATUS_BUS_ACTIVE); i++) {
		IODelay(1);
	}
	setState(QUP_STATE_RESET);
	if (ok && rlen) {
		memcpy(rbuf, fRx + RX_TAG_BYTES, rlen);
	}
	IOLockUnlock(fLock);
	return ok;
}

void
TulipI2C::waitInterrupt(bool readDone)
{
	if (!fInterrupts) {
		return;
	}
	*reg(QUP_OPERATIONAL) = QUP_OP_OUT_SVC | QUP_OP_IN_SVC;
	uint32_t op = *reg(QUP_OPERATIONAL);
	if ((op & QUP_OP_IN_NOT_EMPTY) || (readDone && !(op & QUP_OP_OUT_NOT_EMPTY)) ||
	    (*reg(QUP_I2C_STATUS) & I2C_STATUS_ERROR_MASK) || (*reg(QUP_ERROR_FLAGS) & QUP_ERROR_MASK)) {
		return;
	}
	assert_wait_timeout((event_t)this, THREAD_UNINT, SLEEP_US, NSEC_PER_USEC);
	getProvider()->enableInterrupt(0);
	thread_block(THREAD_CONTINUE_NULL);
}

void
TulipI2C::interrupt(void *refCon __unused, IOService *nub, int source)
{
	nub->disableInterrupt(source);
	thread_wakeup((event_t)this);
}

bool
TulipI2C::readRegister(uint8_t addr, uint8_t regAddr, uint8_t *buf, uint32_t len)
{
	return transfer(addr, &regAddr, 1, buf, len);
}

/* The core clock from the XO, the bus clock vote, and the SDA and SCL pins. */
bool
TulipI2C::powerOn(IOService *provider)
{
	uint32_t cbcr, rcgr, vote[3], pins[2], function;
	if (!propertyU32(provider, "clock-cbcr", &cbcr, 1) || !propertyU32(provider, "clock-rcgr", &rcgr, 1) ||
	    !propertyU32(provider, "clock-vote", vote, 3) || !propertyU32(provider, "gpios", pins, 2) ||
	    !propertyU32(provider, "gpio-function", &function, 1)) {
		IOLog("TulipI2C: %s: missing clock or pin properties\n", provider->getName());
		return false;
	}
	TulipTLMM *tlmm = TulipTLMM::wait();
	if (!tlmm) {
		IOLog("TulipI2C: no TLMM\n");
		return false;
	}
	bool ok = tulip_clock_vote(kTulipGCC, vote[0], vote[1], vote[2]) && tulip_clock_rcg_set(kTulipGCC, rcgr, 0, 0, 0) &&
	    tulip_clock_branch_enable(kTulipGCC, cbcr) &&
	    tlmm->configure(pins[0], function, kTulipPullNone, 2, false) &&
	    tlmm->configure(pins[1], function, kTulipPullNone, 2, false);
	tlmm->release();
	return ok;
}

bool
TulipI2C::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	fLock = IOLockAlloc();
	fMap = provider->mapDeviceMemoryWithIndex(0);
	if (!fLock || !fMap || !powerOn(provider)) {
		IOLog("TulipI2C: %s does not start\n", provider->getName());
		return false;
	}
	/* Registering waits for the GIC driver, so only try once it runs. */
	OSDictionary *matching = serviceMatching("TulipGIC");
	IOService *gic = matching ? waitForMatchingService(matching, 5 * NSEC_PER_SEC) : NULL;
	OSSafeReleaseNULL(matching);
	if (gic && provider->getProperty(gIOInterruptControllersKey)) {
		fInterrupts = provider->registerInterrupt(0, this,
		    OSMemberFunctionCast(IOInterruptAction, this, &TulipI2C::interrupt)) == kIOReturnSuccess;
	}
	OSSafeReleaseNULL(gic);
	setName(provider->getName());
	IOLog("TulipI2C: %s, QUP version 0x%08x, %s\n", getName(), *reg(QUP_HW_VERSION),
	    fInterrupts ? "interrupts" : "polling");
	registerService();
	return true;
}
