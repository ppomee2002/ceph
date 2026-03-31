// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
#pragma once

#include <cmath>
#include <cstdint>
#include <string>

/** Squared L2; sqrt omitted for ordering-only use. */
inline float l2_dist_sq(const float* a, const float* b, int dim) {
  float sum = 0;
  for (int i = 0; i < dim; i++) {
    float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}

inline float l2_dist(const float* a, const float* b, int dim) {
  return sqrtf(l2_dist_sq(a, b, dim));
}

/** Leading-one bit width; mirrors Ceph stable_mod / raw_hash_to_pg helpers. */
inline unsigned cbits32(uint32_t v) {
  if (v == 0) return 0;
  return 32 - __builtin_clz(static_cast<unsigned>(v));
}

/** LSH bit width aligned to pg_num (e.g. 9 bits for 512 PGs). */
inline unsigned valid_lsh_bits(uint32_t pg_num) {
  return (pg_num <= 1) ? 1u : cbits32(pg_num - 1);
}

inline uint32_t hash_to_pg(uint32_t h, uint32_t pg_num) {
  if (pg_num <= 0) return 0;
  uint32_t pg_num_mask = (1u << cbits32(pg_num - 1)) - 1;
  int x = static_cast<int>(h);
  int b = static_cast<int>(pg_num);
  int bmask = static_cast<int>(pg_num_mask);
  if (static_cast<unsigned>(x & bmask) < static_cast<unsigned>(b))
    return static_cast<uint32_t>(x & bmask);
  return static_cast<uint32_t>(x & (bmask >> 1));
}

/** 32-bit finalizer mix so PG mapping can use all hash bits. */
inline uint32_t mix_hash32(uint32_t x) {
  x ^= x >> 16;
  x *= 0x7feb352du;
  x ^= x >> 15;
  x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

inline uint32_t hash_to_pg_mixed(uint32_t h, uint32_t pg_num) {
  return hash_to_pg(mix_hash32(h), pg_num);
}

inline uint32_t map_hash_to_pg(uint32_t h, uint32_t pg_num, const std::string& mode) {
  if (mode == "mixed")
    return hash_to_pg_mixed(h, pg_num);
  return hash_to_pg(h, pg_num);
}
