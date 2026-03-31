// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-

#include "ceph_vector_bench_recall.h"
#include "ceph_vector_bench_config.h"
#include "ceph_vector_bench_io.h"
#include "ceph_vector_bench_pg.h"

#include "common/errno.h"
#include "global/global_context.h"

#include "include/rados/librados.hpp"

extern "C" {
#include "crush/hash.h"
}

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

using namespace librados;

int run_vector_bench_recall(const VectorBenchOptions& opt) {
  if (opt.pool_name.empty() || opt.query_file.empty() || opt.gt_file.empty() ||
      opt.fvecs_file.empty()) {
    std::cerr << "error: --recall requires -p, -q (--query), --gt, -f (base .fvecs)\n";
    return 1;
  }
  size_t qd, qn;
  float* queries = fvecs_read(opt.query_file.c_str(), &qd, &qn);
  if (!queries) return 1;
  if (opt.max_queries > 0 && qn > opt.max_queries) qn = opt.max_queries;

  size_t gt_k, gt_n;
  int* gt = ivecs_read(opt.gt_file.c_str(), &gt_k, &gt_n);
  if (!gt) {
    delete[] queries;
    return 1;
  }
  if (gt_n != qn) {
    std::cerr << "error: query count (" << qn << ") != ground truth rows (" << gt_n << ")\n";
    delete[] queries;
    delete[] gt;
    return 1;
  }

  size_t bd, bn;
  float* base_vectors = fvecs_read(opt.fvecs_file.c_str(), &bd, &bn);
  if (!base_vectors) {
    delete[] queries;
    delete[] gt;
    return 1;
  }
  if (opt.max_vectors > 0 && bn > opt.max_vectors)
    bn = opt.max_vectors;
  if (qd != bd) {
    std::cerr << "error: query dim (" << qd << ") != base dim (" << bd << ")\n";
    delete[] base_vectors;
    delete[] queries;
    delete[] gt;
    return 1;
  }

  Rados rados;
  int ret = rados.init_with_context(g_ceph_context);
  if (ret < 0) {
    std::cerr << "couldn't initialize rados: " << cpp_strerror(ret) << std::endl;
    delete[] base_vectors;
    delete[] queries;
    delete[] gt;
    return 1;
  }
  ret = rados.connect();
  if (ret < 0) {
    std::cerr << "couldn't connect: " << cpp_strerror(ret) << std::endl;
    delete[] base_vectors;
    delete[] queries;
    delete[] gt;
    return 1;
  }

  IoCtx ioctx;
  ret = rados.ioctx_create(opt.pool_name.c_str(), ioctx);
  if (ret < 0) {
    std::cerr << "error: cannot open pool '" << opt.pool_name << "' (ret=" << ret << ")\n";
    delete[] base_vectors;
    delete[] queries;
    delete[] gt;
    return 1;
  }

  std::unordered_map<uint32_t, std::vector<std::pair<size_t, std::vector<float>>>> pg_vecs;
  ioctx.set_namespace(all_nspaces);
  std::cout << "scanning pool '" << opt.pool_name << "' (id=" << ioctx.get_id() << ") ..."
            << std::endl;
  size_t scan_count = 0;
  try {
    for (auto it = ioctx.nobjects_begin(); it != ioctx.nobjects_end(); ++it) {
      std::string oid = it->get_oid();

      size_t idx = 0;
      uint32_t pg = 0;
      if (oid.size() > opt.obj_prefix.size()) {
        const char* p = oid.c_str() + opt.obj_prefix.size();
        char* end = nullptr;
        idx = strtoull(p, &end, 10);
        if (end && end[0] == '_' && end[1] == 'p' && end[2] == 'g') {
          pg = static_cast<uint32_t>(strtoul(end + 3, nullptr, 10));
        } else {
          continue;
        }
      }
      if (idx >= bn)
        continue;

      std::vector<float> vec(base_vectors + idx * bd, base_vectors + idx * bd + bd);
      pg_vecs[pg].emplace_back(idx, std::move(vec));
      scan_count++;
      if (scan_count % 100000 == 0)
        std::cout << "  scanned " << scan_count << " objects" << std::endl;
    }
  } catch (const std::exception& e) {
    std::cerr << "pool scan error: " << e.what() << std::endl;
    delete[] base_vectors;
    delete[] queries;
    delete[] gt;
    return 1;
  }
  std::cout << "done: " << scan_count << " objects in " << pg_vecs.size() << " PGs" << std::endl;

  double recall_sum = 0;
  size_t valid_queries = 0;
  size_t probe_pg_sum = 0;
  size_t cand_sum = 0;
  size_t empty_target_queries = 0;
  double best_vote_sum = 0;
  double vote_concentration_sum = 0;
  std::vector<std::pair<float, size_t>> dist_idx;

  auto recall_t0 = std::chrono::steady_clock::now();
  for (size_t q = 0; q < qn; q++) {
    std::unordered_map<uint32_t, size_t> pg_votes;
    for (uint32_t t = 0; t < opt.num_tables; t++) {
      __u32 raw_hash = crush_hash32_lsh_multi(queries + q * qd, static_cast<int>(qd),
                                                static_cast<int>(t));
      uint32_t pg = map_hash_to_pg(raw_hash, opt.pg_num, opt.pg_map_mode);
      pg_votes[pg]++;
    }

    std::vector<uint32_t> target_pgs;
    double this_best_vote = 0, this_concentration = 0;

    std::vector<std::pair<uint32_t, size_t>> ranked(pg_votes.begin(), pg_votes.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
      if (a.second != b.second) return a.second > b.second;
      return a.first < b.first;
    });
    if (!ranked.empty()) {
      this_best_vote = ranked[0].second;
      this_concentration = (double)ranked[0].second / opt.num_tables;
    }

    if (opt.table_combine == "and") {
      for (const auto& kv : ranked) {
        if (kv.second == opt.num_tables)
          target_pgs.push_back(kv.first);
      }
      if (opt.probe_pgs > 0 && target_pgs.size() > opt.probe_pgs)
        target_pgs.resize(opt.probe_pgs);
    } else if (opt.probe_mode == "vote") {
      for (size_t i = 0; i < ranked.size() && i < opt.probe_pgs; i++)
        target_pgs.push_back(ranked[i].first);
    } else {
      for (const auto& kv : pg_votes)
        target_pgs.push_back(kv.first);
    }
    if (target_pgs.empty()) {
      empty_target_queries++;
      continue;
    }

    std::unordered_map<size_t, const float*> cand_dedup;
    for (uint32_t pg : target_pgs) {
      auto pit = pg_vecs.find(pg);
      if (pit != pg_vecs.end())
        for (const auto& c : pit->second)
          cand_dedup[c.first] = c.second.data();
    }
    std::vector<std::pair<size_t, const float*>> all_cands(cand_dedup.begin(), cand_dedup.end());
    if (all_cands.empty())
      continue;

    probe_pg_sum += target_pgs.size();
    cand_sum += all_cands.size();
    best_vote_sum += this_best_vote;
    vote_concentration_sum += this_concentration;

    dist_idx.clear();
    for (const auto& c : all_cands) {
      float d2 = l2_dist_sq(queries + q * qd, c.second, static_cast<int>(bd));
      dist_idx.emplace_back(d2, c.first);
    }
    std::partial_sort(dist_idx.begin(),
                      dist_idx.begin() + std::min(gt_k, dist_idx.size()),
                      dist_idx.end());

    std::unordered_set<size_t> pg_topk;
    for (size_t i = 0; i < std::min(gt_k, dist_idx.size()); i++)
      pg_topk.insert(dist_idx[i].second);

    size_t hits = 0;
    for (size_t i = 0; i < gt_k; i++) {
      int ii = gt[q * gt_k + i];
      if (ii >= 0 && pg_topk.count(static_cast<size_t>(ii))) hits++;
    }
    recall_sum += (gt_k > 0) ? (double)hits / gt_k : 0;
    valid_queries++;
  }
  auto recall_t1 = std::chrono::steady_clock::now();
  double recall_sec = std::chrono::duration<double>(recall_t1 - recall_t0).count();

  double avg_recall = (valid_queries > 0) ? (100.0 * recall_sum / valid_queries) : 0;
  double avg_probe_pgs = (valid_queries > 0) ? (double)probe_pg_sum / valid_queries : 0;
  double avg_cands = (valid_queries > 0) ? (double)cand_sum / valid_queries : 0;
  double latency_ms = (valid_queries > 0 && recall_sec > 0) ? (recall_sec * 1000.0 / valid_queries) : 0;
  double qps = (recall_sec > 0) ? (valid_queries / recall_sec) : 0;
  double search_space_pct = (bn > 0 && avg_cands > 0) ? (100.0 * avg_cands / bn) : 0;

  double avg_best_vote = (valid_queries > 0) ? (best_vote_sum / valid_queries) : 0;
  double avg_concentration = (valid_queries > 0) ? (100.0 * vote_concentration_sum / valid_queries) : 0;

  std::cout << "\n=== Recall (pg_num=" << opt.pg_num << ", num_tables=" << opt.num_tables
            << ", k=" << gt_k << ", queries=" << valid_queries << "/" << qn << ") ===\n";
  std::cout << "  PG map mode: " << opt.pg_map_mode << "\n";
  std::cout << "  Table combine: " << opt.table_combine << "\n";
  std::cout << "  Average Recall@" << gt_k << ": " << avg_recall << "%\n";
  std::cout << "  Probe mode: " << opt.probe_mode << ", probe_pgs=" << opt.probe_pgs << "\n";
  std::cout << "  Avg PGs probed: " << avg_probe_pgs << "\n";
  std::cout << "  Avg candidates: " << avg_cands << "\n";
  std::cout << "  Empty-target queries: " << empty_target_queries << "\n";
  if (opt.probe_mode == "vote") {
    std::cout << "  Avg best vote: " << avg_best_vote << ", vote concentration: " << avg_concentration << "%\n";
  }
  std::cout << "\n=== Query Performance ===\n";
  std::cout << "  Latency (avg):  " << latency_ms << " ms/query\n";
  std::cout << "  QPS:            " << qps << " queries/s\n";
  std::cout << "  Search space:   " << search_space_pct << "% of base (" << avg_cands << " / " << bn << ")\n";

  delete[] base_vectors;
  delete[] queries;
  delete[] gt;
  return 0;
}
