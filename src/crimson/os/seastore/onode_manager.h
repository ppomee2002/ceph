// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#pragma once

#include <functional>
#include <iostream>
#include <optional>

#include <boost/intrusive_ptr.hpp>
#include <boost/smart_ptr/intrusive_ref_counter.hpp>
#include <seastar/core/future.hh>

#include "include/buffer_fwd.h"
#include "include/ceph_assert.h"
#include "common/hobject.h"

#include "crimson/common/errorator.h"
#include "crimson/os/seastore/onode.h"
#include "crimson/os/seastore/seastore_types.h"
#include "crimson/os/seastore/transaction_manager.h"
#include "crimson/osd/exceptions.h"

namespace crimson::os::seastore {

/**
 * TreeBoundaryQuery
 *
 * Driver for query-directed multi-path descent over one OnodeManager-backed
 * key tree. The caller supplies a priority function over a subtree's
 * (lower_excl, upper_incl] ghobject_t key range, nullopt at either end
 * meaning -/+infinity; this object owns the frontier of sibling subtrees
 * discovered so far. The only current caller is seastore.cc's
 * query_vectors(), which passes `priority` as a closure over its own
 * Dq/tau.
 *
 * priority() returning nullopt means the subtree cannot hold anything
 * acceptable and is not explored. Otherwise the result is a sort key with
 * smaller meaning more promising; its units are up to the caller.
 */
struct tree_boundary_candidate_t {
  OnodeRef onode;
  ghobject_t oid;
};

using tree_boundary_priority_fn_t = std::function<
  std::optional<double>(
    const std::optional<ghobject_t>& lower_excl,
    const std::optional<ghobject_t>& upper_incl)>;

class TreeBoundaryQuery {
public:
  using iertr = base_iertr;
  using ret = iertr::future<std::vector<tree_boundary_candidate_t>>;

  virtual ~TreeBoundaryQuery() = default;

  /// Descends to `anchor`'s exact key and returns it plus every accepted
  /// neighboring entry in that same leaf page, never a sibling leaf.
  /// Sibling subtrees found along the way are enqueued for later
  /// expand_one() calls. Call at most once, before any expand_one().
  virtual ret primary(
    Transaction &t,
    const ghobject_t &anchor,
    tree_boundary_priority_fn_t priority) = 0;

  /// Pops and descends the most promising pending subtree. `priority` is
  /// re-evaluated here because the caller's bound may have narrowed since
  /// the entry was enqueued; entries it now rejects are dropped without
  /// counting against the caller's budget. Sets *has_more to whether any
  /// still-accepted entry remains. Returns an empty vector if nothing was
  /// popped.
  virtual ret expand_one(
    Transaction &t,
    tree_boundary_priority_fn_t priority,
    bool *has_more) = 0;

};
using TreeBoundaryQueryRef = std::unique_ptr<TreeBoundaryQuery>;

class OnodeManager {
public:
  using mkfs_iertr = base_iertr;
  using mkfs_ret = mkfs_iertr::future<>;
  virtual mkfs_ret mkfs(Transaction &t) = 0;

  using contains_onode_iertr = base_iertr;
  using contains_onode_ret = contains_onode_iertr::future<bool>;
  virtual contains_onode_ret contains_onode(
    Transaction &trans,
    const ghobject_t &hoid) = 0;

  using get_onode_iertr = base_iertr::extend<
    crimson::ct_error::enoent>;
  using get_onode_ret = get_onode_iertr::future<
    OnodeRef>;
  virtual get_onode_ret get_onode(
    Transaction &trans,
    const ghobject_t &hoid) = 0;

  using get_or_create_onode_iertr = base_iertr::extend<
    crimson::ct_error::value_too_large>;
  using get_or_create_onode_ret = get_or_create_onode_iertr::future<
    OnodeRef>;
  virtual get_or_create_onode_ret get_or_create_onode(
    Transaction &trans,
    const ghobject_t &hoid) = 0;

  using get_or_create_onodes_iertr = base_iertr::extend<
    crimson::ct_error::value_too_large>;
  using get_or_create_onodes_ret = get_or_create_onodes_iertr::future<
    std::vector<OnodeRef>>;
  virtual get_or_create_onodes_ret get_or_create_onodes(
    Transaction &trans,
    const std::vector<ghobject_t> &hoids) = 0;

  using erase_onode_iertr = base_iertr;
  using erase_onode_ret = erase_onode_iertr::future<>;
  virtual erase_onode_ret erase_onode(
    Transaction &trans,
    OnodeRef &onode) = 0;

  using list_onodes_iertr = base_iertr;
  using list_onodes_bare_ret = std::tuple<std::vector<ghobject_t>, ghobject_t>;
  using list_onodes_ret = list_onodes_iertr::future<list_onodes_bare_ret>;
  virtual list_onodes_ret list_onodes(
    Transaction &trans,
    const ghobject_t& start,
    const ghobject_t& end,
    uint64_t limit) = 0;

  /// See TreeBoundaryQuery above. Each instance holds the frontier state
  /// for one query, unlike this long-lived per-shard OnodeManager.
  virtual TreeBoundaryQueryRef create_tree_boundary_query() = 0;

  virtual ~OnodeManager() {}
};
using OnodeManagerRef = std::unique_ptr<OnodeManager>;

}
