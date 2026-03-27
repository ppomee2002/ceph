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
    } else if (ceph_argparse_witharg(args, i, &val, "--write-top-pgs", (char*)nullptr)) {
      opt.write_top_pgs = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
    } else if (ceph_argparse_witharg(args, i, &val, "--probe-mode", (char*)nullptr)) {
      opt.probe_mode = val;
      if (opt.probe_mode != "union" && opt.probe_mode != "vote") opt.probe_mode = "union";
    } else if (ceph_argparse_witharg(args, i, &val, "--probe-pgs", (char*)nullptr)) {
      opt.probe_pgs = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (opt.probe_pgs == 0) opt.probe_pgs = 1;
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
