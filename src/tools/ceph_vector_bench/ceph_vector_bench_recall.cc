// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-

#include "ceph_vector_bench_recall.h"
#include "ceph_vector_bench_config.h"
#include "ceph_vector_bench_io.h"
#include "ceph_vector_bench_pg.h"
#include "ceph_vector_bench_table_set.h"

#include "common/errno.h"
#include "global/global_context.h"

#include "include/rados/librados.hpp"

extern "C" {
#include "crush/hash.h"
}

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
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
} // namespace

int run_vector_bench_recall(const VectorBenchOptions& opt) {
  if (opt.pool_name.empty() || opt.query_file.empty() || opt.gt_file.empty() || opt.fvecs_file.empty()) {
    std::cerr << "error: --recall requires -p, -q, --gt, -f\n";
    return 1;
  }

  size_t qd, qn, bd, bn, gt_k, gt_n;
  float* queries = fvecs_read(opt.query_file.c_str(), &qd, &qn);
  if (!queries) return 1;
  if (opt.max_queries > 0 && qn > opt.max_queries) qn = opt.max_queries;
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
  if (qd != bd || gt_n != qn) {
    std::cerr << "error: input dimensions/count mismatch\n";
    delete[] queries;
    delete[] gt;
    delete[] base;
    return 1;
  }

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
  ioctx.set_namespace(all_nspaces);
  for (auto it = ioctx.nobjects_begin(); it != ioctx.nobjects_end(); ++it) {
    size_t idx = 0;
    uint32_t pg = 0;
    if (!parse_oid_idx_pg(it->get_oid(), opt.obj_prefix, &idx, &pg)) continue;
    if (idx >= bn) continue;
    std::vector<float> vec(base + idx * bd, base + idx * bd + bd);
    pg_vecs[pg].emplace_back(idx, std::move(vec));
  }

  double recall_sum = 0;
  size_t valid_queries = 0;
  size_t probe_pg_sum = 0;
  size_t cand_sum = 0;
  size_t empty_target_queries = 0;
  std::vector<std::pair<float, size_t>> dist_idx;

  auto t0 = std::chrono::steady_clock::now();
  for (size_t q = 0; q < qn; ++q) {
    std::unordered_set<uint32_t> target_pgs;

    if (opt.table_set_size > 0) {
      auto set_votes = build_set_votes(queries + q * qd, static_cast<int>(qd), opt);
      for (const auto& sv : set_votes) {
        append_probe_pgs_for_set(sv, opt, &target_pgs);
      }
    } else {
      std::unordered_map<uint32_t, size_t> pg_votes;
      for (uint32_t t = 0; t < opt.num_tables; ++t) {
        __u32 raw_hash = crush_hash32_lsh_multi(queries + q * qd, static_cast<int>(qd), static_cast<int>(t));
        uint32_t pg = map_hash_to_pg(raw_hash, opt.pg_num, opt.pg_map_mode);
        pg_votes[pg]++;
      }
      std::vector<std::pair<uint32_t, size_t>> ranked(pg_votes.begin(), pg_votes.end());
      std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
      });
      if (opt.table_combine == "and") {
        for (const auto& kv : ranked) {
          if (kv.second == opt.num_tables) target_pgs.insert(kv.first);
        }
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
      continue;
    }

    std::unordered_map<size_t, const float*> dedup;
    for (uint32_t pg : target_pgs) {
      auto it = pg_vecs.find(pg);
      if (it == pg_vecs.end()) continue;
      for (const auto& c : it->second) dedup[c.first] = c.second.data();
    }
    if (dedup.empty()) continue;

    std::vector<std::pair<size_t, const float*>> cands(dedup.begin(), dedup.end());
    probe_pg_sum += target_pgs.size();
    cand_sum += cands.size();

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
    recall_sum += (gt_k > 0) ? static_cast<double>(hits) / gt_k : 0;
    valid_queries++;
  }
  auto t1 = std::chrono::steady_clock::now();

  double sec = std::chrono::duration<double>(t1 - t0).count();
  double avg_recall = (valid_queries > 0) ? (100.0 * recall_sum / valid_queries) : 0;
  double avg_probe = (valid_queries > 0) ? static_cast<double>(probe_pg_sum) / valid_queries : 0;
  double avg_cands = (valid_queries > 0) ? static_cast<double>(cand_sum) / valid_queries : 0;
  double qps = (sec > 0) ? valid_queries / sec : 0;

  std::cout << "=== Recall ===\n";
  std::cout << "recall@" << gt_k << ": " << avg_recall << "%\n";
  std::cout << "avg_probe_pgs: " << avg_probe << "\n";
  std::cout << "avg_candidates: " << avg_cands << ", empty_target_queries: " << empty_target_queries << "\n";
  std::cout << "qps: " << qps << "\n";

  delete[] queries;
  delete[] gt;
  delete[] base;
  return 0;
}
