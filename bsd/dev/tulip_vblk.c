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
#include <kern/locks.h>
#include <machine/machine_routines.h>
#include <libkern/libkern.h>

extern void flush_dcache64(addr64_t addr, unsigned count, int phys);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_static_vtop(vm_offset_t vaddr);
extern int physio(void (*f_strategy)(buf_t), buf_t bp, dev_t dev, int flags,
    u_int (*f_minphys)(buf_t), struct uio *uio, int blocksize);
extern u_int minphys(buf_t bp);

/* Probe the first virtio-mmio block device and return its block dev_t, or NODEV. */
dev_t tulip_vblk_rootdev(void);

#define VIRTIO_MMIO_BASE        0x0a000000UL
#define VIRTIO_MMIO_STRIDE      0x200
#define VIRTIO_MMIO_SLOTS       32

#define VMMIO_MAGIC             0x000
#define VMMIO_VERSION           0x004
#define VMMIO_DEVICE_ID         0x008
#define VMMIO_DEVICE_FEATURES   0x010
#define VMMIO_DEVICE_FEAT_SEL   0x014
#define VMMIO_DRIVER_FEATURES   0x020
#define VMMIO_DRIVER_FEAT_SEL   0x024
#define VMMIO_QUEUE_SEL         0x030
#define VMMIO_QUEUE_NUM_MAX     0x034
#define VMMIO_QUEUE_NUM         0x038
#define VMMIO_QUEUE_READY       0x044
#define VMMIO_QUEUE_NOTIFY      0x050
#define VMMIO_INT_STATUS        0x060
#define VMMIO_INT_ACK           0x064
#define VMMIO_STATUS            0x070
#define VMMIO_QUEUE_DESC_LO     0x080
#define VMMIO_QUEUE_DESC_HI     0x084
#define VMMIO_QUEUE_AVAIL_LO    0x090
#define VMMIO_QUEUE_AVAIL_HI    0x094
#define VMMIO_QUEUE_USED_LO     0x0a0
#define VMMIO_QUEUE_USED_HI     0x0a4
#define VMMIO_CONFIG            0x100

#define VIRTIO_MAGIC            0x74726976
#define VIRTIO_ID_BLOCK         2

#define VSTAT_ACK               1
#define VSTAT_DRIVER            2
#define VSTAT_DRIVER_OK         4
#define VSTAT_FEATURES_OK       8

#define VBLK_T_IN               0
#define VBLK_T_OUT              1

#define VRING_DESC_F_NEXT       1
#define VRING_DESC_F_WRITE      2

#define QSIZE                   8
#define SECTOR                  512
#define BOUNCE_SIZE             (64 * 1024)

struct vring_desc {
	uint64_t addr;
	uint32_t len;
	uint16_t flags;
	uint16_t next;
};

struct vring_avail {
	uint16_t flags;
	uint16_t idx;
	uint16_t ring[QSIZE];
	uint16_t used_event;
};

struct vring_used_elem {
	uint32_t id;
	uint32_t len;
};

struct vring_used {
	uint16_t flags;
	uint16_t idx;
	struct vring_used_elem ring[QSIZE];
	uint16_t avail_event;
};

struct vblk_req_hdr {
	uint32_t type;
	uint32_t reserved;
	uint64_t sector;
};

struct vblk_dma {
	struct vring_desc desc[QSIZE];
	struct vring_avail avail;
	uint8_t pad0[64];
	struct vring_used used;
	uint8_t pad1[64];
	struct vblk_req_hdr hdr;
	uint8_t status;
	uint8_t pad2[63];
	uint8_t bounce[BOUNCE_SIZE];
};

/* In the kernel image, so it is physically contiguous and ml_static_vtop() works. */
static struct vblk_dma vblk_dma __attribute__((aligned(16384)));

static vm_offset_t vblk_regs;
static uint64_t vblk_sectors;
static uint16_t vblk_used_seen;
static int vblk_bmajor = -1;
static int vblk_cmajor = -1;
static bool vblk_probed;

static LCK_GRP_DECLARE(vblk_lck_grp, "tulip_vblk");
static LCK_MTX_DECLARE(vblk_mtx, &vblk_lck_grp);

static uint32_t
vreg_read(uint32_t off)
{
	return *(volatile uint32_t *)(vblk_regs + off);
}

static void
vreg_write(uint32_t off, uint32_t val)
{
	*(volatile uint32_t *)(vblk_regs + off) = val;
}

static uint64_t
vblk_pa(const void *va)
{
	return ml_static_vtop((vm_offset_t)va);
}

static void
vblk_flush(const void *va, size_t len)
{
	flush_dcache64((addr64_t)(uintptr_t)va, (unsigned)len, FALSE);
}

