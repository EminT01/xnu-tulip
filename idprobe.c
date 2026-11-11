/*
 * idprobe: freestanding aarch64 Linux program (no libc) that reports which
 * ARMv8.x features each CPU core of the phone has. Run it from a recovery
 * (TWRP/OrangeFox) shell: adb push idprobe /tmp/ && adb shell chmod +x /tmp/idprobe && adb shell /tmp/idprobe
 *
 * Why: ios-sweet's kernel is built for ARMv8.2 with 16K pages and PAN
 * (xnu patch 0004: -march=armv8.2-a+crypto, __ARM_16K_PG__, __ARM_PAN_AVAILABLE__).
 * The Redmi Note 6 Pro (SDM636, Kryo 260 = Cortex-A53/A73 class) is ARMv8.0.
 * This tells us, per core, whether 16K granule, PAN, LSE atomics, RDM exist.
 */
typedef unsigned long u64;

static long sys3(long n, long a, long b, long c)
{
	register long x8 __asm__("x8") = n;
	register long x0 __asm__("x0") = a;
	register long x1 __asm__("x1") = b;
	register long x2 __asm__("x2") = c;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory", "cc");
	return x0;
}

#define SYS_write 64
#define SYS_exit 93
#define SYS_sched_setaffinity 122
#define SYS_sched_yield 124

static void out(const char *s)
{
	long n = 0;
	while (s[n])
		n++;
	sys3(SYS_write, 1, (long)s, n);
}

static void hex(u64 v)
{
	char b[19];
	b[0] = '0';
	b[1] = 'x';
	for (int i = 0; i < 16; i++)
		b[2 + i] = "0123456789abcdef"[(v >> (60 - 4 * i)) & 0xf];
	b[18] = 0;
	out(b);
}

static void dec(u64 v)
{
	char b[24];
	int i = 23;
	b[i] = 0;
	do {
		b[--i] = '0' + v % 10;
		v /= 10;
	} while (v);
	out(b + i);
}

static const char *const hwcap_names[32] = {
	"fp", "asimd", "evtstrm", "aes", "pmull", "sha1", "sha2", "crc32",
	"atomics(LSE)", "fphp", "asimdhp", "cpuid", "asimdrdm(RDM)", "jscvt",
	"fcma", "lrcpc", "dcpop", "sha3", "sm3", "sm4", "asimddp", "sha512",
	"sve", "asimdfhm", "dit", "uscat", "ilrcpc", "flagm", "ssbs", "sb",
	"paca", "pacg",
};

#define F(v, lo) (((v) >> (lo)) & 0xf)
#define YESNO(c) ((c) ? "YES" : "no")

void probe_main(u64 *sp)
{
	u64 argc = sp[0];
	char **argv = (char **)(sp + 1);
	char **p = argv + argc + 1;	/* envp */
	while (*p)
		p++;
	u64 *aux = (u64 *)(p + 1);
	u64 hwcap = 0;

	for (; aux[0] != 0; aux += 2)
		if (aux[0] == 16)	/* AT_HWCAP */
			hwcap = aux[1];

	out("== idprobe ==\nAT_HWCAP = ");
	hex(hwcap);
	out("\nfeatures:");
	for (int i = 0; i < 32; i++)
		if (hwcap & (1UL << i)) {
			out(" ");
			out(hwcap_names[i]);
		}
	out("\n");

	if (!(hwcap & (1UL << 11))) {
		out("\nCPUID register emulation is NOT available in this kernel (HWCAP bit 11 clear).\n"
		    "The feature list above is all this tool can tell. Send it together with\n"
		    "the output of collect-tulip-info.sh; the 16K-granule check will then be\n"
		    "done from the shim instead.\n");
		sys3(SYS_exit, 0, 0, 0);
	}

	int any = 0, all_tg16 = 1, all_pan = 1, all_lse = 1, all_rdm = 1;

	for (int cpu = 0; cpu < 8; cpu++) {
		u64 mask = 1UL << cpu;
		u64 midr, m0, m1, i0, i1;

		if (sys3(SYS_sched_setaffinity, 0, 8, (long)&mask) != 0)
			continue;
		sys3(SYS_sched_yield, 0, 0, 0);
		__asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
		__asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(m0));
		__asm__ volatile("mrs %0, id_aa64mmfr1_el1" : "=r"(m1));
		__asm__ volatile("mrs %0, id_aa64isar0_el1" : "=r"(i0));
		__asm__ volatile("mrs %0, id_aa64isar1_el1" : "=r"(i1));

		int tg16 = F(m0, 20) != 0;	/* TGran16: 0 = not implemented */
		int pan = F(m1, 20) != 0;	/* PAN: ARMv8.1 */
		int lse = F(i0, 20) >= 2;	/* Atomic: 2 = LSE */
		int rdm = F(i0, 28) != 0;	/* RDM: ARMv8.1 */

		any = 1;
		all_tg16 &= tg16;
		all_pan &= pan;
		all_lse &= lse;
		all_rdm &= rdm;

		out("\ncpu");
		dec(cpu);
		out(": MIDR=");
		hex(midr);
		out(" (implementer ");
		hex(midr >> 24);
		out(", part ");
		hex((midr >> 4) & 0xfff);
		out(")\n  ID_AA64MMFR0=");
		hex(m0);
		out("  PARange=");
		dec(F(m0, 0));
		out("  TGran16=");
		dec(F(m0, 20));
		out(" TGran4=");
		dec(F(m0, 28));
		out("\n  ID_AA64MMFR1=");
		hex(m1);
		out("  PAN=");
		dec(F(m1, 20));
		out("\n  ID_AA64ISAR0=");
		hex(i0);
		out("  Atomic=");
		dec(F(i0, 20));
		out(" RDM=");
		dec(F(i0, 28));
		out("\n  ID_AA64ISAR1=");
		hex(i1);
		out("\n  16K pages: ");
		out(YESNO(tg16));
		out("   PAN: ");
		out(YESNO(pan));
		out("   LSE atomics: ");
		out(YESNO(lse));
		out("   RDM: ");
		out(YESNO(rdm));
		out("\n");
	}

	out("\n== VERDICT (all online cores) ==\n");
	if (!any) {
		out("could not pin to any core; run again or send the output as is\n");
	} else {
		out("16K page granule on every core: ");
		out(YESNO(all_tg16));
		out("\nPAN (ARMv8.1) on every core:        ");
		out(YESNO(all_pan));
		out("\nLSE atomics on every core:          ");
		out(YESNO(all_lse));
		out("\nRDM on every core:                  ");
		out(YESNO(all_rdm));
		out("\n\nios-sweet needs all four (kernel built -march=armv8.2-a, 16K pages, PAN).\n"
		    "A 'no' on 16K pages is the hard blocker: iOS userland is built for 16K pages.\n");
	}
	sys3(SYS_exit, 0, 0, 0);
	for (;;)
		;
}

__asm__(".globl _start\n"
	"_start:\n"
	"	mov	x0, sp\n"
	"	and	sp, x0, #0xfffffffffffffff0\n"
	"	bl	probe_main\n");
