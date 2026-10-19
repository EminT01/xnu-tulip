/*
 * Copyright (c) 2026 Wiktor Bryk
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>
#include <kern/misc_protos.h>
#include <libkern/crypto/register_crypto.h>
#include <libkern/crypto/rand.h>
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

static struct crypto_functions tulip_crypto_funcs = {
	.random_generate_fn = tulip_random_generate,
	.random_uniform_fn = tulip_random_uniform,
	.random_kmem_ctx_size_fn = tulip_random_kmem_ctx_size,
	.random_kmem_init_fn = tulip_random_kmem_init,
};

void
tulip_crypto_register(void)
{
	register_crypto_functions(&tulip_crypto_funcs);
	register_and_init_prng(&tulip_prng_ctx, &tulip_prng_funcs);
}
