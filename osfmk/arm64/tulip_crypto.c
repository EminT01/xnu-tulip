/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>
#include <kern/misc_protos.h>
#include <libkern/crypto/register_crypto.h>
#include <libkern/crypto/rand.h>
#include <corecrypto/ccmode.h>
#include <corecrypto/ccmode_impl.h>
#include <corecrypto/ccrng.h>
#include <kern/debug.h>
#include <prng/random.h>
#include <pexpert/arm64/board_config.h>
#include <arm64/tulip_crypto.h>

/* Not a CSPRNG: a stand-in until the kernel has a real corecrypto replacement. */
struct tulip_rng {
	uint64_t s[4];
};

static uint64_t
rotl(uint64_t x, int k)
{
	return (x << k) | (x >> (64 - k));
}

static uint64_t
tulip_rng_next(struct tulip_rng *r)
{
	uint64_t result = rotl(r->s[1] * 5, 7) * 9;
	uint64_t t = r->s[1] << 17;

	r->s[2] ^= r->s[0];
	r->s[3] ^= r->s[1];
	r->s[1] ^= r->s[2];
	r->s[0] ^= r->s[3];
	r->s[2] ^= t;
	r->s[3] = rotl(r->s[3], 45);
	return result;
}

static void
tulip_random_kmem_init(crypto_random_ctx_t ctx)
{
	struct tulip_rng *r = ctx;

	for (int i = 0; i < 4; i++) {
		r->s[i] = early_random() | 1;
	}
}

static size_t
tulip_random_kmem_ctx_size(void)
{
	return sizeof(struct tulip_rng);
}

static void
tulip_random_generate(crypto_random_ctx_t ctx, void *random, size_t random_size)
{
	uint8_t *out = random;

	while (random_size) {
		uint64_t v = tulip_rng_next(ctx);
		size_t n = random_size < sizeof(v) ? random_size : sizeof(v);
		memcpy(out, &v, n);
		out += n;
		random_size -= n;
	}
}

static void
tulip_random_uniform(crypto_random_ctx_t ctx, uint64_t bound, uint64_t *random)
{
	uint64_t limit = bound ? UINT64_MAX - (UINT64_MAX % bound) : 0;
	uint64_t v;

	if (bound == 0) {
		*random = 0;
		return;
	}
	do {
		v = tulip_rng_next(ctx);
	} while (v >= limit);
	*random = v % bound;
}

static void
tulip_rng_mix(struct tulip_rng *r, size_t nbytes, const void *data)
{
	const uint8_t *p = data;

	for (size_t i = 0; i < nbytes; i++) {
		r->s[i % 4] ^= (uint64_t)p[i] << (8 * (i % 8));
	}
	for (int i = 0; i < 4; i++) {
		r->s[i] ^= early_random();
		if (r->s[i] == 0) {
			r->s[i] = 1;
		}
	}
}

static struct tulip_rng tulip_prng_gens[MAX_CPUS];
static struct cckprng_ctx tulip_prng_ctx;

static void
tulip_prng_init(struct cckprng_ctx *ctx __unused, size_t seed_nbytes, const void *seed,
    size_t nonce_nbytes, const void *nonce, cckprng_getentropy getentropy __unused,
    void *getentropy_arg __unused)
{
	for (int i = 0; i < MAX_CPUS; i++) {
		tulip_rng_mix(&tulip_prng_gens[i], seed_nbytes, seed);
		tulip_rng_mix(&tulip_prng_gens[i], nonce_nbytes, nonce);
	}
}

static void
tulip_prng_init_with_getentropy(struct cckprng_ctx *ctx, unsigned max_ngens __unused,
    size_t seed_nbytes, const void *seed, size_t nonce_nbytes, const void *nonce,
    cckprng_getentropy getentropy, void *getentropy_arg)
{
	tulip_prng_init(ctx, seed_nbytes, seed, nonce_nbytes, nonce, getentropy, getentropy_arg);
}

static void
tulip_prng_initgen(struct cckprng_ctx *ctx __unused, unsigned gen_idx __unused)
{
}

