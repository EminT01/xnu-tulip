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
#include <corecrypto/ccdigest.h>
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

static void
tulip_digest_init(const struct ccdigest_info *di, ccdigest_ctx_t ctx)
{
	ccdigest_nbits(di, ctx) = 0;
	memcpy(ccdigest_state(di, ctx), di->initial_state, di->state_size);
	ccdigest_num(di, ctx) = 0;
}

static void
tulip_digest_update(const struct ccdigest_info *di, ccdigest_ctx_t ctx, size_t len, const void *data)
{
	const uint8_t *p = data;
	uint8_t *buf = ccdigest_data(di, ctx);

	while (len) {
		unsigned int num = ccdigest_num(di, ctx);
		if (num == 0 && len >= di->block_size) {
			size_t nblocks = len / di->block_size;
			di->compress(ccdigest_state(di, ctx), nblocks, p);
			ccdigest_nbits(di, ctx) += (uint64_t)nblocks * di->block_size * 8;
			p += nblocks * di->block_size;
			len -= nblocks * di->block_size;
			continue;
		}
		size_t n = di->block_size - num;
		if (n > len) {
			n = len;
		}
		memcpy(buf + num, p, n);
		num += (unsigned int)n;
		p += n;
		len -= n;
		if (num == di->block_size) {
			di->compress(ccdigest_state(di, ctx), 1, buf);
			ccdigest_nbits(di, ctx) += di->block_size * 8;
			num = 0;
		}
		ccdigest_num(di, ctx) = num;
	}
}

static const uint32_t sha256_k[64] = {
	0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
	0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
	0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
	0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
	0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
	0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
	0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
	0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};

static const uint32_t sha256_initial[8] = {
	0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19,
};

static uint32_t
ror32(uint32_t x, int k)
{
	return (x >> k) | (x << (32 - k));
}

static void
sha256_compress(ccdigest_state_t state, size_t nblocks, const void *data)
{
	uint32_t *h = (uint32_t *)state;
	const uint8_t *p = data;

	while (nblocks--) {
		uint32_t w[64], a, b, c, d, e, f, g, hh;

		for (int i = 0; i < 16; i++) {
			w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
			    ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
		}
		for (int i = 16; i < 64; i++) {
			uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
			uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
			w[i] = w[i - 16] + s0 + w[i - 7] + s1;
		}
		a = h[0]; b = h[1]; c = h[2]; d = h[3]; e = h[4]; f = h[5]; g = h[6]; hh = h[7];
		for (int i = 0; i < 64; i++) {
			uint32_t t1 = hh + (ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25)) + ((e & f) ^ (~e & g)) + sha256_k[i] + w[i];
			uint32_t t2 = (ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
			hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
		p += 64;
	}
}

static void
sha256_final(const struct ccdigest_info *di, ccdigest_ctx_t ctx, unsigned char *digest)
{
	uint8_t *buf = ccdigest_data(di, ctx);
	unsigned int num = ccdigest_num(di, ctx);
	uint64_t bits = ccdigest_nbits(di, ctx) + (uint64_t)num * 8;
	uint32_t *h = (uint32_t *)ccdigest_state(di, ctx);

	buf[num++] = 0x80;
	if (num > 56) {
		memset(buf + num, 0, 64 - num);
		sha256_compress(ccdigest_state(di, ctx), 1, buf);
		num = 0;
	}
	memset(buf + num, 0, 56 - num);
	for (int i = 0; i < 8; i++) {
		buf[56 + i] = (uint8_t)(bits >> (56 - 8 * i));
	}
	sha256_compress(ccdigest_state(di, ctx), 1, buf);
	for (int i = 0; i < 8; i++) {
		digest[4 * i] = (uint8_t)(h[i] >> 24);
		digest[4 * i + 1] = (uint8_t)(h[i] >> 16);
		digest[4 * i + 2] = (uint8_t)(h[i] >> 8);
		digest[4 * i + 3] = (uint8_t)h[i];
	}
}

static const unsigned char sha256_oid[] = { 0x06, 0x09, 0x60, 0x86, 0x48, 0x01, 0x65, 0x03, 0x04, 0x02, 0x01 };

static const struct ccdigest_info tulip_sha256_di = {
	.output_size = 32,
	.state_size = 32,
	.block_size = 64,
	.oid_size = sizeof(sha256_oid),
	.oid = sha256_oid,
	.initial_state = sha256_initial,
	.compress = sha256_compress,
	.final = sha256_final,
};

static void
sha256_self_test(void)
{
	static const uint8_t expect[32] = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad,
	};
	ccdigest_di_decl(&tulip_sha256_di, ctx);
	uint8_t out[32];

	tulip_digest_init(&tulip_sha256_di, ctx);
	tulip_digest_update(&tulip_sha256_di, ctx, 3, "abc");
	sha256_final(&tulip_sha256_di, ctx, out);
	if (memcmp(out, expect, 32) != 0) {
		panic("tulip_crypto: SHA-256 self-test failed");
	}
}

