// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#ifndef CEPH_COMMON_VECTOR_PG_LSH_BOUNDARY_H
#define CEPH_COMMON_VECTOR_PG_LSH_BOUNDARY_H

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "common/hobject.h"
#include "common/vector_pg_lsh_placement.h"

// Geometry for boundary-aware vector search over one PG's pg-lsh-v0
// sub_oid family; see librados/vector_placement.h for how that family is
// placed and named.
//
// This header only answers which sub_oid coordinates could still hold a
// closer candidate. Tree walking lives in the caller
// (crimson/osd/pg_backend.cc), and nothing here touches VectorNode or OMAP
// layout.
//
// residual_matches() is a post-filter on ONodes the distance_bucket range
// scan already returned, applied before their VectorNodes are read. It is
// not a pruning rule pushed into the tree lookup.
//
// Precondition: pg-lsh-v0 placement buckets vectors by the angular
// distance between the unit normalizations of the vector and the anchor,
// never by raw magnitude. This module normalizes the same way rather than
// assuming the caller did. The resulting Dq is only comparable to the raw
// Euclidean tau from the local top-k accumulator
// (common/vector_query_exec.h) when the vectors are themselves
// unit-L2-normalized, which is what pg-lsh-v0 buckets already assume.
// Callers that cannot guarantee that must not enable boundary-aware
// search.
namespace ceph::rados::vector_pg_lsh_boundary {

using sub_oid_t = ceph::rados::vector_pg_lsh_placement::sub_oid_t;

// Per-query geometry, computed once per query_vectors() call and reused
// across every expansion round (only the tau threshold changes round to
// round, not the query itself).
struct boundary_query_state_t {
  // ||q_hat - a_hat||^2 for the L2-unit-normalized query/anchor. Feeds
  // conservative_distance_bucket_range() via distance_bound_from_tau().
  double Dq = 0;
  // w_bit . residual(q_hat) for each residual bit, in the same
  // (residual_seed, bit) order compute_sub_oid() uses. Empty when
  // residual_bits == 0.
  std::vector<double> query_bit_projection;
};

// Mirrors compute_sub_oid()'s normalization and projection math (see
// librados/vector_placement.h) so Dq and query_bit_projection[] land in
// the space placement used. Returns nullopt on the same degenerate inputs
// compute_sub_oid() rejects: dimension mismatch, non-finite or zero-norm
// anchor.
inline std::optional<boundary_query_state_t> compute_boundary_query_state(
    const std::vector<float>& query_values,
    const std::vector<double>& anchor,
    uint32_t seed,
    uint32_t residual_bits)
{
  if (query_values.empty() || query_values.size() != anchor.size()) {
    return std::nullopt;
  }
  const uint32_t dimension = static_cast<uint32_t>(query_values.size());

  double query_norm_sq = 0;
  for (const float v : query_values) {
    if (!std::isfinite(v)) {
      return std::nullopt;
    }
    query_norm_sq += static_cast<double>(v) * v;
  }
  double anchor_norm_sq = 0;
  for (const double v : anchor) {
    if (!std::isfinite(v)) {
      return std::nullopt;
    }
    anchor_norm_sq += v * v;
  }
  if (anchor_norm_sq <= 0) {
    return std::nullopt;
  }
  const double anchor_inv_norm = 1.0 / std::sqrt(anchor_norm_sq);

  // normalized[]/a_hat match compute_sub_oid()'s `normalized` and
  // `normalized_anchor_component`, including the zero-vector fallback.
  std::vector<double> q_hat(dimension, 0.0);
  if (query_norm_sq > 0) {
    const double inv_norm = 1.0 / std::sqrt(query_norm_sq);
    for (uint32_t i = 0; i < dimension; ++i) {
      q_hat[i] = static_cast<double>(query_values[i]) * inv_norm;
    }
  }
  std::vector<double> a_hat(dimension, 0.0);
  for (uint32_t i = 0; i < dimension; ++i) {
    a_hat[i] = anchor[i] * anchor_inv_norm;
  }

  double Dq = 0;
  double dot_qa = 0;
  for (uint32_t i = 0; i < dimension; ++i) {
    const double diff = q_hat[i] - a_hat[i];
    Dq += diff * diff;
    dot_qa += q_hat[i] * a_hat[i];
  }

  boundary_query_state_t state;
  state.Dq = Dq;

  if (residual_bits != 0) {
    // Component of q_hat orthogonal to a_hat: compute_sub_oid()'s
    // `residual[dim]`.
    std::vector<double> residual_q(dimension, 0.0);
    for (uint32_t i = 0; i < dimension; ++i) {
      residual_q[i] = q_hat[i] - dot_qa * a_hat[i];
    }
    const double inv_sqrt_dim =
      1.0 / std::sqrt(static_cast<double>(dimension));
    // Must match compute_sub_oid()'s residual_seed derivation.
    const uint32_t residual_seed = seed ^ 0xbb67ae85U;
    state.query_bit_projection.assign(residual_bits, 0.0);
    for (uint32_t bit = 0; bit < residual_bits; ++bit) {
      double projection = 0;
      for (uint32_t dim = 0; dim < dimension; ++dim) {
        projection += residual_q[dim] *
          static_cast<double>(
              ceph::rados::vector_pg_lsh_placement::pg_lsh_v0_hyperplane_sign(
                  residual_seed, 0, bit, dim)) *
          inv_sqrt_dim;
      }
      state.query_bit_projection[bit] = projection;
    }
  }
  return state;
}

struct distance_bound_t {
  double min = 0;
  double max = 0;
};

// Triangle-inequality bound on ||v_hat - a_hat||^2 for any candidate
// within Euclidean distance tau of the query: Dmin = max(0, sqrt(Dq)-tau)^2,
// Dmax = (sqrt(Dq)+tau)^2. tau is the current kth-best distance from the
// local top-k accumulator.
inline distance_bound_t distance_bound_from_tau(double Dq, double tau)
{
  const double radius = std::sqrt(std::max(0.0, Dq));
  const double lower = std::max(0.0, radius - tau);
  const double upper = radius + tau;
  return {lower * lower, upper * upper};
}

struct bucket_range_t {
  uint32_t lo = 0;
  uint32_t hi = 0;
};

// Inverts compute_sub_oid()'s quantization (`scaled_distance =
// (1-dot_product)/2`, `distance_bucket = round(scaled_distance*65535) >>
// (16-distance_bucket_bits)`) to the widest distance_bucket range that
// could contain a candidate whose ||v_hat-a_hat||^2 falls in `bound`. For
// unit vectors scaled_distance = ||v_hat-a_hat||^2 / 4 is an identity
// (||x-y||^2 = 2-2(x.y)), so this is the same formula read backwards.
// Rounds outward rather than to nearest so a bucket sitting exactly on the
// Dmin/Dmax boundary is not excluded.
inline bucket_range_t conservative_distance_bucket_range(
    const distance_bound_t& bound, uint32_t distance_bucket_bits)
{
  if (distance_bucket_bits == 0) {
    return {0, 0};
  }
  const double scaled_min = std::clamp(bound.min / 4.0, 0.0, 1.0);
  const double scaled_max = std::clamp(bound.max / 4.0, 0.0, 1.0);
  const uint32_t quantized_min = std::min<uint32_t>(
      65535U, static_cast<uint32_t>(std::floor(scaled_min * 65535.0)));
  const uint32_t quantized_max = std::min<uint32_t>(
      65535U, static_cast<uint32_t>(std::ceil(scaled_max * 65535.0)));
  const uint32_t shift = 16 - distance_bucket_bits;
  return {quantized_min >> shift, quantized_max >> shift};
}

inline uint32_t max_distance_bucket(uint32_t distance_bucket_bits)
{
  return distance_bucket_bits == 0
    ? 0 : ((uint32_t{1} << distance_bucket_bits) - 1);
}

// Expansion ring around the anchor's own bucket, used before the top-k
// accumulator has a kth candidate. Without tau there is no Dmin/Dmax, so
// the caller widens `ring` by one bucket per round instead of scanning
// everything.
inline bucket_range_t ring_bucket_range(
    uint32_t anchor_bucket, uint32_t ring, uint32_t distance_bucket_bits)
{
  if (distance_bucket_bits == 0) {
    return {0, 0};
  }
  const uint32_t max_bucket = max_distance_bucket(distance_bucket_bits);
  const uint32_t lo = ring > anchor_bucket ? 0 : anchor_bucket - ring;
  const uint32_t hi = std::min(max_bucket, anchor_bucket + ring);
  return {lo, hi};
}

inline bool bucket_range_covers_all(
    const bucket_range_t& range, uint32_t distance_bucket_bits)
{
  return range.lo == 0 && range.hi == max_distance_bucket(distance_bucket_bits);
}

struct residual_mask_t {
  // Bit i set => candidate residual_code bit i must equal
  // (required_value >> i) & 1 to remain admissible. Bit i clear =>
  // wildcard (either value admissible).
  uint32_t certain_bits = 0;
  uint32_t required_value = 0;
};

// A bit is certain (non-wildcard) when the query's own hyperplane
// projection magnitude exceeds tau. Each hyperplane weight vector is
// unit-norm and the anchor-orthogonal projection is non-expansive, so
// |projection(v) - projection(q)| <= ||v_hat - q_hat||; a candidate within
// tau of the query therefore cannot flip such a bit. Same zero-crossing
// argument as a per-dimension residual sign, applied to
// compute_sub_oid()'s hyperplane-projection encoding.
inline residual_mask_t compute_residual_wildcard_mask(
    const boundary_query_state_t& state, double tau)
{
  residual_mask_t mask;
  const uint32_t residual_bits =
    static_cast<uint32_t>(state.query_bit_projection.size());
  for (uint32_t bit = 0; bit < residual_bits; ++bit) {
    const double projection = state.query_bit_projection[bit];
    if (std::fabs(projection) > tau) {
      mask.certain_bits |= (uint32_t{1} << bit);
      if (projection >= 0) {
        mask.required_value |= (uint32_t{1} << bit);
      }
    }
  }
  return mask;
}

inline bool residual_matches(uint32_t candidate_residual_code,
                             const residual_mask_t& mask)
{
  return (candidate_residual_code & mask.certain_bits) == mask.required_value;
}

// Splits anchor.oid.name at the last '/' into (prefix including the
// slash, parsed sub_oid). nullopt when the anchor has no pg-lsh-v0 sub_oid
// suffix for these bit widths -- e.g. distance_bucket_bits == 0 &&
// residual_bits == 0, where make_pg_lsh_v0_oid() never appends one -- or
// when the trailing segment does not parse as one.
inline std::optional<std::pair<std::string, sub_oid_t>> split_anchor_sub_oid(
    const hobject_t& anchor,
    uint32_t distance_bucket_bits,
    uint32_t residual_bits)
{
  const std::string& name = anchor.oid.name;
  const size_t slash = name.find_last_of('/');
  if (slash == std::string::npos || slash + 1 >= name.size()) {
    return std::nullopt;
  }
  const std::string_view suffix(
      name.data() + slash + 1, name.size() - slash - 1);
  auto parsed = ceph::rados::vector_pg_lsh_placement::parse_sub_oid(
      suffix, distance_bucket_bits, residual_bits);
  if (!parsed) {
    return std::nullopt;
  }
  return std::make_pair(name.substr(0, slash + 1), *parsed);
}

struct sub_oid_key_range_t {
  ghobject_t start;
  ghobject_t end;
};

// [start, end) bound over every sub_oid sibling of `anchor` whose
// distance_bucket falls in [bucket_lo, bucket_hi], for any residual_code;
// residual filtering is a separate per-candidate step in
// residual_matches(). `anchor`/`shard` fix pool, hash, nspace and locator
// key to the anchor's own values. All pg-lsh-v0 siblings in one PG share a
// locator key and therefore a hash, so hobject_t ordering reduces to a
// string compare on oid.name and the siblings are contiguous. Returns
// nullopt whenever split_anchor_sub_oid() does.
inline std::optional<sub_oid_key_range_t> build_bucket_range_bound(
    const hobject_t& anchor,
    shard_id_t shard,
    uint32_t distance_bucket_bits,
    uint32_t residual_bits,
    uint32_t bucket_lo,
    uint32_t bucket_hi)
{
  auto split = split_anchor_sub_oid(anchor, distance_bucket_bits, residual_bits);
  if (!split) {
    return std::nullopt;
  }
  const std::string& prefix = split->first;
  const uint32_t max_bucket = max_distance_bucket(distance_bucket_bits);
  bucket_lo = std::min(bucket_lo, max_bucket);
  bucket_hi = std::min(bucket_hi, max_bucket);

  auto make_bound = [&](const std::string& suffix) {
    hobject_t bound = anchor;
    bound.oid.name = prefix + suffix;
    return bound;
  };

  const sub_oid_t lo_sub_oid{static_cast<uint16_t>(bucket_lo), 0};
  hobject_t start = make_bound(ceph::rados::vector_pg_lsh_placement::format_sub_oid(
      lo_sub_oid, distance_bucket_bits, residual_bits));

  hobject_t end;
  if (bucket_hi >= max_bucket) {
    // No valid sub_oid suffix starts past 'g', so this sentinel is an
    // exclusive upper bound that cannot reach past this object's prefix
    // into another bucket/index/pg family in the same collection.
    end = make_bound("h");
  } else {
    const sub_oid_t hi_next_sub_oid{static_cast<uint16_t>(bucket_hi + 1), 0};
    end = make_bound(ceph::rados::vector_pg_lsh_placement::format_sub_oid(
        hi_next_sub_oid, distance_bucket_bits, residual_bits));
  }

  return sub_oid_key_range_t{
    ghobject_t{start, 0, shard},
    ghobject_t{end, 0, shard},
  };
}

} // namespace ceph::rados::vector_pg_lsh_boundary

#endif
