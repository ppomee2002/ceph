// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab
/*
 * Ceph - scalable distributed file system
 *
 * Vector benchmark: SIFT .fvecs loader with LSH-based PG placement,
 * locality verification, and Recall@k measurement.
 *
 * Layout (same directory):
 *   ceph_vector_bench_config.h   — CLI option struct
 *   ceph_vector_bench_io.{h,cc}  — .fvecs / .ivecs readers
 *   ceph_vector_bench_pg.h       — L2, hash_to_pg, valid_lsh_bits
 *   ceph_vector_bench_usage.h    — --help text
 *   ceph_vector_bench_verify.cc  — --verify-only
 *   ceph_vector_bench_recall.cc  — --recall
 *   ceph_vector_bench_load.cc    — default load path
 *
 * Usage:
 *   ceph-vector-bench -f sift_base.fvecs -p vector_pool [options]
 *   ceph-vector-bench -f sift_base.fvecs --verify-only --pg-num 256 [options]
 *   ceph-vector-bench --recall -p vector_pool -q sift_query.fvecs --gt sift_groundtruth.ivecs --pg-num 128
 */

#include "ceph_vector_bench_config.h"
#include "ceph_vector_bench_load.h"
#include "ceph_vector_bench_pg.h"
#include "ceph_vector_bench_recall.h"
#include "ceph_vector_bench_usage.h"
#include "ceph_vector_bench_verify.h"

#include "common/ceph_argparse.h"
#include "global/global_context.h"
#include "global/global_init.h"

