/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_static_vtop(vm_offset_t vaddr);

/* Print the SDHCI state ABL left and the GPT partitions, reading only. */
void tulip_emmc_probe(void);

#define SDHC1_HC_BASE           0x0c0c4000UL
#define SDHC1_CQE_BASE          0x0c0c5000UL

#define SDHCI_BLOCK_SIZE        0x04
#define SDHCI_BLOCK_COUNT       0x06
#define SDHCI_ARGUMENT          0x08
#define SDHCI_TRANSFER_MODE     0x0c
#define SDHCI_RESPONSE          0x10
#define SDHCI_BUFFER            0x20
#define SDHCI_PRESENT_STATE     0x24
#define SDHCI_HOST_CONTROL      0x28
#define SDHCI_CLOCK_CONTROL     0x2c
#define SDHCI_INT_STATUS        0x30
#define SDHCI_INT_ENABLE        0x34
#define SDHCI_SIGNAL_ENABLE     0x38
#define SDHCI_HOST_CONTROL2     0x3c
#define SDHCI_CAPABILITIES      0x40

#define SDHCI_CMD_INHIBIT       (1u << 0)
#define SDHCI_DAT_INHIBIT       (1u << 1)

#define SDHCI_INT_CMD_COMPLETE  (1u << 0)
#define SDHCI_INT_XFER_COMPLETE (1u << 1)
#define SDHCI_INT_BUF_READ      (1u << 5)
#define SDHCI_INT_ERROR         (1u << 15)

#define SDHCI_TRNS_BLK_CNT_EN   (1u << 1)
#define SDHCI_TRNS_READ         (1u << 4)

#define SDHCI_CMD_RESP_48       0x02
#define SDHCI_CMD_CRC           0x08
#define SDHCI_CMD_INDEX         0x10
#define SDHCI_CMD_DATA          0x20

#define CQHCI_CFG               0x08
#define CQHCI_CFG_ENABLE        (1u << 0)

#define MMC_READ_SINGLE_BLOCK   17

static vm_offset_t emmc_hc;
static vm_offset_t emmc_cqe;

static uint32_t
hc_read32(uint32_t off)
{
	return *(volatile uint32_t *)(emmc_hc + off);
}

static void
hc_write32(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(emmc_hc + off) = val;
}

static bool
hc_wait(uint32_t mask, bool want_set, const char *what)
{
	uint64_t deadline;

	nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &deadline);
	deadline += mach_absolute_time();
	for (;;) {
		uint32_t st = (mask == (SDHCI_CMD_INHIBIT | SDHCI_DAT_INHIBIT)) ?
		    hc_read32(SDHCI_PRESENT_STATE) : hc_read32(SDHCI_INT_STATUS);
		if (!want_set && !(st & mask)) {
			return true;
		}
		if (want_set && (st & mask)) {
			return true;
		}
		if (want_set && (st & SDHCI_INT_ERROR) && mask != (SDHCI_CMD_INHIBIT | SDHCI_DAT_INHIBIT)) {
			kprintf("tulip_emmc: %s: error, int_status 0x%08x\n", what, st);
			return false;
		}
		if (mach_absolute_time() > deadline) {
			kprintf("tulip_emmc: %s: timeout, present 0x%08x int_status 0x%08x\n", what,
			    hc_read32(SDHCI_PRESENT_STATE), hc_read32(SDHCI_INT_STATUS));
			return false;
		}
	}
}

struct adma2_desc {
	uint16_t attr;
	uint16_t len;
	uint32_t addr_lo;
	uint32_t addr_hi;
} __attribute__((packed));

#define ADMA2_VALID             (1u << 0)
#define ADMA2_END               (1u << 1)
#define ADMA2_ACT_TRAN          (2u << 4)
#define SDHCI_ADMA_ADDRESS      0x58
#define SDHCI_ADMA_ERROR        0x54
#define SDHCI_TRNS_DMA          (1u << 0)
#define SDHCI_CTRL_ADMA64       (3u << 3)

static struct {
	struct adma2_desc desc[1];
	uint8_t pad[64 - sizeof(struct adma2_desc)];
	uint8_t data[512];
} emmc_dma __attribute__((aligned(64)));

