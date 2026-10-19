/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <sys/types.h>
#include <sys/proc.h>
#include <kern/startup.h>
#include <libkern/amfi/amfi.h>

static void
tulip_osent_invalidate(void *osentitlements __unused)
{
}

static void *
tulip_osent_asdict(void *osentitlements __unused)
{
	return NULL;
}

static bool
tulip_osent_get_transmuted(void *osentitlements __unused, const CS_GenericBlob **blob)
{
	*blob = NULL;
	return false;
}

static bool
tulip_osent_get_xml(void *osentitlements __unused, CS_GenericBlob **blob)
{
	*blob = NULL;
	return false;
}

static kern_return_t
tulip_adjust_with_monitor(void *os_entitlements __unused, const CEQueryContext_t ce_ctx __unused,
    const void *monitor_sig_obj __unused, const char *identity __unused, const uint32_t flags __unused)
{
	return KERN_NOT_SUPPORTED;
}

static kern_return_t
tulip_adjust_without_monitor(void *os_entitlements __unused, struct cs_blob *blob __unused)
{
	return KERN_SUCCESS;
}

static kern_return_t
tulip_query_bool(const void *os_entitlements __unused, const char *name __unused)
{
	return KERN_DENIED;
}

static kern_return_t
tulip_query_bool_proc(const proc_t proc __unused, const char *name __unused)
{
	return KERN_DENIED;
}

static kern_return_t
tulip_query_string(const void *os_entitlements __unused, const char *name __unused, const char *value __unused)
{
	return KERN_DENIED;
}

static kern_return_t
tulip_query_string_proc(const proc_t proc __unused, const char *name __unused, const char *value __unused)
{
	return KERN_DENIED;
}

static kern_return_t
tulip_copy_object(const void *os_entitlements __unused, const char *name __unused, void **object)
{
	*object = NULL;
	return KERN_NOT_FOUND;
}

static kern_return_t
tulip_copy_object_proc(const proc_t proc __unused, const char *name __unused, void **object)
{
	*object = NULL;
	return KERN_NOT_FOUND;
}

static const amfi_t tulip_amfi = {
	.OSEntitlements_invalidate = tulip_osent_invalidate,
	.OSEntitlements_asdict = tulip_osent_asdict,
	.OSEntitlements_get_transmuted = tulip_osent_get_transmuted,
	.OSEntitlements_get_xml = tulip_osent_get_xml,
	.OSEntitlements = {
		.version = OSENTITLEMENTS_INTERFACE_VERSION,
		.adjustContextWithMonitor = tulip_adjust_with_monitor,
		.adjustContextWithoutMonitor = tulip_adjust_without_monitor,
		.queryEntitlementBoolean = tulip_query_bool,
		.queryEntitlementBooleanWithProc = tulip_query_bool_proc,
		.queryEntitlementString = tulip_query_string,
		.queryEntitlementStringWithProc = tulip_query_string_proc,
		.copyEntitlementAsOSObject = tulip_copy_object,
		.copyEntitlementAsOSObjectWithProc = tulip_copy_object_proc,
	},
};

static void
tulip_amfi_register(void)
{
	amfi_interface_register(&tulip_amfi);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_FIRST, tulip_amfi_register);
