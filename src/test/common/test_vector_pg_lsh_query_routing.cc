// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "librados/vector_placement.h"

#include <cmath>
#include <set>
#include <vector>

#include <gtest/gtest.h>

namespace vp = librados::vector_placement;

namespace {

constexpr uint32_t kDim = 16;
constexpr uint32_t kBits = 8;
constexpr uint32_t kTables = 4;
constexpr uint32_t kSeed = 1315423911;
constexpr uint32_t kPgNum = 1024;

ceph::bufferlist make_vector(int variant)
{
  std::vector<float> values(kDim);
  for (uint32_t i = 0; i < kDim; ++i) {
    values[i] = static_cast<float>(std::sin(0.7 * i + variant) * (i + 3));
  }
  ceph::bufferlist bl;
  bl.append(reinterpret_cast<const char*>(values.data()),
            values.size() * sizeof(float));
  return bl;
}

std::vector<float> to_floats(const ceph::bufferlist& bl)
{
  std::vector<float> values;
  EXPECT_EQ(0, vp::copy_float32_vector(bl, kDim, &values));
  return values;
}

} // namespace

TEST(VectorPgLshQueryRouting, DefaultsReproduceOriginalOrdering)
{
  const auto query = make_vector(1);
  std::vector<vp::pg_lsh_v0_group_t> original;
  ASSERT_EQ(0, vp::pg_lsh_v0_query_groups(
      query, kDim, kBits, kTables, 2, kSeed, &original));

  // Default routing must produce the same groups in the same order as
  // pg_lsh_v0_query_groups(), so existing callers are unaffected.
  std::vector<vp::pg_lsh_v0_group_t> routed;
  ASSERT_EQ(0, vp::pg_lsh_v0_query_groups_routed(
      query, kDim, kBits, kTables, 2, kSeed,
      vp::pg_lsh_v0_query_routing_t{}, &routed));

  ASSERT_EQ(original.size(), routed.size());
  for (size_t i = 0; i < original.size(); ++i) {
    EXPECT_EQ(original[i].table, routed[i].table) << "group " << i;
    EXPECT_EQ(original[i].lsh_bucket_id, routed[i].lsh_bucket_id) << "group " << i;
    EXPECT_EQ(original[i].hamming_distance, routed[i].hamming_distance)
        << "group " << i;
  }
}

TEST(VectorPgLshQueryRouting, TableCountRestrictsEnumeration)
{
  const auto query = make_vector(2);
  for (uint32_t tables = 1; tables <= kTables; ++tables) {
    vp::pg_lsh_v0_query_routing_t routing;
    routing.table_count = tables;
    std::vector<vp::pg_lsh_v0_group_t> routed;
    ASSERT_EQ(0, vp::pg_lsh_v0_query_groups_routed(
        query, kDim, kBits, kTables, 2, kSeed, routing, &routed));
    ASSERT_FALSE(routed.empty());
    for (const auto& group : routed) {
      EXPECT_LT(group.table, tables);
    }
  }

  // Restricting to more tables than the index has is a caller error, not a
  // silent clamp.
  vp::pg_lsh_v0_query_routing_t too_many;
  too_many.table_count = kTables + 1;
  std::vector<vp::pg_lsh_v0_group_t> routed;
  EXPECT_EQ(-EINVAL, vp::pg_lsh_v0_query_groups_routed(
      query, kDim, kBits, kTables, 2, kSeed, too_many, &routed));
}

TEST(VectorPgLshQueryRoutingWritePath, TableRestrictionStillReachesWritePg)
{
  // The point of --query-table-count: with write_pg_count = d, every vector
  // lands in a PG derived from tables [0, d). A query restricted to those
  // same tables must still be able to address that PG -- otherwise the
  // restriction would trade wasted probes for lost recall.
  for (int variant = 0; variant < 8; ++variant) {
    const auto vec = make_vector(variant);
    std::vector<vp::pg_lsh_v0_group_t> exact;
    ASSERT_EQ(0, vp::pg_lsh_v0_exact_groups(
        vec, kDim, kBits, kTables, kSeed, &exact));
    const auto write_pgs =
      vp::pg_lsh_v0_select_write_pgs(exact, kPgNum, kSeed, 1);
    ASSERT_EQ(1u, write_pgs.size());

    // d=1 always resolves to table 0: exact groups come out in table order
    // and the first unique PG wins.
    EXPECT_EQ(vp::pg_lsh_v0_group_to_pg(0, exact[0].lsh_bucket_id, kPgNum, kSeed),
              write_pgs[0]) << "variant " << variant;

    vp::pg_lsh_v0_query_routing_t routing;
    routing.table_count = 1;
    std::vector<vp::pg_lsh_v0_group_t> routed;
    ASSERT_EQ(0, vp::pg_lsh_v0_query_groups_routed(
        vec, kDim, kBits, kTables, 0, kSeed, routing, &routed));
    const auto ranked =
      vp::pg_lsh_v0_select_unique_pgs(routed, kPgNum, kSeed, 4);
    ASSERT_FALSE(ranked.empty());
    // Querying the vector itself, its own write PG must be rank 1.
    EXPECT_EQ(write_pgs[0], ranked.front().pg) << "variant " << variant;
  }
}

