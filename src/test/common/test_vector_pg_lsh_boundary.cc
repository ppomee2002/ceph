// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "common/vector_pg_lsh_boundary.h"
#include "librados/vector_placement.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

#include <gtest/gtest.h>

using ceph::rados::vector_pg_lsh_boundary::boundary_query_state_t;
using ceph::rados::vector_pg_lsh_boundary::bucket_range_covers_all;
using ceph::rados::vector_pg_lsh_boundary::bucket_range_overlaps_bound_raw_euclidean;
using ceph::rados::vector_pg_lsh_boundary::bucket_range_t;
using ceph::rados::vector_pg_lsh_boundary::build_bucket_range_bound;
using ceph::rados::vector_pg_lsh_boundary::compute_boundary_query_state;
using ceph::rados::vector_pg_lsh_boundary::compute_boundary_query_state_raw_euclidean;
using ceph::rados::vector_pg_lsh_boundary::compute_residual_wildcard_mask;
using ceph::rados::vector_pg_lsh_boundary::conservative_bucket_range_for_child;
using ceph::rados::vector_pg_lsh_boundary::conservative_distance_bucket_range;
using ceph::rados::vector_pg_lsh_boundary::conservative_distance_bucket_range_raw_euclidean;
using ceph::rados::vector_pg_lsh_boundary::distance_bound_from_tau;
using ceph::rados::vector_pg_lsh_boundary::distance_bound_t;
using ceph::rados::vector_pg_lsh_boundary::distance_interval_from_bucket_range_raw_euclidean;
using ceph::rados::vector_pg_lsh_boundary::max_distance_bucket;
using ceph::rados::vector_pg_lsh_boundary::min_possible_sq_distance_raw_euclidean;
using ceph::rados::vector_pg_lsh_boundary::residual_matches;
using ceph::rados::vector_pg_lsh_boundary::ring_bucket_range;
using ceph::rados::vector_pg_lsh_boundary::split_anchor_sub_oid;
using ceph::rados::vector_pg_lsh_placement::distance_geometry_normalized_angular_v0;
using ceph::rados::vector_pg_lsh_placement::distance_geometry_raw_euclidean_v1;
using ceph::rados::vector_pg_lsh_placement::format_sub_oid;
using ceph::rados::vector_pg_lsh_placement::parse_sub_oid;
using ceph::rados::vector_pg_lsh_placement::sub_oid_t;

namespace {

// Mirrors compute_sub_oid()'s own quantization exactly (see
// librados/vector_placement.h), so tests can compute "the bucket
// compute_sub_oid() would have produced for this scaled_distance" without
// depending on librados from this common/ test binary.
uint32_t reference_quantize_bucket(double scaled_distance,
                                   uint32_t distance_bucket_bits)
{
  if (distance_bucket_bits == 0) {
    return 0;
  }
  const auto quantized = static_cast<uint32_t>(
      std::floor(scaled_distance * 65535.0 + 0.5));
  return quantized >> (16 - distance_bucket_bits);
}

} // namespace

TEST(VectorPgLshBoundary, QueryStateOrthogonalUnitVectors)
{
  const std::vector<float> query = {1, 0, 0, 0};
  const std::vector<double> anchor = {0, 1, 0, 0};
  auto state = compute_boundary_query_state(query, anchor, 12345, 0);
  ASSERT_TRUE(state.has_value());
  // ||q_hat - a_hat||^2 for orthogonal unit vectors is 2.
  EXPECT_NEAR(2.0, state->Dq, 1e-9);
  EXPECT_TRUE(state->query_bit_projection.empty());
}

TEST(VectorPgLshBoundary, QueryStateIdenticalDirectionHasZeroDq)
{
  const std::vector<float> query = {3, 0, 0, 0};
  const std::vector<double> anchor = {1, 0, 0, 0};
  auto state = compute_boundary_query_state(query, anchor, 1, 0);
  ASSERT_TRUE(state.has_value());
  EXPECT_NEAR(0.0, state->Dq, 1e-9);
}

