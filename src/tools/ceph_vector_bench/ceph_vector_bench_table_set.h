// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
#pragma once

#include "ceph_vector_bench_config.h"
#include "ceph_vector_bench_pg.h"

extern "C" {
#include "crush/hash.h"
}

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

struct SetCandidate {
  uint32_t pg_id = 0;
  size_t votes = 0;
  int64_t hash = 0;
};

struct TablePgCandidate {
  uint32_t pg_id = 0;
  int64_t hash = 0;
};

struct RankedPgCandidate {
  uint32_t pg_id = 0;
  int64_t hash = 0;
  float score = 0.0f;
};

struct SetVote {
  uint32_t set_id = 0;
  uint32_t table_begin = 0;
  uint32_t table_end = 0;
  std::vector<SetCandidate> ranked;
};

inline uint32_t get_table_set_size(const VectorBenchOptions& opt) {
  return (opt.table_set_size > 0) ? opt.table_set_size : std::max<uint32_t>(1, opt.num_tables);
}

inline uint32_t get_table_set_count(const VectorBenchOptions& opt) {
  const uint32_t set_size = get_table_set_size(opt);
  return std::max<uint32_t>(1, opt.num_tables / set_size);
}

inline bool is_orth_sign_backend(const VectorBenchOptions& opt) {
  return opt.hash_backend == "orth-sign" || opt.hash_backend == "orth-rot";
}

inline bool is_rotation_backend(const VectorBenchOptions& opt) {
  return opt.hash_backend == "rotation" || opt.hash_backend == "faiss_rotation";
}

inline bool is_faiss_rotation_backend(const VectorBenchOptions& opt) {
  return opt.hash_backend == "faiss_rotation";
}

inline bool is_rotation_repr1_backend(const VectorBenchOptions& opt) {
  return opt.hash_backend == "rotation_repr1";
}

inline bool is_pivot_backend(const VectorBenchOptions& opt) {
  return opt.hash_backend == "pivot";
}

inline bool is_hybrid_backend(const VectorBenchOptions& opt) {
  return opt.hash_backend == "hybrid";
}

inline bool is_global_routing_backend(const VectorBenchOptions& opt) {
  return is_pivot_backend(opt) || is_hybrid_backend(opt);
}

inline uint32_t effective_hash_repeat_rounds(const VectorBenchOptions& opt) {
  return is_orth_sign_backend(opt) ? std::max<uint32_t>(1, opt.hash_repeat_rounds) : 1u;
}

struct RotationQuantizationParams {
  int use_dims = 1;
  int bins_per_dim = 2;
  float min_val = -2.5f;
  float width = 1.25f;
  bool calibrated = false;
  size_t sample_count = 0;
};

inline RotationQuantizationParams rotation_params_from_opt(const VectorBenchOptions& opt, int dim) {
  RotationQuantizationParams p;
  p.use_dims = std::max<int>(
    1, std::min<int>(std::min<int>(static_cast<int>(opt.rotation_use_dims), dim), 16));
  p.bins_per_dim = std::max<int>(2, static_cast<int>(opt.rotation_bins_per_dim));
  p.min_val = opt.rotation_min;
  p.width = (opt.rotation_width > 0.0f) ? opt.rotation_width : 1.0f;
  return p;
}

struct RotationReprEntry {
  uint32_t pg_id = 0;
  std::vector<float> rotated;
};

struct RotationReprModel {
  int dim = 0;
  std::vector<RotationReprEntry> reps;

  size_t representative_count() const {
    return reps.size();
  }
};

struct PivotReprEntry {
  uint32_t pg_id = 0;
  std::vector<float> vec;
};

struct PivotModel {
  int dim = 0;
  size_t sample_count = 0;
  std::vector<PivotReprEntry> reps;

  size_t representative_count() const {
    return reps.size();
  }
};

struct HybridGroupReprEntry {
  uint32_t pg_id = 0;
  std::vector<float> vec;
};

struct HybridGroupModel {
  int dim = 0;
  size_t sample_count = 0;
  std::unordered_map<uint32_t, HybridGroupReprEntry> reps;

  size_t representative_count() const {
    return reps.size();
  }
};

inline __u32 table_hash_for_vec(const float* vec, int dim, uint32_t table_id,
                                const VectorBenchOptions& opt,
                                uint32_t repeat_idx);