TEST(VectorPgLshQueryRouting, MarginOrderingIsAscendingInScore)
{
  const auto query = make_vector(3);
  const auto values = to_floats(query);

  vp::pg_lsh_v0_query_routing_t routing;
  routing.table_count = 1;
  routing.margin_ordered_probes = true;
  std::vector<vp::pg_lsh_v0_group_t> routed;
  ASSERT_EQ(0, vp::pg_lsh_v0_query_groups_routed(
      query, kDim, kBits, kTables, 3, kSeed, routing, &routed));
  ASSERT_GT(routed.size(), 8u);

  std::vector<double> projections;
  vp::pg_lsh_v0_bit_projections(values, 0, kBits, kSeed, &projections);
  const uint32_t exact =
    vp::pg_lsh_v0_lsh_bucket_id(values, 0, kBits, kSeed);

  // The exact bucket scores 0 and must lead.
  EXPECT_EQ(exact, routed.front().lsh_bucket_id);
  EXPECT_EQ(0u, routed.front().hamming_distance);

  double previous = -1.0;
  for (const auto& group : routed) {
    const double score =
      vp::pg_lsh_v0_mask_score(group.lsh_bucket_id ^ exact, projections);
    EXPECT_GE(score, previous - 1e-9)
        << "probe order must not decrease in score";
    previous = score;
  }

  // A cheap 2-bit flip can outrank an expensive 1-bit flip. That is the
  // point of the score ordering, so check it happens somewhere rather
  // than degenerating to hamming-distance order.
  bool saw_inversion = false;
  uint32_t seen_distance = 0;
  for (const auto& group : routed) {
    if (group.hamming_distance < seen_distance) {
      saw_inversion = true;
      break;
    }
    seen_distance = std::max(seen_distance, group.hamming_distance);
  }
  EXPECT_TRUE(saw_inversion)
      << "margin ordering never reordered across hamming levels";
}

TEST(VectorPgLshQueryRouting, MarginOrderingKeepsTheSameGroupSet)
{
  // Reordering must not add or drop probes -- only change their order.
  const auto query = make_vector(4);
  const auto key = [](const vp::pg_lsh_v0_group_t& g) {
    return std::pair<uint32_t, uint32_t>(g.table, g.lsh_bucket_id);
  };

  vp::pg_lsh_v0_query_routing_t plain;
  plain.table_count = 2;
  vp::pg_lsh_v0_query_routing_t margin = plain;
  margin.margin_ordered_probes = true;

  std::vector<vp::pg_lsh_v0_group_t> a, b;
  ASSERT_EQ(0, vp::pg_lsh_v0_query_groups_routed(
      query, kDim, kBits, kTables, 3, kSeed, plain, &a));
  ASSERT_EQ(0, vp::pg_lsh_v0_query_groups_routed(
      query, kDim, kBits, kTables, 3, kSeed, margin, &b));
  ASSERT_EQ(a.size(), b.size());

  std::set<std::pair<uint32_t, uint32_t>> set_a, set_b;
  for (const auto& g : a) set_a.insert(key(g));
  for (const auto& g : b) set_b.insert(key(g));
  EXPECT_EQ(set_a, set_b);
}

TEST(VectorPgLshQueryRouting, MarginOrderingIsDeterministic)
{
  const auto query = make_vector(5);
  vp::pg_lsh_v0_query_routing_t routing;
  routing.margin_ordered_probes = true;

  std::vector<vp::pg_lsh_v0_group_t> first, second;
  ASSERT_EQ(0, vp::pg_lsh_v0_query_groups_routed(
      query, kDim, kBits, kTables, 3, kSeed, routing, &first));
  ASSERT_EQ(0, vp::pg_lsh_v0_query_groups_routed(
      query, kDim, kBits, kTables, 3, kSeed, routing, &second));
  ASSERT_EQ(first.size(), second.size());
  for (size_t i = 0; i < first.size(); ++i) {
    EXPECT_EQ(first[i].table, second[i].table) << "group " << i;
    EXPECT_EQ(first[i].lsh_bucket_id, second[i].lsh_bucket_id) << "group " << i;
  }
}

TEST(VectorPgLshQueryRouting, BitProjectionSignsMatchBucketId)
{
  // pg_lsh_v0_bit_projections() and pg_lsh_v0_lsh_bucket_id() must agree
  // on which side of a hyperplane the query fell: the margin score only
  // means anything if it belongs to the bit that was chosen.
  for (int variant = 0; variant < 5; ++variant) {
    const auto values = to_floats(make_vector(variant));
    for (uint32_t table = 0; table < kTables; ++table) {
      std::vector<double> projections;
      vp::pg_lsh_v0_bit_projections(values, table, kBits, kSeed, &projections);
      const uint32_t bucket =
        vp::pg_lsh_v0_lsh_bucket_id(values, table, kBits, kSeed);
      for (uint32_t bit = 0; bit < kBits; ++bit) {
        const bool set = (bucket & (uint32_t{1} << bit)) != 0;
        EXPECT_EQ(set, projections[bit] >= 0)
            << "variant " << variant << " table " << table << " bit " << bit;
      }
    }
  }
}
