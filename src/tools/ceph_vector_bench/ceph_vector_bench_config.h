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
  uint32_t write_top_pgs = 0;
  std::string probe_mode = "union";
  uint32_t probe_pgs = 1;
};