TEST(VectorPgLshBoundary, QueryStateOppositeDirectionHasMaxDq)
{
  const std::vector<float> query = {1, 0};
  const std::vector<double> anchor = {-1, 0};
  auto state = compute_boundary_query_state(query, anchor, 1, 0);
  ASSERT_TRUE(state.has_value());
  // ||q_hat - a_hat||^2 == 4 for antipodal unit vectors.
  EXPECT_NEAR(4.0, state->Dq, 1e-9);
}

TEST(VectorPgLshBoundary, QueryStateRejectsDimensionMismatch)
{
  const std::vector<float> query = {1, 0, 0};
  const std::vector<double> anchor = {1, 0};
  EXPECT_FALSE(compute_boundary_query_state(query, anchor, 1, 0).has_value());
}

TEST(VectorPgLshBoundary, QueryStateRejectsZeroNormAnchor)
{
  const std::vector<float> query = {1, 0};
  const std::vector<double> anchor = {0, 0};
  EXPECT_FALSE(compute_boundary_query_state(query, anchor, 1, 0).has_value());
}

TEST(VectorPgLshBoundary, QueryStateResidualProjectionCount)
{
  const std::vector<float> query = {1, 2, 3, 4, 5, 6, 7, 8};
  const std::vector<double> anchor = {1, 0, 0, 0, 0, 0, 0, 0};
  auto state = compute_boundary_query_state(query, anchor, 999, 6);
  ASSERT_TRUE(state.has_value());
  EXPECT_EQ(6u, state->query_bit_projection.size());
  for (const double projection : state->query_bit_projection) {
    EXPECT_TRUE(std::isfinite(projection));
  }
}

TEST(VectorPgLshBoundary, DistanceBoundClampsLowerAtZero)
{
  // radius = sqrt(1) = 1, tau = 5 -> radius - tau < 0, must clamp to 0.
  const auto bound = distance_bound_from_tau(1.0, 5.0);
  EXPECT_NEAR(0.0, bound.min, 1e-12);
  EXPECT_NEAR(36.0, bound.max, 1e-9); // (1+5)^2
}

TEST(VectorPgLshBoundary, DistanceBoundBasicTriangleInequality)
{
  // radius = sqrt(4) = 2, tau = 1 -> [1, 9] == [(2-1)^2, (2+1)^2].
  const auto bound = distance_bound_from_tau(4.0, 1.0);
  EXPECT_NEAR(1.0, bound.min, 1e-9);
  EXPECT_NEAR(9.0, bound.max, 1e-9);
}

TEST(VectorPgLshBoundary, DistanceBoundZeroTauIsExactPoint)
{
  const auto bound = distance_bound_from_tau(4.0, 0.0);
  EXPECT_NEAR(4.0, bound.min, 1e-9);
  EXPECT_NEAR(4.0, bound.max, 1e-9);
}

TEST(VectorPgLshBoundary, BucketRangeZeroBitsIsAlwaysBucketZero)
{
  const distance_bound_t bound{0.5, 3.9};
  const auto range = conservative_distance_bucket_range(bound, 0);
  EXPECT_EQ(0u, range.lo);
  EXPECT_EQ(0u, range.hi);
}

TEST(VectorPgLshBoundary, BucketRangeCoversFullSpaceUnderMaxUncertainty)
{
  const distance_bound_t bound{0.0, 4.0}; // scaled_distance in [0, 1]
  const auto range = conservative_distance_bucket_range(bound, 4);
  EXPECT_EQ(0u, range.lo);
  EXPECT_EQ(max_distance_bucket(4), range.hi);
}

TEST(VectorPgLshBoundary, BucketRangeBucketZeroBoundary)
{
  // scaled_distance exactly 0 -> Dv = 0 -> compute_sub_oid() always places
  // this at bucket 0, regardless of distance_bucket_bits.
  const distance_bound_t bound{0.0, 0.0};
  for (const uint32_t bits : {1u, 4u, 8u, 16u}) {
    const auto range = conservative_distance_bucket_range(bound, bits);
    EXPECT_EQ(0u, range.lo) << "bits=" << bits;
    EXPECT_EQ(0u, range.hi) << "bits=" << bits;
    EXPECT_EQ(reference_quantize_bucket(0.0, bits), range.lo);
  }
}