inline int rotation_apply_for_backend(const float* vec, int dim, uint32_t table_id,
                                      const VectorBenchOptions& opt, float* out_rotated) {
  if (is_faiss_rotation_backend(opt)) {
    return crush_hash32_faiss_rotation_apply(
      vec, dim, static_cast<int>(table_id), opt.rot_seed, out_rotated, dim);
  }
  return crush_hash32_rotation_apply(
    vec, dim, static_cast<int>(table_id), opt.rot_seed, out_rotated, dim);
}

inline RotationQuantizationParams calibrate_rotation_quant_params(const float* sample_vectors,
                                                                  size_t sample_vector_count,
                                                                  int dim,
                                                                  const VectorBenchOptions& opt) {
  RotationQuantizationParams p = rotation_params_from_opt(opt, dim);
  if (!is_rotation_backend(opt) || !opt.rotation_auto_calibration) return p;
  if (!sample_vectors || sample_vector_count == 0 || dim <= 0) return p;

  const size_t target_samples = std::max<size_t>(1, opt.rotation_calibration_samples);
  const size_t stride = std::max<size_t>(1, sample_vector_count / target_samples);
  const uint32_t table_samples = std::max<uint32_t>(
    1u, std::min<uint32_t>(opt.num_tables, 8u));
  std::vector<float> rotated(static_cast<size_t>(dim));
  std::vector<float> values;
  values.reserve(
    target_samples * static_cast<size_t>(table_samples) * static_cast<size_t>(p.use_dims));

  size_t seen = 0;
  for (size_t i = 0; i < sample_vector_count && seen < target_samples; i += stride, ++seen) {
    const float* vec = sample_vectors + i * static_cast<size_t>(dim);
    for (uint32_t ts = 0; ts < table_samples; ++ts) {
      const uint32_t table_id = (opt.num_tables <= 1)
        ? 0u
        : static_cast<uint32_t>((static_cast<uint64_t>(ts) * (opt.num_tables - 1)) /
                                 (table_samples - 1));
      if (rotation_apply_for_backend(vec, dim, table_id, opt, rotated.data()) <= 0) continue;
      for (int d = 0; d < p.use_dims; ++d) values.push_back(rotated[d]);
    }
  }

  if (values.size() < 2) return p;
  std::sort(values.begin(), values.end());
  p.sample_count = seen;

  float lo = values.front();
  float hi = values.back();
  const float clip = opt.rotation_calibration_clip_percentile;
  if (clip > 0.0f && clip < 0.5f) {
    const size_t n = values.size();
    const size_t lo_idx = std::min(n - 1, static_cast<size_t>(clip * static_cast<float>(n)));
    const size_t hi_idx = std::max<size_t>(
      lo_idx + 1, static_cast<size_t>((1.0f - clip) * static_cast<float>(n)) - 1);
    lo = values[lo_idx];
    hi = values[std::min(n - 1, hi_idx)];
  }

  if (!(hi > lo)) return p;
  const float width = (hi - lo) / static_cast<float>(p.bins_per_dim);
  if (width <= std::numeric_limits<float>::epsilon()) return p;

  p.min_val = lo;
  p.width = width;
  p.calibrated = true;
  return p;
}

inline RotationReprModel build_rotation_repr1_model(const float* vectors, size_t vector_count,
                                                    int dim, const VectorBenchOptions& opt) {
  RotationReprModel model;
  model.dim = dim;
  if (!is_rotation_repr1_backend(opt) || !vectors || vector_count == 0 || dim <= 0 || opt.pg_num == 0) {
    return model;
  }

  std::unordered_set<uint32_t> have_pg;
  std::vector<float> rotated(static_cast<size_t>(dim));
  model.reps.reserve(std::min<size_t>(vector_count, opt.pg_num));

  for (size_t i = 0; i < vector_count; ++i) {
    const float* vec = vectors + i * static_cast<size_t>(dim);
    if (rotation_apply_for_backend(vec, dim, 0, opt, rotated.data()) <= 0) {
      continue;
    }
    const uint32_t pg = crush_hash32_rotation_seed_pg(rotated.data(), dim, opt.pg_num);
    if (!have_pg.insert(pg).second) {
      continue;
    }
    model.reps.push_back({pg, rotated});
    if (model.reps.size() >= opt.pg_num) {
      break;
    }
  }
  return model;
}

