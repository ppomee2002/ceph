#ifndef CEPH_CRUSH_HASH_H
#define CEPH_CRUSH_HASH_H

#ifdef __KERNEL__
# include <linux/types.h>
#else
# include "crush_compat.h"
#endif

#define CRUSH_HASH_RJENKINS1   0

#define CRUSH_HASH_DEFAULT CRUSH_HASH_RJENKINS1

#ifdef __cplusplus
extern "C" {
#endif

extern const char *crush_hash_name(int type);

extern __u32 crush_hash32(int type, __u32 a);
extern __u32 crush_hash32_2(int type, __u32 a, __u32 b);
extern __u32 crush_hash32_3(int type, __u32 a, __u32 b, __u32 c);
extern __u32 crush_hash32_4(int type, __u32 a, __u32 b, __u32 c, __u32 d);
extern __u32 crush_hash32_5(int type, __u32 a, __u32 b, __u32 c, __u32 d,
			    __u32 e);

#ifndef __KERNEL__
/*
 * Sign Random Projection (Cosine LSH):
 * - One hash function produces exactly one bit: sign(r_i dot x)
 * - K-bit hash is composed by concatenating K independent 1-bit hashes.
 */
extern int crush_hash32_lsh_bit(const float *vector, int dim, int bit_idx);
extern int crush_hash32_lsh_multi_bit(const float *vector, int dim, int table_id, int bit_idx);
/* Compose K bits for single-table hash (K in [1, 32], clamped). */
extern __u32 crush_hash32_lsh_k(const float *vector, int dim, int num_bits);
/* Compose K bits for one table in multi-table mode (K in [1, 32], clamped). */
extern __u32 crush_hash32_lsh_multi_k(const float *vector, int dim, int table_id, int num_bits);
/* Fill out_hashes[t] with table t hash, t in [0, num_tables). */
extern void crush_hash32_lsh_multi_tables(const float *vector, int dim, int num_tables,
					  int num_bits, __u32 *out_hashes);
/* Backward-compatible 32-bit single-table hash. */
extern __u32 crush_hash32_lsh(const float *vector, int dim);
/* Backward-compatible, masked to num_bits (for pg_num alignment). */
extern __u32 crush_hash32_lsh_n(const float *vector, int dim, int num_bits);
/* Backward-compatible 32-bit multi-table hash (table-specific hyperplanes). */
extern __u32 crush_hash32_lsh_multi(const float *vector, int dim, int table_id);
/*
 * Orthogonal sign-hash (table-specific, deterministic):
 * - Build 32 orthonormal projection vectors per table.
 * - Hash bit i = sign(q_i dot x), i in [0, num_bits).
 * - This is bit-hash routing, NOT continuous random-rotation routing.
 */
extern int crush_hash32_orth_multi_bit(const float *vector, int dim, int table_id,
				       int bit_idx, __u32 rot_seed);
extern __u32 crush_hash32_orth_multi_k(const float *vector, int dim, int table_id,
				       int num_bits, __u32 rot_seed);
extern __u32 crush_hash32_orth_multi(const float *vector, int dim, int table_id,
				     __u32 rot_seed);
/*
 * Random rotation (Faiss RandomRotationMatrix style) APIs:
 * - Build deterministic table-specific orthonormal rotation matrix R.
 * - out_rotated = R * vector (continuous float output, not sign-bit hash).
 */
extern int crush_hash32_rotation_apply(const float *vector, int dim, int table_id,
				       __u32 rot_seed, float *out_rotated, int out_dim);
/*
 * Faiss-like random rotation:
 * - Gaussian random matrix initialization.
 * - Gram-Schmidt orthonormalization.
 * - Deterministic for (rot_seed, table_id, dim).
 */
extern int crush_hash32_faiss_rotation_apply(const float *vector, int dim, int table_id,
					     __u32 rot_seed, float *out_rotated, int out_dim);
/*
 * Reference-compatible helper for faiss_rotation verification:
 * - follows faiss RandomRotationMatrix init/apply convention.
 */
extern int crush_hash32_faiss_rotation_apply_ref(const float *vector, int dim, int table_id,
						 __u32 rot_seed, float *out_rotated, int out_dim);
/*
 * Coarse quantization on first use_dims entries:
 *   b_i = floor((y_i - min_val) / width), clamped to [0, bins_per_dim-1].
 * Returns number of produced bins (0 on invalid input).
 */
extern int crush_hash32_rotation_quantize_prefix(const float *rotated, int dim,
						 int use_dims, float min_val, float width,
						 int bins_per_dim, int *out_bins);
/* Encode (b_0, ..., b_{m-1}) into deterministic 64-bit bucket id. */
extern __u64 crush_hash32_rotation_encode_bucket(const int *bins, int use_dims,
						 int bins_per_dim);
/* PG mapping policy for rotation buckets: pg = bucket_id % pg_num. */
extern __u32 crush_hash32_rotation_bucket_to_pg(__u64 bucket_id, __u32 pg_num);
/*
 * Prototype helper for rotation_repr1 backend:
 * - deterministically map rotated vector to a seed PG id.
 * - used only to choose first representative assignment candidates.
 */
extern __u32 crush_hash32_rotation_seed_pg(const float *rotated, int dim, __u32 pg_num);
/*
 * Optional neighboring bucket generation for recall near quantization boundaries.
 * neighbor_step=0 => only primary bucket.
 * Returns written bucket count (up to max_bucket_ids).
 */
extern int crush_hash32_rotation_neighbor_buckets(const int *bins, int use_dims,
						  int bins_per_dim, int neighbor_step,
						  __u64 *out_bucket_ids, int max_bucket_ids);
#endif

#ifdef __cplusplus
}
#endif

#endif
