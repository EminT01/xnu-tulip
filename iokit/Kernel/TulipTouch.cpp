/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/hid/IOHIDEvent.h>
#include <IOKit/hid/IOHIDUsageTables.h>
#include "hid/include/IOHIDEventService.h"

extern "C" {
#include <kern/clock.h>
}

/* Register blocks of the /arm-io/touch node, in its "reg" order. */
enum {
	kRegQUP,
	kRegClock,
	kRegClockVote,
	kRegI2CPins,
	kRegTouchPins,
};

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
#define QUP_OP_IN_NOT_EMPTY     (1u << 5)
#define QUP_CONFIG_I2C          (2u << 8 | 7)
#define QUP_IO_MODE_REPACK      (3u << 14)
#define QUP_V2_TAGS_EN          1
#define QUP_ERROR_MASK          0x7cu
#define I2C_STATUS_ERROR_MASK   0x38000fcu
#define I2C_STATUS_NACK         (1u << 3)
#define I2C_STATUS_BUS_ACTIVE   (1u << 8)

#define TAG_START               0x81
#define TAG_DATA_WRITE          0x82
#define TAG_DATA_READ_STOP      0x87
#define RX_TAG_BYTES            2

/* 400 kHz from the 19.2 MHz XO, with Linux's fast-mode formula: fs = (19.2 MHz / 400 kHz - 6) * 2 / 3. */
#define QUP_CLK_CTL_400K        (14u << 16 | 3u << 8 | 28u)

/* GCC: the QUP1 I2C core clock and its source, and the BLSP1 bus clock vote (Linux clk/qcom/gcc-sdm660.c). */
#define GCC_QUP1_I2C_CBCR       0x008
#define GCC_QUP1_I2C_CMD_RCGR   0x020
#define GCC_QUP1_I2C_CFG_RCGR   0x024
#define GCC_BLSP1_AHB_VOTE      0x004
#define GCC_BLSP1_AHB_VOTE_BIT  (1u << 17)
#define CBCR_ENABLE             (1u << 0)
#define CBCR_OFF                (1u << 31)
#define RCGR_UPDATE             (1u << 0)

/* TLMM pin registers, 0x1000 apart. */
#define PIN_CFG                 0x0
#define PIN_IO                  0x4
#define PIN_INTR_CFG            0x8
#define PIN_INTR_STATUS         0xc
#define PIN_IO_IN               (1u << 0)
#define PIN_IO_OUT              (1u << 1)
#define PIN_CFG_I2C             (3u << 2)
#define PIN_CFG_GPIO_PULL_UP    (3u | 7u << 6)
#define PIN_CFG_OUTPUT          (1u << 9)
/* Latch falling edges in the status register, without an interrupt to the GIC. */
#define PIN_INTR_FALLING_RAW    (2u << 2 | 1u << 1 | 1u << 4)

/* Novatek NT36672A at I2C address 0x01 (Linux drivers/input/touchscreen/novatek-nvt-ts.c). */
#define NVT_ADDR                0x01
#define NVT_TOUCH_START         0x00
#define NVT_TOUCH_SIZE          6
#define NVT_MAX_TOUCHES         10
#define NVT_PARAMS_START        0x78
#define NVT_PARAMS_SIZE         0x0f
#define NVT_TOUCH_INVALID       0xff
#define NVT_TOUCH_NEW           1
#define NVT_TOUCH_UPDATE        2

#define POLL_MS                 8

class TulipTouch : public IOHIDEventService
{
	OSDeclareDefaultStructors(TulipTouch);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual UInt32 getPrimaryUsagePage() APPLE_KEXT_OVERRIDE { return kHIDPage_Digitizer; }
	virtual UInt32 getPrimaryUsage() APPLE_KEXT_OVERRIDE { return kHIDUsage_Dig_TouchScreen; }

protected:
	virtual const char *getProduct() APPLE_KEXT_OVERRIDE { return "Touch Screen"; }

private:
	IOMemoryMap *fMap[5];
	IOTimerEventSource *fTimer;
	uint32_t fWidth, fHeight;
	uint32_t fCanvasWidth, fCanvasHeight;
	int32_t fOutX, fOutY, fOutWidth, fOutHeight;
	bool fDown[NVT_MAX_TOUCHES];
	IOFixed fX[NVT_MAX_TOUCHES], fY[NVT_MAX_TOUCHES];