inline PivotModel build_pivot_model(const float* vectors, size_t vector_count,
                                    int dim, const VectorBenchOptions& opt) {
  PivotModel model;
  model.dim = dim;
  if (!is_global_routing_backend(opt) || !vectors || vector_count == 0 || dim <= 0 || opt.pg_num == 0) {
    return model;
  }

  const size_t sample_limit = std::min<size_t>(
    vector_count, std::max<size_t>(1, static_cast<size_t>(opt.pivot_build_sample)));
  const size_t stride = std::max<size_t>(1, vector_count / sample_limit);
  std::vector<std::vector<float>> samples;
  samples.reserve(sample_limit);
  for (size_t i = 0; i < vector_count && samples.size() < sample_limit; i += stride) {
    const float* vec = vectors + i * static_cast<size_t>(dim);
    samples.emplace_back(vec, vec + dim);
  }
  if (samples.empty()) {
    return model;
  }

  model.sample_count = samples.size();
  const size_t pivot_count = std::min<size_t>(static_cast<size_t>(opt.pg_num), samples.size());
  model.reps.reserve(pivot_count);

  std::vector<float> min_dist(samples.size(), std::numeric_limits<float>::max());
  model.reps.push_back({0u, samples[0]});

  for (size_t i = 0; i < samples.size(); ++i) {
    min_dist[i] = l2_dist_sq(samples[i].data(), samples[0].data(), dim);
  }
  min_dist[0] = 0.0f;

  while (model.reps.size() < pivot_count) {
    size_t best_idx = 0;
    float best_dist = -1.0f;
    for (size_t i = 0; i < samples.size(); ++i) {
      if (min_dist[i] > best_dist) {
        best_dist = min_dist[i];
        best_idx = i;
      }
    }
    if (best_dist <= 0.0f) {
      break;
    }
    model.reps.push_back({static_cast<uint32_t>(model.reps.size()), samples[best_idx]});
    min_dist[best_idx] = 0.0f;
    for (size_t i = 0; i < samples.size(); ++i) {
      const float d2 = l2_dist_sq(samples[i].data(), samples[best_idx].data(), dim);
      if (d2 < min_dist[i]) {
        min_dist[i] = d2;
      }
    }
  }

  return model;
}

inline std::vector<TablePgCandidate> rotation_repr1_candidates_for_vec(
  const float* vec, int dim, const VectorBenchOptions& opt, const RotationReprModel& model) {
  std::vector<TablePgCandidate> out;
  if (!is_rotation_repr1_backend(opt) || !vec || dim <= 0 || model.reps.empty()) {
    return out;
  }

  std::vector<float> rotated(static_cast<size_t>(dim));
  if (rotation_apply_for_backend(vec, dim, 0, opt, rotated.data()) <= 0) {
    return out;
  }

  struct DistPg {
    float dist = 0.0f;
    uint32_t pg_id = 0;
  };

  std::vector<DistPg> ranked;
  ranked.reserve(model.reps.size());
  for (const auto& rep : model.reps) {
    ranked.push_back({l2_dist_sq(rotated.data(), rep.rotated.data(), dim), rep.pg_id});
  }

  std::sort(ranked.begin(), ranked.end(), [](const DistPg& a, const DistPg& b) {
    if (a.dist != b.dist) return a.dist < b.dist;
    return a.pg_id < b.pg_id;
  });

  size_t keep = 1;
  if (!opt.repeat_select_single_pg && opt.probe_mode == "vote") {
    keep = std::max<size_t>(1, static_cast<size_t>(opt.probe_pgs));
  }
  keep = std::min(keep, ranked.size());
  out.reserve(keep);
  for (size_t i = 0; i < keep; ++i) {
    out.push_back({ranked[i].pg_id, static_cast<int64_t>(ranked[i].pg_id)});
  }
  return out;
}