static bool
vblk_init_device(vm_offset_t regs)
{
	uint64_t pa;

	vblk_regs = regs;
	vreg_write(VMMIO_STATUS, 0);
	vreg_write(VMMIO_STATUS, VSTAT_ACK);
	vreg_write(VMMIO_STATUS, VSTAT_ACK | VSTAT_DRIVER);

	vreg_write(VMMIO_DEVICE_FEAT_SEL, 1);
	if (!(vreg_read(VMMIO_DEVICE_FEATURES) & 1)) {
		printf("tulip_vblk: device lacks VIRTIO_F_VERSION_1\n");
		return false;
	}
	vreg_write(VMMIO_DRIVER_FEAT_SEL, 1);
	vreg_write(VMMIO_DRIVER_FEATURES, 1);
	vreg_write(VMMIO_DRIVER_FEAT_SEL, 0);
	vreg_write(VMMIO_DRIVER_FEATURES, 0);
	vreg_write(VMMIO_STATUS, VSTAT_ACK | VSTAT_DRIVER | VSTAT_FEATURES_OK);
	if (!(vreg_read(VMMIO_STATUS) & VSTAT_FEATURES_OK)) {
		printf("tulip_vblk: features not accepted\n");
		return false;
	}

	vreg_write(VMMIO_QUEUE_SEL, 0);
	if (vreg_read(VMMIO_QUEUE_NUM_MAX) < QSIZE) {
		printf("tulip_vblk: queue too small\n");
		return false;
	}
	bzero(&vblk_dma, offsetof(struct vblk_dma, bounce));
	vblk_flush(&vblk_dma, offsetof(struct vblk_dma, bounce));
	vreg_write(VMMIO_QUEUE_NUM, QSIZE);
	pa = vblk_pa(vblk_dma.desc);
	vreg_write(VMMIO_QUEUE_DESC_LO, (uint32_t)pa);
	vreg_write(VMMIO_QUEUE_DESC_HI, (uint32_t)(pa >> 32));
	pa = vblk_pa(&vblk_dma.avail);
	vreg_write(VMMIO_QUEUE_AVAIL_LO, (uint32_t)pa);
	vreg_write(VMMIO_QUEUE_AVAIL_HI, (uint32_t)(pa >> 32));
	pa = vblk_pa(&vblk_dma.used);
	vreg_write(VMMIO_QUEUE_USED_LO, (uint32_t)pa);
	vreg_write(VMMIO_QUEUE_USED_HI, (uint32_t)(pa >> 32));
	vreg_write(VMMIO_QUEUE_READY, 1);

	vblk_sectors = (uint64_t)vreg_read(VMMIO_CONFIG) | ((uint64_t)vreg_read(VMMIO_CONFIG + 4) << 32);
	vreg_write(VMMIO_STATUS, VSTAT_ACK | VSTAT_DRIVER | VSTAT_FEATURES_OK | VSTAT_DRIVER_OK);
	vblk_used_seen = 0;
	return true;
}

static int
vblk_xfer(int write, uint64_t sector, uint32_t len)
{
	struct vblk_dma *d = &vblk_dma;
	uint16_t avail_idx;
	uint64_t spins = 0;

	d->hdr.type = write ? VBLK_T_OUT : VBLK_T_IN;
	d->hdr.reserved = 0;
	d->hdr.sector = sector;
	d->status = 0xff;

	d->desc[0] = (struct vring_desc){ vblk_pa(&d->hdr), sizeof(d->hdr), VRING_DESC_F_NEXT, 1 };
	d->desc[1] = (struct vring_desc){ vblk_pa(d->bounce), len,
		                          (uint16_t)(VRING_DESC_F_NEXT | (write ? 0 : VRING_DESC_F_WRITE)), 2 };
	d->desc[2] = (struct vring_desc){ vblk_pa(&d->status), 1, VRING_DESC_F_WRITE, 0 };

	avail_idx = d->avail.idx;
	d->avail.ring[avail_idx % QSIZE] = 0;
	vblk_flush(d, offsetof(struct vblk_dma, bounce));
	vblk_flush(d->bounce, len);
	__asm__ volatile ("dsb sy" ::: "memory");
	d->avail.idx = avail_idx + 1;
	vblk_flush(&d->avail, sizeof(d->avail));
	__asm__ volatile ("dsb sy" ::: "memory");
	vreg_write(VMMIO_QUEUE_NOTIFY, 0);

	for (;;) {
		vblk_flush(&d->used, sizeof(d->used));
		if (*(volatile uint16_t *)&d->used.idx != vblk_used_seen) {
			break;
		}
		if (++spins > 4000000000ULL) {
			printf("tulip_vblk: request timed out\n");
			return EIO;
		}
	}
	vblk_used_seen++;
	vreg_write(VMMIO_INT_ACK, vreg_read(VMMIO_INT_STATUS));
	vblk_flush(d, offsetof(struct vblk_dma, bounce));
	vblk_flush(d->bounce, len);
	return d->status == 0 ? 0 : EIO;
}

