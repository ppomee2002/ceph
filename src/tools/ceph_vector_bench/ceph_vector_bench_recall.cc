// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-

#include "ceph_vector_bench_recall.h"
#include "ceph_vector_bench_annoy.h"
#include "ceph_vector_bench_config.h"
#include "ceph_vector_bench_io.h"
#include "ceph_vector_bench_pg.h"
#include "ceph_vector_bench_table_set.h"

#include "common/errno.h"
#include "global/global_context.h"

#include "include/rados/librados.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace librados;

namespace {
bool parse_oid_idx_pg(const std::string& oid, const std::string& prefix, size_t* idx, uint32_t* pg) {
  if (oid.size() <= prefix.size()) return false;
  const char* p = oid.c_str() + prefix.size();
  char* end = nullptr;
  *idx = strtoull(p, &end, 10);
  if (!end) return false;

  if (end[0] == '_' && end[1] == 's') {
    char* end_set = nullptr;
    (void)strtoul(end + 2, &end_set, 10);
    if (!end_set || end_set[0] != '_' || end_set[1] != 'p' || end_set[2] != 'g') return false;
    *pg = static_cast<uint32_t>(strtoul(end_set + 3, nullptr, 10));
    return true;
  }
  if (end[0] == '_' && end[1] == 'p' && end[2] == 'g') {
    *pg = static_cast<uint32_t>(strtoul(end + 3, nullptr, 10));
    return true;
  }
  return false;
}

double percentile_sorted_ms(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0.0;
  if (p <= 0.0) return sorted.front();
  if (p >= 100.0) return sorted.back();
  const double pos = (p / 100.0) * static_cast<double>(sorted.size() - 1);
  const size_t lo = static_cast<size_t>(pos);
  const size_t hi = std::min(sorted.size() - 1, lo + 1);
  const double w = pos - static_cast<double>(lo);
  return sorted[lo] * (1.0 - w) + sorted[hi] * w;
}

std::string pg_manifest_oid(uint32_t pg_id) {
  char oid[64];
  snprintf(oid, sizeof(oid), "__pg_manifest_pg%u", pg_id);
  return std::string(oid);
}

std::vector<std::string> parse_manifest_oids(const bufferlist& bl) {
  std::vector<std::string> out;
  std::string body(bl.length(), '\0');
  if (!body.empty()) {
    auto it = bl.cbegin();
    it.copy(body.size(), body.data());
  }
  std::istringstream iss(body);
  std::string line;
  while (std::getline(iss, line)) {
    if (!line.empty()) out.push_back(line);
  }
  return out;
}
} // namespace