inline std::vector<RankedPgCandidate> pivot_candidates_for_vec(
  const float* vec, int dim, const VectorBenchOptions& opt,
  const PivotModel& model, size_t keep_n) {
  std::vector<RankedPgCandidate> out;
  if (!vec || dim <= 0 || model.reps.empty()) {
    return out;
  }

  struct DistPg {
    float dist = 0.0f;
    uint32_t pg_id = 0;
  };

  std::vector<DistPg> ranked;
  ranked.reserve(model.reps.size());
  for (const auto& rep : model.reps) {
    ranked.push_back({l2_dist_sq(vec, rep.vec.data(), dim), rep.pg_id});
  }
  std::sort(ranked.begin(), ranked.end(), [](const DistPg& a, const DistPg& b) {
    if (a.dist != b.dist) return a.dist < b.dist;
    return a.pg_id < b.pg_id;
  });

  if (keep_n == 0) {
    keep_n = std::max<size_t>(1, static_cast<size_t>(opt.pivot_probe_budget));
  }
  keep_n = std::min(keep_n, ranked.size());
  out.reserve(keep_n);
  for (size_t i = 0; i < keep_n; ++i) {
    out.push_back({ranked[i].pg_id, static_cast<int64_t>(ranked[i].pg_id),
                   1.0f / (1.0f + static_cast<float>(i))});
  }
  return out;
}

inline std::vector<std::pair<uint32_t, size_t>> lsh_ranked_pgs_for_vec(
  const float* vec, int dim, const VectorBenchOptions& opt) {
  std::unordered_map<uint32_t, size_t> pg_votes;
  for (uint32_t t = 0; t < opt.num_tables; ++t) {
    const uint32_t rounds = effective_hash_repeat_rounds(opt);
    for (uint32_t r = 0; r < rounds; ++r) {
      const __u32 raw_hash = table_hash_for_vec(vec, dim, t, opt, r);
      const uint32_t pg = map_hash_to_pg(raw_hash, opt.pg_num, opt.pg_map_mode);
      pg_votes[pg]++;
    }
  }

  std::vector<std::pair<uint32_t, size_t>> ranked(pg_votes.begin(), pg_votes.end());
  std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) {
    if (a.second != b.second) return a.second > b.second;
    return a.first < b.first;
  });
  return ranked;
}

inline HybridGroupModel build_hybrid_group_model(
  const float* vectors, size_t vector_count, int dim, const VectorBenchOptions& opt) {
  HybridGroupModel model;
  model.dim = dim;
  if (!is_hybrid_backend(opt) || !vectors || vector_count == 0 || dim <= 0 || opt.pg_num == 0) {
    return model;
  }

  const size_t sample_limit = std::min<size_t>(
    vector_count, std::max<size_t>(1, static_cast<size_t>(opt.pivot_build_sample)));
  const size_t stride = std::max<size_t>(1, vector_count / sample_limit);

  struct Acc {
    std::vector<float> sum;
    size_t count = 0;
  };
  std::unordered_map<uint32_t, Acc> accs;

  for (size_t i = 0; i < vector_count && model.sample_count < sample_limit; i += stride, ++model.sample_count) {
    const float* vec = vectors + i * static_cast<size_t>(dim);
    auto ranked = lsh_ranked_pgs_for_vec(vec, dim, opt);
    if (ranked.empty()) {
      continue;
    }
    const uint32_t pg_id = ranked.front().first;
    auto& acc = accs[pg_id];
    if (acc.sum.empty()) {
      acc.sum.assign(vec, vec + dim);
    } else {
      for (int d = 0; d < dim; ++d) {
        acc.sum[static_cast<size_t>(d)] += vec[d];
      }
    }
    acc.count++;
  }

  for (auto& kv : accs) {
    if (kv.second.count == 0 || kv.second.sum.empty()) {
      continue;
    }
    const float inv = 1.0f / static_cast<float>(kv.second.count);
    for (float& x : kv.second.sum) {
      x *= inv;
    }
    model.reps.emplace(kv.first, HybridGroupReprEntry{kv.first, std::move(kv.second.sum)});
  }
  return model;
}

inline __u32 table_hash_for_vec(const float* vec, int dim, uint32_t table_id,
                                const VectorBenchOptions& opt,
                                uint32_t repeat_idx = 0) {
  if (is_orth_sign_backend(opt)) {
    const int bits = static_cast<int>(
      (opt.hash_bits > 0) ? std::min<uint32_t>(opt.hash_bits, 32u) : valid_lsh_bits(opt.pg_num));
    const uint32_t seed = opt.rot_seed + repeat_idx * opt.repeat_seed_stride;
    return crush_hash32_orth_multi_k(vec, dim, static_cast<int>(table_id), bits, seed);
  }
  return crush_hash32_lsh_multi(vec, dim, static_cast<int>(table_id));
}