static void
tulip_vblk_strategy(buf_t bp)
{
	caddr_t vaddr;
	int write = !(buf_flags(bp) & B_READ);
	uint64_t sector = (uint64_t)buf_blkno(bp);
	uint32_t count = buf_count(bp);
	uint32_t done = 0;
	int error = 0;

	if (count % SECTOR || sector + count / SECTOR > vblk_sectors) {
		buf_seterror(bp, EINVAL);
		buf_biodone(bp);
		return;
	}
	if (buf_map(bp, &vaddr)) {
		buf_seterror(bp, EIO);
		buf_biodone(bp);
		return;
	}

	lck_mtx_lock(&vblk_mtx);
	while (done < count && !error) {
		uint32_t n = MIN(count - done, (uint32_t)BOUNCE_SIZE);
		if (write) {
			memcpy(vblk_dma.bounce, vaddr + done, n);
		}
		error = vblk_xfer(write, sector + done / SECTOR, n);
		if (!error && !write) {
			memcpy(vaddr + done, vblk_dma.bounce, n);
		}
		done += n;
	}
	lck_mtx_unlock(&vblk_mtx);

	buf_unmap(bp);
	if (error) {
		buf_seterror(bp, error);
	} else {
		buf_setresid(bp, 0);
	}
	buf_biodone(bp);
}

static int
tulip_vblk_open(dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return minor(dev) == 0 && vblk_regs ? 0 : ENXIO;
}

static int
tulip_vblk_close(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
tulip_vblk_psize(__unused dev_t dev)
{
	return vblk_sectors > INT32_MAX ? INT32_MAX : (int)vblk_sectors;
}

static int
tulip_vblk_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p)
{
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
	case DKIOCGETPHYSICALBLOCKSIZE:
		*(uint32_t *)data = SECTOR;
		return 0;
	case DKIOCSETBLOCKSIZE:
		return *(uint32_t *)data == SECTOR ? 0 : EINVAL;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = vblk_sectors;
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 1;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*(uint64_t *)data = BOUNCE_SIZE / SECTOR;
		return 0;
	case DKIOCGETMAXSEGMENTCOUNTREAD:
	case DKIOCGETMAXSEGMENTCOUNTWRITE:
		*(uint64_t *)data = 32;
		return 0;
	case DKIOCSYNCHRONIZE:
		return 0;
	default:
		return ENOTTY;
	}
}

static int
tulip_vblk_rw(dev_t dev, struct uio *uio, __unused int ioflag)
{
	return physio(tulip_vblk_strategy, NULL, dev,
	           uio_rw(uio) == UIO_READ ? B_READ : B_WRITE, minphys, uio, SECTOR);
}

static const struct bdevsw tulip_vblk_bdevsw = {
	.d_open     = tulip_vblk_open,
	.d_close    = tulip_vblk_close,
	.d_strategy = tulip_vblk_strategy,
	.d_ioctl    = tulip_vblk_ioctl,
	.d_dump     = eno_dump,
	.d_psize    = tulip_vblk_psize,
	.d_type     = D_DISK,
};

static const struct cdevsw tulip_vblk_cdevsw = {
	.d_open       = tulip_vblk_open,
	.d_close      = tulip_vblk_close,
	.d_read       = tulip_vblk_rw,
	.d_write      = tulip_vblk_rw,
	.d_ioctl      = tulip_vblk_ioctl,
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

static bool
tulip_vblk_probe(void)
{
	vm_offset_t window = ml_io_map(VIRTIO_MMIO_BASE, VIRTIO_MMIO_SLOTS * VIRTIO_MMIO_STRIDE);

	if (!window) {
		return false;
	}
	for (int i = 0; i < VIRTIO_MMIO_SLOTS; i++) {
		vm_offset_t regs = window + (vm_offset_t)i * VIRTIO_MMIO_STRIDE;
		uint32_t magic = *(volatile uint32_t *)(regs + VMMIO_MAGIC);
		uint32_t version = *(volatile uint32_t *)(regs + VMMIO_VERSION);
		uint32_t id = *(volatile uint32_t *)(regs + VMMIO_DEVICE_ID);

		if (magic != VIRTIO_MAGIC || id != VIRTIO_ID_BLOCK) {
			continue;
		}
		if (version != 2) {
			printf("tulip_vblk: slot %d is virtio-mmio version %u, need 2\n", i, version);
			continue;
		}
		if (vblk_init_device(regs)) {
			printf("tulip_vblk: slot %d, %llu sectors\n", i, vblk_sectors);
			return true;
		}
	}
	return false;
}

dev_t
tulip_vblk_rootdev(void)
{
	if (!vblk_probed) {
		vblk_probed = true;
		if (!tulip_vblk_probe()) {
			vblk_regs = 0;
			return NODEV;
		}
		vblk_bmajor = bdevsw_add(-1, &tulip_vblk_bdevsw);
		if (vblk_bmajor < 0) {
			return NODEV;
		}
		vblk_cmajor = cdevsw_add_with_bdev(-1, &tulip_vblk_cdevsw, vblk_bmajor);
		if (vblk_cmajor < 0) {
			return NODEV;
		}
		devfs_make_node(makedev(vblk_bmajor, 0), DEVFS_BLOCK, UID_ROOT, GID_OPERATOR, 0640, "vblk0");
		devfs_make_node(makedev(vblk_cmajor, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rvblk0");
	}
	return vblk_regs && vblk_bmajor >= 0 ? makedev(vblk_bmajor, 0) : NODEV;
}
