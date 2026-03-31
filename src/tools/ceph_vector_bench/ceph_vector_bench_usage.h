// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
#pragma once

#include <iostream>

inline void vector_bench_usage(std::ostream& out) {
  out << "usage: ceph-vector-bench -f <fvecs_file> -p <pool> [options]\n"
      << "       ceph-vector-bench -f <fvecs_file> --verify-only [options]\n"
      << "       ceph-vector-bench --recall -f <base.fvecs> -p <pool> -q <query.fvecs> --gt <gt.ivecs> [options]\n"
      << "\n"
      << "Load: Load SIFT/fvecs into Ceph with LSH-based PG placement.\n"
      << "Verify: Measure if similar vectors land in same PG (no Ceph needed).\n"
      << "Recall: LSH Recall vs Ground Truth (multi-table Fan-out, requires Ceph + loaded data).\n"
      << "\n"
      << "required (load): -f, -p\n"
      << "required (verify): -f, --verify-only\n"
      << "required (recall): -p, -q, --gt, --pg-num, -f (base .fvecs for local lookup)\n"
      << "\n"
      << "  -f, --file <file>     .fvecs input (e.g. sift_base.fvecs)\n"
      << "  -p, --pool <pool>     target pool (load/recall)\n"
      << "  -q, --query <file>    query .fvecs for recall (e.g. sift_query.fvecs)\n"
      << "  --verify-only        verify LSH locality, no Ceph connection\n"
      << "  --recall             measure single-PG Recall vs ground truth\n"
      << "  --num-tables <N>     LSH tables for Fan-out (default: 128). Load/Recall.\n"
      << "  --write-top-pgs <N>  load: per-vector PG votes top-N only (0=all unique PGs)\n"
      << "  --pg-map-mode <m>    PG mapping mode: stable|mixed (default: stable)\n"
      << "  --table-combine <m>  multi-table combine: or|and (default: or)\n"
      << "  --pg-num <N>         PG count, sets LSH bits (default: 256). Load: match pool.\n"
      << "  --gt <ivecs>         ground truth (sift_groundtruth.ivecs)\n"
      << "  -n, --num <N>        limit base vectors (0=all)\n"
      << "  -Q, --query-num <N>  limit queries for recall (0=all)\n"
      << "  -o, --object-prefix  object prefix (default: vec_)\n"
      << "  -C, --create-pool    create pool (load)\n"
      << "  --probe-mode <m>     recall: union|vote (default: union)\n"
      << "  --probe-pgs <N>      recall: top-N PGs in vote mode (default: 1)\n";
}