inline std::vector<TablePgCandidate> rotation_candidates_for_vec(const float* vec, int dim,
                                                                 uint32_t table_id,
                                                                 const VectorBenchOptions& opt,
                                                                 const RotationQuantizationParams* rqp = nullptr) {
  std::vector<TablePgCandidate> out;
  if (dim <= 0) return out;

  std::vector<float> rotated(dim);
  if (rotation_apply_for_backend(vec, dim, table_id, opt, rotated.data()) <= 0) {
    return out;
  }

  const RotationQuantizationParams params =
    (rqp != nullptr) ? *rqp : rotation_params_from_opt(opt, dim);
  const int use_dims = params.use_dims;
  const int bins_per_dim = params.bins_per_dim;
  std::vector<int> bins(use_dims);
  if (crush_hash32_rotation_quantize_prefix(rotated.data(), dim, use_dims, params.min_val,
                                            params.width, bins_per_dim, bins.data()) <= 0) {
    return out;
  }

  int max_bucket_ids = 1;
  const int span = std::max<int>(0, static_cast<int>(opt.rotation_neighbor_step));
  for (int i = 0; i < use_dims; ++i) {
    if (max_bucket_ids > 4096 / (2 * span + 1)) {
      max_bucket_ids = 4096;
      break;
    }
    max_bucket_ids *= (2 * span + 1);
  }
  if (max_bucket_ids <= 0) max_bucket_ids = 1;

  std::vector<__u64> bucket_ids(max_bucket_ids);
  int bucket_count = crush_hash32_rotation_neighbor_buckets(
    bins.data(), use_dims, bins_per_dim, span, bucket_ids.data(), max_bucket_ids);
  if (bucket_count <= 0) {
    bucket_ids[0] = crush_hash32_rotation_encode_bucket(bins.data(), use_dims, bins_per_dim);
    bucket_count = 1;
  }

  std::unordered_set<uint32_t> dedup_pg;
  out.reserve(static_cast<size_t>(bucket_count));
  for (int i = 0; i < bucket_count; ++i) {
    uint32_t pg = crush_hash32_rotation_bucket_to_pg(bucket_ids[i], opt.pg_num);
    if (!dedup_pg.insert(pg).second) continue;
    out.push_back({pg, static_cast<int64_t>(bucket_ids[i])});
  }
  return out;
}

inline std::vector<TablePgCandidate> table_pg_candidates_for_vec(const float* vec, int dim,
                                                                 uint32_t table_id,
                                                                 const VectorBenchOptions& opt,
                                                                 uint32_t repeat_idx = 0,
                                                                 const RotationQuantizationParams* rqp = nullptr,
                                                                 const RotationReprModel* repr_model = nullptr) {
  if (is_rotation_repr1_backend(opt)) {
    if (!repr_model) return {};
    return rotation_repr1_candidates_for_vec(vec, dim, opt, *repr_model);
  }
  if (is_rotation_backend(opt)) {
    return rotation_candidates_for_vec(vec, dim, table_id, opt, rqp);
  }
  __u32 raw_hash = table_hash_for_vec(vec, dim, table_id, opt, repeat_idx);
  uint32_t pg = map_hash_to_pg(raw_hash, opt.pg_num, opt.pg_map_mode);
  return {{pg, static_cast<int64_t>(raw_hash)}};
}

inline std::vector<RankedPgCandidate> global_pg_candidates_for_vec(
  const float* vec, int dim, const VectorBenchOptions& opt,
  const PivotModel& pivot_model,
  const HybridGroupModel* hybrid_group_model = nullptr) {
  std::vector<RankedPgCandidate> out;
  if (!is_global_routing_backend(opt) || !vec || dim <= 0) {
    return out;
  }

  if (is_pivot_backend(opt)) {
    return pivot_candidates_for_vec(
      vec, dim, opt, pivot_model, static_cast<size_t>(opt.pivot_probe_budget));
  }

  std::vector<std::pair<uint32_t, size_t>> lsh_ranked = lsh_ranked_pgs_for_vec(vec, dim, opt);
  if (opt.hybrid_lsh_vote_topk > 0 && lsh_ranked.size() > opt.hybrid_lsh_vote_topk) {
    lsh_ranked.resize(opt.hybrid_lsh_vote_topk);
  }

  out.reserve(lsh_ranked.size());
  for (size_t i = 0; i < lsh_ranked.size(); ++i) {
    const uint32_t pg_id = lsh_ranked[i].first;
    float score = 4.0f * static_cast<float>(lsh_ranked[i].second);
    score += 1.0f / (1.0f + static_cast<float>(i));

    if (hybrid_group_model) {
      const auto it = hybrid_group_model->reps.find(pg_id);
      if (it != hybrid_group_model->reps.end()) {
        const float d2 = l2_dist_sq(vec, it->second.vec.data(), dim);
        score += 1.0f / (1.0f + d2);
      }
    }
    out.push_back({pg_id, static_cast<int64_t>(pg_id), score});
  }
  std::sort(out.begin(), out.end(), [](const RankedPgCandidate& a, const RankedPgCandidate& b) {
    if (a.score != b.score) return a.score > b.score;
    return a.pg_id < b.pg_id;
  });
  return out;
}

