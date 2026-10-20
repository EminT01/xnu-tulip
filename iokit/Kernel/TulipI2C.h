/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#ifndef _IOKIT_TULIPI2C_H
#define _IOKIT_TULIPI2C_H

#include <IOKit/IOService.h>
#include <IOKit/IOLocks.h>
#include <IOKit/IOMemoryDescriptor.h>

#define TULIP_I2C_MAX_WRITE 24
#define TULIP_I2C_MAX_READ  255

/* A Qualcomm BLSP QUP v2 in I2C master mode: FIFO mode with v2 tags, polled, 400 kHz. */
class TulipI2C : public IOService
{
	OSDeclareDefaultStructors(TulipI2C);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;

	/* Finds the bus that a device node names in its "i2c-bus" property. */
	static TulipI2C *forDevice(IOService *device);

	/* Writes wlen bytes, then reads rlen bytes after a repeated START; either length may be 0. */
	bool transfer(uint8_t addr, const uint8_t *wbuf, uint32_t wlen, uint8_t *rbuf, uint32_t rlen);
	bool readRegister(uint8_t addr, uint8_t reg, uint8_t *buf, uint32_t len);

private:
	IOMemoryMap *fMap;
	IOLock *fLock;
	uint8_t fRx[2 + TULIP_I2C_MAX_READ];

	volatile uint32_t *reg(uint32_t offset);
	bool qupState(uint32_t state);
	bool setState(uint32_t state);
	bool powerOn(IOService *provider);
};

#endif /* _IOKIT_TULIPI2C_H */