static void
dcache_clean_inval(const void *va, size_t len)
{
	for (uintptr_t a = (uintptr_t)va & ~63UL; a < (uintptr_t)va + len; a += 64) {
		__asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static int
emmc_read_block(uint32_t lba, uint8_t *out)
{
	uint64_t data_pa = ml_static_vtop((vm_offset_t)emmc_dma.data);
	uint64_t desc_pa = ml_static_vtop((vm_offset_t)emmc_dma.desc);

	if (!hc_wait(SDHCI_CMD_INHIBIT | SDHCI_DAT_INHIBIT, false, "idle")) {
		return EIO;
	}
	emmc_dma.desc[0].attr = ADMA2_VALID | ADMA2_END | ADMA2_ACT_TRAN;
	emmc_dma.desc[0].len = 512;
	emmc_dma.desc[0].addr_lo = (uint32_t)data_pa;
	emmc_dma.desc[0].addr_hi = (uint32_t)(data_pa >> 32);
	dcache_clean_inval(&emmc_dma, sizeof(emmc_dma));

	hc_write32(SDHCI_HOST_CONTROL, (hc_read32(SDHCI_HOST_CONTROL) & ~0x18u) | SDHCI_CTRL_ADMA64);
	hc_write32(SDHCI_ADMA_ADDRESS, (uint32_t)desc_pa);
	hc_write32(SDHCI_ADMA_ADDRESS + 4, (uint32_t)(desc_pa >> 32));
	hc_write32(SDHCI_INT_STATUS, 0xffffffff);
	hc_write32(SDHCI_BLOCK_SIZE, (1u << 16) | 512);
	hc_write32(SDHCI_ARGUMENT, lba);
	uint32_t cmd = (MMC_READ_SINGLE_BLOCK << 8) | SDHCI_CMD_DATA | SDHCI_CMD_INDEX |
	    SDHCI_CMD_CRC | SDHCI_CMD_RESP_48;
	hc_write32(SDHCI_TRANSFER_MODE, (cmd << 16) | SDHCI_TRNS_READ | SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_DMA);

	if (!hc_wait(SDHCI_INT_CMD_COMPLETE, true, "cmd17")) {
		return EIO;
	}
	if (!hc_wait(SDHCI_INT_XFER_COMPLETE, true, "dma")) {
		kprintf("tulip_emmc: adma error 0x%08x desc_pa 0x%llx data_pa 0x%llx\n",
		    hc_read32(SDHCI_ADMA_ERROR), desc_pa, data_pa);
		return EIO;
	}
	hc_write32(SDHCI_INT_STATUS, 0xffffffff);
	dcache_clean_inval(emmc_dma.data, 512);
	memcpy(out, emmc_dma.data, 512);
	return 0;
}

static void
gpt_name(const uint8_t *utf16, char *out, size_t outlen)
{
	size_t i;

	for (i = 0; i + 1 < outlen && i < 36 && utf16[2 * i]; i++) {
		out[i] = (char)utf16[2 * i];
	}
	out[i] = 0;
}

static uint64_t
le64(const uint8_t *p)
{
	uint64_t v;

	memcpy(&v, p, 8);
	return v;
}

static uint32_t
le32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, 4);
	return v;
}

void
tulip_emmc_probe(void)
{
	static uint8_t buf[512];

	emmc_hc = ml_io_map(SDHC1_HC_BASE, 0x1000);
	emmc_cqe = ml_io_map(SDHC1_CQE_BASE, 0x1000);

	uint32_t cqcfg = *(volatile uint32_t *)(emmc_cqe + CQHCI_CFG);
	kprintf("tulip_emmc: present 0x%08x hostctl 0x%08x clock 0x%08x hostctl2 0x%08x caps 0x%08x cqcfg 0x%08x\n",
	    hc_read32(SDHCI_PRESENT_STATE), hc_read32(SDHCI_HOST_CONTROL), hc_read32(SDHCI_CLOCK_CONTROL),
	    hc_read32(SDHCI_HOST_CONTROL2), hc_read32(SDHCI_CAPABILITIES), cqcfg);
	if (cqcfg & CQHCI_CFG_ENABLE) {
		*(volatile uint32_t *)(emmc_cqe + CQHCI_CFG) = cqcfg & ~CQHCI_CFG_ENABLE;
		kprintf("tulip_emmc: CQE was on, now cqcfg 0x%08x\n", *(volatile uint32_t *)(emmc_cqe + CQHCI_CFG));
	}

	uint32_t clk = hc_read32(SDHCI_CLOCK_CONTROL);
	hc_write32(SDHCI_CLOCK_CONTROL, (clk & 0x00ffffffu) | (0x06u << 24));
	uint64_t rdl;
	nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &rdl);
	rdl += mach_absolute_time();
	while ((hc_read32(SDHCI_CLOCK_CONTROL) >> 24) & 0x06) {
		if (mach_absolute_time() > rdl) {
			kprintf("tulip_emmc: CMD/DATA reset timeout\n");
			break;
		}
	}
	kprintf("tulip_emmc: after reset: present 0x%08x hostctl 0x%08x clock 0x%08x\n",
	    hc_read32(SDHCI_PRESENT_STATE), hc_read32(SDHCI_HOST_CONTROL), hc_read32(SDHCI_CLOCK_CONTROL));

	hc_write32(SDHCI_SIGNAL_ENABLE, 0);
	hc_write32(SDHCI_INT_ENABLE, 0xffffffff);
	hc_write32(SDHCI_INT_STATUS, 0xffffffff);

	if (emmc_read_block(1, buf) != 0) {
		return;
	}
	if (memcmp(buf, "EFI PART", 8) != 0) {
		kprintf("tulip_emmc: LBA 1 is not a GPT header: %02x %02x %02x %02x %02x %02x %02x %02x\n",
		    buf[0], buf[1], buf[2], buf[3], buf[4], buf[5], buf[6], buf[7]);
		return;
	}
	uint64_t entries_lba = le64(buf + 72);
	uint32_t nentries = le32(buf + 80);
	uint32_t esize = le32(buf + 84);
	kprintf("tulip_emmc: GPT: %u entries of %u bytes at LBA %llu, disk last LBA %llu\n",
	    nentries, esize, entries_lba, le64(buf + 32));
	if (esize != 128 || nentries > 256) {
		return;
	}

	unsigned found = 0;
	for (uint32_t i = 0; i < nentries; i++) {
		if (i % 4 == 0 && emmc_read_block((uint32_t)(entries_lba + i / 4), buf) != 0) {
			return;
		}
		const uint8_t *e = buf + (i % 4) * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		if (first == 0) {
			continue;
		}
		char name[37];
		gpt_name(e + 56, name, sizeof(name));
		found++;
		if (!strcmp(name, "userdata") || !strcmp(name, "cache") || !strcmp(name, "boot") ||
		    !strcmp(name, "recovery")) {
			kprintf("tulip_emmc: p%u %s: LBA %llu-%llu (%llu MiB)\n", i + 1, name, first, last,
			    (last - first + 1) / 2048);
		}
	}
	kprintf("tulip_emmc: %u partitions\n", found);
}
