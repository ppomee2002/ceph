#ifdef __KERNEL__
# include <linux/crush/hash.h>
#else
# include "hash.h"
# include <math.h>
# include <stdlib.h>
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
#define ROT_MAX_NEIGHBOR_DIMS 16

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

static int lsh_normalize_dim(int dim)
{
	if (dim <= 0)
		return 0;
	if (dim > LSH_MAX_DIM)
		return LSH_MAX_DIM;
	return dim;
}

static int lsh_normalize_bits(int num_bits)
{
	if (num_bits <= 0)
		return 1;
	if (num_bits > LSH_NUM_HYPERPLANES)
		return LSH_NUM_HYPERPLANES;
	return num_bits;
}

static int lsh_normalize_table_id(int table_id)
{
	int t = table_id % LSH_MAX_TABLES;
	if (t < 0)
		t += LSH_MAX_TABLES;
	return t;
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
 * Build 32 orthonormal projection vectors for orthogonal sign-hash
 * using modified Gram-Schmidt. Deterministic by seed.
 */
static void orth_init_hyperplanes_seeded(float planes[LSH_NUM_HYPERPLANES][LSH_MAX_DIM],
					 int dim, unsigned int seed)
{
	unsigned int state = seed;
	const float eps = 1e-6f;
	dim = lsh_normalize_dim(dim);
	if (dim <= 0)
		return;

	for (int i = 0; i < LSH_NUM_HYPERPLANES; i++) {
		float norm2 = 0.0f;
		int ok = 0;

		for (int attempt = 0; attempt < 8 && !ok; attempt++) {
			for (int j = 0; j < dim; j++)
				planes[i][j] = lsh_rand_u01(&state);

			for (int k = 0; k < i; k++) {
				float proj = 0.0f;
				for (int j = 0; j < dim; j++)
					proj += planes[i][j] * planes[k][j];
				for (int j = 0; j < dim; j++)
					planes[i][j] -= proj * planes[k][j];
			}

			norm2 = 0.0f;
			for (int j = 0; j < dim; j++)
				norm2 += planes[i][j] * planes[i][j];
			if (norm2 > eps)
				ok = 1;
		}

		if (!ok) {
			for (int j = 0; j < dim; j++)
				planes[i][j] = 0.0f;
			planes[i][i % dim] = 1.0f;
			for (int k = 0; k < i; k++) {
				float proj = 0.0f;
				for (int j = 0; j < dim; j++)
					proj += planes[i][j] * planes[k][j];
				for (int j = 0; j < dim; j++)
					planes[i][j] -= proj * planes[k][j];
			}
			norm2 = 0.0f;
			for (int j = 0; j < dim; j++)
				norm2 += planes[i][j] * planes[i][j];
			if (norm2 <= eps)
				norm2 = 1.0f;
		}

		{
			float inv = 1.0f / sqrtf(norm2);
			for (int j = 0; j < dim; j++)
				planes[i][j] *= inv;
		}
		for (int j = dim; j < LSH_MAX_DIM; j++)
			planes[i][j] = 0.0f;
	}
}

/*
 * Sign Random Projection (Cosine LSH): 32 hyperplanes, dot products,
 * sign bits -> single __u32. Deterministic, no malloc, C99 only.
 */
int crush_hash32_lsh_bit(const float *vector, int dim, int bit_idx)
{
	static float lsh_planes[LSH_NUM_HYPERPLANES][LSH_MAX_DIM];
	static int lsh_done;
	float dot = 0.0f;
	const float *v;
	const float *h;
	int d;

	dim = lsh_normalize_dim(dim);
	if (dim <= 0)
		return 0;
	if (bit_idx < 0 || bit_idx >= LSH_NUM_HYPERPLANES)
		return 0;

	if (!lsh_done) {
		lsh_init_hyperplanes(lsh_planes);
		lsh_done = 1;
	}

	v = vector;
	h = lsh_planes[bit_idx];
	d = dim;
	while (d--)
		dot += *v++ * *h++;

	return (dot >= 0.0f) ? 1 : 0;
}

/*
 * Multi-table LSH: table_id selects different random hyperplanes.
 * Seed = base + table_id * 99991 for deterministic, independent tables.
 * Used for Fan-out: N tables -> N hashes -> N PGs (with dedup).
 */
int crush_hash32_lsh_multi_bit(const float *vector, int dim, int table_id, int bit_idx)
{
	static float lsh_planes_multi[LSH_MAX_TABLES][LSH_NUM_HYPERPLANES][LSH_MAX_DIM];
	static int lsh_multi_inited[LSH_MAX_TABLES];
	float dot = 0.0f;
	const float *v;
	const float *h;
	int d;

	dim = lsh_normalize_dim(dim);
	if (dim <= 0)
		return 0;
	if (bit_idx < 0 || bit_idx >= LSH_NUM_HYPERPLANES)
		return 0;

	table_id = lsh_normalize_table_id(table_id);

	if (!lsh_multi_inited[table_id]) {
		unsigned int seed = 1315423911u + ((unsigned int)table_id * 99991u);
		lsh_init_hyperplanes_seeded(lsh_planes_multi[table_id], seed);
		lsh_multi_inited[table_id] = 1;
	}

	v = vector;
	h = lsh_planes_multi[table_id][bit_idx];
	d = dim;
	while (d--)
		dot += *v++ * *h++;

	return (dot >= 0.0f) ? 1 : 0;
}

__u32 crush_hash32_lsh_k(const float *vector, int dim, int num_bits)
{
	__u32 hash = 0;
	int bits = lsh_normalize_bits(num_bits);
	for (int i = 0; i < bits; i++) {
		if (crush_hash32_lsh_bit(vector, dim, i))
			hash |= (1u << i);
	}
	return hash;
}

__u32 crush_hash32_lsh_multi_k(const float *vector, int dim, int table_id, int num_bits)
{
	__u32 hash = 0;
	int bits = lsh_normalize_bits(num_bits);
	for (int i = 0; i < bits; i++) {
		if (crush_hash32_lsh_multi_bit(vector, dim, table_id, i))
			hash |= (1u << i);
	}
	return hash;
}

void crush_hash32_lsh_multi_tables(const float *vector, int dim, int num_tables,
				   int num_bits, __u32 *out_hashes)
{
	if (!out_hashes || num_tables <= 0)
		return;
	for (int t = 0; t < num_tables; t++) {
		out_hashes[t] = crush_hash32_lsh_multi_k(vector, dim, t, num_bits);
	}
}

__u32 crush_hash32_lsh(const float *vector, int dim)
{
	return crush_hash32_lsh_k(vector, dim, LSH_NUM_HYPERPLANES);
}

__u32 crush_hash32_lsh_n(const float *vector, int dim, int num_bits)
{
	if (num_bits <= 0 || num_bits >= 32)
		return crush_hash32_lsh_k(vector, dim, LSH_NUM_HYPERPLANES);
	return crush_hash32_lsh_k(vector, dim, num_bits);
}

__u32 crush_hash32_lsh_multi(const float *vector, int dim, int table_id)
{
	__u32 hash = crush_hash32_lsh_multi_k(vector, dim, table_id, LSH_NUM_HYPERPLANES);
	return hash;
}

int crush_hash32_orth_multi_bit(const float *vector, int dim, int table_id, int bit_idx,
				__u32 rot_seed)
{
	static float orth_planes_multi[LSH_MAX_TABLES][LSH_NUM_HYPERPLANES][LSH_MAX_DIM];
	static int orth_multi_inited[LSH_MAX_TABLES];
	static int orth_multi_dim[LSH_MAX_TABLES];
	static __u32 orth_multi_seed[LSH_MAX_TABLES];
	float dot = 0.0f;
	const float *v;
	const float *h;
	int d;

	dim = lsh_normalize_dim(dim);
	if (dim <= 0)
		return 0;
	if (bit_idx < 0 || bit_idx >= LSH_NUM_HYPERPLANES)
		return 0;
	table_id = lsh_normalize_table_id(table_id);

	if (!orth_multi_inited[table_id] ||
	    orth_multi_dim[table_id] != dim ||
	    orth_multi_seed[table_id] != rot_seed) {
		unsigned int seed = (unsigned int)rot_seed + ((unsigned int)table_id * 99991u);
		orth_init_hyperplanes_seeded(orth_planes_multi[table_id], dim, seed);
		orth_multi_dim[table_id] = dim;
		orth_multi_seed[table_id] = rot_seed;
		orth_multi_inited[table_id] = 1;
	}

	v = vector;
	h = orth_planes_multi[table_id][bit_idx];
	d = dim;
	while (d--)
		dot += *v++ * *h++;

	return (dot >= 0.0f) ? 1 : 0;
}

__u32 crush_hash32_orth_multi_k(const float *vector, int dim, int table_id, int num_bits,
				__u32 rot_seed)
{
	__u32 hash = 0;
	int bits = lsh_normalize_bits(num_bits);
	for (int i = 0; i < bits; i++) {
		if (crush_hash32_orth_multi_bit(vector, dim, table_id, i, rot_seed))
			hash |= (1u << i);
	}
	return hash;
}

__u32 crush_hash32_orth_multi(const float *vector, int dim, int table_id, __u32 rot_seed)
{
	return crush_hash32_orth_multi_k(vector, dim, table_id, LSH_NUM_HYPERPLANES, rot_seed);
}

static int rotation_normalize_use_dims(int use_dims, int dim)
{
	if (use_dims <= 0)
		return 0;
	if (use_dims > dim)
		return dim;
	return use_dims;
}

static int rotation_normalize_bins(int bins_per_dim)
{
	if (bins_per_dim < 2)
		return 2;
	if (bins_per_dim > 1024)
		return 1024;
	return bins_per_dim;
}

static float *rotation_get_matrix_legacy(int dim, int table_id, __u32 rot_seed)
{
	static float *rot_matrix_multi_legacy[LSH_MAX_TABLES];
	static int rot_matrix_dim_legacy[LSH_MAX_TABLES];
	static __u32 rot_matrix_seed_legacy[LSH_MAX_TABLES];
	static int rot_matrix_inited_legacy[LSH_MAX_TABLES];
	const float eps = 1e-6f;
	float *mat;
	unsigned int state;
	int cells;

	dim = lsh_normalize_dim(dim);
	if (dim <= 0)
		return NULL;
	table_id = lsh_normalize_table_id(table_id);

	if (rot_matrix_inited_legacy[table_id] &&
	    rot_matrix_dim_legacy[table_id] == dim &&
	    rot_matrix_seed_legacy[table_id] == rot_seed) {
		return rot_matrix_multi_legacy[table_id];
	}

	if (rot_matrix_multi_legacy[table_id]) {
		free(rot_matrix_multi_legacy[table_id]);
		rot_matrix_multi_legacy[table_id] = NULL;
	}

	cells = dim * dim;
	mat = (float *)malloc((size_t)cells * sizeof(float));
	if (!mat) {
		rot_matrix_inited_legacy[table_id] = 0;
		return NULL;
	}

	state = (unsigned int)rot_seed + ((unsigned int)table_id * 99991u);

	for (int i = 0; i < dim; i++) {
		float norm2 = 0.0f;
		int ok = 0;
		float *row = mat + (size_t)i * dim;

		for (int attempt = 0; attempt < 8 && !ok; attempt++) {
			for (int j = 0; j < dim; j++)
				row[j] = lsh_rand_u01(&state);

			for (int k = 0; k < i; k++) {
				float proj = 0.0f;
				const float *prev = mat + (size_t)k * dim;
				for (int j = 0; j < dim; j++)
					proj += row[j] * prev[j];
				for (int j = 0; j < dim; j++)
					row[j] -= proj * prev[j];
			}

			norm2 = 0.0f;
			for (int j = 0; j < dim; j++)
				norm2 += row[j] * row[j];
			if (norm2 > eps)
				ok = 1;
		}

		if (!ok) {
			for (int j = 0; j < dim; j++)
				row[j] = 0.0f;
			row[i % dim] = 1.0f;
			for (int k = 0; k < i; k++) {
				float proj = 0.0f;
				const float *prev = mat + (size_t)k * dim;
				for (int j = 0; j < dim; j++)
					proj += row[j] * prev[j];
				for (int j = 0; j < dim; j++)
					row[j] -= proj * prev[j];
			}
			norm2 = 0.0f;
			for (int j = 0; j < dim; j++)
				norm2 += row[j] * row[j];
			if (norm2 <= eps)
				norm2 = 1.0f;
		}

		{
			float inv = 1.0f / sqrtf(norm2);
			for (int j = 0; j < dim; j++)
				row[j] *= inv;
		}
	}

	rot_matrix_multi_legacy[table_id] = mat;
	rot_matrix_dim_legacy[table_id] = dim;
	rot_matrix_seed_legacy[table_id] = rot_seed;
	rot_matrix_inited_legacy[table_id] = 1;
	return mat;
}

int crush_hash32_rotation_apply(const float *vector, int dim, int table_id,
				__u32 rot_seed, float *out_rotated, int out_dim)
{
	float *mat;

	dim = lsh_normalize_dim(dim);
	if (!vector || !out_rotated || dim <= 0 || out_dim < dim)
		return 0;

	mat = rotation_get_matrix_legacy(dim, table_id, rot_seed);
	if (!mat)
		return 0;

	for (int i = 0; i < dim; i++) {
		const float *row = mat + (size_t)i * dim;
		float dot = 0.0f;
		for (int j = 0; j < dim; j++)
			dot += row[j] * vector[j];
		out_rotated[i] = dot;
	}
	return dim;
}

int crush_hash32_faiss_rotation_apply(const float *vector, int dim, int table_id,
				      __u32 rot_seed, float *out_rotated, int out_dim)
{
	return crush_hash32_faiss_rotation_apply_ref(
		vector, dim, table_id, rot_seed, out_rotated, out_dim);
}

int crush_hash32_rotation_quantize_prefix(const float *rotated, int dim,
					  int use_dims, float min_val, float width,
					  int bins_per_dim, int *out_bins)
{
	int m;

	dim = lsh_normalize_dim(dim);
	if (!rotated || !out_bins || dim <= 0 || width <= 0.0f)
		return 0;
	bins_per_dim = rotation_normalize_bins(bins_per_dim);
	m = rotation_normalize_use_dims(use_dims, dim);
	if (m <= 0)
		return 0;

	for (int i = 0; i < m; i++) {
		float scaled = (rotated[i] - min_val) / width;
		int b = (int)floorf(scaled);
		if (b < 0)
			b = 0;
		if (b >= bins_per_dim)
			b = bins_per_dim - 1;
		out_bins[i] = b;
	}
	return m;
}

__u64 crush_hash32_rotation_encode_bucket(const int *bins, int use_dims, int bins_per_dim)
{
	__u64 bucket = 0;
	__u64 stride = 1;
	int bpd = rotation_normalize_bins(bins_per_dim);
	if (!bins || use_dims <= 0)
		return 0;

	for (int i = 0; i < use_dims; i++) {
		int b = bins[i];
		if (b < 0)
			b = 0;
		if (b >= bpd)
			b = bpd - 1;
		bucket += ((__u64)b) * stride;
		stride *= (__u64)bpd;
	}
	return bucket;
}

__u32 crush_hash32_rotation_bucket_to_pg(__u64 bucket_id, __u32 pg_num)
{
	if (pg_num == 0)
		return 0;
	return (__u32)(bucket_id % (__u64)pg_num);
}

__u32 crush_hash32_rotation_seed_pg(const float *rotated, int dim, __u32 pg_num)
{
	__u32 h = 1315423911u;
	int m;

	dim = lsh_normalize_dim(dim);
	if (!rotated || dim <= 0 || pg_num == 0)
		return 0;

	m = dim;
	if (m > 8)
		m = 8;

	for (int i = 0; i < m; i++) {
		union {
			float f;
			__u32 u;
		} v;
		v.f = rotated[i];
		h = crush_hash32_2(CRUSH_HASH_DEFAULT,
				   h ^ ((__u32)i * 2654435761u),
				   v.u + 0x9e3779b9u);
	}
	return h % pg_num;
}

int crush_hash32_rotation_neighbor_buckets(const int *bins, int use_dims,
					   int bins_per_dim, int neighbor_step,
					   __u64 *out_bucket_ids, int max_bucket_ids)
{
	int m;
	int bpd;
	int ranges[ROT_MAX_NEIGHBOR_DIMS];
	int pos[ROT_MAX_NEIGHBOR_DIMS];
	int steps[ROT_MAX_NEIGHBOR_DIMS];
	int emitted = 0;

	if (!bins || !out_bucket_ids || max_bucket_ids <= 0)
		return 0;

	m = rotation_normalize_use_dims(use_dims, ROT_MAX_NEIGHBOR_DIMS);
	if (m <= 0)
		return 0;
	if (m > ROT_MAX_NEIGHBOR_DIMS)
		m = ROT_MAX_NEIGHBOR_DIMS;
	bpd = rotation_normalize_bins(bins_per_dim);
	if (neighbor_step < 0)
		neighbor_step = 0;

	for (int i = 0; i < m; i++) {
		int lo = bins[i] - neighbor_step;
		int hi = bins[i] + neighbor_step;
		if (lo < 0)
			lo = 0;
		if (hi >= bpd)
			hi = bpd - 1;
		ranges[i] = lo;
		pos[i] = lo;
		steps[i] = hi - lo + 1;
		if (steps[i] <= 0)
			steps[i] = 1;
	}

	while (emitted < max_bucket_ids) {
		int qbins[ROT_MAX_NEIGHBOR_DIMS];
		__u64 bucket;
		int dup = 0;

		for (int i = 0; i < m; i++)
			qbins[i] = pos[i];
		bucket = crush_hash32_rotation_encode_bucket(qbins, m, bpd);

		for (int i = 0; i < emitted; i++) {
			if (out_bucket_ids[i] == bucket) {
				dup = 1;
				break;
			}
		}
		if (!dup)
			out_bucket_ids[emitted++] = bucket;

		for (int d = 0; d < m; d++) {
			if (pos[d] + 1 < ranges[d] + steps[d]) {
				pos[d]++;
				break;
			}
			pos[d] = ranges[d];
			if (d == m - 1)
				return emitted;
		}
	}
	return emitted;
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
