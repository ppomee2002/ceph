// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
#pragma once

#include "ceph_vector_bench_config.h"
#include "ceph_vector_bench_pg.h"

extern "C" {
#include "crush/hash.h"
}

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct SetCandidate {
  uint32_t pg_id = 0;
  size_t votes = 0;
  int64_t hash = 0;
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

inline __u32 table_hash_for_vec(const float* vec, int dim, uint32_t table_id,
                                const VectorBenchOptions& opt,
                                uint32_t repeat_idx = 0) {
  if (opt.hash_backend == "orth-rot") {
    const int bits = static_cast<int>(
      (opt.hash_bits > 0) ? std::min<uint32_t>(opt.hash_bits, 32u) : valid_lsh_bits(opt.pg_num));
    const uint32_t seed = opt.rot_seed + repeat_idx * opt.repeat_seed_stride;
    return crush_hash32_orth_multi_k(vec, dim, static_cast<int>(table_id), bits, seed);
  }
  return crush_hash32_lsh_multi(vec, dim, static_cast<int>(table_id));
}

inline std::vector<SetVote> build_set_votes(const float* vec, int dim, const VectorBenchOptions& opt) {
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
      const uint32_t rounds = (opt.hash_backend == "orth-rot")
        ? std::max<uint32_t>(1, opt.hash_repeat_rounds)
        : 1u;
      for (uint32_t r = 0; r < rounds; ++r) {
        __u32 raw_hash = table_hash_for_vec(vec, dim, t, opt, r);
        uint32_t pg = map_hash_to_pg(raw_hash, opt.pg_num, opt.pg_map_mode);
        pg_votes[pg]++;
        if (pg_hash.find(pg) == pg_hash.end()) {
          pg_hash[pg] = static_cast<int64_t>(raw_hash);
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
      const uint32_t rounds = (opt.hash_backend == "orth-rot")
        ? std::max<uint32_t>(1, opt.hash_repeat_rounds)
        : 1u;
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