static void
tulip_prng_reseed(struct cckprng_ctx *ctx __unused, size_t nbytes, const void *seed)
{
	for (int i = 0; i < MAX_CPUS; i++) {
		tulip_rng_mix(&tulip_prng_gens[i], nbytes, seed);
	}
}

static void
tulip_prng_refresh(struct cckprng_ctx *ctx __unused)
{
}

static void
tulip_prng_generate(struct cckprng_ctx *ctx __unused, unsigned gen_idx, size_t nbytes, void *out)
{
	tulip_random_generate(&tulip_prng_gens[gen_idx % MAX_CPUS], out, nbytes);
}

static const struct cckprng_funcs tulip_prng_funcs = {
	.init = tulip_prng_init,
	.initgen = tulip_prng_initgen,
	.reseed = tulip_prng_reseed,
	.refresh = tulip_prng_refresh,
	.generate = tulip_prng_generate,
	.init_with_getentropy = tulip_prng_init_with_getentropy,
};


/* AES (FIPS-197), byte oriented. Tables are derived at registration time. */
static uint8_t aes_sbox[256], aes_inv_sbox[256];

struct tulip_aes_ctx {
	uint32_t rounds;
	uint8_t rk[240];
};

static uint8_t
rotl8(uint8_t x, int k)
{
	return (uint8_t)((x << k) | (x >> (8 - k)));
}

static uint8_t
xtime(uint8_t x)
{
	return (uint8_t)((x << 1) ^ ((x & 0x80) ? 0x1b : 0));
}

static uint8_t
gmul(uint8_t a, uint8_t b)
{
	uint8_t r = 0;

	while (b) {
		if (b & 1) {
			r ^= a;
		}
		a = xtime(a);
		b >>= 1;
	}
	return r;
}

static void
aes_tables_init(void)
{
	uint8_t p = 1, q = 1;

	do {
		p = (uint8_t)(p ^ (p << 1) ^ ((p & 0x80) ? 0x1b : 0));
		q ^= (uint8_t)(q << 1);
		q ^= (uint8_t)(q << 2);
		q ^= (uint8_t)(q << 4);
		if (q & 0x80) {
			q ^= 0x09;
		}
		aes_sbox[p] = (uint8_t)(q ^ rotl8(q, 1) ^ rotl8(q, 2) ^ rotl8(q, 3) ^ rotl8(q, 4) ^ 0x63);
	} while (p != 1);
	aes_sbox[0] = 0x63;
	for (int i = 0; i < 256; i++) {
		aes_inv_sbox[aes_sbox[i]] = (uint8_t)i;
	}
}

static int
aes_expand(struct tulip_aes_ctx *ctx, size_t key_len, const uint8_t *key)
{
	unsigned nk, total;
	uint8_t rcon = 1;

	if (key_len != 16 && key_len != 24 && key_len != 32) {
		return -1;
	}
	nk = (unsigned)key_len / 4;
	ctx->rounds = nk + 6;
	total = 4 * (ctx->rounds + 1);
	memcpy(ctx->rk, key, key_len);
	for (unsigned i = nk; i < total; i++) {
		uint8_t t[4];

		memcpy(t, &ctx->rk[(i - 1) * 4], 4);
		if (i % nk == 0) {
			uint8_t t0 = t[0];
			t[0] = (uint8_t)(aes_sbox[t[1]] ^ rcon);
			t[1] = aes_sbox[t[2]];
			t[2] = aes_sbox[t[3]];
			t[3] = aes_sbox[t0];
			rcon = xtime(rcon);
		} else if (nk > 6 && i % nk == 4) {
			for (int j = 0; j < 4; j++) {
				t[j] = aes_sbox[t[j]];
			}
		}
		for (int j = 0; j < 4; j++) {
			ctx->rk[i * 4 + j] = ctx->rk[(i - nk) * 4 + j] ^ t[j];
		}
	}
	return 0;
}

static void
aes_add_round_key(uint8_t *st, const struct tulip_aes_ctx *ctx, unsigned round)
{
	for (int i = 0; i < 16; i++) {
		st[i] ^= ctx->rk[round * 16 + i];
	}
}