inline std::vector<SetVote> build_set_votes(const float* vec, int dim, const VectorBenchOptions& opt,
                                            const RotationQuantizationParams* rqp = nullptr,
                                            const RotationReprModel* repr_model = nullptr) {
  std::vector<SetVote> out;
  const uint32_t set_size = get_table_set_size(opt);
  const uint32_t set_count = get_table_set_count(opt);
  const std::string& combine_mode = (opt.table_set_size > 0) ? opt.set_combine : opt.table_combine;
  out.reserve(set_count);

  for (uint32_t s = 0; s < set_count; ++s) {
    const uint32_t tb = s * set_size;
    const uint32_t te = std::min<uint32_t>(opt.num_tables, tb + set_size);

    std::unordered_map<uint32_t, size_t> pg_votes;
    std::unordered_map<uint32_t, int64_t> pg_hash;
    for (uint32_t t = tb; t < te; ++t) {
      const uint32_t rounds = effective_hash_repeat_rounds(opt);
      for (uint32_t r = 0; r < rounds; ++r) {
        auto candidates = table_pg_candidates_for_vec(vec, dim, t, opt, r, rqp, repr_model);
        for (const auto& c : candidates) {
          pg_votes[c.pg_id]++;
          if (pg_hash.find(c.pg_id) == pg_hash.end()) {
            pg_hash[c.pg_id] = c.hash;
          }
        }
      }
    }

    std::vector<SetCandidate> ranked;
    ranked.reserve(pg_votes.size());
    for (const auto& kv : pg_votes) {
      ranked.push_back({kv.first, kv.second, pg_hash[kv.first]});
    }
    std::sort(ranked.begin(), ranked.end(), [](const SetCandidate& a, const SetCandidate& b) {
      if (a.votes != b.votes) return a.votes > b.votes;
      return a.pg_id < b.pg_id;
    });

    if (combine_mode == "and") {
      const uint32_t rounds = effective_hash_repeat_rounds(opt);
      const uint32_t need = (te - tb) * rounds;
      ranked.erase(
        std::remove_if(ranked.begin(), ranked.end(), [need](const SetCandidate& c) {
          return c.votes != need;
        }),
        ranked.end());
    }

    out.push_back(SetVote{s, tb, te, std::move(ranked)});
  }
  return out;
}

inline void append_probe_pgs_for_set(const SetVote& sv,
                                     const VectorBenchOptions& opt,
                                     std::unordered_set<uint32_t>* target_pgs) {
  size_t keep = (opt.table_set_size > 0) ? opt.probe_pgs_per_set : opt.probe_pgs;
  if (keep == 0) keep = 1;
  for (size_t i = 0; i < sv.ranked.size() && i < keep; ++i) {
    target_pgs->insert(sv.ranked[i].pg_id);
  }
}

inline std::vector<uint32_t> replica_sets_for(uint32_t set_id,
                                              const VectorBenchOptions& opt,
                                              uint32_t set_count) {
  std::vector<uint32_t> out;
  if (opt.table_set_size == 0 || set_count <= 1) {
    return out;
  }
  if (opt.set_replica_mode == "paired") {
    uint32_t buddy = (set_id % 2 == 0) ? (set_id + 1) : (set_id - 1);
    if (buddy < set_count) out.push_back(buddy);
  } else if (opt.set_replica_mode == "ring") {
    uint32_t cnt = std::min<uint32_t>(opt.replica_set_count, set_count - 1);
    for (uint32_t i = 1; i <= cnt; ++i) {
      out.push_back((set_id + i) % set_count);
    }
  }
  return out;
}
