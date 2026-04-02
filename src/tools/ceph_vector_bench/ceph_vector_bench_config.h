// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

/** CLI options shared by verify / recall / load paths. */
struct VectorBenchOptions {
  std::string fvecs_file;
  std::string pool_name;
  std::string query_file;
  std::string gt_file;
  std::string obj_prefix = "vec_";
  size_t max_vectors = 0;
  size_t max_queries = 0;
  bool create_pool = false;
  bool verify_only = false;
  bool recall_mode = false;
  uint32_t pg_num = 256;
  uint32_t num_tables = 128;
  uint32_t table_set_size = 0;   // 0 => keep legacy all-table path
  uint32_t write_top_pgs_per_set = 1;
  uint32_t write_top_pgs = 0;
  std::string pg_map_mode = "stable";
  std::string table_combine = "or";
  std::string set_combine = "and";
  std::string set_replica_mode = "paired"; // none|paired|ring
  uint32_t replica_set_count = 1;
  std::string probe_mode = "union";
  uint32_t probe_pgs = 1;
  uint32_t probe_pgs_per_set = 1;
};
