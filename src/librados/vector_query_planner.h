// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#ifndef CEPH_LIBRADOS_VECTOR_QUERY_PLANNER_H
#define CEPH_LIBRADOS_VECTOR_QUERY_PLANNER_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include <errno.h>

#include "include/buffer.h"
#include "include/object.h"
#include "include/rados/vector_ops.h"
#include "librados/vector_placement.h"

namespace librados {
namespace vector_query {

struct probe_t {
  object_t oid;
  std::string placement_key;
};

struct plan_t {
  uint32_t algorithm_id = 0;
  uint32_t algorithm_version = 0;
  std::vector<probe_t> probes;
};

struct routed_request_t {
  object_t oid;
  std::string placement_key;
  ceph::bufferlist payload;
};

inline int build_hash_v0_plan(
    const ceph::rados::query_vectors_request_t& req,
    plan_t *plan)
{
  if (plan == nullptr || req.bucket_name.empty() || req.index_name.empty() ||
      req.query_vector.length() == 0) {
    return -EINVAL;
  }
  if (req.algorithm_version !=
      ceph::rados::vector_query_algorithm_version_0) {
    return -EOPNOTSUPP;
  }

  plan->algorithm_id = req.algorithm_id;
  plan->algorithm_version = req.algorithm_version;
  plan->probes.clear();

  // hash-v0 is a provisional deterministic planner for plumbing tests only.
  // It hashes query bytes and does not provide ANN locality. Future PRs will
  // add bit-LSH, pivot, and hybrid planners behind this dispatch layer.
  const auto placement = vector_placement::compute_hash_v0_placement(
      req.bucket_name, req.index_name, req.query_vector);
  plan->probes.push_back({
    placement.oid,
    placement.placement_key,
  });
  return 0;
}

inline int build_plan(const ceph::rados::query_vectors_request_t& req,
                      plan_t *plan)
{
  if (plan == nullptr) {
    return -EINVAL;
  }
  plan->algorithm_id = 0;
  plan->algorithm_version = 0;
  plan->probes.clear();

  switch (req.algorithm_id) {
  case ceph::rados::vector_query_algorithm_hash:
    return build_hash_v0_plan(req, plan);
  case ceph::rados::vector_query_algorithm_bit_lsh:
    return -EOPNOTSUPP;
  default:
    return -EOPNOTSUPP;
  }
}

inline int build_routed_requests(
    const ceph::rados::query_vectors_request_t& req,
    const plan_t& plan,
    std::vector<routed_request_t> *requests)
{
  if (requests == nullptr) {
    return -EINVAL;
  }
  requests->clear();
  if (plan.probes.empty()) {
    return -EINVAL;
  }

  for (const auto& probe : plan.probes) {
    ceph::rados::query_vectors_request_t probe_req = req;
    probe_req.algorithm_id = plan.algorithm_id;
    probe_req.algorithm_version = plan.algorithm_version;

    ceph::bufferlist payload;
    encode(probe_req, payload);
    requests->push_back({
      probe.oid,
      probe.placement_key,
      std::move(payload),
    });
  }
  return 0;
}

} // namespace vector_query
} // namespace librados

#endif
