/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/buf.h>
#include <sys/conf.h>
#include <sys/disk.h>
#include <sys/uio.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <miscfs/devfs/devfs.h>
#include <kern/clock.h>
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <libkern/libkern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_static_vtop(vm_offset_t vaddr);
extern int physio(void (*f_strategy)(buf_t), buf_t bp, dev_t dev, int flags,
    u_int (*f_minphys)(buf_t), struct uio *uio, int blocksize);
extern u_int minphys(buf_t bp);

/* Print the SDHCI state ABL left and the GPT partitions, reading only. */
void tulip_emmc_probe(void);
/* Return the block dev_t of the eMMC partition named by tulip_emmc_part (default userdata), or NODEV. */
dev_t tulip_emmc_rootdev(void);

#define SDHC1_HC_BASE           0x0c0c4000UL
#define SDHC1_CQE_BASE          0x0c0c5000UL
/* SDCC1 apps clock RCG in GCC: CMD, CFG (source in bits 8-10, half-integer divider in 0-4), M, N, D. */
#define GCC_SDCC1_APPS_RCG      0x0011602cUL
#define SECTOR                  512
#define BOUNCE_SIZE             (64 * 1024)
#define DESC_LEN                (16 * 1024)
#define NDESC                   (BOUNCE_SIZE / DESC_LEN)

#define SDHCI_BLOCK_SIZE        0x04
#define SDHCI_ARGUMENT          0x08
#define SDHCI_TRANSFER_MODE     0x0c
#define SDHCI_PRESENT_STATE     0x24
#define SDHCI_HOST_CONTROL      0x28
#define SDHCI_CLOCK_CONTROL     0x2c
#define SDHCI_INT_STATUS        0x30
#define SDHCI_INT_ENABLE        0x34
#define SDHCI_SIGNAL_ENABLE     0x38
#define SDHCI_HOST_CONTROL2     0x3c
#define SDHCI_CAPABILITIES      0x40
#define SDHCI_ADMA_ERROR        0x54
#define SDHCI_ADMA_ADDRESS      0x58

#define SDHCI_CMD_INHIBIT       (1u << 0)
#define SDHCI_DAT_INHIBIT       (1u << 1)

#define SDHCI_INT_CMD_COMPLETE  (1u << 0)
#define SDHCI_INT_XFER_COMPLETE (1u << 1)
#define SDHCI_INT_ERROR         (1u << 15)

#define SDHCI_TRNS_DMA          (1u << 0)
#define SDHCI_TRNS_BLK_CNT_EN   (1u << 1)
#define SDHCI_TRNS_AUTO_CMD12   (1u << 2)
#define SDHCI_TRNS_READ         (1u << 4)
#define SDHCI_TRNS_MULTI        (1u << 5)

#define SDHCI_CMD_RESP_48       0x02
#define SDHCI_CMD_RESP_48_BUSY  0x03
#define SDHCI_CMD_CRC           0x08
#define SDHCI_CMD_INDEX         0x10
#define SDHCI_CMD_DATA          0x20
#define SDHCI_CTRL_ADMA64       (3u << 3)

#define CQHCI_CFG               0x08
#define CQHCI_CFG_ENABLE        (1u << 0)

#define MMC_SWITCH              6
#define MMC_SEND_EXT_CSD        8
#define MMC_READ_SINGLE_BLOCK   17
#define MMC_READ_MULTIPLE_BLOCK 18
#define MMC_WRITE_BLOCK         24
#define MMC_WRITE_MULTIPLE_BLOCK 25

#define EXT_CSD_FLUSH_CACHE     32
#define EXT_CSD_CACHE_CTRL      33
#define EXT_CSD_CACHE_SIZE      249
#define MMC_SWITCH_WRITE_BYTE   3

#define ADMA2_VALID             (1u << 0)
#define ADMA2_END               (1u << 1)
#define ADMA2_ACT_TRAN          (2u << 4)

struct adma2_desc {
	uint16_t attr;
	uint16_t len;
	uint32_t addr_lo;
	uint32_t addr_hi;
} __attribute__((packed));

static struct {
	struct adma2_desc desc[NDESC];
	uint8_t pad[64 - (NDESC * sizeof(struct adma2_desc)) % 64];
	uint8_t data[BOUNCE_SIZE];
} emmc_dma __attribute__((aligned(16384)));