TEST(VectorPgLshBoundary, BucketRangeMaximumBucketBoundary)
{
  // scaled_distance exactly 1 (Dv == 4, the largest possible unit-vector
  // squared distance) -> compute_sub_oid() always places this at the
  // highest bucket.
  const distance_bound_t bound{4.0, 4.0};
  for (const uint32_t bits : {1u, 4u, 8u, 16u}) {
    const auto range = conservative_distance_bucket_range(bound, bits);
    EXPECT_EQ(max_distance_bucket(bits), range.lo) << "bits=" << bits;
    EXPECT_EQ(max_distance_bucket(bits), range.hi) << "bits=" << bits;
    EXPECT_EQ(reference_quantize_bucket(1.0, bits), range.hi);
  }
}

TEST(VectorPgLshBoundary, BucketRangeNeverExcludesTrueBoundaryBucket)
{
  // distance_bucket_bits = 4 -> bucket boundaries at scaled_distance =
  // k/16 for k in [0, 16]; Dv = 4 * scaled_distance. Check every boundary
  // that a real (nonzero-width) [Dmin, Dmax] band straddling it must keep
  // both adjacent buckets in range, and that a zero-width band sitting
  // exactly on the boundary still contains whichever bucket
  // reference_quantize_bucket() (== compute_sub_oid()'s own formula)
  // actually produces.
  constexpr uint32_t bits = 4;
  for (uint32_t k = 1; k < 16; ++k) {
    const double boundary_scaled = static_cast<double>(k) / 16.0;
    const double boundary_Dv = 4.0 * boundary_scaled;
    const double epsilon = 1e-6;

    const distance_bound_t straddling{
      boundary_Dv - epsilon, boundary_Dv + epsilon};
    const auto range = conservative_distance_bucket_range(straddling, bits);
    const uint32_t below_bucket =
      reference_quantize_bucket(boundary_scaled - epsilon / 4.0, bits);
    const uint32_t above_bucket =
      reference_quantize_bucket(boundary_scaled + epsilon / 4.0, bits);
    EXPECT_LE(range.lo, below_bucket) << "k=" << k;
    EXPECT_GE(range.hi, above_bucket) << "k=" << k;

    const distance_bound_t exact{boundary_Dv, boundary_Dv};
    const auto exact_range = conservative_distance_bucket_range(exact, bits);
    const uint32_t exact_bucket =
      reference_quantize_bucket(boundary_scaled, bits);
    EXPECT_LE(exact_range.lo, exact_bucket) << "k=" << k;
    EXPECT_GE(exact_range.hi, exact_bucket) << "k=" << k;
  }
}

TEST(VectorPgLshBoundary, RingBucketRangeClampsAtEdges)
{
  const auto low = ring_bucket_range(0, 3, 4); // anchor_bucket=0, ring=3
  EXPECT_EQ(0u, low.lo);
  EXPECT_EQ(3u, low.hi);

  const auto high = ring_bucket_range(15, 3, 4); // max bucket for 4 bits
  EXPECT_EQ(12u, high.lo);
  EXPECT_EQ(15u, high.hi);
}

TEST(VectorPgLshBoundary, RingBucketRangeCoversAllDetection)
{
  EXPECT_FALSE(bucket_range_covers_all(bucket_range_t{1, 14}, 4));
  EXPECT_TRUE(bucket_range_covers_all(bucket_range_t{0, 15}, 4));
  EXPECT_TRUE(bucket_range_covers_all(ring_bucket_range(0, 15, 4), 4));
}

TEST(VectorPgLshBoundary, ResidualWildcardMaskThreshold)
{
  ceph::rados::vector_pg_lsh_boundary::boundary_query_state_t state;
  state.query_bit_projection = {5.0, -3.0, 0.5, -0.5};
  const auto mask = compute_residual_wildcard_mask(state, 1.0);
  // bits 0,1: |projection| > tau=1 -> certain. bits 2,3: wildcard.
  EXPECT_EQ(0b0011u, mask.certain_bits);
  EXPECT_EQ(0b0001u, mask.required_value); // bit0 positive, bit1 negative

  // Certain bits must match exactly; wildcard bits (2,3) accept either
  // value.
  EXPECT_TRUE(residual_matches(0b0001, mask));
  EXPECT_TRUE(residual_matches(0b1101, mask));
  EXPECT_FALSE(residual_matches(0b0000, mask)); // bit0 wrong
  EXPECT_FALSE(residual_matches(0b0011, mask)); // bit1 wrong
}