static void
aes_encrypt_block(const struct tulip_aes_ctx *ctx, const uint8_t *in, uint8_t *out)
{
	uint8_t st[16], t[16];

	memcpy(st, in, 16);
	aes_add_round_key(st, ctx, 0);
	for (unsigned round = 1; round <= ctx->rounds; round++) {
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 4; r++) {
				t[c * 4 + r] = aes_sbox[st[((c + r) % 4) * 4 + r]];
			}
		}
		if (round != ctx->rounds) {
			for (int c = 0; c < 4; c++) {
				uint8_t *a = &t[c * 4];
				uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
				a[0] = (uint8_t)(xtime(a0) ^ xtime(a1) ^ a1 ^ a2 ^ a3);
				a[1] = (uint8_t)(a0 ^ xtime(a1) ^ xtime(a2) ^ a2 ^ a3);
				a[2] = (uint8_t)(a0 ^ a1 ^ xtime(a2) ^ xtime(a3) ^ a3);
				a[3] = (uint8_t)(xtime(a0) ^ a0 ^ a1 ^ a2 ^ xtime(a3));
			}
		}
		memcpy(st, t, 16);
		aes_add_round_key(st, ctx, round);
	}
	memcpy(out, st, 16);
}

static void
aes_decrypt_block(const struct tulip_aes_ctx *ctx, const uint8_t *in, uint8_t *out)
{
	uint8_t st[16], t[16];

	memcpy(st, in, 16);
	aes_add_round_key(st, ctx, ctx->rounds);
	for (unsigned round = ctx->rounds; round-- > 0;) {
		for (int c = 0; c < 4; c++) {
			for (int r = 0; r < 4; r++) {
				t[((c + r) % 4) * 4 + r] = aes_inv_sbox[st[c * 4 + r]];
			}
		}
		memcpy(st, t, 16);
		aes_add_round_key(st, ctx, round);
		if (round != 0) {
			for (int c = 0; c < 4; c++) {
				uint8_t *a = &st[c * 4];
				uint8_t a0 = a[0], a1 = a[1], a2 = a[2], a3 = a[3];
				a[0] = (uint8_t)(gmul(a0, 14) ^ gmul(a1, 11) ^ gmul(a2, 13) ^ gmul(a3, 9));
				a[1] = (uint8_t)(gmul(a0, 9) ^ gmul(a1, 14) ^ gmul(a2, 11) ^ gmul(a3, 13));
				a[2] = (uint8_t)(gmul(a0, 13) ^ gmul(a1, 9) ^ gmul(a2, 14) ^ gmul(a3, 11));
				a[3] = (uint8_t)(gmul(a0, 11) ^ gmul(a1, 13) ^ gmul(a2, 9) ^ gmul(a3, 14));
			}
		}
	}
	memcpy(out, st, 16);
}

static int
tulip_aes_cbc_init(const struct ccmode_cbc *cbc __unused, cccbc_ctx *ctx, size_t key_len, const void *key)
{
	return aes_expand((struct tulip_aes_ctx *)ctx, key_len, key);
}

static int
tulip_aes_cbc_encrypt(const cccbc_ctx *ctx, cccbc_iv *iv, size_t nblocks, const void *in, void *out)
{
	const uint8_t *ip = in;
	uint8_t *op = out, *v = (uint8_t *)iv;

	while (nblocks--) {
		uint8_t x[16];
		for (int i = 0; i < 16; i++) {
			x[i] = ip[i] ^ v[i];
		}
		aes_encrypt_block((const struct tulip_aes_ctx *)ctx, x, op);
		memcpy(v, op, 16);
		ip += 16;
		op += 16;
	}
	return 0;
}

static int
tulip_aes_cbc_decrypt(const cccbc_ctx *ctx, cccbc_iv *iv, size_t nblocks, const void *in, void *out)
{
	const uint8_t *ip = in;
	uint8_t *op = out, *v = (uint8_t *)iv;

	while (nblocks--) {
		uint8_t c[16];
		memcpy(c, ip, 16);
		aes_decrypt_block((const struct tulip_aes_ctx *)ctx, c, op);
		for (int i = 0; i < 16; i++) {
			op[i] ^= v[i];
		}
		memcpy(v, c, 16);
		ip += 16;
		op += 16;
	}
	return 0;
}

