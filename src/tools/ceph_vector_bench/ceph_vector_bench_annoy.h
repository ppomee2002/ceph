// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
#pragma once

#include "ceph_vector_bench_config.h"
#include "ceph_vector_bench_table_set.h"

#include <cstddef>
#include <cstdint>
#include <vector>

struct AnnoyReprEntry {
  uint32_t pg_id = 0;
  std::vector<float> rotated;
};

struct AnnoyTreeNode {
  bool leaf = true;
  int left = -1;
  int right = -1;
  std::vector<int> item_indices;
  std::vector<float> normal;
  float bias = 0.0f;
};

struct AnnoyTree {
  int root = -1;
  std::vector<AnnoyTreeNode> nodes;
};

struct AnnoyModel {
  int dim = 0;
  bool angular = false;
  uint32_t n_trees = 0;
  uint32_t search_k = 0;
  uint32_t leaf_size = 0;
  uint32_t seed = 0;
  std::vector<AnnoyReprEntry> reps;
  std::vector<AnnoyTree> trees;

  size_t representative_count() const {
    return reps.size();
  }
};

inline bool is_annoy_backend(const VectorBenchOptions& opt) {
  return opt.hash_backend == "annoy" || opt.hash_backend == "annoy_tree";
}

AnnoyModel build_annoy_model(const float* vectors, size_t vector_count, int dim, const VectorBenchOptions& opt);

std::vector<TablePgCandidate> annoy_candidates_for_vec(
  const float* vec, int dim, const VectorBenchOptions& opt, const AnnoyModel& model, size_t keep_n);
