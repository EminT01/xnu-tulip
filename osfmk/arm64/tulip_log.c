/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#define TULIP_LOG_SIZE 262144
#define TULIP_CRUMB_MAGIC 0x54554c4950435242ULL /* "TULIPCRB" */

uint64_t tulip_crumbs[64] __attribute__((section("__DATA,__data"))) = { TULIP_CRUMB_MAGIC };
char tulip_log[TULIP_LOG_SIZE] __attribute__((section("__DATA,__data"))) = { ' ' };
uint64_t tulip_log_pos __attribute__((section("__DATA,__data"))) = 1;

static inline void
tulip_clean_line(const void *addr)
{
	__asm__ volatile ("dc cvac, %0" :: "r"(addr) : "memory");
	__asm__ volatile ("dsb sy" ::: "memory");
}

void tulip_crumb(unsigned int idx, uint64_t val);
void tulip_log_putc(char c);

void
tulip_crumb(unsigned int idx, uint64_t val)
{
	if (idx >= 64) {
		return;
	}
	tulip_crumbs[0] = TULIP_CRUMB_MAGIC;
	tulip_crumbs[idx] = val;
	tulip_clean_line(&tulip_crumbs[0]);
	tulip_clean_line(&tulip_crumbs[idx]);
}

void
tulip_log_putc(char c)
{
	uint64_t pos = tulip_log_pos;
	tulip_log[pos % TULIP_LOG_SIZE] = c;
	tulip_log_pos = pos + 1;
	if (c == '\n') {
		tulip_clean_line(&tulip_log[pos % TULIP_LOG_SIZE]);
		tulip_clean_line((const void *)&tulip_log_pos);
	}
}
