// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-

#include "ceph_vector_bench_load.h"
#include "ceph_vector_bench_config.h"
#include "ceph_vector_bench_io.h"
#include "ceph_vector_bench_pg.h"
#include "ceph_vector_bench_table_set.h"

#include "common/errno.h"
#include "global/global_context.h"

#include "include/rados/librados.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <iostream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace librados;

int run_vector_bench_load(const VectorBenchOptions& opt) {
  Rados rados;
  int ret = rados.init_with_context(g_ceph_context);
  if (ret < 0) {
    std::cerr << "couldn't initialize rados: " << cpp_strerror(ret) << std::endl;
    return 1;
  }
  ret = rados.connect();
  if (ret < 0) {
    std::cerr << "couldn't connect to cluster: " << cpp_strerror(ret) << std::endl;
    return 1;
  }

  if (opt.create_pool) {
    ret = rados.pool_create(opt.pool_name.c_str());
    if (ret < 0 && ret != -EEXIST) {
      std::cerr << "pool create failed: " << cpp_strerror(ret) << std::endl;
      return 1;
    }
  }

  IoCtx ioctx;
  ret = rados.ioctx_create(opt.pool_name.c_str(), ioctx);
  if (ret < 0) {
    std::cerr << "cannot open pool " << opt.pool_name << ": " << cpp_strerror(ret) << std::endl;
    return 1;
  }

  size_t d, n;
  float* vectors = fvecs_read(opt.fvecs_file.c_str(), &d, &n);
  if (!vectors) return 1;
  if (opt.max_vectors > 0 && n > opt.max_vectors) n = opt.max_vectors;

  struct WriteOp {
    size_t vec_idx;
    uint32_t set_id;
    uint32_t pg_id;
    int64_t hash;
    bool set_mode;
  };

  std::vector<WriteOp> all_ops;
  std::unordered_map<uint32_t, size_t> pg_distribution;
  size_t selected_pg_total = 0;
  size_t skipped_vectors = 0;
  size_t replica_write_total = 0;

  for (size_t i = 0; i < n; ++i) {
    float* vec = vectors + i * d;
    const size_t begin_count = all_ops.size();

    if (opt.table_set_size > 0) {
      auto set_votes = build_set_votes(vec, static_cast<int>(d), opt);
      std::unordered_set<uint64_t> dedup_set_pg;

      for (const auto& sv : set_votes) {
        size_t keep_n = std::min<size_t>(opt.write_top_pgs_per_set, sv.ranked.size());
        if (opt.repeat_select_single_pg) keep_n = std::min<size_t>(keep_n, 1);
        if (opt.write_top_pgs > 0) keep_n = std::min<size_t>(keep_n, opt.write_top_pgs);
        for (size_t r = 0; r < keep_n; ++r) {
          const auto& c = sv.ranked[r];
          uint64_t key = (static_cast<uint64_t>(sv.set_id) << 32) | c.pg_id;
          if (!dedup_set_pg.insert(key).second) continue;
          all_ops.push_back({i, sv.set_id, c.pg_id, c.hash, true});
          pg_distribution[c.pg_id]++;
          selected_pg_total++;
        }

        auto replica_sets = replica_sets_for(sv.set_id, opt, static_cast<uint32_t>(set_votes.size()));
        for (uint32_t rs : replica_sets) {
          if (rs >= set_votes.size()) continue;
          const auto& rv = set_votes[rs];
          size_t replica_keep_n = std::min<size_t>(opt.write_top_pgs_per_set, rv.ranked.size());
          if (opt.repeat_select_single_pg) replica_keep_n = std::min<size_t>(replica_keep_n, 1);
          if (opt.write_top_pgs > 0) replica_keep_n = std::min<size_t>(replica_keep_n, opt.write_top_pgs);
          for (size_t rr = 0; rr < replica_keep_n; ++rr) {
            const auto& c = rv.ranked[rr];
            uint64_t key = (static_cast<uint64_t>(rs) << 32) | c.pg_id;
            if (!dedup_set_pg.insert(key).second) continue;
            all_ops.push_back({i, rs, c.pg_id, c.hash, true});
            pg_distribution[c.pg_id]++;
            selected_pg_total++;
            replica_write_total++;
          }
        }
      }
    } else {
      std::unordered_map<uint32_t, size_t> pg_votes;
      std::unordered_map<uint32_t, int64_t> pg_to_hash;
      for (uint32_t t = 0; t < opt.num_tables; ++t) {
        const uint32_t rounds = (opt.hash_backend == "orth-rot")
          ? std::max<uint32_t>(1, opt.hash_repeat_rounds)
          : 1u;
        for (uint32_t r = 0; r < rounds; ++r) {
          __u32 raw_hash = table_hash_for_vec(vec, static_cast<int>(d), t, opt, r);
          uint32_t pg_id = map_hash_to_pg(raw_hash, opt.pg_num, opt.pg_map_mode);
          pg_votes[pg_id]++;
          if (pg_to_hash.find(pg_id) == pg_to_hash.end()) pg_to_hash[pg_id] = static_cast<int64_t>(raw_hash);
        }
      }

      std::vector<std::pair<uint32_t, size_t>> ranked(pg_votes.begin(), pg_votes.end());
      std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
      });

      std::vector<uint32_t> selected_pgs;
      if (opt.table_combine == "and") {
        const uint32_t rounds = (opt.hash_backend == "orth-rot")
          ? std::max<uint32_t>(1, opt.hash_repeat_rounds)
          : 1u;
        const uint32_t need = opt.num_tables * rounds;
        for (const auto& kv : ranked) {
          if (kv.second == need) selected_pgs.push_back(kv.first);
        }
      } else {
        for (const auto& kv : ranked) selected_pgs.push_back(kv.first);
      }

      size_t keep_n = selected_pgs.size();
      if (opt.repeat_select_single_pg) keep_n = std::min<size_t>(keep_n, 1);
      if (opt.write_top_pgs > 0) keep_n = std::min<size_t>(keep_n, opt.write_top_pgs);
      for (size_t r = 0; r < keep_n; ++r) {
        uint32_t pg_id = selected_pgs[r];
        all_ops.push_back({i, 0, pg_id, pg_to_hash[pg_id], false});
        pg_distribution[pg_id]++;
        selected_pg_total++;
      }
    }

    if (all_ops.size() == begin_count) skipped_vectors++;
  }

  const size_t concurrency = 128;
  std::vector<bufferlist> bl_pool(concurrency);
  std::vector<AioCompletion*> comp_pool(concurrency);

  std::cout << "loading " << n << " vectors (dim=" << d << ") from " << opt.fvecs_file
            << " -> pool " << opt.pool_name
            << " (num_tables=" << opt.num_tables
            << ", table_set_size=" << opt.table_set_size
            << ", hash_backend=" << opt.hash_backend
            << ", rot_seed=" << opt.rot_seed
            << ", hash_bits=" << ((opt.hash_bits > 0) ? opt.hash_bits : valid_lsh_bits(opt.pg_num))
            << ", hash_repeat_rounds=" << opt.hash_repeat_rounds
            << ", repeat_select_single_pg=" << (opt.repeat_select_single_pg ? "true" : "false")
            << ", table_combine=" << opt.table_combine
            << ", set_combine=" << opt.set_combine
            << ", set_replica_mode=" << opt.set_replica_mode
            << ", write_top_pgs_per_set=" << opt.write_top_pgs_per_set
            << ", fan-out writes=" << all_ops.size() << ")\n";

  auto t0 = std::chrono::steady_clock::now();
  size_t written = 0;
  for (size_t off = 0; off < all_ops.size(); off += concurrency) {
    size_t batch = std::min(concurrency, all_ops.size() - off);
    for (size_t i = 0; i < batch; ++i) comp_pool[i] = rados.aio_create_completion();

    for (size_t i = 0; i < batch; ++i) {
      const auto& op = all_ops[off + i];
      float* vec = vectors + op.vec_idx * d;
      ioctx.locator_set_hash(op.hash);
      char oid[256];
      if (op.set_mode) {
        snprintf(oid, sizeof(oid), "%s%08zu_s%u_pg%u", opt.obj_prefix.c_str(), op.vec_idx, op.set_id, op.pg_id);
      } else {
        snprintf(oid, sizeof(oid), "%s%08zu_pg%u", opt.obj_prefix.c_str(), op.vec_idx, op.pg_id);
      }
      bl_pool[i].clear();
      bl_pool[i].append(reinterpret_cast<const char*>(vec), d * sizeof(float));
      ret = ioctx.aio_write_full(oid, comp_pool[i], bl_pool[i]);
      if (ret < 0) {
        std::cerr << "aio_write_full failed: " << cpp_strerror(ret) << std::endl;
        delete[] vectors;
        return 1;
      }
    }

    for (size_t i = 0; i < batch; ++i) {
      comp_pool[i]->wait_for_complete();
      ret = comp_pool[i]->get_return_value();
      comp_pool[i]->release();
      if (ret < 0) {
        std::cerr << "write failed: " << cpp_strerror(ret) << std::endl;
        delete[] vectors;
        return 1;
      }
      written++;
    }
    ioctx.locator_set_hash(-1);
  }
  delete[] vectors;

  auto t1 = std::chrono::steady_clock::now();
  double sec = std::chrono::duration<double>(t1 - t0).count();
  double overhead = (n > 0) ? static_cast<double>(all_ops.size()) / n : 0;
  double avg_sel = (n > 0) ? static_cast<double>(selected_pg_total) / n : 0;
  std::cout << "done: " << written << " writes in " << sec << "s\n";
  std::cout << "storage_overhead=" << overhead
            << "x, avg_selected_pgs_per_vector=" << avg_sel
            << ", replica_writes=" << replica_write_total
            << ", skipped_vectors=" << skipped_vectors << std::endl;
  return 0;
}
