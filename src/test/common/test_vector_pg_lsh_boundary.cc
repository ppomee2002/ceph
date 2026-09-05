// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "common/vector_pg_lsh_boundary.h"

#include <cmath>

#include <gtest/gtest.h>

using ceph::rados::vector_pg_lsh_boundary::bucket_range_covers_all;
using ceph::rados::vector_pg_lsh_boundary::bucket_range_t;
using ceph::rados::vector_pg_lsh_boundary::build_bucket_range_bound;
using ceph::rados::vector_pg_lsh_boundary::compute_boundary_query_state;
using ceph::rados::vector_pg_lsh_boundary::compute_residual_wildcard_mask;
using ceph::rados::vector_pg_lsh_boundary::conservative_distance_bucket_range;
using ceph::rados::vector_pg_lsh_boundary::distance_bound_from_tau;
using ceph::rados::vector_pg_lsh_boundary::distance_bound_t;
using ceph::rados::vector_pg_lsh_boundary::max_distance_bucket;
using ceph::rados::vector_pg_lsh_boundary::residual_matches;
using ceph::rados::vector_pg_lsh_boundary::ring_bucket_range;
using ceph::rados::vector_pg_lsh_boundary::split_anchor_sub_oid;
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
