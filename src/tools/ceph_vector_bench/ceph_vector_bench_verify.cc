// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-

#include "ceph_vector_bench_verify.h"
#include "ceph_vector_bench_config.h"
#include "ceph_vector_bench_io.h"
#include "ceph_vector_bench_pg.h"

extern "C" {
#include "crush/hash.h"
}

#include <cstdlib>
#include <ctime>
#include <iostream>
#include <unordered_map>
#include <vector>

int run_vector_bench_verify(const VectorBenchOptions& opt) {
  srand(static_cast<unsigned>(time(nullptr)));
  if (opt.fvecs_file.empty()) {
    std::cerr << "error: -f required for --verify-only\n";
    return 1;
  }
  size_t d, n;
  float* vectors = fvecs_read(opt.fvecs_file.c_str(), &d, &n);
  if (!vectors) return 1;
  if (opt.max_vectors > 0 && n > opt.max_vectors) n = opt.max_vectors;

  unsigned vbits = valid_lsh_bits(opt.pg_num);
  std::vector<uint32_t> pg(n);
  for (size_t i = 0; i < n; i++) {
    uint32_t lsh_h = crush_hash32_lsh_n(vectors + i * d, static_cast<int>(d), vbits);
    pg[i] = map_hash_to_pg(lsh_h, opt.pg_num, opt.pg_map_mode);
  }

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
    intra_sum += l2_dist(vectors + i * d, vectors + j * d, static_cast<int>(d));
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
    inter_sum += l2_dist(vectors + i * d, vectors + j * d, static_cast<int>(d));
    inter_count++;
  }
  double inter_avg = (inter_count > 0) ? (inter_sum / inter_count) : 0;

  std::cout << "=== LSH Locality Verification (n=" << n << ", dim=" << d
            << ", pg_num=" << opt.pg_num << ", lsh_bits=" << vbits << ") ===\n";
  std::cout << "  PG map mode: " << opt.pg_map_mode << "\n";
  std::cout << "  intra-PG avg L2 distance: " << intra_avg << "\n";
  std::cout << "  inter-PG avg L2 distance: " << inter_avg << "\n";
  std::cout << "  ratio (inter/intra):      " << (intra_avg > 0 ? inter_avg / intra_avg : 0)
            << "  (expect > 1 if LSH groups similar vectors)\n";

  if (!opt.gt_file.empty()) {
    size_t gt_k, gt_n;
    int* gt = ivecs_read(opt.gt_file.c_str(), &gt_k, &gt_n);
    if (gt) {
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
      double baseline_pct = 100.0 / opt.pg_num;
      std::cout << "  random baseline: ~" << baseline_pct << "% (LSH should be >> this)\n";
      delete[] gt;
    }
  }

  delete[] vectors;
  return 0;
}