	volatile uint32_t *reg(int block, uint32_t offset);
	bool qupState(uint32_t state);
	bool setState(uint32_t state);
	bool i2cRead(uint8_t addr, uint8_t regAddr, uint8_t *buf, uint32_t len);
	void powerOn();
	void report(const uint8_t *buf);
	static void poll(OSObject *owner, IOTimerEventSource *sender);
};

OSDefineMetaClassAndStructors(TulipTouch, IOHIDEventService);

/* pos / size as a 16.16 fraction, clamped to [0, 1). */
static IOFixed
unitFixed(int32_t pos, int32_t size)
{
	int64_t v = ((int64_t)pos << 16) / size;
	return (IOFixed)(v < 0 ? 0 : (v > 0xffff ? 0xffff : v));
}

volatile uint32_t *
TulipTouch::reg(int block, uint32_t offset)
{
	return (volatile uint32_t *)(fMap[block]->getVirtualAddress() + offset);
}

bool
TulipTouch::qupState(uint32_t state)
{
	for (int i = 0; i < 100; i++) {
		uint32_t s = *reg(kRegQUP, QUP_STATE);
		if ((s & QUP_STATE_VALID) && (s & QUP_STATE_MASK) == state) {
			return true;
		}
		IODelay(1);
	}
	return false;
}

bool
TulipTouch::setState(uint32_t state)
{
	for (int i = 0; i < 100 && !(*reg(kRegQUP, QUP_STATE) & QUP_STATE_VALID); i++) {
		IODelay(1);
	}
	*reg(kRegQUP, QUP_STATE) = state;
	return qupState(state);
}

/* One combined transfer: START, write the register address, repeated START, read len bytes, STOP. */
bool
TulipTouch::i2cRead(uint8_t addr, uint8_t regAddr, uint8_t *buf, uint32_t len)
{
	const uint8_t tags[] = {
		TAG_START, (uint8_t)(addr << 1), TAG_DATA_WRITE, 1, regAddr,
		TAG_START, (uint8_t)(addr << 1 | 1), TAG_DATA_READ_STOP, (uint8_t)len,
	};
	uint32_t rx = RX_TAG_BYTES + len, got = 0;
	uint8_t data[RX_TAG_BYTES + NVT_MAX_TOUCHES * NVT_TOUCH_SIZE];
	if (len == 0 || rx > sizeof(data)) {
		return false;
	}
	*reg(kRegQUP, QUP_SW_RESET) = 1;
	if (!qupState(QUP_STATE_RESET)) {
		return false;
	}
	*reg(kRegQUP, QUP_CONFIG) = QUP_CONFIG_I2C;
	*reg(kRegQUP, QUP_I2C_MASTER_GEN) = QUP_V2_TAGS_EN;
	for (int i = 0; i < 100 && !(*reg(kRegQUP, QUP_STATE) & QUP_STATE_I2C_MAST_GEN); i++) {
		IODelay(1);
	}
	*reg(kRegQUP, QUP_IO_MODE) = QUP_IO_MODE_REPACK;
	*reg(kRegQUP, QUP_MX_OUTPUT_CNT) = 0;
	*reg(kRegQUP, QUP_MX_INPUT_CNT) = 0;
	*reg(kRegQUP, QUP_MX_WRITE_CNT) = sizeof(tags);
	*reg(kRegQUP, QUP_MX_READ_CNT) = rx;

	if (!setState(QUP_STATE_RUN)) {
		return false;
	}
	*reg(kRegQUP, QUP_I2C_CLK_CTL) = QUP_CLK_CTL_400K;
	if (!setState(QUP_STATE_PAUSE)) {
		return false;
	}
	for (uint32_t i = 0; i < sizeof(tags); i += 4) {
		uint32_t word = 0;
		for (uint32_t j = 0; j < 4 && i + j < sizeof(tags); j++) {
			word |= (uint32_t)tags[i + j] << (8 * j);
		}
		*reg(kRegQUP, QUP_OUT_FIFO_BASE) = word;
	}
	bool ok = setState(QUP_STATE_RUN);
	uint64_t deadline;
	clock_interval_to_deadline(20, NSEC_PER_MSEC, &deadline);
	while (ok && got < rx) {
		uint32_t i2c = *reg(kRegQUP, QUP_I2C_STATUS) & I2C_STATUS_ERROR_MASK;
		uint32_t qup = *reg(kRegQUP, QUP_ERROR_FLAGS) & QUP_ERROR_MASK;
		if (i2c || qup || mach_absolute_time() > deadline) {
			if (!(i2c & I2C_STATUS_NACK)) {
				kprintf("TulipTouch: I2C 0x%02x reg 0x%02x failed: i2c 0x%x qup 0x%x, %u of %u bytes\n",
				    addr, regAddr, i2c, qup, got, rx);
			}
			*reg(kRegQUP, QUP_I2C_STATUS) = i2c;
			*reg(kRegQUP, QUP_ERROR_FLAGS) = qup;
			ok = false;
			break;
		}
		if (*reg(kRegQUP, QUP_OPERATIONAL) & QUP_OP_IN_NOT_EMPTY) {
			uint32_t word = *reg(kRegQUP, QUP_IN_FIFO_BASE);
			for (int j = 0; j < 4 && got < rx; j++, word >>= 8) {
				data[got++] = (uint8_t)word;
			}
		}
	}
	for (int i = 0; ok && i < 1000 && (*reg(kRegQUP, QUP_I2C_STATUS) & I2C_STATUS_BUS_ACTIVE); i++) {
		IODelay(1);
	}
	setState(QUP_STATE_RESET);
	if (ok) {
		memcpy(buf, data + RX_TAG_BYTES, len);
	}
	return ok;
}

