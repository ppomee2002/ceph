// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-

#include "ceph_vector_bench_annoy.h"

#include "ceph_vector_bench_pg.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <queue>
#include <random>
#include <unordered_set>

namespace {

float dot_product(const std::vector<float>& a, const std::vector<float>& b) {
  float s = 0.0f;
  const size_t n = std::min(a.size(), b.size());
  for (size_t i = 0; i < n; ++i) s += a[i] * b[i];
  return s;
}

void normalize_vec(std::vector<float>* v) {
  float norm2 = 0.0f;
  for (float x : *v) norm2 += x * x;
  if (norm2 <= std::numeric_limits<float>::epsilon()) return;
  const float inv = 1.0f / std::sqrt(norm2);
  for (float& x : *v) x *= inv;
}

float repr_dist(const std::vector<float>& query, const AnnoyReprEntry& rep, bool angular) {
  if (angular) {
    return 1.0f - dot_product(query, rep.rotated);
  }
  return l2_dist_sq(query.data(), rep.rotated.data(), static_cast<int>(query.size()));
}

void fallback_median_split(const std::vector<int>& items,
                           const std::vector<AnnoyReprEntry>& reps,
                           std::mt19937* rng,
                           std::vector<int>* left,
                           std::vector<int>* right) {
  std::vector<float> axis = reps[items[(*rng)() % items.size()]].rotated;
  std::vector<std::pair<float, int>> proj;
  proj.reserve(items.size());
  for (int idx : items) {
    proj.emplace_back(dot_product(reps[idx].rotated, axis), idx);
  }
  std::sort(proj.begin(), proj.end(), [](const auto& a, const auto& b) {
    if (a.first != b.first) return a.first < b.first;
    return a.second < b.second;
  });
  const size_t mid = proj.size() / 2;
  for (size_t i = 0; i < proj.size(); ++i) {
    if (i < mid) left->push_back(proj[i].second);
    else right->push_back(proj[i].second);
  }
}

int build_tree_rec(AnnoyTree* tree,
                   const std::vector<AnnoyReprEntry>& reps,
                   const std::vector<int>& items,
                   uint32_t leaf_size,
                   std::mt19937* rng) {
  const int node_id = static_cast<int>(tree->nodes.size());
  tree->nodes.emplace_back();
  AnnoyTreeNode& node = tree->nodes[node_id];

  if (items.size() <= leaf_size || items.size() < 2) {
    node.leaf = true;
    node.item_indices = items;
    return node_id;
  }

  const int a = items[(*rng)() % items.size()];
  int b = a;
  for (int tries = 0; tries < 4 && b == a; ++tries) {
    b = items[(*rng)() % items.size()];
  }
  if (b == a) {
    node.leaf = true;
    node.item_indices = items;
    return node_id;
  }

  node.leaf = false;
  node.normal.resize(reps[a].rotated.size());
  float na2 = 0.0f;
  float nb2 = 0.0f;
  for (size_t d = 0; d < node.normal.size(); ++d) {
    node.normal[d] = reps[a].rotated[d] - reps[b].rotated[d];
    na2 += reps[a].rotated[d] * reps[a].rotated[d];
    nb2 += reps[b].rotated[d] * reps[b].rotated[d];
  }
  node.bias = 0.5f * (na2 - nb2);

  std::vector<int> left;
  std::vector<int> right;
  left.reserve(items.size());
  right.reserve(items.size());
  for (int idx : items) {
    const float side = dot_product(reps[idx].rotated, node.normal) - node.bias;
    if (side <= 0.0f) left.push_back(idx);
    else right.push_back(idx);
  }

  if (left.empty() || right.empty()) {
    left.clear();
    right.clear();
    fallback_median_split(items, reps, rng, &left, &right);
  }
  if (left.empty() || right.empty()) {
    node.leaf = true;
    node.item_indices = items;
    node.normal.clear();
    return node_id;
  }

  const int left_id = build_tree_rec(tree, reps, left, leaf_size, rng);
  const int right_id = build_tree_rec(tree, reps, right, leaf_size, rng);
  tree->nodes[node_id].left = left_id;
  tree->nodes[node_id].right = right_id;
  return node_id;
}

void descend_greedy_collect(const AnnoyTree& tree,
                            uint32_t tree_idx,
                            int start_node,
                            const std::vector<float>& qvec,
                            std::unordered_set<int>* out_items,
                            std::priority_queue<std::pair<float, uint64_t>,
                                                std::vector<std::pair<float, uint64_t>>,
                                                std::greater<>>& fringe) {
  int node_id = start_node;
  while (node_id >= 0) {
    const auto& node = tree.nodes[node_id];
    if (node.leaf) {
      for (int item : node.item_indices) out_items->insert(item);
      return;
    }
    const float side = dot_product(qvec, node.normal) - node.bias;
    const int near = (side <= 0.0f) ? node.left : node.right;
    const int far = (side <= 0.0f) ? node.right : node.left;
    if (far >= 0) {
      const uint64_t key = (static_cast<uint64_t>(tree_idx) << 32) |
                           static_cast<uint32_t>(far);
      fringe.push({std::fabs(side), key});
    }
    node_id = near;
  }
}

} // namespace

