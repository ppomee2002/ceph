#ifdef __KERNEL__
# include <linux/crush/hash.h>
#else
# include "hash.h"
#endif

/*
 * Robert Jenkins' function for mixing 32-bit values
 * http://burtleburtle.net/bob/hash/evahash.html
 * a, b = random bits, c = input and output
 */
#define crush_hashmix(a, b, c) do {			\
		a = a-b;  a = a-c;  a = a^(c>>13);	\
		b = b-c;  b = b-a;  b = b^(a<<8);	\
		c = c-a;  c = c-b;  c = c^(b>>13);	\
		a = a-b;  a = a-c;  a = a^(c>>12);	\
		b = b-c;  b = b-a;  b = b^(a<<16);	\
		c = c-a;  c = c-b;  c = c^(b>>5);	\
		a = a-b;  a = a-c;  a = a^(c>>3);	\
		b = b-c;  b = b-a;  b = b^(a<<10);	\
		c = c-a;  c = c-b;  c = c^(b>>15);	\
	} while (0)

#define crush_hash_seed 1315423911

static __u32 crush_hash32_rjenkins1(__u32 a)
{
	__u32 hash = crush_hash_seed ^ a;
	__u32 b = a;
	__u32 x = 231232;
	__u32 y = 1232;
	crush_hashmix(b, x, hash);
	crush_hashmix(y, a, hash);
	return hash;
}

static __u32 crush_hash32_rjenkins1_2(__u32 a, __u32 b)
{
	__u32 hash = crush_hash_seed ^ a ^ b;
	__u32 x = 231232;
	__u32 y = 1232;
	crush_hashmix(a, b, hash);
	crush_hashmix(x, a, hash);
	crush_hashmix(b, y, hash);
	return hash;
}

static __u32 crush_hash32_rjenkins1_3(__u32 a, __u32 b, __u32 c)
{
	__u32 hash = crush_hash_seed ^ a ^ b ^ c;
	__u32 x = 231232;
	__u32 y = 1232;
	crush_hashmix(a, b, hash);
	crush_hashmix(c, x, hash);
	crush_hashmix(y, a, hash);
	crush_hashmix(b, x, hash);
	crush_hashmix(y, c, hash);
	return hash;
}

static __u32 crush_hash32_rjenkins1_4(__u32 a, __u32 b, __u32 c, __u32 d)
{
	__u32 hash = crush_hash_seed ^ a ^ b ^ c ^ d;
	__u32 x = 231232;
	__u32 y = 1232;
	crush_hashmix(a, b, hash);
	crush_hashmix(c, d, hash);
	crush_hashmix(a, x, hash);
	crush_hashmix(y, b, hash);
	crush_hashmix(c, x, hash);
	crush_hashmix(y, d, hash);
	return hash;
}

static __u32 crush_hash32_rjenkins1_5(__u32 a, __u32 b, __u32 c, __u32 d,
				      __u32 e)
{
	__u32 hash = crush_hash_seed ^ a ^ b ^ c ^ d ^ e;
	__u32 x = 231232;
	__u32 y = 1232;
	crush_hashmix(a, b, hash);
	crush_hashmix(c, d, hash);
	crush_hashmix(e, x, hash);
	crush_hashmix(y, a, hash);
	crush_hashmix(b, x, hash);
	crush_hashmix(y, c, hash);
	crush_hashmix(d, x, hash);
	crush_hashmix(y, e, hash);
	return hash;
}


#ifndef __KERNEL__

#define LSH_NUM_HYPERPLANES  32
#define LSH_MAX_DIM         1024
#define LSH_MAX_TABLES      256

/*
 * Deterministic PRNG for hyperplane init (LCG, no external deps).
 * Seed 1315423911 matches crush_hash_seed for consistency.
 */
static float lsh_rand_u01(unsigned int *state)
{
	*state = *state * 1103515245u + 12345u;
	return (float)(*state) / (float)(0xffffffffu) * 2.0f - 1.0f;
}

static void lsh_init_hyperplanes(float planes[LSH_NUM_HYPERPLANES][LSH_MAX_DIM])
{
	unsigned int state = 1315423911u;
	for (int i = 0; i < LSH_NUM_HYPERPLANES; i++) {
		for (int j = 0; j < LSH_MAX_DIM; j++) {
			planes[i][j] = lsh_rand_u01(&state);
		}
	}
}

/*
 * Initialize hyperplanes for a specific table_id (different seed per table).
 * Used for multi-table LSH (Fan-out architecture).
 */