void
TulipTouch::powerOn()
{
	*reg(kRegClockVote, GCC_BLSP1_AHB_VOTE) |= GCC_BLSP1_AHB_VOTE_BIT;
	*reg(kRegClock, GCC_QUP1_I2C_CFG_RCGR) = 0;
	*reg(kRegClock, GCC_QUP1_I2C_CMD_RCGR) |= RCGR_UPDATE;
	for (int i = 0; i < 1000 && (*reg(kRegClock, GCC_QUP1_I2C_CMD_RCGR) & RCGR_UPDATE); i++) {
		IODelay(1);
	}
	*reg(kRegClock, GCC_QUP1_I2C_CBCR) |= CBCR_ENABLE;
	for (int i = 0; i < 1000 && (*reg(kRegClock, GCC_QUP1_I2C_CBCR) & CBCR_OFF); i++) {
		IODelay(1);
	}

	/* GPIO 2 and 3 (SDA, SCL) to blsp_i2c1; GPIO 66 (reset) and 67 (interrupt) as in Android's ts_active. */
	*reg(kRegI2CPins, 0x0000 + PIN_CFG) = PIN_CFG_I2C;
	*reg(kRegI2CPins, 0x1000 + PIN_CFG) = PIN_CFG_I2C;
	*reg(kRegTouchPins, 0x0000 + PIN_IO) = PIN_IO_OUT;
	*reg(kRegTouchPins, 0x0000 + PIN_CFG) = PIN_CFG_GPIO_PULL_UP | PIN_CFG_OUTPUT;
	*reg(kRegTouchPins, 0x1000 + PIN_CFG) = PIN_CFG_GPIO_PULL_UP;
	*reg(kRegTouchPins, 0x1000 + PIN_INTR_CFG) = PIN_INTR_FALLING_RAW;
	*reg(kRegTouchPins, 0x1000 + PIN_INTR_STATUS) = 0;
}

