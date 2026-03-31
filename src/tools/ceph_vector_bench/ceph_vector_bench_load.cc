// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-

#include "ceph_vector_bench_load.h"
#include "ceph_vector_bench_config.h"
#include "ceph_vector_bench_io.h"
#include "ceph_vector_bench_pg.h"

#include "common/errno.h"
#include "global/global_context.h"

#include "include/rados/librados.hpp"

extern "C" {
#include "crush/hash.h"
}

#include <chrono>
#include <cstdio>
#include <algorithm>
#include <iostream>
#include <unordered_map>
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
  if (!vectors) {
    return 1;
  }

  if (opt.max_vectors > 0 && n > opt.max_vectors) {
    n = opt.max_vectors;
  }

  struct WriteOp { size_t vec_idx; uint32_t pg_id; int64_t hash; };
  std::vector<WriteOp> all_ops;
  std::unordered_map<uint32_t, size_t> pg_distribution;
  all_ops.reserve(n * opt.num_tables);
  size_t selected_pg_total = 0;
  size_t skipped_vectors = 0;

  for (size_t i = 0; i < n; i++) {
    float* vec = vectors + i * d;
    std::unordered_map<uint32_t, size_t> pg_votes;
    std::unordered_map<uint32_t, int64_t> pg_to_hash;
    for (uint32_t t = 0; t < opt.num_tables; t++) {
      __u32 raw_hash = crush_hash32_lsh_multi(vec, static_cast<int>(d), static_cast<int>(t));
      uint32_t pg_id = map_hash_to_pg(raw_hash, opt.pg_num, opt.pg_map_mode);
      pg_votes[pg_id]++;
      if (pg_to_hash.find(pg_id) == pg_to_hash.end())
        pg_to_hash[pg_id] = static_cast<int64_t>(raw_hash);
    }

    std::vector<std::pair<uint32_t, size_t>> ranked(pg_votes.begin(), pg_votes.end());
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
      if (a.second != b.second) return a.second > b.second;
      return a.first < b.first;
    });

    std::vector<uint32_t> selected_pgs;
    if (opt.table_combine == "and") {
      for (const auto& kv : ranked) {
        if (kv.second == opt.num_tables)
          selected_pgs.push_back(kv.first);
      }
    } else {
      for (const auto& kv : ranked)
        selected_pgs.push_back(kv.first);
    }

    size_t keep_n = selected_pgs.size();
    if (opt.write_top_pgs > 0)
      keep_n = std::min<size_t>(keep_n, opt.write_top_pgs);
    if (keep_n == 0) {
      skipped_vectors++;
      continue;
    }
    selected_pg_total += keep_n;

    for (size_t r = 0; r < keep_n; r++) {
      uint32_t pg_id = selected_pgs[r];
      all_ops.push_back({i, pg_id, pg_to_hash[pg_id]});
      pg_distribution[pg_id]++;
    }
  }

  const size_t concurrency = 128;
  std::vector<librados::bufferlist> bl_pool(concurrency);
  std::vector<librados::AioCompletion*> comp_pool(concurrency);

  std::cout << "loading " << n << " vectors (dim=" << d << ") from " << opt.fvecs_file
            << " -> pool " << opt.pool_name << " (pg_num=" << opt.pg_num
            << ", num_tables=" << opt.num_tables << ", write_top_pgs=" << opt.write_top_pgs
            << ", pg_map_mode=" << opt.pg_map_mode
            << ", table_combine=" << opt.table_combine
            << ", fan-out writes=" << all_ops.size()
            << ", concurrency=" << concurrency << ")"
            << std::endl;

  auto t0 = std::chrono::steady_clock::now();
  size_t written = 0;
  size_t report_interval = std::max<size_t>(1, all_ops.size() / 20);
  size_t last_report = 0;

  for (size_t offset = 0; offset < all_ops.size(); offset += concurrency) {
    size_t batch = std::min(concurrency, all_ops.size() - offset);
    for (size_t k = 0; k < batch; k++)
      comp_pool[k] = rados.aio_create_completion();
    for (size_t j = 0; j < batch; j++) {
      const WriteOp& op = all_ops[offset + j];
      float* vec = vectors + op.vec_idx * d;

      ioctx.locator_set_hash(op.hash);

      char oid[256];
      snprintf(oid, sizeof(oid), "%s%08zu_pg%u", opt.obj_prefix.c_str(), op.vec_idx, op.pg_id);

      bl_pool[j].clear();
      bl_pool[j].append(reinterpret_cast<const char*>(vec), d * sizeof(float));

      ret = ioctx.aio_write_full(oid, comp_pool[j], bl_pool[j]);
      if (ret < 0) {
        std::cerr << "aio_write_full failed at vec " << op.vec_idx << " pg " << op.pg_id
                  << ": " << cpp_strerror(ret) << std::endl;
        for (size_t k = 0; k < batch; k++)
          comp_pool[k]->release();
        delete[] vectors;
        return 1;
      }
    }
    for (size_t j = 0; j < batch; j++) {
      comp_pool[j]->wait_for_complete();
      ret = comp_pool[j]->get_return_value();
      comp_pool[j]->release();
      if (ret < 0) {
        const WriteOp& op = all_ops[offset + j];
        std::cerr << "write failed at vec " << op.vec_idx << " pg " << op.pg_id
                  << ": " << cpp_strerror(ret) << std::endl;
        for (size_t k = j + 1; k < batch; k++)
          comp_pool[k]->release();
        delete[] vectors;
        return 1;
      }
      written++;
    }
    ioctx.locator_set_hash(-1);

    size_t done = offset + batch;
    if (done - last_report >= report_interval || done == all_ops.size()) {
      last_report = done;
      auto t1 = std::chrono::steady_clock::now();
      double sec = std::chrono::duration<double>(t1 - t0).count();
      double rate = done / sec;
      std::cout << "  " << done << "/" << all_ops.size() << " writes (" << (100 * done / all_ops.size()) << "%) "
                << rate << " write/s" << std::endl;
    }
  }

  delete[] vectors;

  auto t1 = std::chrono::steady_clock::now();
  double sec = std::chrono::duration<double>(t1 - t0).count();
  std::cout << "done: " << written << " fan-out writes (" << n << " vectors) in " << sec << "s ("
            << (written / sec) << " write/s)" << std::endl;

  size_t total_objects = all_ops.size();
  size_t pg_count = pg_distribution.size();
  size_t min_per_pg = total_objects, max_per_pg = 0;
  size_t sum_per_pg = 0;
  for (const auto& kv : pg_distribution) {
    size_t c = kv.second;
    if (c < min_per_pg) min_per_pg = c;
    if (c > max_per_pg) max_per_pg = c;
    sum_per_pg += c;
  }
  if (pg_count > 0) {
    double avg_per_pg = (double)sum_per_pg / pg_count;
    double storage_overhead = (n > 0) ? (double)total_objects / n : 0;
    double avg_selected_pgs = (n > 0) ? (double)selected_pg_total / n : 0;
    std::cout << "\n=== Storage Statistics ===\n";
    std::cout << "  Total objects (fan-out): " << total_objects << "\n";
    std::cout << "  Unique vectors:         " << n << "\n";
    std::cout << "  Avg selected PGs/vec:   " << avg_selected_pgs << "\n";
    std::cout << "  Storage overhead:       " << storage_overhead << "x (objects/vectors)\n";
    std::cout << "  Skipped vectors:        " << skipped_vectors << "\n";
    std::cout << "  PGs used:               " << pg_count << " / " << opt.pg_num << "\n";
    std::cout << "  Objects per PG: min=" << min_per_pg << ", max=" << max_per_pg
              << ", avg=" << avg_per_pg << "\n";
  } else {
    std::cout << "\n=== Storage Statistics ===\n";
    std::cout << "  Total objects (fan-out): 0\n";
    std::cout << "  Unique vectors:         " << n << "\n";
    std::cout << "  Avg selected PGs/vec:   0\n";
    std::cout << "  Storage overhead:       0x (objects/vectors)\n";
    std::cout << "  Skipped vectors:        " << skipped_vectors << "\n";
    std::cout << "  PGs used:               0 / " << opt.pg_num << "\n";
  }

  return 0;
}