TEST(VectorPgLshBoundary, ResidualWildcardMaskAllWildcardAtInfiniteTau)
{
  ceph::rados::vector_pg_lsh_boundary::boundary_query_state_t state;
  state.query_bit_projection = {5.0, -3.0, 0.5};
  const auto mask = compute_residual_wildcard_mask(
      state, std::numeric_limits<double>::infinity());
  EXPECT_EQ(0u, mask.certain_bits);
  EXPECT_TRUE(residual_matches(0, mask));
  EXPECT_TRUE(residual_matches(0b111, mask));
}

TEST(VectorPgLshBoundary, SplitAnchorSubOidRoundTripsWithFormat)
{
  const sub_oid_t sub_oid{0x4, 0x5};
  hobject_t anchor;
  anchor.oid = object_t(
      ".rados.vector/v1/pg-lsh-v0/aa/bb/pg_3/" +
      format_sub_oid(sub_oid, 4, 4));

  auto split = split_anchor_sub_oid(anchor, 4, 4);
  ASSERT_TRUE(split.has_value());
  EXPECT_EQ(".rados.vector/v1/pg-lsh-v0/aa/bb/pg_3/", split->first);
  EXPECT_EQ(sub_oid.distance_bucket, split->second.distance_bucket);
  EXPECT_EQ(sub_oid.residual_code, split->second.residual_code);
}

TEST(VectorPgLshBoundary, SplitAnchorSubOidRejectsDisabledSubOid)
{
  hobject_t anchor;
  anchor.oid = object_t(".rados.vector/v1/pg-lsh-v0/aa/bb/pg_3");
  EXPECT_FALSE(split_anchor_sub_oid(anchor, 0, 0).has_value());
}

TEST(VectorPgLshBoundary, SplitAnchorSubOidRejectsMalformedSuffix)
{
  hobject_t anchor;
  anchor.oid = object_t(".rados.vector/v1/pg-lsh-v0/aa/bb/pg_3/not-a-sub-oid");
  EXPECT_FALSE(split_anchor_sub_oid(anchor, 4, 4).has_value());
}

TEST(VectorPgLshBoundary, BuildBucketRangeBoundOrdering)
{
  const sub_oid_t sub_oid{0x4, 0x5};
  hobject_t anchor;
  anchor.oid = object_t(
      ".rados.vector/v1/pg-lsh-v0/aa/bb/pg_3/" +
      format_sub_oid(sub_oid, 4, 4));
  anchor.pool = 1;

  auto bound = build_bucket_range_bound(
      anchor, shard_id_t::NO_SHARD, 4, 4, /*lo=*/2, /*hi=*/5);
  ASSERT_TRUE(bound.has_value());
  EXPECT_LT(bound->start, bound->end);
  EXPECT_EQ(bound->start.hobj.pool, anchor.pool);
}

TEST(VectorPgLshBoundary, BuildBucketRangeBoundOverflowSentinelStaysOrdered)
{
  const sub_oid_t sub_oid{0xf, 0xf};
  hobject_t anchor;
  anchor.oid = object_t(
      ".rados.vector/v1/pg-lsh-v0/aa/bb/pg_3/" +
      format_sub_oid(sub_oid, 4, 4));

  // bucket_hi == max_bucket triggers the "h" sentinel overflow path.
  auto bound = build_bucket_range_bound(
      anchor, shard_id_t::NO_SHARD, 4, 4, /*lo=*/15, /*hi=*/15);
  ASSERT_TRUE(bound.has_value());
  EXPECT_LT(bound->start, bound->end);
  // The anchor's own object must itself fall inside [start, end).
  ghobject_t anchor_ghobj{anchor, 0, shard_id_t::NO_SHARD};
  EXPECT_GE(anchor_ghobj, bound->start);
  EXPECT_LT(anchor_ghobj, bound->end);
}