bool
TulipTouch::start(IOService *provider)
{
	for (int i = 0; i < 5; i++) {
		fMap[i] = provider->mapDeviceMemoryWithIndex(i);
		if (!fMap[i]) {
			IOLog("TulipTouch: no register block %d\n", i);
			return false;
		}
	}
	powerOn();
	kprintf("TulipTouch: QUP version 0x%08x, io mode 0x%08x, clock 0x%08x\n", *reg(kRegQUP, QUP_HW_VERSION),
	    *reg(kRegQUP, QUP_IO_MODE), *reg(kRegClock, GCC_QUP1_I2C_CBCR));

	uint8_t params[NVT_PARAMS_SIZE];
	bool found = i2cRead(NVT_ADDR, NVT_PARAMS_START, params, sizeof(params));
	if (!found) {
		*reg(kRegTouchPins, PIN_IO) = 0;
		IOSleep(10);
		*reg(kRegTouchPins, PIN_IO) = PIN_IO_OUT;
		IOSleep(200);
		found = i2cRead(NVT_ADDR, NVT_PARAMS_START, params, sizeof(params));
	}
	if (!found) {
		IOLog("TulipTouch: no answer from the touch controller\n");
		return false;
	}
	fWidth = (uint32_t)params[4] << 8 | params[5];
	fHeight = (uint32_t)params[6] << 8 | params[7];
	IOLog("TulipTouch: %ux%u, %u touches, chip 0x%02x\n", fWidth, fHeight, params[9], params[14]);
	if (fWidth == 0 || fWidth > 4096 || fHeight == 0 || fHeight > 4096) {
		return false;
	}

	/* Touch coordinates are panel pixels; iOS wants the canvas that AppleCLCD shows the center of. */
	fCanvasWidth = fWidth;
	fCanvasHeight = fHeight;
	IORegistryEntry *disp = IORegistryEntry::fromPath("/arm-io/disp0", gIODTPlane);
	if (disp) {
		OSData *w = OSDynamicCast(OSData, disp->getProperty("canvas-width"));
		OSData *h = OSDynamicCast(OSData, disp->getProperty("canvas-height"));
		if (w && h && w->getLength() == 4 && h->getLength() == 4) {
			fCanvasWidth = *(const uint32_t *)w->getBytesNoCopy();
			fCanvasHeight = *(const uint32_t *)h->getBytesNoCopy();
		}
		disp->release();
	}
	/* AppleCLCD shows the canvas scaled to fit the panel and centered; touches map back through the same fit. */
	uint64_t scale = ((uint64_t)fWidth << 16) / fCanvasWidth;
	if (((uint64_t)fHeight << 16) / fCanvasHeight < scale) {
		scale = ((uint64_t)fHeight << 16) / fCanvasHeight;
	}
	if (scale > 1ULL << 16) {
		scale = 1ULL << 16;
	}
	fOutWidth = (int32_t)((fCanvasWidth * scale) >> 16);
	fOutHeight = (int32_t)((fCanvasHeight * scale) >> 16);
	fOutX = ((int32_t)fWidth - fOutWidth) / 2;
	fOutY = ((int32_t)fHeight - fOutHeight) / 2;

	if (!IOHIDEventService::start(provider)) {
		return false;
	}
	setProperty("DisplayIntegrated", kOSBooleanTrue);
	/* 68x144 mm, 16.16 fixed point. */
	setProperty("SurfaceWidth", 68ULL << 16, 32);
	setProperty("SurfaceHeight", 144ULL << 16, 32);

	IOWorkLoop *wl = getWorkLoop();
	fTimer = IOTimerEventSource::timerEventSource(this, &TulipTouch::poll);
	if (!wl || !fTimer || wl->addEventSource(fTimer) != kIOReturnSuccess) {
		return false;
	}
	registerService();
	fTimer->setTimeoutMS(POLL_MS);
	return true;
}

