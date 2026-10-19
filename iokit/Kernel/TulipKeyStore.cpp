/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>

extern "C" {
#include <machine/atomic.h>
#include <sys/proc.h>
}

/* Stands in for Apple's AppleKeyStore: every call succeeds; only the lock state is answered, as unlocked. */
class AppleKeyStore : public IOService
{
	OSDeclareDefaultStructors(AppleKeyStore);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
	    IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

class AppleKeyStoreUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleKeyStoreUserClient);

public:
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleKeyStore, IOService);
OSDefineMetaClassAndStructors(AppleKeyStoreUserClient, IOUserClient);

static uint64_t tulip_keystore_selectors_seen[2];

/* Selectors of iOS 16's AppleKeyStore.framework. */
enum {
	kSelGetLockState = 7,
	kSelGetDeviceState = 17,
	kSelGetExtendedDeviceState = 35,
};

#define LOCK_STATE_UNLOCKED_SINCE_BOOT 0x4

/* DER SET { SEQUENCE { UTF8String "ss", INTEGER 4 }, SEQUENCE { UTF8String "sls", INTEGER 0 } }: unlocked. */
static const uint8_t gDeviceState[] = {
	0x31, 0x13,
	0x30, 0x07, 0x0c, 0x02, 's', 's', 0x02, 0x01, 0x04,
	0x30, 0x08, 0x0c, 0x03, 's', 'l', 's', 0x02, 0x01, 0x00,
};

bool
AppleKeyStore::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	registerService();
	return true;
}

IOReturn
AppleKeyStore::newUserClient(task_t owningTask, void *securityID __unused, UInt32 type, IOUserClient **handler)
{
	AppleKeyStoreUserClient *client = OSTypeAlloc(AppleKeyStoreUserClient);

	if (!client || !client->initWithTask(owningTask, NULL, type) || !client->attach(this) ||
	    !client->start(this)) {
		if (client) {
			client->detach(this);
			client->release();
		}
		return kIOReturnNoMemory;
	}
	*handler = client;
	return kIOReturnSuccess;
}

IOReturn
AppleKeyStoreUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn
AppleKeyStoreUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch __unused, OSObject *target __unused, void *reference __unused)
{
	if (selector < 128) {
		uint64_t bit = 1ULL << (selector % 64);
		if (!(os_atomic_or_orig(&tulip_keystore_selectors_seen[selector / 64], bit, relaxed) & bit)) {
			char name[32];
			proc_selfname(name, sizeof(name));
			kprintf("TulipKeyStore: %s selector %u in %u/%u out %u/%u\n", name, selector,
			    args->scalarInputCount, args->structureInputSize, args->scalarOutputCount,
			    args->structureOutputSize);
		}
	}
	for (uint32_t i = 0; i < args->scalarOutputCount; i++) {
		args->scalarOutput[i] = 0;
	}
	if (args->structureOutput && args->structureOutputSize) {
		bzero(args->structureOutput, args->structureOutputSize);
	}
	if (args->structureOutputDescriptor) {
		args->structureOutputDescriptorSize = 0;
	}
	if (selector == kSelGetLockState && args->scalarOutputCount >= 1) {
		args->scalarOutput[0] = LOCK_STATE_UNLOCKED_SINCE_BOOT;
	} else if ((selector == kSelGetDeviceState || selector == kSelGetExtendedDeviceState) && args->structureOutput) {
		if (args->structureOutputSize < sizeof(gDeviceState)) {
			return kIOReturnNoSpace;
		}
		memcpy(args->structureOutput, gDeviceState, sizeof(gDeviceState));
		args->structureOutputSize = sizeof(gDeviceState);
	}
	return kIOReturnSuccess;
}