// ---------------------------------------------------------------------
// Raw-Euclidean geometry (distance_geometry_raw_euclidean_v1): the math
// tau-based subtree exclusion relies on. See
// bucket_range_overlaps_bound_raw_euclidean() in vector_pg_lsh_boundary.h
// and make_priority() in seastore.cc, the only caller of that path.
// ---------------------------------------------------------------------

TEST(VectorPgLshBoundaryRawEuclidean, QueryStateIsRawNotNormalized)
{
  // Same direction, different magnitude: normalized_angular reports
  // Dq == 0 here (see QueryStateIdenticalDirectionHasZeroDq above).
  // Raw-Euclidean must keep the magnitude: Dq == ||q-anchor||^2.
  const std::vector<float> query = {3, 0, 0, 0};
  const std::vector<double> anchor = {1, 0, 0, 0};
  auto state = compute_boundary_query_state_raw_euclidean(query, anchor, 1, 0);
  ASSERT_TRUE(state.has_value());
  EXPECT_NEAR(4.0, state->Dq, 1e-9); // (3-1)^2
}

TEST(VectorPgLshBoundaryRawEuclidean, QueryStateZeroAnchorIsAllowed)
{
  // normalized_angular rejects a zero-norm anchor because it divides by
  // ||anchor|| to unit-normalize (QueryStateRejectsZeroNormAnchor). Raw
  // Euclidean has no such division, so the origin is a valid anchor.
  const std::vector<float> query = {3, 4};
  const std::vector<double> anchor = {0, 0};
  auto state = compute_boundary_query_state_raw_euclidean(query, anchor, 1, 0);
  ASSERT_TRUE(state.has_value());
  EXPECT_NEAR(25.0, state->Dq, 1e-9); // 3^2+4^2
}

TEST(VectorPgLshBoundaryRawEuclidean, QueryStateRejectsDimensionMismatch)
{
  const std::vector<float> query = {1, 0, 0};
  const std::vector<double> anchor = {1, 0};
  EXPECT_FALSE(
      compute_boundary_query_state_raw_euclidean(query, anchor, 1, 0)
        .has_value());
}

TEST(VectorPgLshBoundaryRawEuclidean, IntervalTopBucketIsUnbounded)
{
  // The bucket compute_sub_oid_raw_euclidean() clamps any Dv past
  // raw_distance_scale_max into. Its pruning interval must be [x, +inf),
  // not capped at scale_max, or a candidate whose raw distance sits past
  // the configured scale would be excluded.
  constexpr uint32_t bits = 4;
  constexpr double scale = 100.0;
  const auto top = distance_interval_from_bucket_range_raw_euclidean(
      bucket_range_t{max_distance_bucket(bits), max_distance_bucket(bits)},
      bits, scale);
  EXPECT_TRUE(std::isinf(top.max));
  EXPECT_GT(top.min, 0.0);

  // Non-top buckets stay finite.
  const auto mid = distance_interval_from_bucket_range_raw_euclidean(
      bucket_range_t{0, 0}, bits, scale);
  EXPECT_FALSE(std::isinf(mid.max));
}

TEST(VectorPgLshBoundaryRawEuclidean, IntervalBottomBucketStartsAtZero)
{
  constexpr uint32_t bits = 4;
  const auto bottom = distance_interval_from_bucket_range_raw_euclidean(
      bucket_range_t{0, 0}, bits, 100.0);
  EXPECT_NEAR(0.0, bottom.min, 1e-9);
}

