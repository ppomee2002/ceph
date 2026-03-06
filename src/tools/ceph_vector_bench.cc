// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab
/*
 * Ceph - scalable distributed file system
 *
 * SIFT .fvecs → LSH PG placement → Ceph pool loader/verifier
 *
 * Usage:
 *   ceph-vector-bench -f sift_base.fvecs -p vector_pool [options]
 *   ceph-vector-bench -f sift_base.fvecs --verify-only --pg-num 256 [options]
 *   ceph-vector-bench --recall -p vector_pool -q sift_query.fvecs --gt sift_groundtruth.ivecs --pg-num 128
 *
 * .fvecs: 4B dim + dim*4B floats per vector (ANN standard)
 */

#include "include/rados/librados.hpp"

extern "C" {
#include "crush/hash.h"
}

#include "common/ceph_argparse.h"
#include "common/errno.h"
#include "global/global_context.h"
#include "global/global_init.h"

#include <cmath>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cstring>
#include <iostream>
#include <fstream>
#include <string>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using namespace librados;

/* Load fvecs. [4B dim][dim*4B floats] per vector. Returns new[] - caller frees. */
static float* fvecs_read(const char* fname, size_t* d_out, size_t* n_out) {
  std::ifstream f(fname, std::ios::binary);
  if (!f) {
    std::cerr << "could not open " << fname << std::endl;
    return nullptr;
  }

  int d;
  f.read(reinterpret_cast<char*>(&d), sizeof(int));
  if (d <= 0 || d >= 1000000) {
    std::cerr << "unreasonable dimension: " << d << std::endl;
    return nullptr;
  }

  f.seekg(0, std::ios::end);
  size_t sz = f.tellg();
  f.seekg(0, std::ios::beg);

  size_t vec_bytes = (d + 1) * sizeof(float);
  if (sz % vec_bytes != 0) {
    std::cerr << "weird file size " << sz << " for dim " << d << std::endl;
    return nullptr;
  }
  size_t n = sz / vec_bytes;

  float* x = new float[n * d];
  for (size_t i = 0; i < n; i++) {
    int dummy;
    f.read(reinterpret_cast<char*>(&dummy), sizeof(int));
    f.read(reinterpret_cast<char*>(x + i * d), d * sizeof(float));
  }

  *d_out = d;
  *n_out = n;
  return x;
}

/* Load ivecs (SIFT GT). Row i = k NN indices. Returns new[] - caller frees. */
static int* ivecs_read(const char* fname, size_t* k_out, size_t* n_out) {
  std::ifstream f(fname, std::ios::binary);
  if (!f) {
    std::cerr << "could not open " << fname << std::endl;
    return nullptr;
  }
  int k;
  f.read(reinterpret_cast<char*>(&k), sizeof(int));
  if (k <= 0 || k >= 100000) {
    std::cerr << "unreasonable k: " << k << std::endl;
    return nullptr;
  }
  f.seekg(0, std::ios::end);
  size_t sz = f.tellg();
  f.seekg(0, std::ios::beg);
  size_t row_bytes = (k + 1) * sizeof(int);
  if (sz % row_bytes != 0) {
    std::cerr << "weird ivecs size " << sz << std::endl;
    return nullptr;
  }
  size_t n = sz / row_bytes;
  int* gt = new int[n * k];
  for (size_t i = 0; i < n; i++) {
    int dummy;
    f.read(reinterpret_cast<char*>(&dummy), sizeof(int));
    f.read(reinterpret_cast<char*>(gt + i * k), k * sizeof(int));
  }
  *k_out = k;
  *n_out = n;
  return gt;
}

/* L2^2 - skip sqrt for ordering */
static float l2_dist_sq(const float* a, const float* b, int dim) {
  float sum = 0;
  for (int i = 0; i < dim; i++) {
    float d = a[i] - b[i];
    sum += d * d;
  }
  return sum;
}