int run_vector_bench_recall(const VectorBenchOptions& opt) {
  if (opt.pool_name.empty() || opt.query_file.empty() || opt.gt_file.empty() || opt.fvecs_file.empty()) {
    std::cerr << "error: --recall requires -p, -q, --gt, -f\n";
    return 1;
  }

  size_t qd, qn, bd, bn, gt_k, gt_n;
  float* queries = fvecs_read(opt.query_file.c_str(), &qd, &qn);
  if (!queries) return 1;
  int* gt = ivecs_read(opt.gt_file.c_str(), &gt_k, &gt_n);
  if (!gt) {
    delete[] queries;
    return 1;
  }
  float* base = fvecs_read(opt.fvecs_file.c_str(), &bd, &bn);
  if (!base) {
    delete[] queries;
    delete[] gt;
    return 1;
  }
  if (opt.max_vectors > 0 && bn > opt.max_vectors) bn = opt.max_vectors;
  if (qd != bd || gt_n < qn) {
    std::cerr << "error: input dimensions/count mismatch\n";
    delete[] queries;
    delete[] gt;
    delete[] base;
    return 1;
  }
  if (opt.max_queries > 0 && qn > opt.max_queries) qn = opt.max_queries;
  const RotationQuantizationParams rotation_params = calibrate_rotation_quant_params(
    base, bn, static_cast<int>(bd), opt);
  const uint32_t effective_rounds = effective_hash_repeat_rounds(opt);
  const RotationReprModel repr_model = build_rotation_repr1_model(
    base, bn, static_cast<int>(bd), opt);
  const AnnoyModel annoy_model = build_annoy_model(
    base, bn, static_cast<int>(bd), opt);
  const PivotModel pivot_model = build_pivot_model(
    base, bn, static_cast<int>(bd), opt);
  const HybridGroupModel hybrid_group_model = build_hybrid_group_model(
    base, bn, static_cast<int>(bd), opt);

  Rados rados;
  int ret = rados.init_with_context(g_ceph_context);
  if (ret < 0 || (ret = rados.connect()) < 0) {
    std::cerr << "rados init/connect failed: " << cpp_strerror(ret) << std::endl;
    delete[] queries;
    delete[] gt;
    delete[] base;
    return 1;
  }
  IoCtx ioctx;
  ret = rados.ioctx_create(opt.pool_name.c_str(), ioctx);
  if (ret < 0) {
    std::cerr << "cannot open pool: " << cpp_strerror(ret) << std::endl;
    delete[] queries;
    delete[] gt;
    delete[] base;
    return 1;
  }

  std::unordered_map<uint32_t, std::vector<std::pair<size_t, std::vector<float>>>> pg_vecs;
  const bool memory_mode = (opt.qps_mode == "memory");
  if (memory_mode) {
    ioctx.set_namespace(all_nspaces);
    for (auto it = ioctx.nobjects_begin(); it != ioctx.nobjects_end(); ++it) {
      size_t idx = 0;
      uint32_t pg = 0;
      if (!parse_oid_idx_pg(it->get_oid(), opt.obj_prefix, &idx, &pg)) continue;
      if (idx >= bn) continue;
      std::vector<float> vec(base + idx * bd, base + idx * bd + bd);
      pg_vecs[pg].emplace_back(idx, std::move(vec));
    }
  }

  double recall_sum = 0;
  size_t recall_queries = 0;
  size_t measured_queries = 0;
  size_t probe_pg_sum = 0;
  size_t cand_sum = 0;
  size_t empty_target_queries = 0;
  std::vector<std::pair<float, size_t>> dist_idx;
  std::vector<double> latency_ms;
  latency_ms.reserve(qn);

  const size_t warmup_queries = (!memory_mode) ? std::min<size_t>(qn, opt.warmup_queries) : 0;
  auto t0 = std::chrono::steady_clock::now();
  bool timer_started = false;
  for (size_t q = 0; q < qn; ++q) {
    const bool measured = (q >= warmup_queries);
    if (measured && !timer_started) {
      t0 = std::chrono::steady_clock::now();
      timer_started = true;
    }
    auto q0 = std::chrono::steady_clock::now();
    std::unordered_set<uint32_t> target_pgs;

    if (is_global_routing_backend(opt)) {
      auto candidates = global_pg_candidates_for_vec(
        queries + q * qd, static_cast<int>(qd), opt, pivot_model, &hybrid_group_model);
      if (opt.repeat_select_single_pg) {
        if (!candidates.empty()) target_pgs.insert(candidates.front().pg_id);
      } else if (opt.probe_mode == "vote") {
        const size_t keep_n = std::min<size_t>(candidates.size(), std::max<uint32_t>(1, opt.probe_pgs));
        for (size_t i = 0; i < keep_n; ++i) {
          target_pgs.insert(candidates[i].pg_id);
        }
      } else {
        for (const auto& c : candidates) target_pgs.insert(c.pg_id);
      }
    } else if (is_annoy_backend(opt)) {
      size_t keep_n = std::max<size_t>(1, opt.probe_pgs);
      auto candidates = annoy_candidates_for_vec(
        queries + q * qd, static_cast<int>(qd), opt, annoy_model, keep_n);
      for (const auto& c : candidates) target_pgs.insert(c.pg_id);
    } else if (is_rotation_repr1_backend(opt)) {
      auto candidates = rotation_repr1_candidates_for_vec(
        queries + q * qd, static_cast<int>(qd), opt, repr_model);
      for (const auto& c : candidates) {
        target_pgs.insert(c.pg_id);
      }
    } else if (opt.table_set_size > 0) {
      auto set_votes = build_set_votes(
        queries + q * qd, static_cast<int>(qd), opt, &rotation_params, &repr_model);
      for (const auto& sv : set_votes) {
        if (opt.repeat_select_single_pg) {
          if (!sv.ranked.empty()) target_pgs.insert(sv.ranked.front().pg_id);
        } else {
          append_probe_pgs_for_set(sv, opt, &target_pgs);
        }
      }
    } else {
      std::unordered_map<uint32_t, size_t> pg_votes;
      for (uint32_t t = 0; t < opt.num_tables; ++t) {
        const uint32_t rounds = effective_rounds;
        for (uint32_t r = 0; r < rounds; ++r) {
          auto candidates = table_pg_candidates_for_vec(
            queries + q * qd, static_cast<int>(qd), t, opt, r, &rotation_params, &repr_model);
          for (const auto& c : candidates) {
            pg_votes[c.pg_id]++;
          }
        }
      }
      std::vector<std::pair<uint32_t, size_t>> ranked(pg_votes.begin(), pg_votes.end());
      std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
      });
      if (opt.table_combine == "and") {
        const uint32_t need = opt.num_tables * effective_rounds;
        for (const auto& kv : ranked) {
          if (kv.second == need) target_pgs.insert(kv.first);
        }
      } else if (opt.repeat_select_single_pg) {
        if (!ranked.empty()) target_pgs.insert(ranked.front().first);
      } else if (opt.probe_mode == "vote") {
        for (size_t i = 0; i < ranked.size() && i < opt.probe_pgs; ++i) {
          target_pgs.insert(ranked[i].first);
        }
      } else {
        for (const auto& kv : ranked) target_pgs.insert(kv.first);
      }
    }

    if (target_pgs.empty()) {
      empty_target_queries++;
      if (measured) {
        auto q1 = std::chrono::steady_clock::now();
        latency_ms.push_back(std::chrono::duration<double, std::milli>(q1 - q0).count());
        measured_queries++;
      }
      continue;
    }

    std::unordered_map<size_t, const float*> dedup;
    std::unordered_map<size_t, std::vector<float>> fetched;
    if (memory_mode) {
      for (uint32_t pg : target_pgs) {
        auto it = pg_vecs.find(pg);
        if (it == pg_vecs.end()) continue;
        for (const auto& c : it->second) dedup[c.first] = c.second.data();
      }
    } else {
      for (uint32_t pg : target_pgs) {
        bufferlist bl;
        int r = ioctx.read(pg_manifest_oid(pg), bl, std::numeric_limits<int>::max(), 0);
        if (r < 0) continue;
        for (const auto& oid : parse_manifest_oids(bl)) {
          size_t idx = 0;
          uint32_t parsed_pg = 0;
          if (!parse_oid_idx_pg(oid, opt.obj_prefix, &idx, &parsed_pg)) continue;
          if (idx >= bn || parsed_pg != pg || dedup.find(idx) != dedup.end()) continue;
          bufferlist vec_bl;
          ioctx.locator_set_hash(static_cast<int64_t>(pg));
          r = ioctx.read(oid, vec_bl, std::numeric_limits<int>::max(), 0);
          ioctx.locator_set_hash(-1);
          if (r < 0) continue;
          if (static_cast<size_t>(r) != bd * sizeof(float)) continue;
          std::vector<float> vec(bd);
          vec_bl.begin().copy(static_cast<size_t>(r), reinterpret_cast<char*>(vec.data()));
          dedup[idx] = fetched.emplace(idx, std::move(vec)).first->second.data();
        }
      }
    }
    if (dedup.empty()) {
      if (measured) {
        auto q1 = std::chrono::steady_clock::now();
        latency_ms.push_back(std::chrono::duration<double, std::milli>(q1 - q0).count());
        measured_queries++;
      }
      continue;
    }

    std::vector<std::pair<size_t, const float*>> cands(dedup.begin(), dedup.end());
    if (measured) {
      probe_pg_sum += target_pgs.size();
      cand_sum += cands.size();
    }

    dist_idx.clear();
    for (const auto& c : cands) {
      dist_idx.emplace_back(l2_dist_sq(queries + q * qd, c.second, static_cast<int>(bd)), c.first);
    }
    std::partial_sort(dist_idx.begin(), dist_idx.begin() + std::min(gt_k, dist_idx.size()), dist_idx.end());

    std::unordered_set<size_t> topk;
    for (size_t i = 0; i < std::min(gt_k, dist_idx.size()); ++i) topk.insert(dist_idx[i].second);

    size_t hits = 0;
    for (size_t i = 0; i < gt_k; ++i) {
      int ii = gt[q * gt_k + i];
      if (ii >= 0 && topk.count(static_cast<size_t>(ii))) hits++;
    }
    if (measured) {
      recall_sum += (gt_k > 0) ? static_cast<double>(hits) / gt_k : 0;
      recall_queries++;
      auto q1 = std::chrono::steady_clock::now();
      latency_ms.push_back(std::chrono::duration<double, std::milli>(q1 - q0).count());
      measured_queries++;
    }
  }
  auto t1 = std::chrono::steady_clock::now();

  const double sec = timer_started ? std::chrono::duration<double>(t1 - t0).count() : 0.0;
  const double avg_recall = (recall_queries > 0) ? (100.0 * recall_sum / recall_queries) : 0;
  const double avg_probe = (measured_queries > 0) ? static_cast<double>(probe_pg_sum) / measured_queries : 0;
  const double avg_cands = (measured_queries > 0) ? static_cast<double>(cand_sum) / measured_queries : 0;
  const double qps = (sec > 0) ? measured_queries / sec : 0;
  double avg_latency = 0.0;
  double p50_latency = 0.0;
  double p95_latency = 0.0;
  if (!latency_ms.empty()) {
    double sum = 0.0;
    for (double v : latency_ms) sum += v;
    avg_latency = sum / static_cast<double>(latency_ms.size());
    std::sort(latency_ms.begin(), latency_ms.end());
    p50_latency = percentile_sorted_ms(latency_ms, 50.0);
    p95_latency = percentile_sorted_ms(latency_ms, 95.0);
  }

  std::cout << "=== Recall ===\n";
  std::cout << "hash_backend: " << opt.hash_backend
            << ", qps_mode: " << opt.qps_mode
            << ", warmup_queries: " << warmup_queries
            << ", rot_seed: " << opt.rot_seed
            << ", hash_bits: " << ((opt.hash_bits > 0) ? opt.hash_bits : valid_lsh_bits(opt.pg_num))
            << ", hash_repeat_rounds: " << opt.hash_repeat_rounds
            << ", effective_hash_rounds: " << effective_rounds
            << ", rotation_use_dims: " << opt.rotation_use_dims
            << ", rotation_bins_per_dim: " << opt.rotation_bins_per_dim
            << ", rotation_min: " << rotation_params.min_val
            << ", rotation_width: " << rotation_params.width
            << ", rotation_neighbor_step: " << opt.rotation_neighbor_step
            << ", rotation_auto_calibration: " << (opt.rotation_auto_calibration ? "true" : "false")
            << ", rotation_calibrated: " << (rotation_params.calibrated ? "true" : "false")
            << ", rotation_calibration_samples: " << rotation_params.sample_count
            << ", rotation_calibration_clip_percentile: " << opt.rotation_calibration_clip_percentile
            << ", repr_backend: " << (is_rotation_repr1_backend(opt) ? "true" : "false")
            << ", repr_count: " << repr_model.representative_count()
            << ", pivot_backend: " << (is_pivot_backend(opt) ? "true" : "false")
            << ", hybrid_backend: " << (is_hybrid_backend(opt) ? "true" : "false")
            << ", pivot_repr_count: " << pivot_model.representative_count()
            << ", pivot_sample_count: " << pivot_model.sample_count
            << ", hybrid_group_repr_count: " << hybrid_group_model.representative_count()
            << ", pivot_probe_budget: " << opt.pivot_probe_budget
            << ", hybrid_lsh_vote_topk: " << opt.hybrid_lsh_vote_topk
            << ", hybrid_pivot_topk: " << opt.hybrid_pivot_topk
            << ", annoy_backend: " << (is_annoy_backend(opt) ? "true" : "false")
            << ", annoy_repr_count: " << annoy_model.representative_count()
            << ", annoy_n_trees: " << annoy_model.n_trees
            << ", annoy_search_k: " << annoy_model.search_k
            << ", annoy_leaf_size: " << annoy_model.leaf_size
            << ", annoy_dist: " << opt.annoy_dist
            << ", annoy_seed: " << opt.annoy_seed
            << ", repeat_select_single_pg: " << (opt.repeat_select_single_pg ? "true" : "false")
            << "\n";
  std::cout << "recall@" << gt_k << ": " << avg_recall << "%\n";
  std::cout << "avg_probe_pgs: " << avg_probe << "\n";
  std::cout << "avg_candidates: " << avg_cands << ", empty_target_queries: " << empty_target_queries << "\n";
  std::cout << "qps: " << qps << "\n";
  if (opt.latency_report || !memory_mode) {
    std::cout << "avg_latency_ms: " << avg_latency << "\n";
    std::cout << "p50_latency_ms: " << p50_latency << "\n";
    std::cout << "p95_latency_ms: " << p95_latency << "\n";
  }

  delete[] queries;
  delete[] gt;
  delete[] base;
  return 0;
}
