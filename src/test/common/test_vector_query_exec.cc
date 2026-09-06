// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include "common/vector_query_exec.h"

#include <optional>

#include <gtest/gtest.h>

using ceph::rados::vector_query_exec::tree_boundary_tau_improved;

// current_tau() is nullopt until local_top_k results have been retained,
// meaning no bound exists yet rather than the worst possible one. The
// no-improvement termination check must not read that as no improvement,
// or query_vectors_tree_boundary() stops before top-k is filled.

TEST(VectorQueryExec, TopKStillUnfilledAlwaysContinues)
{
  // tau_after == nullopt: top-k not yet filled, regardless of tau_before.
  EXPECT_TRUE(tree_boundary_tau_improved(std::nullopt, std::nullopt));
  EXPECT_TRUE(tree_boundary_tau_improved(1.5f, std::nullopt));
}

TEST(VectorQueryExec, FirstBoundEstablishedContinues)
{
  // tau_before == nullopt, tau_after has a value: the bound was just
  // established for the first time.
  EXPECT_TRUE(tree_boundary_tau_improved(std::nullopt, 2.0f));
}

TEST(VectorQueryExec, BothPresentImprovedContinues)
{
  EXPECT_TRUE(tree_boundary_tau_improved(2.0f, 1.0f));
}

TEST(VectorQueryExec, BothPresentNoImprovementStops)
{
  EXPECT_FALSE(tree_boundary_tau_improved(1.0f, 1.0f));
  EXPECT_FALSE(tree_boundary_tau_improved(1.0f, 1.5f));
}
