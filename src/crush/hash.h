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
/* Sign Random Projection (Cosine LSH) - embedding vector to 32-bit hash */
extern __u32 crush_hash32_lsh(const float *vector, int dim);
/* Same as above but mask to num_bits (for pg_num alignment) */
extern __u32 crush_hash32_lsh_n(const float *vector, int dim, int num_bits);
/* Multi-table LSH: table_id selects different hyperplanes (Fan-out architecture) */
extern __u32 crush_hash32_lsh_multi(const float *vector, int dim, int table_id);
#endif

#endif