void
TulipTouch::report(const uint8_t *buf)
{
	bool down[NVT_MAX_TOUCHES] = {};
	IOFixed x[NVT_MAX_TOUCHES], y[NVT_MAX_TOUCHES];

	memcpy(x, fX, sizeof(x));
	memcpy(y, fY, sizeof(y));
	for (int i = 0; i < NVT_MAX_TOUCHES; i++) {
		const uint8_t *t = buf + i * NVT_TOUCH_SIZE;
		int type = t[0] & 7, slot = (t[0] >> 3) - 1;
		if (t[0] == NVT_TOUCH_INVALID || slot < 0 || slot >= NVT_MAX_TOUCHES ||
		    (type != NVT_TOUCH_NEW && type != NVT_TOUCH_UPDATE)) {
			continue;
		}
		int32_t px = (int32_t)((uint32_t)t[1] << 4 | t[3] >> 4) - fOutX;
		int32_t py = (int32_t)((uint32_t)t[2] << 4 | (t[3] & 0xf)) - fOutY;
		down[slot] = true;
		x[slot] = unitFixed(px, fOutWidth);
		y[slot] = unitFixed(py, fOutHeight);
	}

	uint64_t now = mach_absolute_time();
	IOHIDEvent *collection = IOHIDEvent::digitizerEvent(now, 0, kIOHIDDigitizerTransducerTypeHand, false, 0, 0, 0);
	if (!collection) {
		return;
	}
	collection->setIntegerValue(kIOHIDEventFieldDigitizerCollection, TRUE);
	UInt32 collectionMask = 0;
	bool any = false, changed = false;
	IOFixed cx = 0, cy = 0;
	for (int i = 0; i < NVT_MAX_TOUCHES; i++) {
		if (!down[i] && !fDown[i]) {
			continue;
		}
		UInt32 mask = 0;
		if (down[i] != fDown[i]) {
			mask |= kIOHIDDigitizerEventTouch | kIOHIDDigitizerEventRange;
		}
		if (down[i] && !fDown[i]) {
			mask |= kIOHIDDigitizerEventIdentity;
		}
		if (down[i] && fDown[i] && (x[i] != fX[i] || y[i] != fY[i])) {
			mask |= kIOHIDDigitizerEventPosition;
		}
		IOHIDEvent *finger = IOHIDEvent::digitizerEvent(now, (UInt32)i + 2, kIOHIDDigitizerTransducerTypeFinger,
		    down[i], down[i] ? 1 : 0, x[i], y[i]);
		if (finger) {
			finger->setIntegerValue(kIOHIDEventFieldDigitizerIndex, i);
			finger->setIntegerValue(kIOHIDEventFieldDigitizerIdentity, i + 2);
			finger->setIntegerValue(kIOHIDEventFieldDigitizerTouch, down[i]);
			finger->setIntegerValue(kIOHIDEventFieldDigitizerEventMask, mask);
			collection->appendChild(finger);
			finger->release();
		}
		if (down[i] != fDown[i]) {
			kprintf("TulipTouch: finger %d %s\n", i, down[i] ? "down" : "up");
		}
		if (down[i] && !any) {
			cx = x[i];
			cy = y[i];
		}
		collectionMask |= mask;
		any |= down[i];
		changed |= mask != 0;
		fDown[i] = down[i];
		fX[i] = x[i];
		fY[i] = y[i];
	}
	if (changed) {
		collection->setFixedValue(kIOHIDEventFieldDigitizerX, cx);
		collection->setFixedValue(kIOHIDEventFieldDigitizerY, cy);
		collection->setIntegerValue(kIOHIDEventFieldDigitizerRange, any);
		collection->setIntegerValue(kIOHIDEventFieldDigitizerTouch, any);
		collection->setIntegerValue(kIOHIDEventFieldDigitizerEventMask, collectionMask);
		collection->setIntegerValue(kIOHIDEventFieldDigitizerButtonMask, any ? 1 : 0);
		dispatchEvent(collection);
	}
	collection->release();
}

void
TulipTouch::poll(OSObject *owner, IOTimerEventSource *sender)
{
	TulipTouch *self = (TulipTouch *)owner;
	volatile uint32_t *status = self->reg(kRegTouchPins, 0x1000 + PIN_INTR_STATUS);
	bool line = !(*self->reg(kRegTouchPins, 0x1000 + PIN_IO) & PIN_IO_IN);
	bool any = false;

	for (int i = 0; i < NVT_MAX_TOUCHES; i++) {
		any |= self->fDown[i];
	}
	if (*status || line || any) {
		*status = 0;
		uint8_t buf[NVT_MAX_TOUCHES * NVT_TOUCH_SIZE];
		if (self->i2cRead(NVT_ADDR, NVT_TOUCH_START, buf, sizeof(buf))) {
			self->report(buf);
		}
	}
	sender->setTimeoutMS(POLL_MS);
}