AnnoyModel build_annoy_model(const float* vectors, size_t vector_count, int dim, const VectorBenchOptions& opt) {
  AnnoyModel model;
  model.dim = dim;
  model.angular = (opt.annoy_dist == "angular");
  model.n_trees = std::max<uint32_t>(1, opt.annoy_n_trees);
  model.search_k = std::max<uint32_t>(1, opt.annoy_search_k);
  model.leaf_size = std::max<uint32_t>(1, opt.annoy_leaf_size);
  model.seed = opt.annoy_seed;

  if (!is_annoy_backend(opt) || !vectors || vector_count == 0 || dim <= 0 || opt.pg_num == 0) {
    return model;
  }

  std::unordered_set<uint32_t> have_pg;
  std::vector<float> rotated(static_cast<size_t>(dim));
  model.reps.reserve(std::min<size_t>(vector_count, opt.pg_num));

  const size_t sample_limit = (opt.annoy_build_sample > 0)
                                ? std::min<size_t>(vector_count, opt.annoy_build_sample)
                                : vector_count;
  const size_t stride = std::max<size_t>(1, vector_count / sample_limit);
  size_t sampled = 0;
  for (size_t i = 0; i < vector_count && sampled < sample_limit; i += stride, ++sampled) {
    const float* vec = vectors + i * static_cast<size_t>(dim);
    if (rotation_apply_for_backend(vec, dim, 0, opt, rotated.data()) <= 0) continue;
    if (model.angular) normalize_vec(&rotated);
    const uint32_t pg = crush_hash32_rotation_seed_pg(rotated.data(), dim, opt.pg_num);
    if (!have_pg.insert(pg).second) continue;
    model.reps.push_back({pg, rotated});
    if (model.reps.size() >= opt.pg_num) break;
  }

  if (model.reps.empty()) return model;

  std::vector<int> all_items(model.reps.size());
  for (size_t i = 0; i < model.reps.size(); ++i) {
    all_items[i] = static_cast<int>(i);
  }

  model.trees.reserve(model.n_trees);
  for (uint32_t t = 0; t < model.n_trees; ++t) {
    std::mt19937 rng(model.seed + 0x9e3779b9u * (t + 1));
    AnnoyTree tree;
    tree.nodes.reserve(model.reps.size() * 2);
    tree.root = build_tree_rec(&tree, model.reps, all_items, model.leaf_size, &rng);
    model.trees.push_back(std::move(tree));
  }
  return model;
}

std::vector<TablePgCandidate> annoy_candidates_for_vec(
  const float* vec, int dim, const VectorBenchOptions& opt, const AnnoyModel& model, size_t keep_n) {
  std::vector<TablePgCandidate> out;
  if (!is_annoy_backend(opt) || !vec || dim <= 0 || model.reps.empty() || model.trees.empty()) {
    return out;
  }

  std::vector<float> rotated(static_cast<size_t>(dim));
  if (rotation_apply_for_backend(vec, dim, 0, opt, rotated.data()) <= 0) {
    return out;
  }
  if (model.angular) normalize_vec(&rotated);

  if (keep_n >= model.reps.size()) {
    struct DistPg {
      float dist = 0.0f;
      uint32_t pg_id = 0;
    };
    std::vector<DistPg> ranked;
    ranked.reserve(model.reps.size());
    for (const auto& rep : model.reps) {
      ranked.push_back({repr_dist(rotated, rep, model.angular), rep.pg_id});
    }
    std::sort(ranked.begin(), ranked.end(), [](const DistPg& a, const DistPg& b) {
      if (a.dist != b.dist) return a.dist < b.dist;
      return a.pg_id < b.pg_id;
    });
    out.reserve(ranked.size());
    for (const auto& item : ranked) {
      out.push_back({item.pg_id, static_cast<int64_t>(item.pg_id)});
    }
    return out;
  }

  std::unordered_set<int> repr_ids;
  std::priority_queue<std::pair<float, uint64_t>,
                      std::vector<std::pair<float, uint64_t>>,
                      std::greater<>> fringe;
  size_t visited_budget = 0;
  const size_t budget = std::max<size_t>(1, opt.annoy_search_k);
  for (uint32_t ti = 0; ti < model.trees.size(); ++ti) {
    const auto& tree = model.trees[ti];
    descend_greedy_collect(tree, ti, tree.root, rotated, &repr_ids, fringe);
    visited_budget++;
  }
  while (!fringe.empty() && visited_budget < budget) {
    const uint64_t key = fringe.top().second;
    fringe.pop();
    const uint32_t tree_idx = static_cast<uint32_t>(key >> 32);
    const int node_id = static_cast<int>(static_cast<uint32_t>(key & 0xffffffffu));
    if (tree_idx < model.trees.size()) {
      const auto& tree = model.trees[tree_idx];
      if (node_id >= 0 && node_id < static_cast<int>(tree.nodes.size())) {
        descend_greedy_collect(tree, tree_idx, node_id, rotated, &repr_ids, fringe);
      }
    }
    visited_budget++;
  }

  struct DistPg {
    float dist = 0.0f;
    uint32_t pg_id = 0;
  };
  std::vector<DistPg> ranked;
  ranked.reserve(repr_ids.size());
  for (int idx : repr_ids) {
    if (idx < 0 || idx >= static_cast<int>(model.reps.size())) continue;
    const auto& rep = model.reps[static_cast<size_t>(idx)];
    ranked.push_back({repr_dist(rotated, rep, model.angular), rep.pg_id});
  }
  if (ranked.empty()) return out;

  std::sort(ranked.begin(), ranked.end(), [](const DistPg& a, const DistPg& b) {
    if (a.dist != b.dist) return a.dist < b.dist;
    return a.pg_id < b.pg_id;
  });

  if (keep_n == 0) {
    keep_n = std::max<size_t>(1, opt.probe_pgs);
  }
  keep_n = std::min(keep_n, ranked.size());
  out.reserve(keep_n);
  for (size_t i = 0; i < keep_n; ++i) {
    out.push_back({ranked[i].pg_id, static_cast<int64_t>(ranked[i].pg_id)});
  }
  return out;
}
