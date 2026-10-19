/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <sys/param.h>
#include <sys/proc.h>
#include <sys/priv.h>
#include <machine/atomic.h>
#include <pexpert/pexpert.h>
#include <security/mac_policy.h>

void tulip_sandbox_policy_init(void);

static int
tulip_sandbox_syscall(proc_t p __unused, int cmd __unused, user_addr_t arg __unused)
{
	return 0;
}

static uint64_t tulip_sandbox_denied[256];

/* Apple's Sandbox grants privileges by entitlement; grant the ones iOS needs and log the others once. */
static int
tulip_sandbox_priv_grant(kauth_cred_t cred __unused, int priv)
{
	if (priv == PRIV_WORK_INTERVAL) {
		return 0;
	}
	if (priv >= 0 && priv < 256 * 64) {
		uint64_t bit = 1ULL << (priv % 64);
		if (!(os_atomic_or_orig(&tulip_sandbox_denied[priv / 64], bit, relaxed) & bit)) {
			char name[32];
			proc_selfname(name, sizeof(name));
			kprintf("tulip_sandbox: %s denied privilege %d\n", name, priv);
		}
	}
	return EPERM;
}

SECURITY_READ_ONLY_EARLY(static struct mac_policy_ops) tulip_sandbox_ops = {
	.mpo_policy_syscall = tulip_sandbox_syscall,
	.mpo_priv_grant = tulip_sandbox_priv_grant,
};

SECURITY_READ_ONLY_LATE(static struct mac_policy_conf) tulip_sandbox_conf = {
	.mpc_name = "Sandbox",
	.mpc_fullname = "ios-tulip allow-all sandbox",
	.mpc_ops = &tulip_sandbox_ops,
	.mpc_loadtime_flags = 0,
	.mpc_runtime_flags = 0,
};

SECURITY_READ_ONLY_LATE(static mac_policy_handle_t) tulip_sandbox_handle;

/* Register a policy named "Sandbox" that answers every sandbox query with "allowed" and grants the work interval privilege. */
void
tulip_sandbox_policy_init(void)
{
	mac_policy_register(&tulip_sandbox_conf, &tulip_sandbox_handle, NULL);
}
