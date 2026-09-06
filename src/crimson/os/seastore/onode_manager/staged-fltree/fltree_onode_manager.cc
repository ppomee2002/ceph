// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

#include <algorithm>
#include <limits>

#include "crimson/os/seastore/logging.h"

#include "crimson/os/seastore/onode_manager/staged-fltree/fltree_onode_manager.h"

SET_SUBSYS(seastore_onode);

namespace crimson::os::seastore::onode {

void FLTreeOnode::Recorder::apply_value_delta(
  ceph::bufferlist::const_iterator &bliter,
  NodeExtentMutable &value,
  laddr_offset_t value_addr_offset)
{
  LOG_PREFIX(FLTreeOnode::Recorder::apply_value_delta);
  delta_op_t op;
  try {
    ceph::decode(op, bliter);
    auto &mlayout = *reinterpret_cast<onode_layout_t*>(value.get_write());
    switch (op) {
    case delta_op_t::UNSET_NEED_COW:
      DEBUG("setting need_cow");
      bliter.copy(
	sizeof(mlayout.need_cow),
	(char *)&mlayout.need_cow);
      break;
    case delta_op_t::SET_NEED_COW:
      DEBUG("setting need_cow");
      bliter.copy(
	sizeof(mlayout.need_cow),
	(char *)&mlayout.need_cow);
      break;
    case delta_op_t::UPDATE_ONODE_SIZE:
      DEBUG("update onode size");
      bliter.copy(sizeof(mlayout.size), (char *)&mlayout.size);
      break;
    case delta_op_t::UPDATE_OMAP_ROOT:
      DEBUG("update omap root");
      bliter.copy(sizeof(mlayout.omap_root), (char *)&mlayout.omap_root);
      break;
    case delta_op_t::UPDATE_XATTR_ROOT:
      DEBUG("update xattr root");
      bliter.copy(sizeof(mlayout.xattr_root), (char *)&mlayout.xattr_root);
      break;
    case delta_op_t::UPDATE_OBJECT_DATA:
      DEBUG("update object data");
      bliter.copy(sizeof(mlayout.object_data), (char *)&mlayout.object_data);
      break;
    case delta_op_t::UPDATE_OBJECT_INFO:
      DEBUG("update object info");
      bliter.copy(onode_layout_t::MAX_OI_LENGTH, (char *)&mlayout.oi[0]);
      ceph::decode(mlayout.oi_size, bliter);
      break;
    case delta_op_t::UPDATE_SNAPSET:
      DEBUG("update snapset");
      bliter.copy(onode_layout_t::MAX_SS_LENGTH, (char *)&mlayout.ss[0]);
      ceph::decode(mlayout.ss_size, bliter);
      break;
    case delta_op_t::CLEAR_OBJECT_INFO:
      DEBUG("clear object info");
      memset(&mlayout.oi[0], 0, mlayout.oi_size);
      mlayout.oi_size = 0;
      break;
    case delta_op_t::CLEAR_SNAPSET:
      DEBUG("clear snapset");
      memset(&mlayout.ss[0], 0, mlayout.ss_size);
      mlayout.ss_size = 0;
      break;
    case delta_op_t::CREATE_DEFAULT:
      mlayout = onode_layout_t{};
      break;
    case delta_op_t::UPDATE_VECTOR_INDEX_LADDR:
      DEBUG("update vector node laddr");
      bliter.copy(
	sizeof(mlayout.vector_index_laddr),
	(char *)&mlayout.vector_index_laddr);
      break;
    default:
      ceph_abort();
    }
  } catch (buffer::error& e) {
    ceph_abort();
  }
}

void FLTreeOnode::Recorder::encode_update(
  NodeExtentMutable &payload_mut, delta_op_t op)
{
  LOG_PREFIX(FLTreeOnode::Recorder::encode_update);
  auto &layout = *reinterpret_cast<const onode_layout_t*>(
    payload_mut.get_read());
  auto &encoded = get_encoded(payload_mut);
  ceph::encode(op, encoded);
  switch(op) {
  case delta_op_t::UNSET_NEED_COW:
    DEBUG("setting need_cow");
    encoded.append(
      (const char *)&layout.need_cow,
      sizeof(layout.need_cow));
    break;
  case delta_op_t::SET_NEED_COW:
    DEBUG("setting need_cow");
    encoded.append(
      (const char *)&layout.need_cow,
      sizeof(layout.need_cow));
    break;
  case delta_op_t::UPDATE_ONODE_SIZE:
    DEBUG("update onode size");
    encoded.append(
      (const char *)&layout.size,
      sizeof(layout.size));
    break;
  case delta_op_t::UPDATE_OMAP_ROOT:
    DEBUG("update omap root");
    encoded.append(
      (const char *)&layout.omap_root,
      sizeof(layout.omap_root));
    break;
  case delta_op_t::UPDATE_XATTR_ROOT:
    DEBUG("update xattr root");
    encoded.append(
      (const char *)&layout.xattr_root,
      sizeof(layout.xattr_root));
    break;
  case delta_op_t::UPDATE_OBJECT_DATA:
    DEBUG("update object data");
    encoded.append(
      (const char *)&layout.object_data,
      sizeof(layout.object_data));
    break;
  case delta_op_t::UPDATE_OBJECT_INFO:
    DEBUG("update object info");
    encoded.append(
      (const char *)&layout.oi[0],
      onode_layout_t::MAX_OI_LENGTH);
    ceph::encode(layout.oi_size, encoded);
    break;
  case delta_op_t::UPDATE_SNAPSET:
    DEBUG("update snapset");
    encoded.append(
      (const char *)&layout.ss[0],
      onode_layout_t::MAX_SS_LENGTH);
    ceph::encode(layout.ss_size, encoded);
    break;
  case delta_op_t::UPDATE_VECTOR_INDEX_LADDR:
    DEBUG("update vector node laddr");
    encoded.append(
      (const char *)&layout.vector_index_laddr,
      sizeof(layout.vector_index_laddr));
    break;
  case delta_op_t::CREATE_DEFAULT:
    DEBUG("create default layout");
    [[fallthrough]];
  case delta_op_t::CLEAR_OBJECT_INFO:
    DEBUG("clear object info");
    [[fallthrough]];
  case delta_op_t::CLEAR_SNAPSET:
    DEBUG("clear snapset");
    break;
  default:
    ceph_abort();
  }
}

FLTreeOnodeManager::contains_onode_ret FLTreeOnodeManager::contains_onode(
  Transaction &trans,
  const ghobject_t &hoid)
{
  return tree.contains(trans, hoid);
}

FLTreeOnodeManager::get_onode_ret FLTreeOnodeManager::get_onode(
  Transaction &trans,
  const ghobject_t &hoid)
{
  LOG_PREFIX(FLTreeOnodeManager::get_onode);
  return tree.find(
    trans, hoid
  ).si_then([this, &hoid, &trans, FNAME](auto cursor) -> get_onode_ret {
    if (cursor == tree.end()) {
      DEBUGT("no entry for {}", trans, hoid);
      return crimson::ct_error::enoent::make();
    }
    auto val = OnodeRef(new FLTreeOnode(hoid.hobj, cursor.value()));
    assert(val->get_clone_prefix());
    return get_onode_iertr::make_ready_future<OnodeRef>(
      val
    );
  });
}

namespace {
struct ghobj_cmp_t {
  bool same_object;
  bool equal;
};
ghobj_cmp_t compare_ghobj(ghobject_t &identity, const ghobject_t &base) {
  auto snap_bak = identity.hobj.snap;
  identity.hobj.snap = base.hobj.snap;
  bool same_object = (identity == base);
  bool equal = same_object && (snap_bak == base.hobj.snap);
  identity.hobj.snap = snap_bak;
  return {same_object, equal};
}
}

FLTreeOnodeManager::get_or_create_onode_ret
FLTreeOnodeManager::get_or_create_onode(
  Transaction &trans,
  const ghobject_t &hoid)
{
  LOG_PREFIX(FLTreeOnodeManager::get_or_create_onode);
  auto [cursor, created] = co_await tree.insert(
    trans, hoid,
    OnodeTree::tree_value_config_t{sizeof(onode_layout_t)});

  auto flonode = new FLTreeOnode(hoid.hobj, cursor.value());
  if (!created) {
    DEBUGT("onode exists for {}", trans, hoid);
    assert(flonode->get_clone_prefix());
    co_return flonode;
  }
  assert(!cursor.is_end());
  // new onode created
  DEBUGT("created onode for entry for {}", trans, hoid);
  flonode->create_default_layout(trans);

  // handle object id from sibling
  auto onode = OnodeRef(flonode);
  if (!hoid.hobj.is_head()) {
    // cur onode is not head, move to next onode to check if it's
    // the clone sibling under the same object.
    auto next_cursor = co_await tree.get_next(trans, cursor);
    if (!next_cursor.is_end()) {
      auto nxt_oid = next_cursor.get_ghobj();
      auto ret = compare_ghobj(nxt_oid, hoid);
      assert(!ret.equal);
      if (ret.same_object) {
        auto nxt_onode = next_cursor.value();
        auto prefix = nxt_onode.get_clone_prefix();
        auto object_id = prefix->get_local_object_id();
        onode->set_sibling_object_id(object_id);
        co_return onode;
      } // not same object, fallthrough to search prev onode
    } // is end, fallthrough to search prev onode
  } // is head, fallthrough to search prev onode

  auto hint = hoid;
  hint.hobj.snap = 0;
  auto prev_cursor = co_await tree.lower_bound(trans, hint);

  assert(!prev_cursor.is_end());
  auto prv_oid = prev_cursor.get_ghobj();
  auto ret = compare_ghobj(prv_oid, hoid);
  if (!ret.equal && ret.same_object) {
    // sibling clone onode exists
    auto prv_onode = prev_cursor.value();
    auto prefix = prv_onode.get_clone_prefix();
    auto object_id = prefix->get_local_object_id();
    onode->set_sibling_object_id(object_id);
  } // else sibling clone onode not found, hoid is the first clone
  co_return onode;
}

FLTreeOnodeManager::get_or_create_onodes_ret
FLTreeOnodeManager::get_or_create_onodes(
  Transaction &trans,
  const std::vector<ghobject_t> &hoids)
{
  return seastar::do_with(
    std::vector<OnodeRef>(),
    [this, &hoids, &trans](auto &ret) {
      ret.reserve(hoids.size());
      return trans_intr::do_for_each(
        hoids,
        [this, &trans, &ret](auto &hoid) {
          return get_or_create_onode(trans, hoid
          ).si_then([&ret](auto &&onoderef) {
            ret.push_back(std::move(onoderef));
          });
        }).si_then([&ret] {
          return std::move(ret);
        });
    });
}

FLTreeOnodeManager::erase_onode_ret FLTreeOnodeManager::erase_onode(
  Transaction &trans,
  OnodeRef &onode)
{
  auto &flonode = static_cast<FLTreeOnode&>(*onode);
  assert(flonode.is_alive());
  flonode.mark_delete();
  return tree.erase(trans, flonode);
}

FLTreeOnodeManager::list_onodes_ret FLTreeOnodeManager::list_onodes(
  Transaction &trans,
  const ghobject_t& start,
  const ghobject_t& end,
  uint64_t limit)
{
  LOG_PREFIX(FLTreeOnodeManager::list_onodes);
  DEBUGT("start {}, end {}, limit {}", trans, start, end, limit);
  return tree.lower_bound(trans, start
  ).si_then([this, &trans, end, limit] (auto&& cursor) {
    using crimson::os::seastore::onode::full_key_t;
    return seastar::do_with(
        limit,
        std::move(cursor),
        list_onodes_bare_ret(),
        [this, &trans, end] (auto& to_list, auto& current_cursor, auto& ret) {
      return trans_intr::repeat(
          [this, &trans, end, &to_list, &current_cursor, &ret] ()
          -> eagain_ifuture<seastar::stop_iteration> {
	LOG_PREFIX(FLTreeOnodeManager::list_onodes);
        if (current_cursor.is_end()) {
	  DEBUGT("reached the onode tree end", trans);
          std::get<1>(ret) = ghobject_t::get_max();
          return seastar::make_ready_future<seastar::stop_iteration>(
            seastar::stop_iteration::yes);
        } else if (current_cursor.get_ghobj() >= end) {
	  DEBUGT("reached the end {} > {}",
	    trans, current_cursor.get_ghobj(), end);
          std::get<1>(ret) = end;
          return seastar::make_ready_future<seastar::stop_iteration>(
            seastar::stop_iteration::yes);
        }
        if (to_list == 0) {
	  DEBUGT("reached the limit", trans);
          std::get<1>(ret) = current_cursor.get_ghobj();
          return seastar::make_ready_future<seastar::stop_iteration>(
            seastar::stop_iteration::yes);
        }
	auto ghobj = current_cursor.get_ghobj();
	DEBUGT("found onode for {}", trans, ghobj);
        std::get<0>(ret).emplace_back(std::move(ghobj));
        return tree.get_next(trans, current_cursor
        ).si_then([&to_list, &current_cursor] (auto&& next_cursor) mutable {
          // we intentionally hold the current_cursor during get_next() to
          // accelerate tree lookup.
          --to_list;
          current_cursor = next_cursor;
          return seastar::make_ready_future<seastar::stop_iteration>(
	        seastar::stop_iteration::no);
        });
      }).si_then([&ret] () mutable {
        return seastar::make_ready_future<list_onodes_bare_ret>(
            std::move(ret));
       //  return ret;
      });
    });
  });
}

eagain_ifuture<Ref<Node>> FLTreeTreeBoundaryQuery::descend_primary(
    context_t c,
    Ref<Node> node,
    const key_hobj_t &key,
    MatchHistory &history,
    const crimson::os::seastore::tree_boundary_priority_fn_t &priority,
    std::vector<frontier_entry_t> &frontier)
{
  if (node->level() == 0) {
    return eagain_iertr::make_ready_future<Ref<Node>>(node);
  }
  auto *internal = static_cast<InternalNode*>(node.get());
  return internal->get_primary_child_range(c, key, history
  ).si_then([c, node, &key, &history, &priority, &frontier]
      (auto child_range) mutable -> eagain_ifuture<Ref<Node>> {
    const bool gated = history.get<STAGE_LEFT>().has_value() &&
                        *history.get<STAGE_LEFT>() == MatchKindCMP::EQ;
    if (!gated) {
      return FLTreeTreeBoundaryQuery::descend_primary(
          c, child_range.node, key, history, priority, frontier);
    }
    // Gated level: the anchor's own (shard, pool, crush) matched this
    // node's separator, so this node's children are the ones that can
    // hold the anchor PG's key run. Enumerate outward from the primary
    return enqueue_slot_children(
        c, node, key, child_range.pos, priority, frontier
    ).si_then([c, child_range, &key, &history, &priority, &frontier]
        () -> eagain_ifuture<Ref<Node>> {
        return FLTreeTreeBoundaryQuery::descend_primary(
            c, child_range.node, key, history, priority, frontier);
    });
  });
}

eagain_ifuture<> FLTreeTreeBoundaryQuery::enqueue_slot_children(
    context_t c,
    Ref<Node> node,
    const key_hobj_t &key,
    const search_position_t &primary_pos,
    const crimson::os::seastore::tree_boundary_priority_fn_t &priority,
    std::vector<frontier_entry_t> &frontier)
{
  auto *internal = static_cast<InternalNode*>(node.get());
  std::vector<InternalNode::child_probe_t> probes;
  bool ignored_truncated = false;
  // Reads separator keys only -- no child extent is touched here, so a
  // child the bound is about to refuse costs nothing.
  internal->enumerate_left_slot(
      key, primary_pos, kMaxSlotChildren, probes, &ignored_truncated);

  std::vector<std::pair<double, InternalNode::child_probe_t>> admitted;
  admitted.reserve(probes.size());
  for (const auto &probe : probes) {
    if (probe.pos == primary_pos) {
      // The primary path descends into this one directly; queueing it too
      // would double-visit the same subtree.
      continue;
    }
    auto p = priority(probe.lower_excl, probe.upper_incl);
    if (!p) {
      continue;
    }
    admitted.emplace_back(*p, probe);
  }
  // Closest first. `priority` returns a lower bound on the distance the
  // child's own key range could hold, so this is a distance ordering, not
  // a key ordering.
  std::stable_sort(admitted.begin(), admitted.end(),
                   [](const auto &lhs, const auto &rhs) {
                     return lhs.first < rhs.first;
                   });
  return seastar::do_with(
      std::move(admitted), Ref<Node>(node),
      [c, &frontier](auto &admitted, auto &node) {
    return trans_intr::do_for_each(
        admitted.begin(), admitted.end(),
        [c, &frontier, &node](auto &entry) {
      auto *internal = static_cast<InternalNode*>(node.get());
      return internal->materialize_child(c, entry.second
      ).si_then([&frontier](auto range) {
        frontier.push_back({range.node, range.lower_excl, range.upper_incl});
      });
    });
  });
}

void FLTreeTreeBoundaryQuery::collect_leaf_local(
    LeafNode &leaf,
    const search_position_t &start_pos,
    const ghobject_t &start_key,
    const crimson::os::seastore::tree_boundary_priority_fn_t &priority,
    std::vector<ghobject_t> &out_oids,
    bool *edge_prev,
    bool *edge_next)
{
  *edge_prev = false;
  *edge_next = false;
  // The start entry is a candidate like any other and clears the same
  // point test the walked neighbors do. It is not guaranteed admissible:
  // expand_one() enters a leaf at lookup_smallest(), that page's first
  // entry, and a leaf can hold keys from more than one PG, so that entry
  // may belong to a neighboring PG. A containment gate that admits only
  // fully contained ranges makes this unreachable, so it changes nothing
  // today, but it is what allows admitting partially contained ranges.
  search_position_t begin_pos = start_pos;
  ghobject_t begin_key = start_key;
  if (!priority(begin_key, begin_key)) {
    // The entry this page was entered at is not admissible, but the page
    // may still be: expand_one() enters at the page's first entry, and a
    // page admitted for overlapping the anchor's key band often starts
    // with the previous band's keys. Returning here would discard every
    // admissible entry further in.
    //
    // Seek forward to the first entry the bound accepts instead. Bounded
    // by this page's entry count on an extent already in memory, so no
    // extra I/O; it moves where the walk starts, not what it emits.
    bool found = false;
    search_position_t pos = begin_pos;
    while (true) {
      auto next = leaf.get_next_entry_local(pos);
      if (!next) {
        *edge_next = true;
        break;
      }
      if (priority(next->key, next->key)) {
        begin_pos = next->cursor->get_position_ro();
        begin_key = next->key;
        found = true;
        break;
      }
      pos = next->cursor->get_position_ro();
    }
    if (!found) {
      return;
    }
  }
  out_oids.push_back(begin_key);
  {
    search_position_t pos = begin_pos;
    while (true) {
      auto prev = leaf.get_prev_entry_local(pos);
      if (!prev) {
        // Ran off this page's first entry: get_prev_entry_local() never
        // crosses into a sibling leaf. A structural stop rather than a
        // bound rejection, so report it and let the caller offer the
        // adjacent sibling leaf to `priority`.
        *edge_prev = true;
        break;
      }
      // A single leaf entry is a point, not a range: use its own key as
      // both bounds so priority() decodes its exact distance_bucket,
      // rather than (incorrectly) widening toward -infinity.
      auto p = priority(prev->key, prev->key);
      if (!p) {
        break;
      }
      out_oids.push_back(prev->key);
      pos = prev->cursor->get_position_ro();
    }
  }
  {
    search_position_t pos = begin_pos;
    while (true) {
      auto next = leaf.get_next_entry_local(pos);
      if (!next) {
        *edge_next = true;
        break;
      }
      auto p = priority(next->key, next->key);
      if (!p) {
        break;
      }
      out_oids.push_back(next->key);
      pos = next->cursor->get_position_ro();
    }
  }
}

void FLTreeTreeBoundaryQuery::order_oids_by_priority(
    std::vector<ghobject_t> *oids,
    const crimson::os::seastore::tree_boundary_priority_fn_t &priority)
{
  if (oids->size() < 2) {
    return;
  }
  // A batch comes out in key order, which within one PG is
  // (distance_bucket, residual_code): ascending distance from the PG's
  // anchor, not from the query. Reorder it by the key the frontier already
  // uses between subtrees, priority() on the entry's own exact key, so the
  // caller opens the closest ONodes first.
  //
  // Nothing is added or dropped. Every entry passed the same predicate
  // during collection, so a nullopt here means tau narrowed mid-batch;
  // those are ordered last rather than discarded, keeping this a
  // permutation.
  std::vector<std::pair<double, ghobject_t>> keyed;
  keyed.reserve(oids->size());
  for (auto &oid : *oids) {
    auto p = priority(oid, oid);
    keyed.emplace_back(p.value_or(std::numeric_limits<double>::max()),
                       std::move(oid));
  }
  std::stable_sort(keyed.begin(), keyed.end(),
                   [](const auto &lhs, const auto &rhs) {
                     return lhs.first < rhs.first;
                   });
  oids->clear();
  for (auto &entry : keyed) {
    oids->push_back(std::move(entry.second));
  }
}

void FLTreeTreeBoundaryQuery::filter_new_oids(std::vector<ghobject_t> *oids)
{
  std::vector<ghobject_t> kept;
  kept.reserve(oids->size());
  for (auto &oid : *oids) {
    if (collected_oids.insert(oid).second) {
      kept.push_back(std::move(oid));
    }
  }
  *oids = std::move(kept);
}

bool FLTreeTreeBoundaryQuery::push_frontier_deduped(
    Ref<Node> node,
    const std::optional<ghobject_t> &lower_excl,
    const std::optional<ghobject_t> &upper_incl)
{
  auto same = [&lower_excl, &upper_incl](
      const std::optional<ghobject_t> &l,
      const std::optional<ghobject_t> &u) {
    return l == lower_excl && u == upper_incl;
  };
  for (const auto &e : frontier) {
    if (same(e.lower_excl, e.upper_incl)) {
      return false;
    }
  }
  for (const auto &e : expanded_ranges) {
    if (same(e.lower_excl, e.upper_incl)) {
      return false;
    }
  }
  frontier.push_back({std::move(node), lower_excl, upper_incl});
  return true;
}

void FLTreeTreeBoundaryQuery::consider_sibling(
    std::optional<InternalNode::child_range_t> range,
    const crimson::os::seastore::tree_boundary_priority_fn_t &priority)
{
  if (!range) {
    // The leaf is its parent's first or tail child, so the adjacent leaf
    // lives under the grandparent, out of reach of this one-level lookup.
    // Counted as sibling_leaf_absent rather than worked around.
    return;
  }
  auto p = priority(range->lower_excl, range->upper_incl);
  if (!p) {
    // A bound decision -- different PG, or outside the current tau
    // distance/residual space -- so stopping here is correct.
    return;
  }
  push_frontier_deduped(
      range->node, range->lower_excl, range->upper_incl);
}

eagain_ifuture<> FLTreeTreeBoundaryQuery::enqueue_sibling_leaves(
    context_t c,
    Ref<Node> leaf_node,
    bool want_prev,
    bool want_next,
    const crimson::os::seastore::tree_boundary_priority_fn_t &priority)
{
  if (!want_prev && !want_next) {
    return eagain_iertr::now();
  }
  return seastar::do_with(
      std::move(leaf_node),
      [this, c, want_prev, want_next, &priority]
      (auto &leaf_node) -> eagain_ifuture<> {
    auto *leaf = static_cast<LeafNode*>(leaf_node.get());
    auto next_step = [this, c, leaf, want_next, &priority]()
        -> eagain_ifuture<> {
      if (!want_next) {
        return eagain_iertr::now();
      }
      return leaf->get_next_sibling_range(c
      ).si_then([this, &priority](auto range) {
        consider_sibling(std::move(range), priority);
        return eagain_iertr::now();
      });
    };
    if (!want_prev) {
      return next_step();
    }
    return leaf->get_prev_sibling_range(c
    ).si_then([this, &priority](auto range) {
      consider_sibling(std::move(range), priority);
      return eagain_iertr::now();
    }).si_then([next_step] {
      return next_step();
    });
  });
}

FLTreeTreeBoundaryQuery::ret FLTreeTreeBoundaryQuery::materialize(
    Transaction &t, std::vector<ghobject_t> oids)
{
  return seastar::do_with(
      std::move(oids),
      std::vector<crimson::os::seastore::tree_boundary_candidate_t>(),
      [this, &t](auto &oids, auto &out) {
    return trans_intr::do_for_each(
        oids.begin(), oids.end(),
        [this, &t, &out](const ghobject_t &oid) {
      return tree.find(t, oid
      ).si_then([this, &out, &oid](auto cursor) {
        if (cursor == tree.end()) {
          // discovered as a tracked key moments ago; a concurrent erase
          // within the same transaction is not expected here, but treat
          // defensively as "not a candidate" rather than asserting.
          return;
        }
        auto val = OnodeRef(new FLTreeOnode(oid.hobj, cursor.value()));
        out.push_back(
            crimson::os::seastore::tree_boundary_candidate_t{val, oid});
      });
    }).si_then([&out]() mutable {
      return std::move(out);
    });
  });
}

FLTreeTreeBoundaryQuery::ret FLTreeTreeBoundaryQuery::primary(
    Transaction &t,
    const ghobject_t &anchor,
    crimson::os::seastore::tree_boundary_priority_fn_t priority)
{
  return seastar::do_with(
      tree.get_node_context(t),
      key_hobj_t{anchor},
      MatchHistory{},
      std::move(priority),
      std::vector<ghobject_t>(),
      [this, &t](auto &ctx, auto &key, auto &history, auto &priority, auto &oids) {
    return tree.get_root_node(t
    ).si_then([this, ctx, &key, &history, &priority](auto root) {
      return descend_primary(ctx, root, key, history, priority, frontier);
    }).si_then([this, ctx, &key, &history, &priority, &oids](auto leaf_node) {
      auto *leaf = static_cast<LeafNode*>(leaf_node.get());
      return leaf->get_primary_entry(ctx, key, history
      ).si_then([this, ctx, leaf_node, leaf, &priority, &oids](auto entry)
          -> eagain_ifuture<> {
        if (!entry) {
          return eagain_iertr::now();
        }
        bool edge_prev = false;
        bool edge_next = false;
        collect_leaf_local(*leaf, entry->cursor->get_position_ro(),
                            entry->key, priority, oids,
                            &edge_prev, &edge_next);
        return enqueue_sibling_leaves(ctx, leaf_node, edge_prev, edge_next,
                                      priority);
      });
    }).si_then([this, &t, &oids, &priority]() mutable {
      filter_new_oids(&oids);
      order_oids_by_priority(&oids, priority);
      return materialize(t, std::move(oids));
    });
  });
}

eagain_ifuture<Ref<Node>> FLTreeTreeBoundaryQuery::descend_best(
    context_t c,
    Ref<Node> node,
    const crimson::os::seastore::tree_boundary_priority_fn_t &priority,
    std::vector<frontier_entry_t> &frontier)
{
  if (node->level() == 0) {
    return eagain_iertr::make_ready_future<Ref<Node>>(node);
  }
  auto *internal = static_cast<InternalNode*>(node.get());
  return internal->get_prev_child_range(c, search_position_t::end()
  ).si_then([c, node, &priority, &frontier]
      (auto p_last) -> eagain_ifuture<Ref<Node>> {
    if (!p_last) {
      return eagain_iertr::make_ready_future<Ref<Node>>();
    }
    auto *internal = static_cast<InternalNode*>(node.get());
    auto last_pos = p_last->pos;
    return internal->get_next_child_range(c, last_pos
    ).si_then([c, node, &priority, &frontier,
               p_last = std::move(*p_last)](auto tail) mutable
        -> eagain_ifuture<Ref<Node>> {
      return seastar::do_with(
          std::optional<InternalNode::child_range_t>(),
          std::optional<double>(),
          std::vector<InternalNode::child_range_t>(),
          std::move(p_last),
          [c, node, &priority, &frontier, tail = std::move(tail)]
          (auto &best, auto &best_priority, auto &others, auto &cur) mutable
              -> eagain_ifuture<Ref<Node>> {
        auto consider = [&best, &best_priority, &others, &priority]
            (InternalNode::child_range_t candidate) {
          auto p = priority(candidate.lower_excl, candidate.upper_incl);
          if (!p) {
            return;
          }
          if (!best_priority || *p < *best_priority) {
            if (best) {
              others.push_back(std::move(*best));
            }
            best = std::move(candidate);
            best_priority = p;
          } else {
            others.push_back(std::move(candidate));
          }
        };
        if (tail) {
          consider(std::move(*tail));
        }
        consider(cur);
        return trans_intr::repeat(
            [c, node, &cur, consider]() mutable
                -> eagain_ifuture<seastar::stop_iteration> {
          auto *internal = static_cast<InternalNode*>(node.get());
          return internal->get_prev_child_range(c, cur.pos
          ).si_then([&cur, consider](auto prev) mutable {
            if (!prev) {
              return seastar::stop_iteration::yes;
            }
            cur = std::move(*prev);
            consider(cur);
            return seastar::stop_iteration::no;
          });
        }).si_then([c, &priority, &best, &others, &frontier]() mutable
            -> eagain_ifuture<Ref<Node>> {
          for (auto &o : others) {
            frontier.push_back({o.node, o.lower_excl, o.upper_incl});
          }
          if (!best) {
            return eagain_iertr::make_ready_future<Ref<Node>>();
          }
          return FLTreeTreeBoundaryQuery::descend_best(
              c, best->node, priority, frontier);
        });
      });
    });
  });
}
FLTreeTreeBoundaryQuery::ret FLTreeTreeBoundaryQuery::expand_one(
    Transaction &t,
    crimson::os::seastore::tree_boundary_priority_fn_t priority,
    bool *has_more)
{
  // Pops frontier entries until one yields at least one oid this query has
  // not already returned, or nothing admissible is left.
  //
  // Sibling-leaf continuation makes a leaf reachable by more than one
  // route, so an expansion can produce only oids already collected. An
  // empty candidate list would look to query_vectors_tree_boundary() like
  // an exhausted frontier, and that caller stops the search on one. The
  // loop terminates because each iteration removes an entry and
  // push_frontier_deduped() never re-adds a range in expanded_ranges.
  return seastar::do_with(
      tree.get_node_context(t),
      std::move(priority),
      std::vector<ghobject_t>(),
      [this, &t, has_more](auto &ctx, auto &priority, auto &oids) {
    return trans_intr::repeat(
        [this, &ctx, &priority, &oids, has_more]()
            -> eagain_ifuture<seastar::stop_iteration> {
      // Re-evaluate pending entries against the current priority, since
      // tau may have narrowed since they were enqueued. Entries it now
      // rejects are dropped: tau only narrows within one query, so they
      // cannot become admissible again. Pick the lowest-priority
      // survivor.
      std::vector<frontier_entry_t> survivors;
      survivors.reserve(frontier.size());
      std::optional<double> best_p;
      size_t best_idx = 0;
      bool have_best = false;
      for (auto &entry : frontier) {
        auto p = priority(entry.lower_excl, entry.upper_incl);
        if (!p) {
          continue;
        }
        if (!have_best || *p < *best_p) {
          best_p = p;
          best_idx = survivors.size();
          have_best = true;
        }
        survivors.push_back(std::move(entry));
      }
      frontier = std::move(survivors);
      if (!have_best) {
        *has_more = false;
        return eagain_iertr::make_ready_future<seastar::stop_iteration>(
            seastar::stop_iteration::yes);
      }
      auto picked_node = std::move(frontier[best_idx].node);
      expanded_ranges.push_back({frontier[best_idx].lower_excl,
                                 frontier[best_idx].upper_incl});
      frontier.erase(frontier.begin() + best_idx);
      *has_more = !frontier.empty();

      return seastar::do_with(
          std::move(picked_node),
          [this, &ctx, &priority, &oids](auto &picked_node) {
        return descend_best(ctx, picked_node, priority, frontier
        ).si_then([this, &ctx, &priority, &oids]
            (Ref<Node> leaf_node) -> eagain_ifuture<> {
          if (!leaf_node) {
            // The narrowed bound rejected every child at some level of
            // this subtree, so there is nothing to collect.
            return eagain_iertr::now();
          }
          return seastar::do_with(std::move(leaf_node),
              [this, &ctx, &priority, &oids](auto &leaf_node) {
            return leaf_node->lookup_smallest(ctx
            ).si_then([this, &ctx, &leaf_node, &priority, &oids](auto cursor)
                -> eagain_ifuture<> {
              if (cursor->is_end()) {
                return eagain_iertr::now();
              }
              auto *leaf = static_cast<LeafNode*>(leaf_node.get());
              auto key_view = cursor->get_key_view(ctx.vb.get_header_magic());
              bool edge_prev = false;
              bool edge_next = false;
              collect_leaf_local(*leaf, cursor->get_position_ro(),
                                 key_view.to_ghobj(), priority, oids,
                                 &edge_prev, &edge_next);
              return enqueue_sibling_leaves(ctx, leaf_node, edge_prev,
                                            edge_next, priority);
            });
          });
        }).si_then([this, &oids, &priority] {
          filter_new_oids(&oids);
          order_oids_by_priority(&oids, priority);
          return oids.empty()
            ? seastar::stop_iteration::no
            : seastar::stop_iteration::yes;
        });
      });
    }).si_then([this, &t, &oids] {
      return materialize(t, std::move(oids));
    });
  });
}

FLTreeOnodeManager::~FLTreeOnodeManager() {}

}