TEST(VectorPgLshBoundaryRawEuclidean, ConservativeRangeRoundTripsAtBoundaries)
{
  // Mirrors BucketRangeNeverExcludesTrueBoundaryBucket, but for the
  // raw-Euclidean scale/inverse-quantization math instead of the fixed
  // [0,4] normalized-angular range.
  constexpr uint32_t bits = 4;
  constexpr double scale = 64.0;
  for (uint32_t k = 1; k < 16; ++k) {
    const double boundary_scaled = static_cast<double>(k) / 16.0;
    const double boundary_Dv = scale * boundary_scaled;
    const double epsilon = 1e-6;

    const distance_bound_t straddling{
      boundary_Dv - epsilon, boundary_Dv + epsilon};
    const auto range = conservative_distance_bucket_range_raw_euclidean(
        straddling, bits, scale);
    // The range must cover both sides of the boundary.
    const uint32_t below_bucket = static_cast<uint32_t>(
        std::floor((boundary_scaled - epsilon / scale) * 65535.0 + 0.5)) >>
        (16 - bits);
    const uint32_t above_bucket = static_cast<uint32_t>(
        std::floor((boundary_scaled + epsilon / scale) * 65535.0 + 0.5)) >>
        (16 - bits);
    EXPECT_LE(range.lo, below_bucket) << "k=" << k;
    EXPECT_GE(range.hi, above_bucket) << "k=" << k;
  }
}

TEST(VectorPgLshBoundaryRawEuclidean, OverlapFalseWhenIntervalStrictlyOutsideBound)
{
  constexpr uint32_t bits = 8;
  constexpr double scale = 100.0;
  // Bucket 0 spans raw distance [0, ~0.39] at 8 bits / scale 100. A bound
  // of [50, 60] cannot possibly overlap it.
  const distance_bound_t bound{50.0, 60.0};
  EXPECT_FALSE(bucket_range_overlaps_bound_raw_euclidean(
      bucket_range_t{0, 0}, bits, scale, bound));
}

TEST(VectorPgLshBoundaryRawEuclidean, OverlapTrueWhenBoundTouchesIntervalExactly)
{
  // A bound landing exactly on an interval edge must not be excluded,
  // matching conservative_distance_bucket_range() in the normalized
  // case.
  constexpr uint32_t bits = 4;
  constexpr double scale = 16.0;
  const auto interval = distance_interval_from_bucket_range_raw_euclidean(
      bucket_range_t{2, 2}, bits, scale);
  ASSERT_FALSE(std::isinf(interval.max));
  const distance_bound_t touching_from_above{interval.max, interval.max + 5.0};
  EXPECT_TRUE(bucket_range_overlaps_bound_raw_euclidean(
      bucket_range_t{2, 2}, bits, scale, touching_from_above));
  const distance_bound_t touching_from_below{interval.min - 5.0, interval.min};
  EXPECT_TRUE(bucket_range_overlaps_bound_raw_euclidean(
      bucket_range_t{2, 2}, bits, scale, touching_from_below));
}

TEST(VectorPgLshBoundaryRawEuclidean, OverlapAlwaysTrueForUnboundedTopBucket)
{
  // No finite bound can ever exclude the top bucket on its upper side,
  // since that bucket's own interval is unbounded above.
  constexpr uint32_t bits = 4;
  constexpr double scale = 10.0;
  const distance_bound_t far_bound{
    1000.0, std::numeric_limits<double>::infinity()};
  EXPECT_TRUE(bucket_range_overlaps_bound_raw_euclidean(
      bucket_range_t{max_distance_bucket(bits), max_distance_bucket(bits)},
      bits, scale, far_bound));
}

TEST(VectorPgLshBoundaryRawEuclidean, MinPossibleDistanceZeroWhenDqInsideInterval)
{
  constexpr uint32_t bits = 4;
  constexpr double scale = 16.0;
  const auto interval = distance_interval_from_bucket_range_raw_euclidean(
      bucket_range_t{4, 4}, bits, scale);
  ASSERT_FALSE(std::isinf(interval.max));
  const double mid_Dq = (interval.min + interval.max) / 2.0;
  EXPECT_NEAR(
      0.0,
      min_possible_sq_distance_raw_euclidean(
          mid_Dq, bucket_range_t{4, 4}, bits, scale),
      1e-9);
}

