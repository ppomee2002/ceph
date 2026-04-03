#ifndef CEPH_CRUSH_HASH_H
#define CEPH_CRUSH_HASH_H

#ifdef __KERNEL__
# include <linux/types.h>
#else
# include "crush_compat.h"
#endif

#define CRUSH_HASH_RJENKINS1   0

#define CRUSH_HASH_DEFAULT CRUSH_HASH_RJENKINS1

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
#endif

#endif
