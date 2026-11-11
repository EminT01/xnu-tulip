# Redmi Note 6 Pro (tulip, SDM636): ölçülen donanım bilgisi

Kaynak: kullanıcının telefonundan alınan cpuinfo/iomem/dmesg/partitions + stok boot.img içindeki DTB #29.

## CPU
- 8 çekirdek: 0-3 Kryo 260 Silver (part 0x801, A53 sınıfı), 4-7 Kryo 260 Gold (part 0x800, A73 sınıfı)
- Özellikler: fp asimd aes pmull sha1 sha2 crc32. LSE/RDM yok -> ARMv8.0. PAN yok.
- 16K sayfa: A53'te yok (ARM TRM). Çekirdek 4K olmalı (ios-vince yaklaşımı).

## Boot imajı (stok)
- Android boot header v0, sayfa 4096, kernel 0x8000, ramdisk 0x1000000, tags 0x100, second 0xf00000
- Kernel: gzip Image + eklenmiş 36 DTB (Image.gz-dtb tarzı). Yük 0x1985000 bayt.
- Payload'ın arkasında 1320 baytlık DER imza bloğu (0x1985000, Xiaomi boot imzası), sonrası sıfır dolgu. Kırpılmış imaj 0x1986000 bayt (imza dahil).
- Doğru DTB: #29 "SDM 636 PM660 + PM660L MTP E7T", msm-id 345, board-id 0x40008
- cmdline'da earlycon=msm_serial_dm,0xc170000 var (seri konsol adresi)

## Adresler (DTB #29'dan)
- GICv3: dağıtıcı 0x17a00000 (0x10000), yeniden dağıtıcı 0x17b00000 (0x100000), stride 0x20000
- Timer: arch timer 19,2 MHz (0x124f800); mmio timer 0x17920000
- PSCI 1.0, yöntem smc
- eMMC: sdhci-msm-v5, hc 0x0c0c4000, core 0x0c0c5000 (HS400 ES, CMDQ). userdata = mmcblk0p64
- Dokunmatik: focaltech,fts @ 0x38, i2c@0c175000 (qcom,i2c-msm-v2, BLSP), reset GPIO 66, irq GPIO 67
- Splash/framebuffer bölgesi: 0x9d400000, 36 MiB (cont_splash_mem). Panel 1080x2280 Tianma NT36672A video modu
- RAM (iomem): 0x40000000-0x855fffff, 0x88f00000-0x8abfffff, 0x94b00000-0xfeabffff (~2,9 GiB)
- Kullanılabilir ramoops: 0xa0000000, 4 MiB (pstore, çökme günlüğü için faydalı)

## vince'e göre farklar
| | vince (MSM8953) | tulip (SDM636) |
|---|---|---|
| GIC | v2 @0x0b000000 | v3 @0x17a00000 |
| eMMC | 0x07824900 | 0x0c0c4000 |
| Dokunmatik | Synaptics 0x20, I2C-3 | FocalTech 0x38, c175000 |
| Ekran | 1080x2160, 24 bpp | 1080x2280, fb 0x9d400000 (bpp doğrulanacak) |
| CPU | 8x A53 | 4x A53 + 4x A73 |