static float l2_dist(const float* a, const float* b, int dim) {
  return sqrtf(l2_dist_sq(a, b, dim));
}

/* Ceph stable_mod - matches raw_hash_to_pg() */
static inline unsigned cbits32(uint32_t v) {
  if (v == 0) return 0;
  return 32 - __builtin_clz(static_cast<unsigned>(v));
}
static inline uint32_t hash_to_pg(uint32_t h, uint32_t pg_num) {
  if (pg_num <= 0) return 0;
  uint32_t pg_num_mask = (1u << cbits32(pg_num - 1)) - 1;
  int x = static_cast<int>(h);
  int b = static_cast<int>(pg_num);
  int bmask = static_cast<int>(pg_num_mask);
  if (static_cast<unsigned>(x & bmask) < static_cast<unsigned>(b))
    return static_cast<uint32_t>(x & bmask);
  return static_cast<uint32_t>(x & (bmask >> 1));
}

static void usage(std::ostream& out) {
  out << "usage: ceph-vector-bench -f <fvecs_file> -p <pool> [options]\n"
      << "       ceph-vector-bench -f <fvecs_file> --verify-only [options]\n"
      << "       ceph-vector-bench --recall -f <base.fvecs> -p <pool> -q <query.fvecs> --gt <gt.ivecs> [options]\n"
      << "\n"
      << "Load: Load SIFT/fvecs into Ceph with LSH-based PG placement.\n"
      << "Verify: Measure if similar vectors land in same PG (no Ceph needed).\n"
      << "Recall: LSH Recall vs Ground Truth (single/multi-probe, requires Ceph + loaded data).\n"
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
      << "  --probe <M>          max PGs to probe per query (default: 1, multi-probe LSH)\n"
      << "  --pg-num <N>         PG count (default: 256)\n"
      << "  --gt <ivecs>         ground truth (sift_groundtruth.ivecs)\n"
      << "  -n, --num <N>        limit base vectors (0=all)\n"
      << "  -Q, --query-num <N>  limit queries for recall (0=all)\n"
      << "  -o, --object-prefix  object prefix (default: vec_)\n"
      << "  -C, --create-pool    create pool (load)\n";
}

