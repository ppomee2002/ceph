// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:nil -*-
// vim: ts=8 sw=2 sts=2 expandtab

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
    auto *internal = static_cast<InternalNode*>(node.get());
    return internal->get_prev_child_range(c, child_range.pos
    ).si_then([c, internal, child_range, &key, &history, &priority, &frontier]
        (auto prev) mutable -> eagain_ifuture<Ref<Node>> {
      if (prev) {
        auto p = priority(prev->lower_excl, prev->upper_incl);
        if (p) {
          frontier.push_back(
              {prev->node, prev->lower_excl, prev->upper_incl});
        }
      }
      return internal->get_next_child_range(c, child_range.pos
      ).si_then([c, child_range, &key, &history, &priority, &frontier]
          (auto next) mutable -> eagain_ifuture<Ref<Node>> {
        if (next) {
          auto p = priority(next->lower_excl, next->upper_incl);
          if (p) {
            frontier.push_back(
                {next->node, next->lower_excl, next->upper_incl});
          }
        }
        return FLTreeTreeBoundaryQuery::descend_primary(
            c, child_range.node, key, history, priority, frontier);
      });
    });
  });
}

void FLTreeTreeBoundaryQuery::collect_leaf_local(
    LeafNode &leaf,
    const search_position_t &start_pos,
    const ghobject_t &start_key,
    const crimson::os::seastore::tree_boundary_priority_fn_t &priority,
    std::vector<ghobject_t> &out_oids)
{
  out_oids.push_back(start_key);
  {
    search_position_t pos = start_pos;
    while (true) {
      auto prev = leaf.get_prev_entry_local(pos);
      if (!prev) {
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
    search_position_t pos = start_pos;
    while (true) {
      auto next = leaf.get_next_entry_local(pos);
      if (!next) {
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
      ).si_then([this, leaf_node, leaf, &priority, &oids](auto entry) {
        if (entry) {
          collect_leaf_local(*leaf, entry->cursor->get_position_ro(),
                              entry->key, priority, oids);
        }
        return seastar::now();
      });
    }).si_then([this, &t, &oids]() mutable {
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
      // No stored children at all: only possible for a rare, transient
      // level-tail internal node whose sole child is its own tail, which
      // this generic, position-relative enumeration (get_prev_child_range/
      // get_next_child_range, both requiring a reference slot to walk
      // from) cannot reach on its own. This never produces an incorrect
      // result -- only a missed, very narrow exploration opportunity.
      return eagain_iertr::make_ready_future<Ref<Node>>();
    }
    auto *internal = static_cast<InternalNode*>(node.get());
    auto last_pos = p_last->pos;
    return internal->get_next_child_range(c, last_pos
    ).si_then([c, node, &priority, &frontier,
               p_last = std::move(*p_last)](auto tail) mutable
        -> eagain_ifuture<Ref<Node>> {
      return seastar::do_with(
          std::optional<InternalNode::child_range_t>(),  // best
          std::optional<double>(),                       // best_priority
          std::vector<InternalNode::child_range_t>(),    // others (push to
                                                          // frontier later)
          std::move(p_last),                             // cur
          [c, node, &priority, &frontier, tail = std::move(tail)]
          (auto &best, auto &best_priority, auto &others,
           auto &cur) mutable -> eagain_ifuture<Ref<Node>> {
        // Evaluates one child against `priority`; tracks the lowest-
        // priority admissible child in `best`, demoting any
        // previous best (and every other admissible child) into
        // `others` for the caller to push onto the global frontier.
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
            // Every child was rejected by the current bound: nothing
            // left to explore in this subtree.
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
  // Re-evaluate every pending frontier entry against the CURRENT priority
  // (tau may have narrowed since some of these were enqueued): drop any
  // entry priority now rejects -- tau only narrows over the course of one
  // query, so a rejected entry can never become admissible again -- and
  // pick the lowest-priority survivor to descend into next.
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
    // Nothing left in the frontier still satisfies priority: it has
    // already been dropped above (tau only narrows, so none of these can
    // become relevant again this query) -- report no more work.
    *has_more = false;
    return seastar::do_with(
        std::vector<ghobject_t>(),
        [this, &t](auto &oids) { return materialize(t, std::move(oids)); });
  }
  auto picked_node = std::move(frontier[best_idx].node);
  frontier.erase(frontier.begin() + best_idx);
  *has_more = !frontier.empty();

  return seastar::do_with(
      tree.get_node_context(t),
      std::move(priority),
      std::vector<ghobject_t>(),
      std::move(picked_node),
      [this, &t](auto &ctx, auto &priority, auto &oids, auto &picked_node) {
    return descend_best(ctx, picked_node, priority, frontier
    ).si_then([this, ctx, &priority, &oids]
        (Ref<Node> leaf_node) -> eagain_ifuture<> {
      if (!leaf_node) {
        // The now-current (narrower) priority bound rejected every child
        // at some level of this subtree: nothing to collect here.
        return eagain_iertr::now();
      }
      return seastar::do_with(std::move(leaf_node),
          [this, ctx, &priority, &oids](auto &leaf_node) {
        return leaf_node->lookup_smallest(ctx
        ).si_then([this, ctx, &leaf_node, &priority, &oids](auto cursor) {
          if (cursor->is_end()) {
            return seastar::now();
          }
          auto *leaf = static_cast<LeafNode*>(leaf_node.get());
          auto key_view = cursor->get_key_view(ctx.vb.get_header_magic());
          collect_leaf_local(*leaf, cursor->get_position_ro(),
                             key_view.to_ghobj(), priority, oids);
          return seastar::now();
        });
      });
    }).si_then([this, &t, &oids]() mutable {
      return materialize(t, std::move(oids));
    });
  });
}

FLTreeOnodeManager::~FLTreeOnodeManager() {}

}