// No-false-negative check over a random point cloud around an anchor.
// Every point within tau of the query must (a) never be excluded by
// bucket_range_overlaps_bound_raw_euclidean() on its own exact
// distance_bucket, and (b) always satisfy residual_matches() against the
// tau-derived wildcard mask. Together that covers the put and query paths
// agreeing on the bucket, and the Cauchy-Schwarz argument in
// compute_residual_wildcard_mask().
TEST(VectorPgLshBoundaryRawEuclidean, NoFalseNegativeExclusionSynthetic)
{
  constexpr uint32_t dimension = 8;
  constexpr uint32_t bits = 6;
  constexpr uint32_t residual_bits = 4;
  constexpr double scale = 400.0;
  constexpr uint32_t seed = 424242;
  constexpr int point_count = 2000;

  std::vector<double> anchor(dimension);
  std::mt19937 rng(12345);
  std::uniform_real_distribution<double> coord_dist(-10.0, 10.0);
  for (auto &c : anchor) c = coord_dist(rng);

  std::vector<float> query(dimension);
  for (auto &q : query) q = static_cast<float>(coord_dist(rng));

  auto qstate = compute_boundary_query_state_raw_euclidean(
      query, anchor, seed, residual_bits);
  ASSERT_TRUE(qstate.has_value());

  librados::vector_placement::pg_lsh_v0::sub_oid_config_t config;
  config.dimension = dimension;
  config.seed = seed;
  config.distance_bucket_bits = bits;
  config.residual_bits = residual_bits;
  config.anchor = std::span<const double>(anchor);
  config.distance_geometry = distance_geometry_raw_euclidean_v1;
  config.raw_distance_scale_max = scale;

  std::vector<std::vector<float>> points(point_count, std::vector<float>(dimension));
  std::vector<double> exact_sq_dist(point_count);
  for (int i = 0; i < point_count; ++i) {
    for (auto &c : points[i]) c = static_cast<float>(coord_dist(rng));
    double d = 0;
    for (uint32_t dim = 0; dim < dimension; ++dim) {
      const double diff = points[i][dim] - query[dim];
      d += diff * diff;
    }
    exact_sq_dist[i] = d;
  }

  std::vector<double> sorted_dist = exact_sq_dist;
  std::sort(sorted_dist.begin(), sorted_dist.end());
  constexpr int k = 10;
  const double tau = std::sqrt(sorted_dist[k - 1]);

  const auto bound = distance_bound_from_tau(qstate->Dq, tau);
  const auto mask = compute_residual_wildcard_mask(*qstate, tau);

  int checked_within_tau = 0;
  for (int i = 0; i < point_count; ++i) {
    if (std::sqrt(exact_sq_dist[i]) > tau) {
      continue;
    }
    ++checked_within_tau;
    ceph::bufferlist bl;
    bl.append(reinterpret_cast<const char *>(points[i].data()),
               dimension * sizeof(float));
    librados::vector_placement::pg_lsh_v0::sub_oid_t sub_oid;
    ASSERT_EQ(0, librados::vector_placement::pg_lsh_v0::compute_sub_oid(
                     bl, config, &sub_oid));

    const bucket_range_t exact_range{sub_oid.distance_bucket, sub_oid.distance_bucket};
    EXPECT_TRUE(bucket_range_overlaps_bound_raw_euclidean(
        exact_range, bits, scale, bound))
        << "point " << i << " (true distance within tau) was excluded "
        << "by distance-bucket pruning -- false negative";

    EXPECT_TRUE(residual_matches(sub_oid.residual_code, mask))
        << "point " << i << " (true distance within tau) was excluded "
        << "by residual pruning -- false negative";
  }
  // Sanity: the synthetic setup actually exercised the within-tau path.
  EXPECT_GE(checked_within_tau, k);
}

// Boundary/rounding regression on compute_sub_oid_raw_euclidean()'s
// quantization, through the shared sub_oid_config_t entry point rather
// than a copy of its formula.
TEST(VectorPgLshBoundaryRawEuclidean, PlacementClampsAtScaleMaxToTopBucket)
{
  constexpr uint32_t dimension = 2;
  constexpr uint32_t bits = 4;
  std::vector<double> anchor = {0.0, 0.0};
  librados::vector_placement::pg_lsh_v0::sub_oid_config_t config;
  config.dimension = dimension;
  config.seed = 1;
  config.distance_bucket_bits = bits;
  config.residual_bits = 0;
  config.anchor = std::span<const double>(anchor);
  config.distance_geometry = distance_geometry_raw_euclidean_v1;
  config.raw_distance_scale_max = 10.0;

  // ||v-anchor||^2 == 10000, far past scale_max == 10: must clamp into the
  // top bucket rather than erroring or wrapping.
  std::vector<float> far_point = {100.0f, 0.0f};
  ceph::bufferlist bl;
  bl.append(reinterpret_cast<const char *>(far_point.data()),
             dimension * sizeof(float));
  librados::vector_placement::pg_lsh_v0::sub_oid_t sub_oid;
  ASSERT_EQ(0, librados::vector_placement::pg_lsh_v0::compute_sub_oid(
                   bl, config, &sub_oid));
  EXPECT_EQ(max_distance_bucket(bits), sub_oid.distance_bucket);
}

