/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOService.h>
#include <IOKit/IOUserClient.h>
#include <IOKit/IOLib.h>

extern "C" {
#include <machine/atomic.h>
}

/* Stands in for Apple's AMFI IOKit service: every request to its user client succeeds. */
class AppleMobileFileIntegrity : public IOService
{
	OSDeclareDefaultStructors(AppleMobileFileIntegrity);

public:
	virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
	virtual IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
	    IOUserClient **handler) APPLE_KEXT_OVERRIDE;
};

class AppleMobileFileIntegrityUserClient : public IOUserClient
{
	OSDeclareDefaultStructors(AppleMobileFileIntegrityUserClient);

public:
	virtual IOReturn clientClose(void) APPLE_KEXT_OVERRIDE;
	virtual IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args,
	    IOExternalMethodDispatch *dispatch, OSObject *target, void *reference) APPLE_KEXT_OVERRIDE;
};

OSDefineMetaClassAndStructors(AppleMobileFileIntegrity, IOService);
OSDefineMetaClassAndStructors(AppleMobileFileIntegrityUserClient, IOUserClient);

static uint64_t tulip_amfi_selectors_seen;

bool
AppleMobileFileIntegrity::start(IOService *provider)
{
	if (!IOService::start(provider)) {
		return false;
	}
	registerService();
	return true;
}

IOReturn
AppleMobileFileIntegrity::newUserClient(task_t owningTask, void *securityID __unused, UInt32 type,
    IOUserClient **handler)
{
	AppleMobileFileIntegrityUserClient *client = OSTypeAlloc(AppleMobileFileIntegrityUserClient);

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
AppleMobileFileIntegrityUserClient::clientClose(void)
{
	terminate();
	return kIOReturnSuccess;
}

IOReturn
AppleMobileFileIntegrityUserClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args,
    IOExternalMethodDispatch *dispatch __unused, OSObject *target __unused, void *reference __unused)
{
	if (selector < 64 && !(os_atomic_or_orig(&tulip_amfi_selectors_seen, 1ULL << selector, relaxed) &
	    (1ULL << selector))) {
		IOLog("TulipAMFI: selector %u (scalars in %u, struct in %u) accepted\n", selector,
		    args->scalarInputCount, args->structureInputSize);
	}
	args->scalarOutputCount = 0;
	args->structureOutputSize = 0;
	return kIOReturnSuccess;
}