static int
tulip_aes_ecb_init(const struct ccmode_ecb *ecb __unused, ccecb_ctx *ctx, size_t key_len, const void *key)
{
	return aes_expand((struct tulip_aes_ctx *)ctx, key_len, key);
}

static int
tulip_aes_ecb_encrypt(const ccecb_ctx *ctx, size_t nblocks, const void *in, void *out)
{
	for (size_t i = 0; i < nblocks; i++) {
		aes_encrypt_block((const struct tulip_aes_ctx *)ctx, (const uint8_t *)in + 16 * i, (uint8_t *)out + 16 * i);
	}
	return 0;
}

static int
tulip_aes_ecb_decrypt(const ccecb_ctx *ctx, size_t nblocks, const void *in, void *out)
{
	for (size_t i = 0; i < nblocks; i++) {
		aes_decrypt_block((const struct tulip_aes_ctx *)ctx, (const uint8_t *)in + 16 * i, (uint8_t *)out + 16 * i);
	}
	return 0;
}

static const struct ccmode_cbc tulip_aes_cbc_enc = {
	.size = sizeof(struct tulip_aes_ctx),
	.block_size = 16,
	.init = tulip_aes_cbc_init,
	.cbc = tulip_aes_cbc_encrypt,
};

static const struct ccmode_cbc tulip_aes_cbc_dec = {
	.size = sizeof(struct tulip_aes_ctx),
	.block_size = 16,
	.init = tulip_aes_cbc_init,
	.cbc = tulip_aes_cbc_decrypt,
};

static const struct ccmode_ecb tulip_aes_ecb_enc = {
	.size = sizeof(struct tulip_aes_ctx),
	.block_size = 16,
	.init = tulip_aes_ecb_init,
	.ecb = tulip_aes_ecb_encrypt,
};

static const struct ccmode_ecb tulip_aes_ecb_dec = {
	.size = sizeof(struct tulip_aes_ctx),
	.block_size = 16,
	.init = tulip_aes_ecb_init,
	.ecb = tulip_aes_ecb_decrypt,
};

static void
aes_self_test(void)
{
	static const uint8_t key[16] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	};
	static const uint8_t plain[16] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
	};
	static const uint8_t cipher[16] = {
		0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30, 0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a,
	};
	struct tulip_aes_ctx ctx;
	uint8_t out[16], back[16];

	aes_expand(&ctx, sizeof(key), key);
	aes_encrypt_block(&ctx, plain, out);
	aes_decrypt_block(&ctx, out, back);
	if (memcmp(out, cipher, 16) != 0 || memcmp(back, plain, 16) != 0) {
		panic("tulip_crypto: AES-128 self-test failed");
	}
}

extern void read_random(void *buffer, u_int numBytes);

static int
tulip_ccrng_generate(struct ccrng_state *rng __unused, size_t outlen, void *out)
{
	read_random(out, (u_int)outlen);
	return 0;
}

static struct ccrng_state tulip_ccrng = {
	.generate = tulip_ccrng_generate,
};

static struct ccrng_state *
tulip_ccrng_fn(int *error)
{
	if (error) {
		*error = 0;
	}
	return &tulip_ccrng;
}

static struct crypto_functions tulip_crypto_funcs = {
	.random_generate_fn = tulip_random_generate,
	.random_uniform_fn = tulip_random_uniform,
	.random_kmem_ctx_size_fn = tulip_random_kmem_ctx_size,
	.random_kmem_init_fn = tulip_random_kmem_init,
	.ccaes_ecb_encrypt = &tulip_aes_ecb_enc,
	.ccaes_ecb_decrypt = &tulip_aes_ecb_dec,
	.ccaes_cbc_encrypt = &tulip_aes_cbc_enc,
	.ccaes_cbc_decrypt = &tulip_aes_cbc_dec,
	.ccrng_fn = tulip_ccrng_fn,
};

void
tulip_crypto_register(void)
{
	aes_tables_init();
	aes_self_test();
	register_crypto_functions(&tulip_crypto_funcs);
	register_and_init_prng(&tulip_prng_ctx, &tulip_prng_funcs);
}