#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, const char** argv)
{
  std::vector<const char*> args(argv, argv + argc);
  if (args.empty()) {
    vector_bench_usage(std::cerr);
    return 1;
  }

  [[maybe_unused]] auto cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
                                          CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  VectorBenchOptions opt;

  std::vector<const char*>::iterator i;
  for (i = args.begin(); i != args.end();) {
    std::string val;
    if (ceph_argparse_double_dash(args, i)) {
      break;
    } else if (ceph_argparse_witharg(args, i, &val, "-f", "--file", (char*)nullptr)) {
      opt.fvecs_file = val;
    } else if (ceph_argparse_witharg(args, i, &val, "-p", "--pool", (char*)nullptr)) {
      opt.pool_name = val;
    } else if (ceph_argparse_witharg(args, i, &val, "-q", "--query", (char*)nullptr)) {
      opt.query_file = val;
    } else if (ceph_argparse_witharg(args, i, &val, "-n", "--num", (char*)nullptr)) {
      opt.max_vectors = strtoull(val.c_str(), nullptr, 10);
    } else if (ceph_argparse_witharg(args, i, &val, "-Q", "--query-num", (char*)nullptr)) {
      opt.max_queries = strtoull(val.c_str(), nullptr, 10);
    } else if (ceph_argparse_witharg(args, i, &val, "-o", "--object-prefix", (char*)nullptr)) {
      opt.obj_prefix = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--pg-num", (char*)nullptr)) {
      opt.pg_num = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.pg_num == 0) opt.pg_num = 256;
    } else if (ceph_argparse_witharg(args, i, &val, "--num-tables", (char*)nullptr)) {
      opt.num_tables = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.num_tables == 0) opt.num_tables = 128;
    } else if (ceph_argparse_witharg(args, i, &val, "--table-set-size", (char*)nullptr)) {
      opt.table_set_size = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
    } else if (ceph_argparse_witharg(args, i, &val, "--write-top-pgs-per-set", (char*)nullptr)) {
      opt.write_top_pgs_per_set = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.write_top_pgs_per_set == 0) opt.write_top_pgs_per_set = 1;
    } else if (ceph_argparse_witharg(args, i, &val, "--write-top-pgs", (char*)nullptr)) {
      opt.write_top_pgs = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
    } else if (ceph_argparse_witharg(args, i, &val, "--pg-map-mode", (char*)nullptr)) {
      opt.pg_map_mode = val;
      if (opt.pg_map_mode != "stable" && opt.pg_map_mode != "mixed")
        opt.pg_map_mode = "stable";
    } else if (ceph_argparse_witharg(args, i, &val, "--table-combine", (char*)nullptr)) {
      opt.table_combine = val;
      if (opt.table_combine != "or" && opt.table_combine != "and")
        opt.table_combine = "or";
    } else if (ceph_argparse_witharg(args, i, &val, "--set-combine", (char*)nullptr)) {
      opt.set_combine = val;
      if (opt.set_combine != "or" && opt.set_combine != "and")
        opt.set_combine = "and";
    } else if (ceph_argparse_witharg(args, i, &val, "--set-replica-mode", (char*)nullptr)) {
      opt.set_replica_mode = val;
      if (opt.set_replica_mode != "none" && opt.set_replica_mode != "paired" &&
          opt.set_replica_mode != "ring")
        opt.set_replica_mode = "paired";
    } else if (ceph_argparse_witharg(args, i, &val, "--replica-set-count", (char*)nullptr)) {
      opt.replica_set_count = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.replica_set_count == 0) opt.replica_set_count = 1;
    } else if (ceph_argparse_witharg(args, i, &val, "--probe-mode", (char*)nullptr)) {
      opt.probe_mode = val;
      if (opt.probe_mode != "union" && opt.probe_mode != "vote") opt.probe_mode = "union";
    } else if (ceph_argparse_witharg(args, i, &val, "--probe-pgs", (char*)nullptr)) {
      opt.probe_pgs = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.probe_pgs == 0) opt.probe_pgs = 1;
    } else if (ceph_argparse_witharg(args, i, &val, "--probe-pgs-per-set", (char*)nullptr)) {
      opt.probe_pgs_per_set = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.probe_pgs_per_set == 0) opt.probe_pgs_per_set = 1;
    } else if (ceph_argparse_witharg(args, i, &val, "--qps-mode", (char*)nullptr)) {
      opt.qps_mode = val;
      if (opt.qps_mode != "memory" && opt.qps_mode != "end_to_end") opt.qps_mode = "memory";
    } else if (ceph_argparse_witharg(args, i, &val, "--warmup-queries", (char*)nullptr)) {
      opt.warmup_queries = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
    } else if (ceph_argparse_flag(args, i, "--latency-report", (char*)nullptr)) {
      opt.latency_report = true;
    } else if (ceph_argparse_witharg(args, i, &val, "--hash-backend", (char*)nullptr)) {
      opt.hash_backend = val;
      if (opt.hash_backend == "orth-rot")
        opt.hash_backend = "orth-sign";
      if (opt.hash_backend == "annoy_tree")
        opt.hash_backend = "annoy";
      if (opt.hash_backend != "lsh" &&
          opt.hash_backend != "orth-sign" &&
          opt.hash_backend != "rotation" &&
          opt.hash_backend != "faiss_rotation" &&
          opt.hash_backend != "rotation_repr1" &&
          opt.hash_backend != "annoy" &&
          opt.hash_backend != "pivot" &&
          opt.hash_backend != "hybrid")
        opt.hash_backend = "lsh";
    } else if (ceph_argparse_witharg(args, i, &val, "--pivot-build-sample", (char*)nullptr)) {
      opt.pivot_build_sample = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.pivot_build_sample == 0) opt.pivot_build_sample = 1;
    } else if (ceph_argparse_witharg(args, i, &val, "--pivot-probe-budget", (char*)nullptr)) {
      opt.pivot_probe_budget = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.pivot_probe_budget == 0) opt.pivot_probe_budget = 1;
    } else if (ceph_argparse_witharg(args, i, &val, "--hybrid-lsh-vote-topk", (char*)nullptr)) {
      opt.hybrid_lsh_vote_topk = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.hybrid_lsh_vote_topk == 0) opt.hybrid_lsh_vote_topk = 1;
    } else if (ceph_argparse_witharg(args, i, &val, "--hybrid-pivot-topk", (char*)nullptr)) {
      opt.hybrid_pivot_topk = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.hybrid_pivot_topk == 0) opt.hybrid_pivot_topk = 1;
    } else if (ceph_argparse_witharg(args, i, &val, "--annoy-n-trees", (char*)nullptr)) {
      opt.annoy_n_trees = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.annoy_n_trees == 0) opt.annoy_n_trees = 1;
    } else if (ceph_argparse_witharg(args, i, &val, "--annoy-search-k", (char*)nullptr)) {
      opt.annoy_search_k = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.annoy_search_k == 0) opt.annoy_search_k = 1;
    } else if (ceph_argparse_witharg(args, i, &val, "--annoy-leaf-size", (char*)nullptr)) {
      opt.annoy_leaf_size = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.annoy_leaf_size == 0) opt.annoy_leaf_size = 1;
    } else if (ceph_argparse_witharg(args, i, &val, "--annoy-build-sample", (char*)nullptr)) {
      opt.annoy_build_sample = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
    } else if (ceph_argparse_witharg(args, i, &val, "--annoy-dist", (char*)nullptr)) {
      opt.annoy_dist = val;
      if (opt.annoy_dist != "l2" && opt.annoy_dist != "angular") opt.annoy_dist = "l2";
    } else if (ceph_argparse_witharg(args, i, &val, "--annoy-seed", (char*)nullptr)) {
      opt.annoy_seed = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
    } else if (ceph_argparse_witharg(args, i, &val, "--rot-seed", (char*)nullptr)) {
      opt.rot_seed = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
    } else if (ceph_argparse_witharg(args, i, &val, "--hash-bits", (char*)nullptr)) {
      opt.hash_bits = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.hash_bits > 32) opt.hash_bits = 32;
    } else if (ceph_argparse_witharg(args, i, &val, "--hash-repeat-rounds", (char*)nullptr)) {
      opt.hash_repeat_rounds = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.hash_repeat_rounds == 0) opt.hash_repeat_rounds = 1;
    } else if (ceph_argparse_witharg(args, i, &val, "--repeat-seed-stride", (char*)nullptr)) {
      opt.repeat_seed_stride = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
    } else if (ceph_argparse_flag(args, i, "--repeat-select-single-pg", (char*)nullptr)) {
      opt.repeat_select_single_pg = true;
    } else if (ceph_argparse_witharg(args, i, &val, "--rotation-use-dims", (char*)nullptr)) {
      opt.rotation_use_dims = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.rotation_use_dims == 0) opt.rotation_use_dims = 1;
    } else if (ceph_argparse_witharg(args, i, &val, "--rotation-bins-per-dim", (char*)nullptr)) {
      opt.rotation_bins_per_dim = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.rotation_bins_per_dim < 2) opt.rotation_bins_per_dim = 2;
    } else if (ceph_argparse_witharg(args, i, &val, "--rotation-min", (char*)nullptr)) {
      opt.rotation_min = strtof(val.c_str(), nullptr);
    } else if (ceph_argparse_witharg(args, i, &val, "--rotation-width", (char*)nullptr)) {
      opt.rotation_width = strtof(val.c_str(), nullptr);
      if (opt.rotation_width <= 0.0f) opt.rotation_width = 1.0f;
    } else if (ceph_argparse_witharg(args, i, &val, "--rotation-neighbor-step", (char*)nullptr)) {
      opt.rotation_neighbor_step = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
    } else if (ceph_argparse_flag(args, i, "--rotation-auto-calibration", (char*)nullptr)) {
      opt.rotation_auto_calibration = true;
    } else if (ceph_argparse_flag(args, i, "--no-rotation-auto-calibration", (char*)nullptr)) {
      opt.rotation_auto_calibration = false;
    } else if (ceph_argparse_witharg(args, i, &val, "--rotation-calibration-samples", (char*)nullptr)) {
      opt.rotation_calibration_samples = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.rotation_calibration_samples == 0) opt.rotation_calibration_samples = 1;
    } else if (ceph_argparse_witharg(args, i, &val, "--rotation-calibration-clip-percentile", (char*)nullptr)) {
      opt.rotation_calibration_clip_percentile = strtof(val.c_str(), nullptr);
      if (opt.rotation_calibration_clip_percentile < 0.0f) opt.rotation_calibration_clip_percentile = 0.0f;
      if (opt.rotation_calibration_clip_percentile >= 0.5f) opt.rotation_calibration_clip_percentile = 0.49f;
    } else if (ceph_argparse_flag(args, i, "--verify-faiss-rotation-match", (char*)nullptr)) {
      opt.verify_faiss_rotation_match = true;
    } else if (ceph_argparse_witharg(args, i, &val, "--gt", (char*)nullptr)) {
      opt.gt_file = val;
    } else if (ceph_argparse_flag(args, i, "--verify-only", (char*)nullptr)) {
      opt.verify_only = true;
    } else if (ceph_argparse_flag(args, i, "--recall", (char*)nullptr)) {
      opt.recall_mode = true;
    } else if (ceph_argparse_flag(args, i, "-C", "--create-pool", (char*)nullptr)) {
      opt.create_pool = true;
    } else if (ceph_argparse_need_usage(args)) {
      vector_bench_usage(std::cout);
      return 0;
    } else {
      ++i;
    }
  }

  if (opt.table_set_size > 0) {
    if (opt.table_set_size > opt.num_tables) {
      std::cerr << "error: --table-set-size must be <= --num-tables\n";
      return 1;
    }
    if (opt.num_tables % opt.table_set_size != 0) {
      std::cerr << "error: --num-tables must be divisible by --table-set-size\n";
      return 1;
    }
    if (opt.set_replica_mode == "paired") {
      const uint32_t set_count = opt.num_tables / opt.table_set_size;
      if ((set_count % 2) != 0) {
        std::cerr << "error: --set-replica-mode=paired requires even set count\n";
        return 1;
      }
    }
  }

  if (opt.hash_backend == "orth-sign" && opt.hash_bits == 0) {
    opt.hash_bits = valid_lsh_bits(opt.pg_num);
  }

  if ((opt.hash_backend == "rotation" ||
       opt.hash_backend == "faiss_rotation" ||
       opt.hash_backend == "rotation_repr1" ||
       opt.hash_backend == "annoy") &&
      opt.hash_repeat_rounds > 1) {
    std::cerr << "warning: hash_repeat_rounds is ignored for rotation/faiss_rotation/rotation_repr1/annoy backend\n";
  }

  if (opt.verify_only)
    return run_vector_bench_verify(opt);

  if (opt.recall_mode)
    return run_vector_bench_recall(opt);

  if (opt.fvecs_file.empty() || opt.pool_name.empty()) {
    std::cerr << "error: -f and -p are required\n";
    vector_bench_usage(std::cerr);
    return 1;
  }

  return run_vector_bench_load(opt);
}