static const uint32_t md5_t[64] = {
	0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
	0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
	0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
	0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
	0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
	0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
	0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
	0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391,
};

static const uint8_t md5_s[64] = {
	7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
	5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
	4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
	6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21,
};

static const uint32_t md5_initial[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };

static uint32_t
rol32(uint32_t x, int k)
{
	return (x << k) | (x >> (32 - k));
}

static void
md5_compress(ccdigest_state_t state, size_t nblocks, const void *data)
{
	uint32_t *h = (uint32_t *)state;
	const uint8_t *p = data;

	while (nblocks--) {
		uint32_t m[16], a = h[0], b = h[1], c = h[2], d = h[3];

		for (int i = 0; i < 16; i++) {
			m[i] = (uint32_t)p[4 * i] | ((uint32_t)p[4 * i + 1] << 8) |
			    ((uint32_t)p[4 * i + 2] << 16) | ((uint32_t)p[4 * i + 3] << 24);
		}
		for (int i = 0; i < 64; i++) {
			uint32_t f;
			int g;

			if (i < 16) {
				f = (b & c) | (~b & d);
				g = i;
			} else if (i < 32) {
				f = (d & b) | (~d & c);
				g = (5 * i + 1) % 16;
			} else if (i < 48) {
				f = b ^ c ^ d;
				g = (3 * i + 5) % 16;
			} else {
				f = c ^ (b | ~d);
				g = (7 * i) % 16;
			}
			uint32_t t = d;
			d = c;
			c = b;
			b = b + rol32(a + f + md5_t[i] + m[g], md5_s[i]);
			a = t;
		}
		h[0] += a; h[1] += b; h[2] += c; h[3] += d;
		p += 64;
	}
}

static void
md5_final(const struct ccdigest_info *di, ccdigest_ctx_t ctx, unsigned char *digest)
{
	uint8_t *buf = ccdigest_data(di, ctx);
	unsigned int num = ccdigest_num(di, ctx);
	uint64_t bits = ccdigest_nbits(di, ctx) + (uint64_t)num * 8;
	uint32_t *h = (uint32_t *)ccdigest_state(di, ctx);

	buf[num++] = 0x80;
	if (num > 56) {
		memset(buf + num, 0, 64 - num);
		md5_compress(ccdigest_state(di, ctx), 1, buf);
		num = 0;
	}
	memset(buf + num, 0, 56 - num);
	for (int i = 0; i < 8; i++) {
		buf[56 + i] = (uint8_t)(bits >> (8 * i));
	}
	md5_compress(ccdigest_state(di, ctx), 1, buf);
	for (int i = 0; i < 4; i++) {
		digest[4 * i] = (uint8_t)h[i];
		digest[4 * i + 1] = (uint8_t)(h[i] >> 8);
		digest[4 * i + 2] = (uint8_t)(h[i] >> 16);
		digest[4 * i + 3] = (uint8_t)(h[i] >> 24);
	}
}

static const unsigned char md5_oid[] = { 0x06, 0x08, 0x2a, 0x86, 0x48, 0x86, 0xf7, 0x0d, 0x02, 0x05 };

static const struct ccdigest_info tulip_md5_di = {
	.output_size = 16,
	.state_size = 16,
	.block_size = 64,
	.oid_size = sizeof(md5_oid),
	.oid = md5_oid,
	.initial_state = md5_initial,
	.compress = md5_compress,
	.final = md5_final,
};

static void
md5_self_test(void)
{
	static const uint8_t expect[16] = {
		0x90, 0x01, 0x50, 0x98, 0x3c, 0xd2, 0x4f, 0xb0, 0xd6, 0x96, 0x3f, 0x7d, 0x28, 0xe1, 0x7f, 0x72,
	};
	ccdigest_di_decl(&tulip_md5_di, ctx);
	uint8_t out[16];

	tulip_digest_init(&tulip_md5_di, ctx);
	tulip_digest_update(&tulip_md5_di, ctx, 3, "abc");
	md5_final(&tulip_md5_di, ctx, out);
	if (memcmp(out, expect, 16) != 0) {
		panic("tulip_crypto: MD5 self-test failed");
	}
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
	.ccdigest_init_fn = tulip_digest_init,
	.ccdigest_update_fn = tulip_digest_update,
	.ccsha256_di = &tulip_sha256_di,
	.ccmd5_di = &tulip_md5_di,
};

void
tulip_crypto_register(void)
{
	aes_tables_init();
	aes_self_test();
	sha256_self_test();
	md5_self_test();
	register_crypto_functions(&tulip_crypto_funcs);
	register_and_init_prng(&tulip_prng_ctx, &tulip_prng_funcs);
}