TEST(VectorPgLshBoundaryRawEuclidean, PlacementRejectsNonPositiveScale)
{
  constexpr uint32_t dimension = 2;
  std::vector<double> anchor = {0.0, 0.0};
  librados::vector_placement::pg_lsh_v0::sub_oid_config_t config;
  config.dimension = dimension;
  config.seed = 1;
  config.distance_bucket_bits = 4;
  config.residual_bits = 0;
  config.anchor = std::span<const double>(anchor);
  config.distance_geometry = distance_geometry_raw_euclidean_v1;
  config.raw_distance_scale_max = 0; // invalid

  std::vector<float> point = {1.0f, 1.0f};
  ceph::bufferlist bl;
  bl.append(reinterpret_cast<const char *>(point.data()),
             dimension * sizeof(float));
  librados::vector_placement::pg_lsh_v0::sub_oid_t sub_oid;
  EXPECT_EQ(-EINVAL, librados::vector_placement::pg_lsh_v0::compute_sub_oid(
                         bl, config, &sub_oid));
}

// The distance_geometry field must not change normalized_angular_v0
// placement: the default value is
// distance_geometry_normalized_angular_v0, and setting it explicitly must
// take the same path.
TEST(VectorPgLshBoundaryRawEuclidean, DefaultGeometryIsNormalizedAngularV0)
{
  librados::vector_placement::pg_lsh_v0::sub_oid_config_t config;
  EXPECT_EQ(distance_geometry_normalized_angular_v0, config.distance_geometry);
  EXPECT_EQ(0.0, config.raw_distance_scale_max);
}

// The frontier-shrinks-as-tau-improves behavior of
// query_vectors_tree_boundary(), at the math level: a subtree range
// admissible under a looser tau becomes inadmissible once tau narrows past
// it, and one admissible under the tightest tau stays admissible under any
// looser tau. expand_one() re-evaluates pending entries against a fresh tau
// on every call, so that monotonicity is what drops stale entries without
// extra bookkeeping.
TEST(VectorPgLshBoundaryRawEuclidean, NarrowerTauExcludesRangeLooserTauAdmitted)
{
  constexpr uint32_t bits = 6;
  constexpr double scale = 100.0;
  const double Dq = 25.0; // query sits at raw distance 5 from anchor

  // A bucket range whose raw-distance interval is centered around 40-45,
  // i.e. far from Dq=25.
  const bucket_range_t far_bucket{40, 40};
  const auto interval =
    distance_interval_from_bucket_range_raw_euclidean(far_bucket, bits, scale);
  ASSERT_FALSE(std::isinf(interval.max));
  // Pick a query-to-interval gap and two tau values that straddle it.
  const double gap = std::sqrt(interval.min) - std::sqrt(Dq);
  ASSERT_GT(gap, 0.0);
  const double loose_tau = gap + 1.0;   // still reaches the interval
  const double tight_tau = std::max(0.0, gap - 1.0); // falls short

  const auto loose_bound = distance_bound_from_tau(Dq, loose_tau);
  const auto tight_bound = distance_bound_from_tau(Dq, tight_tau);

  EXPECT_TRUE(bucket_range_overlaps_bound_raw_euclidean(
      far_bucket, bits, scale, loose_bound))
      << "looser tau must still admit the far bucket";
  EXPECT_FALSE(bucket_range_overlaps_bound_raw_euclidean(
      far_bucket, bits, scale, tight_bound))
      << "narrower tau must exclude the now-unreachable far bucket";
}