static void lsh_init_hyperplanes_seeded(float planes[LSH_NUM_HYPERPLANES][LSH_MAX_DIM],
					unsigned int seed)
{
	unsigned int state = seed;
	for (int i = 0; i < LSH_NUM_HYPERPLANES; i++) {
		for (int j = 0; j < LSH_MAX_DIM; j++) {
			planes[i][j] = lsh_rand_u01(&state);
		}
	}
}

/*
 * Sign Random Projection (Cosine LSH): 32 hyperplanes, dot products,
 * sign bits -> single __u32. Deterministic, no malloc, C99 only.
 */
__u32 crush_hash32_lsh(const float *vector, int dim)
{
	static float lsh_planes[LSH_NUM_HYPERPLANES][LSH_MAX_DIM];
	static int lsh_done;

	if (dim <= 0)
		return 0;

	if (!lsh_done) {
		lsh_init_hyperplanes(lsh_planes);
		lsh_done = 1;
	}

	if (dim > LSH_MAX_DIM)
		dim = LSH_MAX_DIM;

	__u32 hash = 0;
	for (int i = 0; i < LSH_NUM_HYPERPLANES; i++) {
		float dot = 0.0f;
		const float *v = vector;
		const float *h = lsh_planes[i];
		int d = dim;

		while (d--) {
			dot += *v++ * *h++;
		}

		hash |= ((dot >= 0.0f) ? 1u : 0u) << i;
	}
	return hash;
}

__u32 crush_hash32_lsh_n(const float *vector, int dim, int num_bits)
{
	__u32 full = crush_hash32_lsh(vector, dim);
	if (num_bits <= 0 || num_bits >= 32)
		return full;
	return full & ((1u << num_bits) - 1);
}

/*
 * Multi-table LSH: table_id selects different random hyperplanes.
 * Seed = base + table_id * 99991 for deterministic, independent tables.
 * Used for Fan-out: N tables -> N hashes -> N PGs (with dedup).
 */
__u32 crush_hash32_lsh_multi(const float *vector, int dim, int table_id)
{
	static float lsh_planes_multi[LSH_MAX_TABLES][LSH_NUM_HYPERPLANES][LSH_MAX_DIM];
	static int lsh_multi_inited[LSH_MAX_TABLES];

	if (dim <= 0)
		return 0;

	if (table_id < 0 || table_id >= LSH_MAX_TABLES)
		table_id = table_id % LSH_MAX_TABLES;

	if (!lsh_multi_inited[table_id]) {
		unsigned int seed = 1315423911u + ((unsigned int)table_id * 99991u);
		lsh_init_hyperplanes_seeded(lsh_planes_multi[table_id], seed);
		lsh_multi_inited[table_id] = 1;
	}

	if (dim > LSH_MAX_DIM)
		dim = LSH_MAX_DIM;

	__u32 hash = 0;
	for (int i = 0; i < LSH_NUM_HYPERPLANES; i++) {
		float dot = 0.0f;
		const float *v = vector;
		const float *h = lsh_planes_multi[table_id][i];
		int d = dim;

		while (d--) {
			dot += *v++ * *h++;
		}

		hash |= ((dot >= 0.0f) ? 1u : 0u) << i;
	}
	return hash;
}

#endif /* !__KERNEL__ */

__u32 crush_hash32(int type, __u32 a)
{
	switch (type) {
	case CRUSH_HASH_RJENKINS1:
		return crush_hash32_rjenkins1(a);
	default:
		return 0;
	}
}

__u32 crush_hash32_2(int type, __u32 a, __u32 b)
{
	switch (type) {
	case CRUSH_HASH_RJENKINS1:
		return crush_hash32_rjenkins1_2(a, b);
	default:
		return 0;
	}
}

__u32 crush_hash32_3(int type, __u32 a, __u32 b, __u32 c)
{
	switch (type) {
	case CRUSH_HASH_RJENKINS1:
		return crush_hash32_rjenkins1_3(a, b, c);
	default:
		return 0;
	}
}

__u32 crush_hash32_4(int type, __u32 a, __u32 b, __u32 c, __u32 d)
{
	switch (type) {
	case CRUSH_HASH_RJENKINS1:
		return crush_hash32_rjenkins1_4(a, b, c, d);
	default:
		return 0;
	}
}

__u32 crush_hash32_5(int type, __u32 a, __u32 b, __u32 c, __u32 d, __u32 e)
{
	switch (type) {
	case CRUSH_HASH_RJENKINS1:
		return crush_hash32_rjenkins1_5(a, b, c, d, e);
	default:
		return 0;
	}
}

const char *crush_hash_name(int type)
{
	switch (type) {
	case CRUSH_HASH_RJENKINS1:
		return "rjenkins1";
	default:
		return "unknown";
	}
}