int main(int argc, const char **argv)
{
  std::vector<const char*> args(argv, argv + argc);
  if (args.empty()) {
    usage(std::cerr);
    return 1;
  }

  auto cct = global_init(nullptr, args, CEPH_ENTITY_TYPE_CLIENT,
			 CODE_ENVIRONMENT_UTILITY, 0);
  common_init_finish(g_ceph_context);

  std::string fvecs_file;
  std::string pool_name;
  std::string query_file;
  size_t max_vectors = 0;
  size_t max_queries = 0;
  std::string obj_prefix = "vec_";
  bool create_pool = false;
  bool verify_only = false;
  bool recall_mode = false;
  uint32_t pg_num = 256;
  uint32_t max_probe = 1;
  std::string gt_file;

  std::vector<const char*>::iterator i;
  for (i = args.begin(); i != args.end(); ) {
    std::string val;
    if (ceph_argparse_double_dash(args, i)) {
      break;
    } else if (ceph_argparse_witharg(args, i, &val, "-f", "--file", (char*)nullptr)) {
      fvecs_file = val;
    } else if (ceph_argparse_witharg(args, i, &val, "-p", "--pool", (char*)nullptr)) {
      pool_name = val;
    } else if (ceph_argparse_witharg(args, i, &val, "-q", "--query", (char*)nullptr)) {
      query_file = val;
    } else if (ceph_argparse_witharg(args, i, &val, "-n", "--num", (char*)nullptr)) {
      max_vectors = strtoull(val.c_str(), nullptr, 10);
    } else if (ceph_argparse_witharg(args, i, &val, "-Q", "--query-num", (char*)nullptr)) {
      max_queries = strtoull(val.c_str(), nullptr, 10);
    } else if (ceph_argparse_witharg(args, i, &val, "-o", "--object-prefix", (char*)nullptr)) {
      obj_prefix = val;
    } else if (ceph_argparse_witharg(args, i, &val, "--pg-num", (char*)nullptr)) {
      pg_num = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (pg_num == 0) pg_num = 256;
    } else if (ceph_argparse_witharg(args, i, &val, "--probe", (char*)nullptr)) {
      max_probe = static_cast<uint32_t>(strtoul(val.c_str(), nullptr, 10));
      if (max_probe == 0) max_probe = 1;
    } else if (ceph_argparse_witharg(args, i, &val, "--gt", (char*)nullptr)) {
      gt_file = val;
    } else if (ceph_argparse_flag(args, i, "--verify-only", (char*)nullptr)) {
      verify_only = true;
    } else if (ceph_argparse_flag(args, i, "--recall", (char*)nullptr)) {
      recall_mode = true;
    } else if (ceph_argparse_flag(args, i, "-C", "--create-pool", (char*)nullptr)) {
      create_pool = true;
    } else if (ceph_argparse_need_usage(args)) {
      usage(std::cout);
      return 0;
    } else {
      ++i;
    }
  }

  /* verify-only: LSH locality check (no Ceph) */
  if (verify_only) {
    srand(static_cast<unsigned>(time(nullptr)));
    if (fvecs_file.empty()) {
      std::cerr << "error: -f required for --verify-only\n";
      return 1;
    }
    size_t d, n;
    float* vectors = fvecs_read(fvecs_file.c_str(), &d, &n);
    if (!vectors) return 1;
    if (max_vectors > 0 && n > max_vectors) n = max_vectors;

    std::vector<uint32_t> lsh(n), pg(n);
    for (size_t i = 0; i < n; i++) {
      lsh[i] = crush_hash32_lsh(vectors + i * d, d);
      pg[i] = hash_to_pg(lsh[i], pg_num);
    }

    /* intra-PG vs inter-PG avg L2 distance */
    const size_t intra_samples = 50000;
    const size_t inter_samples = 50000;
    std::unordered_map<uint32_t, std::vector<size_t>> pg_to_idx;
    for (size_t i = 0; i < n; i++)
      pg_to_idx[pg[i]].push_back(i);

    std::vector<std::vector<size_t>*> pg_groups;
    for (auto& kv : pg_to_idx)
      pg_groups.push_back(&kv.second);

    double intra_sum = 0;
    size_t intra_count = 0;
    for (size_t s = 0; s < intra_samples && intra_count < intra_samples; s++) {
      size_t g = s % pg_groups.size();
      const std::vector<size_t>& inds = *pg_groups[g];
      if (inds.size() < 2) continue;
      size_t i = inds[rand() % inds.size()];
      size_t j = inds[rand() % inds.size()];
      if (i == j) continue;
      intra_sum += l2_dist(vectors + i * d, vectors + j * d, d);
      intra_count++;
    }
    double intra_avg = (intra_count > 0) ? (intra_sum / intra_count) : 0;

    double inter_sum = 0;
    size_t inter_count = 0;
    for (size_t s = 0; s < inter_samples; s++) {
      size_t g1 = rand() % pg_groups.size();
      size_t g2 = rand() % pg_groups.size();
      if (g1 == g2) continue;
      const std::vector<size_t>& a1 = *pg_groups[g1];
      const std::vector<size_t>& a2 = *pg_groups[g2];
      if (a1.empty() || a2.empty()) continue;
      size_t i = a1[rand() % a1.size()];
      size_t j = a2[rand() % a2.size()];
      inter_sum += l2_dist(vectors + i * d, vectors + j * d, d);
      inter_count++;
    }
    double inter_avg = (inter_count > 0) ? (inter_sum / inter_count) : 0;

    std::cout << "=== LSH Locality Verification (n=" << n << ", dim=" << d
              << ", pg_num=" << pg_num << ") ===\n";
    std::cout << "  intra-PG avg L2 distance: " << intra_avg << "\n";
    std::cout << "  inter-PG avg L2 distance: " << inter_avg << "\n";
    std::cout << "  ratio (inter/intra):      " << (intra_avg > 0 ? inter_avg / intra_avg : 0)
              << "  (expect > 1 if LSH groups similar vectors)\n";

    /* NN same-PG pair rate: GT k-NN pairs that land in same PG */
    if (!gt_file.empty()) {
      size_t gt_k, gt_n;
      int* gt = ivecs_read(gt_file.c_str(), &gt_k, &gt_n);
      if (gt) {
        /* gt indices → base vectors (0..n-1) */
        size_t same_pairs = 0, total_pairs = 0;
        for (size_t q = 0; q < gt_n; q++) {
          for (size_t i = 0; i < gt_k; i++) {
            int ii = gt[q * gt_k + i];
            if (ii < 0 || (size_t)ii >= n) continue;
            for (size_t j = i + 1; j < gt_k; j++) {
              int jj = gt[q * gt_k + j];
              if (jj < 0 || (size_t)jj >= n) continue;
              total_pairs++;
              if (pg[ii] == pg[jj]) same_pairs++;
            }
          }
        }
        double recall = (total_pairs > 0) ? (100.0 * same_pairs / total_pairs) : 0;
        std::cout << "  NN same-PG pair rate (k=" << gt_k << ", " << gt_n << " queries): "
                  << recall << "% (" << same_pairs << "/" << total_pairs
                  << " NN pairs in same PG)\n";
        double baseline_pct = 100.0 / pg_num;
        std::cout << "  random baseline: ~" << baseline_pct << "% (LSH should be >> this)\n";
        delete[] gt;
      }
    }

    delete[] vectors;
    return 0;
  }

  /* recall: pool scan → single-PG Recall@k vs GT */
  if (recall_mode) {
    if (pool_name.empty() || query_file.empty() || gt_file.empty() || fvecs_file.empty()) {
      std::cerr << "error: --recall requires -p, -q (--query), --gt, -f (base .fvecs)\n";
      return 1;
    }
    size_t qd, qn;
    float* queries = fvecs_read(query_file.c_str(), &qd, &qn);
    if (!queries) return 1;
    if (max_queries > 0 && qn > max_queries) qn = max_queries;

    size_t gt_k, gt_n;
    int* gt = ivecs_read(gt_file.c_str(), &gt_k, &gt_n);
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
    float* base_vectors = fvecs_read(fvecs_file.c_str(), &bd, &bn);
    if (!base_vectors) {
      delete[] queries;
      delete[] gt;
      return 1;
    }
    if (max_vectors > 0 && bn > max_vectors)
      bn = max_vectors;
    if (qd != bd) {
      std::cerr << "error: query dim (" << qd << ") != base dim (" << bd << ")\n";
      delete[] base_vectors;
      delete[] queries;
      delete[] gt;
      return 1;
    }

    Rados rados;
    int     ret = rados.init_with_context(g_ceph_context);
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
    ret = rados.ioctx_create(pool_name.c_str(), ioctx);
    if (ret < 0) {
      std::cerr << "error: cannot open pool '" << pool_name << "' (ret=" << ret << ")\n";
      delete[] base_vectors;
      delete[] queries;
      delete[] gt;
      return 1;
    }
    /* sanity: compare vec_0 from Ceph vs base file */
    {
      char first_oid[256];
      snprintf(first_oid, sizeof(first_oid), "%s%08zu", obj_prefix.c_str(), (size_t)0);
      __u32 lsh0 = crush_hash32_lsh(base_vectors, bd);
      IoCtx verify_ctx;
      if (rados.ioctx_create(pool_name.c_str(), verify_ctx) == 0) {
        verify_ctx.locator_set_hash(static_cast<int64_t>(lsh0));
        librados::bufferlist vbl;
        ret = verify_ctx.read(first_oid, vbl, 0, 0);
        if (ret > 0 && (size_t)ret >= 5 * sizeof(float)) {
          const float* ceph_vec = reinterpret_cast<const float*>(vbl.c_str());
          std::cout << "[Ceph read] vec_00000000 first 5 floats: "
                    << ceph_vec[0] << ", " << ceph_vec[1] << ", " << ceph_vec[2]
                    << ", " << ceph_vec[3] << ", " << ceph_vec[4] << std::endl;
          std::cout << "[base file] first 5 floats: "
                    << base_vectors[0] << ", " << base_vectors[1] << ", " << base_vectors[2]
                    << ", " << base_vectors[3] << ", " << base_vectors[4] << std::endl;
        } else {
          std::cout << "[Ceph read] vec_00000000 read failed or too short (ret=" << ret << ")" << std::endl;
        }
      }
    }

    /* full pool scan → pg_vecs[pg] = (idx, vec). Use base_vectors in mem, not Ceph read. */
    std::unordered_map<uint32_t, std::vector<std::pair<size_t, std::vector<float>>>> pg_vecs;
    ioctx.set_namespace(all_nspaces);
    std::cout << "scanning pool '" << pool_name << "' (id=" << ioctx.get_id() << ") ..."
              << std::endl;
    size_t scan_count = 0;
    try {
      for (auto it = ioctx.nobjects_begin(); it != ioctx.nobjects_end(); ++it) {
        std::string oid = it->get_oid();

        /* parse OID: vec_00039462 -> idx */
        size_t idx = 0;
        if (oid.size() > obj_prefix.size()) {
          const char* p = oid.c_str() + obj_prefix.size();
          idx = strtoull(p, nullptr, 10);
        }
        if (idx >= bn)
          continue;

        /* recompute LSH hash from base_vectors (get_pg_hash_position uses bit-reversed cursor) */
        __u32 real_lsh_hash = crush_hash32_lsh(base_vectors + idx * bd, bd);
        uint32_t pg = hash_to_pg(real_lsh_hash, pg_num);

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

    /* Recall@k, multi-probe */
    double recall_sum = 0;
    size_t valid_queries = 0;
    size_t probe_pg_sum = 0;
    size_t cand_sum = 0;
    std::vector<std::pair<float, size_t>> dist_idx;

    for (size_t q = 0; q < qn; q++) {
      uint32_t lsh_hash = crush_hash32_lsh(queries + q * qd, qd);

      /* PGs: orig hash + 1-bit flips, dedup, cap at max_probe */
      std::unordered_set<uint32_t> target_pgs;
      target_pgs.insert(hash_to_pg(lsh_hash, pg_num));
      for (int i = 0; i < 32 && target_pgs.size() < max_probe; i++)
        target_pgs.insert(hash_to_pg(lsh_hash ^ (1u << i), pg_num));

      /* gather cands from target PGs */
      std::vector<std::pair<size_t, const float*>> all_cands;
      for (uint32_t pg : target_pgs) {
        auto pit = pg_vecs.find(pg);
        if (pit != pg_vecs.end())
          for (const auto& c : pit->second)
            all_cands.emplace_back(c.first, c.second.data());
      }
      if (all_cands.empty())
        continue;

      probe_pg_sum += target_pgs.size();
      cand_sum += all_cands.size();

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

    double avg_recall = (valid_queries > 0) ? (100.0 * recall_sum / valid_queries) : 0;
    double avg_probe_pgs = (valid_queries > 0) ? (double)probe_pg_sum / valid_queries : 0;
    double avg_cands = (valid_queries > 0) ? (double)cand_sum / valid_queries : 0;
    std::cout << "\n=== Recall (pg_num=" << pg_num << ", k=" << gt_k
              << ", probe=" << max_probe << ", queries=" << valid_queries << "/" << qn << ") ===\n";
    std::cout << "  Average Recall@" << gt_k << ": " << avg_recall << "%\n";
    std::cout << "  Avg PGs probed: " << avg_probe_pgs << "\n";
    std::cout << "  Avg candidates: " << avg_cands << "\n";

    delete[] base_vectors;
    delete[] queries;
    delete[] gt;
    return 0;
  }

  if (fvecs_file.empty() || pool_name.empty()) {
    std::cerr << "error: -f and -p are required\n";
    usage(std::cerr);
    return 1;
  }

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

  if (create_pool) {
    ret = rados.pool_create(pool_name.c_str());
    if (ret < 0 && ret != -EEXIST) {
      std::cerr << "pool create failed: " << cpp_strerror(ret) << std::endl;
      return 1;
    }
  }

  IoCtx ioctx;
  ret = rados.ioctx_create(pool_name.c_str(), ioctx);
  if (ret < 0) {
    std::cerr << "cannot open pool " << pool_name << ": " << cpp_strerror(ret) << std::endl;
    return 1;
  }

  size_t d, n;
  float* vectors = fvecs_read(fvecs_file.c_str(), &d, &n);
  if (!vectors) {
    return 1;
  }

  if (max_vectors > 0 && n > max_vectors) {
    n = max_vectors;
  }

  const size_t concurrency = 128;
  std::vector<librados::bufferlist> bl_pool(concurrency);
  std::vector<librados::AioCompletion*> comp_pool(concurrency);

  std::cout << "loading " << n << " vectors (dim=" << d << ") from " << fvecs_file
            << " -> pool " << pool_name << " (concurrency=" << concurrency << ")"
            << std::endl;

  auto t0 = std::chrono::steady_clock::now();
  size_t written = 0;
  size_t report_interval = std::max<size_t>(1, n / 20);
  size_t last_report = 0;

  for (size_t offset = 0; offset < n; offset += concurrency) {
    size_t batch = std::min(concurrency, n - offset);
    for (size_t k = 0; k < batch; k++)
      comp_pool[k] = rados.aio_create_completion();
    for (size_t j = 0; j < batch; j++) {
      size_t i = offset + j;
      float* vec = vectors + i * d;
      __u32 lsh_hash = crush_hash32_lsh(vec, d);

      ioctx.locator_set_hash(static_cast<int64_t>(lsh_hash));

      char oid[256];
      snprintf(oid, sizeof(oid), "%s%08zu", obj_prefix.c_str(), i);

      bl_pool[j].clear();
      bl_pool[j].append(reinterpret_cast<const char*>(vec), d * sizeof(float));

      ret = ioctx.aio_write_full(oid, comp_pool[j], bl_pool[j]);
      if (ret < 0) {
        std::cerr << "aio_write_full failed at vec " << i << ": " << cpp_strerror(ret) << std::endl;
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
        std::cerr << "write failed at vec " << (offset + j) << ": " << cpp_strerror(ret) << std::endl;
        for (size_t k = j + 1; k < batch; k++)
          comp_pool[k]->release();
        delete[] vectors;
        return 1;
      }
      written++;
    }
    ioctx.locator_set_hash(-1);

    size_t done = offset + batch;
    if (done - last_report >= report_interval || done == n) {
      last_report = done;
      auto t1 = std::chrono::steady_clock::now();
      double sec = std::chrono::duration<double>(t1 - t0).count();
      double rate = done / sec;
      std::cout << "  " << done << "/" << n << " (" << (100 * done / n) << "%) "
                << rate << " vec/s" << std::endl;
    }
  }

  delete[] vectors;

  auto t1 = std::chrono::steady_clock::now();
  double sec = std::chrono::duration<double>(t1 - t0).count();
  std::cout << "done: " << written << " vectors in " << sec << "s ("
            << (written / sec) << " vec/s)" << std::endl;

  return 0;
}