static vm_offset_t emmc_hc;
static bool emmc_ready;
static bool emmc_probed;
static bool emmc_writable;
static bool emmc_cache_on;
static uint64_t part_start;
static uint64_t part_sectors;
/* Requests read and written, bytes moved and busy time in absolute time units, for tulip_stats. */
uint64_t tulip_emmc_stats[4];

static int emmc_bmajor = -1;
static int emmc_cmajor = -1;
static LCK_GRP_DECLARE(emmc_lck_grp, "tulip_emmc");
static LCK_MTX_DECLARE(emmc_mtx, &emmc_lck_grp);

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

static void
dcache_clean_inval(const void *va, size_t len)
{
	for (uintptr_t a = (uintptr_t)va & ~63UL; a < (uintptr_t)va + len; a += 64) {
		__asm__ volatile ("dc civac, %0" :: "r"(a) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
wait_idle(void)
{
	uint64_t deadline;

	nanoseconds_to_absolutetime(2000 * NSEC_PER_MSEC, &deadline);
	deadline += mach_absolute_time();
	while (hc_read32(SDHCI_PRESENT_STATE) & (SDHCI_CMD_INHIBIT | SDHCI_DAT_INHIBIT)) {
		if (mach_absolute_time() > deadline) {
			printf("tulip_emmc: busy, present 0x%08x\n", hc_read32(SDHCI_PRESENT_STATE));
			return false;
		}
	}
	return true;
}

static bool
wait_int(uint32_t mask, const char *what)
{
	uint64_t deadline;

	nanoseconds_to_absolutetime(5000 * NSEC_PER_MSEC, &deadline);
	deadline += mach_absolute_time();
	for (;;) {
		uint32_t st = hc_read32(SDHCI_INT_STATUS);
		if (st & mask) {
			return true;
		}
		if (st & SDHCI_INT_ERROR) {
			printf("tulip_emmc: %s: error, int_status 0x%08x adma 0x%08x\n", what, st,
			    hc_read32(SDHCI_ADMA_ERROR));
			return false;
		}
		if (mach_absolute_time() > deadline) {
			printf("tulip_emmc: %s: timeout, present 0x%08x int_status 0x%08x\n", what,
			    hc_read32(SDHCI_PRESENT_STATE), st);
			return false;
		}
	}
}

static void
reset_cmd_data(void)
{
	uint32_t clk = hc_read32(SDHCI_CLOCK_CONTROL);
	uint64_t deadline;

	hc_write32(SDHCI_CLOCK_CONTROL, (clk & 0x00ffffffu) | (0x06u << 24));
	nanoseconds_to_absolutetime(100 * NSEC_PER_MSEC, &deadline);
	deadline += mach_absolute_time();
	while ((hc_read32(SDHCI_CLOCK_CONTROL) >> 24) & 0x06) {
		if (mach_absolute_time() > deadline) {
			printf("tulip_emmc: CMD/DATA reset timeout\n");
			return;
		}
	}
}

static bool
emmc_init(void)
{
	if (emmc_ready) {
		return true;
	}
	emmc_hc = ml_io_map(SDHC1_HC_BASE, 0x1000);
	vm_offset_t cqe = ml_io_map(SDHC1_CQE_BASE, 0x1000);
	if (!emmc_hc || !cqe) {
		return false;
	}
	uint32_t cqcfg = *(volatile uint32_t *)(cqe + CQHCI_CFG);
	if (cqcfg & CQHCI_CFG_ENABLE) {
		*(volatile uint32_t *)(cqe + CQHCI_CFG) = cqcfg & ~CQHCI_CFG_ENABLE;
	}
	reset_cmd_data();
	hc_write32(SDHCI_SIGNAL_ENABLE, 0);
	hc_write32(SDHCI_INT_ENABLE, 0xffffffff);
	hc_write32(SDHCI_INT_STATUS, 0xffffffff);
	emmc_ready = true;
	return true;
}

/* Time spent in each phase of emmc_xfer: setup, command, data, finish. */
static uint64_t emmc_phase_time[4];

/* Move nblocks (at most BOUNCE_SIZE / SECTOR) between emmc_dma.data and the card with ADMA2. */
static int
emmc_xfer(bool write, uint32_t index, uint32_t arg, uint32_t nblocks)
{
	uint32_t len = nblocks * SECTOR;
	uint64_t t0 = mach_absolute_time();

	if (!wait_idle()) {
		reset_cmd_data();
		return EIO;
	}
	for (int i = 0; i < NDESC; i++) {
		uint64_t pa = ml_static_vtop((vm_offset_t)emmc_dma.data + (vm_offset_t)i * DESC_LEN);
		uint32_t chunk = len > (uint32_t)i * DESC_LEN ? MIN(len - (uint32_t)i * DESC_LEN, DESC_LEN) : 0;
		bool last = chunk == 0 || len <= (uint32_t)(i + 1) * DESC_LEN;
		emmc_dma.desc[i].attr = (uint16_t)(ADMA2_VALID | ADMA2_ACT_TRAN | (last ? ADMA2_END : 0));
		emmc_dma.desc[i].len = (uint16_t)chunk;
		emmc_dma.desc[i].addr_lo = (uint32_t)pa;
		emmc_dma.desc[i].addr_hi = (uint32_t)(pa >> 32);
		if (last) {
			break;
		}
	}
	dcache_clean_inval(&emmc_dma, sizeof(emmc_dma.desc) + sizeof(emmc_dma.pad) + len);

	uint64_t desc_pa = ml_static_vtop((vm_offset_t)emmc_dma.desc);
	hc_write32(SDHCI_HOST_CONTROL, (hc_read32(SDHCI_HOST_CONTROL) & ~0x18u) | SDHCI_CTRL_ADMA64);
	hc_write32(SDHCI_ADMA_ADDRESS, (uint32_t)desc_pa);
	hc_write32(SDHCI_ADMA_ADDRESS + 4, (uint32_t)(desc_pa >> 32));
	hc_write32(SDHCI_INT_STATUS, 0xffffffff);
	hc_write32(SDHCI_BLOCK_SIZE, (nblocks << 16) | SECTOR);
	hc_write32(SDHCI_ARGUMENT, arg);

	uint32_t mode = SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_DMA | (write ? 0 : SDHCI_TRNS_READ);
	if (nblocks > 1) {
		mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_AUTO_CMD12;
	}
	uint32_t cmd = (index << 8) | SDHCI_CMD_DATA | SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_48;
	hc_write32(SDHCI_TRANSFER_MODE, (cmd << 16) | mode);
	uint64_t t1 = mach_absolute_time();

	bool ok = wait_int(SDHCI_INT_CMD_COMPLETE, "command");
	uint64_t t2 = mach_absolute_time();
	ok = ok && wait_int(SDHCI_INT_XFER_COMPLETE, "dma");
	uint64_t t3 = mach_absolute_time();
	hc_write32(SDHCI_INT_STATUS, 0xffffffff);
	if (!ok) {
		reset_cmd_data();
		return EIO;
	}
	dcache_clean_inval(emmc_dma.data, len);
	emmc_phase_time[0] += t1 - t0;
	emmc_phase_time[1] += t2 - t1;
	emmc_phase_time[2] += t3 - t2;
	emmc_phase_time[3] += mach_absolute_time() - t3;
	return 0;
}

static int
emmc_read(uint64_t lba, uint32_t nblocks)
{
	return emmc_xfer(false, nblocks > 1 ? MMC_READ_MULTIPLE_BLOCK : MMC_READ_SINGLE_BLOCK, (uint32_t)lba, nblocks);
}

static int
emmc_write(uint64_t lba, uint32_t nblocks)
{
	return emmc_xfer(true, nblocks > 1 ? MMC_WRITE_MULTIPLE_BLOCK : MMC_WRITE_BLOCK, (uint32_t)lba, nblocks);
}

/* Send a command without data that answers with R1b, and wait until the card is no longer busy. */
static int
emmc_cmd_busy(uint32_t index, uint32_t arg)
{
	if (!wait_idle()) {
		reset_cmd_data();
		return EIO;
	}
	hc_write32(SDHCI_INT_STATUS, 0xffffffff);
	hc_write32(SDHCI_ARGUMENT, arg);
	uint32_t cmd = (index << 8) | SDHCI_CMD_INDEX | SDHCI_CMD_CRC | SDHCI_CMD_RESP_48_BUSY;
	hc_write32(SDHCI_TRANSFER_MODE, cmd << 16);

	bool ok = wait_int(SDHCI_INT_CMD_COMPLETE, "command") && wait_int(SDHCI_INT_XFER_COMPLETE, "busy");
	hc_write32(SDHCI_INT_STATUS, 0xffffffff);
	if (!ok) {
		reset_cmd_data();
		return EIO;
	}
	return 0;
}

static int
emmc_flush(void)
{
	if (!emmc_cache_on) {
		return 0;
	}
	return emmc_cmd_busy(MMC_SWITCH, (MMC_SWITCH_WRITE_BYTE << 24) | (EXT_CSD_FLUSH_CACHE << 16) | (1u << 8));
}

static void
emmc_read_ext_csd(void)
{
	if (emmc_xfer(false, MMC_SEND_EXT_CSD, 0, 1) != 0) {
		printf("tulip_emmc: cannot read EXT_CSD\n");
		return;
	}
	const uint8_t *ext = emmc_dma.data;
	uint32_t cache_kib = ((uint32_t)ext[EXT_CSD_CACHE_SIZE] | ((uint32_t)ext[EXT_CSD_CACHE_SIZE + 1] << 8) |
	    ((uint32_t)ext[EXT_CSD_CACHE_SIZE + 2] << 16) | ((uint32_t)ext[EXT_CSD_CACHE_SIZE + 3] << 24));
	emmc_cache_on = cache_kib && (ext[EXT_CSD_CACHE_CTRL] & 1);
	printf("tulip_emmc: cache %u KiB, %s\n", cache_kib, emmc_cache_on ? "on" : "off");
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

static void
gpt_name(const uint8_t *utf16, char *out, size_t outlen)
{
	size_t i;

	for (i = 0; i + 1 < outlen && i < 36 && utf16[2 * i]; i++) {
		out[i] = (char)utf16[2 * i];
	}
	out[i] = 0;
}

/* Walk the GPT. With want == NULL print a few partitions, else look up want. */
static bool
gpt_scan(const char *want, uint64_t *first_out, uint64_t *count_out)
{
	static uint8_t hdr[SECTOR];
	static uint8_t ent[SECTOR];

	if (emmc_read(1, 1) != 0) {
		return false;
	}
	memcpy(hdr, emmc_dma.data, SECTOR);
	if (memcmp(hdr, "EFI PART", 8) != 0) {
		printf("tulip_emmc: LBA 1 is not a GPT header\n");
		return false;
	}
	uint64_t entries_lba = le64(hdr + 72);
	uint32_t nentries = le32(hdr + 80);
	uint32_t esize = le32(hdr + 84);
	if (!want) {
		printf("tulip_emmc: GPT: %u entries of %u bytes at LBA %llu, disk last LBA %llu\n",
		    nentries, esize, entries_lba, le64(hdr + 32));
	}
	if (esize != 128 || nentries > 256) {
		return false;
	}
	for (uint32_t i = 0; i < nentries; i++) {
		if (i % 4 == 0) {
			if (emmc_read(entries_lba + i / 4, 1) != 0) {
				return false;
			}
			memcpy(ent, emmc_dma.data, SECTOR);
		}
		const uint8_t *e = ent + (i % 4) * 128;
		uint64_t first = le64(e + 32), last = le64(e + 40);
		char name[37];
		if (first == 0) {
			continue;
		}
		gpt_name(e + 56, name, sizeof(name));
		if (want && !strcmp(name, want)) {
			*first_out = first;
			*count_out = last - first + 1;
			return true;
		}
		if (!want && (!strcmp(name, "userdata") || !strcmp(name, "cache") || !strcmp(name, "boot"))) {
			printf("tulip_emmc: p%u %s: LBA %llu-%llu (%llu MiB)\n", i + 1, name, first, last,
			    (last - first + 1) / 2048);
		}
	}
	return false;
}

/* Read count transfers of nblocks from the start of userdata and print the time per phase. */
static void
emmc_benchmark(uint32_t count, uint32_t nblocks)
{
	const uint64_t lba = 14417920;
	uint64_t ns[4], total;

	bzero(emmc_phase_time, sizeof(emmc_phase_time));
	uint64_t start = mach_absolute_time();
	for (uint32_t i = 0; i < count; i++) {
		if (emmc_read(lba + (uint64_t)i * nblocks, nblocks) != 0) {
			printf("tulip_emmc: benchmark read %u failed\n", i);
			return;
		}
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &total);
	for (int i = 0; i < 4; i++) {
		absolutetime_to_nanoseconds(emmc_phase_time[i], &ns[i]);
	}
	printf("tulip_emmc: %u x %u KiB in %llu ms (%llu KiB/s); per read us: setup %llu, command %llu, data %llu, finish %llu\n",
	    count, nblocks / 2, total / NSEC_PER_MSEC, (uint64_t)count * nblocks / 2 * NSEC_PER_SEC / MAX(total, 1),
	    ns[0] / count / 1000, ns[1] / count / 1000, ns[2] / count / 1000, ns[3] / count / 1000);
}

void
tulip_emmc_probe(void)
{
	if (!emmc_init()) {
		return;
	}
	printf("tulip_emmc: present 0x%08x hostctl 0x%08x clock 0x%08x hostctl2 0x%08x caps 0x%08x\n",
	    hc_read32(SDHCI_PRESENT_STATE), hc_read32(SDHCI_HOST_CONTROL), hc_read32(SDHCI_CLOCK_CONTROL),
	    hc_read32(SDHCI_HOST_CONTROL2), hc_read32(SDHCI_CAPABILITIES));
	vm_offset_t rcg = ml_io_map(trunc_page(GCC_SDCC1_APPS_RCG), PAGE_SIZE);
	if (rcg) {
		volatile uint32_t *r = (volatile uint32_t *)(rcg + (GCC_SDCC1_APPS_RCG & PAGE_MASK));
		uint32_t cfg = r[1];
		printf("tulip_emmc: sdcc1 rcg cmd 0x%08x cfg 0x%08x (source %u, divider %u.%u) m 0x%x n 0x%x d 0x%x\n",
		    r[0], cfg, (cfg >> 8) & 7, ((cfg & 0x1f) + 1) / 2, (cfg & 0x1f) % 2 ? 0 : 5, r[2], r[3], r[4]);
	}
	if (emmc_xfer(false, MMC_SEND_EXT_CSD, 0, 1) == 0) {
		const uint8_t *ext = emmc_dma.data;
		printf("tulip_emmc: ext_csd rev %u bus_width 0x%x hs_timing 0x%x device_type 0x%x strobe %u\n",
		    ext[192], ext[183], ext[185], ext[196], ext[184]);
	}
	emmc_benchmark(100, 1);
	emmc_benchmark(1024, BOUNCE_SIZE / SECTOR);
	gpt_scan(NULL, NULL, NULL);
}

static void
tulip_emmc_strategy(buf_t bp)
{
	caddr_t vaddr;
	uint64_t sector = (uint64_t)buf_blkno(bp);
	uint32_t count = buf_count(bp);
	uint32_t done = 0;
	int error = 0;

	bool write = !(buf_flags(bp) & B_READ);

	if (write && !emmc_writable) {
		buf_seterror(bp, EROFS);
		buf_biodone(bp);
		return;
	}
	if (count % SECTOR || sector + count / SECTOR > part_sectors) {
		buf_seterror(bp, EINVAL);
		buf_biodone(bp);
		return;
	}
	if (buf_map(bp, &vaddr)) {
		buf_seterror(bp, EIO);
		buf_biodone(bp);
		return;
	}

	lck_mtx_lock(&emmc_mtx);
	uint64_t start = mach_absolute_time();
	while (done < count && !error) {
		uint32_t n = MIN(count - done, (uint32_t)BOUNCE_SIZE);
		uint64_t lba = part_start + sector + done / SECTOR;
		if (write) {
			memcpy(emmc_dma.data, vaddr + done, n);
			error = emmc_write(lba, n / SECTOR);
		} else {
			error = emmc_read(lba, n / SECTOR);
			if (!error) {
				memcpy(vaddr + done, emmc_dma.data, n);
			}
		}
		done += n;
	}
	tulip_emmc_stats[write ? 1 : 0]++;
	tulip_emmc_stats[2] += done;
	tulip_emmc_stats[3] += mach_absolute_time() - start;
	lck_mtx_unlock(&emmc_mtx);

	buf_unmap(bp);
	if (error) {
		buf_seterror(bp, error);
	} else {
		buf_setresid(bp, 0);
	}
	buf_biodone(bp);
}

static int
tulip_emmc_open(dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	if (minor(dev) != 0 || !part_sectors) {
		return ENXIO;
	}
	return ((flags & FWRITE) && !emmc_writable) ? EROFS : 0;
}

static int
tulip_emmc_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
tulip_emmc_psize(__unused dev_t dev)
{
	return part_sectors > INT32_MAX ? INT32_MAX : (int)part_sectors;
}

static int
tulip_emmc_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p)
{
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
	case DKIOCGETPHYSICALBLOCKSIZE:
		*(uint32_t *)data = SECTOR;
		return 0;
	case DKIOCSETBLOCKSIZE:
		return *(uint32_t *)data == SECTOR ? 0 : EINVAL;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = part_sectors;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = emmc_writable;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*(uint64_t *)data = BOUNCE_SIZE / SECTOR;
		return 0;
	case DKIOCGETMAXSEGMENTCOUNTREAD:
	case DKIOCGETMAXSEGMENTCOUNTWRITE:
		*(uint64_t *)data = 32;
		return 0;
	case DKIOCSYNCHRONIZE: {
		lck_mtx_lock(&emmc_mtx);
		int error = emmc_flush();
		lck_mtx_unlock(&emmc_mtx);
		return error;
	}
	default:
		return ENOTTY;
	}
}

static int
tulip_emmc_rw(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(tulip_emmc_strategy, NULL, dev,
	           uio_rw(uio) == UIO_READ ? B_READ : B_WRITE, minphys, uio, SECTOR);
}

static const struct bdevsw tulip_emmc_bdevsw = {
	.d_open     = tulip_emmc_open,
	.d_close    = tulip_emmc_close,
	.d_strategy = tulip_emmc_strategy,
	.d_ioctl    = tulip_emmc_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = tulip_emmc_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw tulip_emmc_cdevsw = {
	.d_open       = tulip_emmc_open,
	.d_close      = tulip_emmc_close,
	.d_read       = tulip_emmc_rw,
	.d_write      = tulip_emmc_rw,
	.d_ioctl      = tulip_emmc_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

dev_t
tulip_emmc_rootdev(void)
{
	if (!emmc_probed) {
		char want[37] = "userdata";
		uint32_t rw = 0;

		emmc_probed = true;
		PE_parse_boot_argn("tulip_emmc_part", want, sizeof(want));
		PE_parse_boot_argn("tulip_emmc_rw", &rw, sizeof(rw));
		if (!emmc_init()) {
			return NODEV;
		}
		lck_mtx_lock(&emmc_mtx);
		bool found = gpt_scan(want, &part_start, &part_sectors);
		if (found && rw) {
			emmc_read_ext_csd();
			emmc_writable = true;
		}
		lck_mtx_unlock(&emmc_mtx);
		if (!found) {
			printf("tulip_emmc: no partition named %s\n", want);
			part_sectors = 0;
			return NODEV;
		}
		printf("tulip_emmc: %s at LBA %llu, %llu sectors, %s\n", want, part_start, part_sectors,
		    emmc_writable ? "read-write" : "read-only");
		emmc_bmajor = bdevsw_add(-1, &tulip_emmc_bdevsw);
		if (emmc_bmajor < 0) {
			return NODEV;
		}
		emmc_cmajor = cdevsw_add_with_bdev(-1, &tulip_emmc_cdevsw, emmc_bmajor);
		if (emmc_cmajor < 0) {
			return NODEV;
		}
		devfs_make_node(makedev(emmc_bmajor, 0), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "emmc0");
		devfs_make_node(makedev(emmc_cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "remmc0");
	}
	return part_sectors && emmc_bmajor >= 0 ? makedev(emmc_bmajor, 0) : NODEV;
}
