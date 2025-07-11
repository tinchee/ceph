// -*- mode:C++; tab-width:8; c-basic-offset:2; indent-tabs-mode:t -*-
// vim: ts=8 sw=2 smarttab

#pragma once

#include "crimson/os/seastore/cached_extent.h"
#include "crimson/os/seastore/transaction.h"
#include "crimson/os/seastore/root_block.h"

namespace crimson::os::seastore {

template <typename ParentT>
class child_pos_t {
public:
  child_pos_t(TCachedExtentRef<ParentT> stable_parent, btreenode_pos_t pos)
    : stable_parent(stable_parent), pos(pos) {}

  TCachedExtentRef<ParentT> get_parent() {
    ceph_assert(stable_parent);
    return stable_parent;
  }
  btreenode_pos_t get_pos() {
    return pos;
  }
  template <typename ChildT>
  void link_child(ChildT *c) {
    get_parent()->link_child(c, pos);
  }
private:
  TCachedExtentRef<ParentT> stable_parent;
  btreenode_pos_t pos = std::numeric_limits<btreenode_pos_t>::max();
};

using get_child_iertr = trans_iertr<crimson::errorator<
  crimson::ct_error::input_output_error>>;
template <typename T>
using get_child_ifut = get_child_iertr::future<TCachedExtentRef<T>>;

template <typename ParentT, typename ChildT>
struct get_child_ret_t {
  std::variant<child_pos_t<ParentT>, get_child_ifut<ChildT>> ret;
  get_child_ret_t(child_pos_t<ParentT> pos)
    : ret(std::move(pos)) {}
  get_child_ret_t(get_child_ifut<ChildT> child)
    : ret(std::move(child)) {}

  bool has_child() const {
    return ret.index() == 1;
  }

  child_pos_t<ParentT> &get_child_pos() {
    ceph_assert(ret.index() == 0);
    return std::get<0>(ret);
  }

  get_child_ifut<ChildT> &get_child_fut() {
    ceph_assert(ret.index() == 1);
    return std::get<1>(ret);
  }
};

template <typename T, typename key_t>
struct node_cmp_t {
  using is_transparent = key_t;
  bool operator()(const TCachedExtentRef<T> &l,
		  const TCachedExtentRef<T> &r) const {
    assert(l->get_end() <= r->get_begin()
      || r->get_end() <= l->get_begin()
      || (l->get_begin() == r->get_begin()
	  && l->get_end() == r->get_end()));
    return l->get_begin() < r->get_begin();
  }
  bool operator()(const key_t &l, const TCachedExtentRef<T> &r) const {
    return l < r->get_begin();
  }
  bool operator()(const TCachedExtentRef<T> &l, const key_t &r) const {
    return l->get_begin() < r;
  }
};

template <
  typename ParentT,
  typename node_key_t,
  typename Comparator = node_cmp_t<ParentT, node_key_t>>
class ParentNode;

// link the root of the tree with its parent
template <typename ParentT, typename RootT>
class TreeRootLinker {
public:
  static void link_root(TCachedExtentRef<ParentT> &root_parent, RootT* root_node);
  static void unlink_root(TCachedExtentRef<ParentT> &root_parent);
};

// RootChildNode is (can be) the root of the tree.
// It serves the responsibility to be a child of the RootBlock.
// Logically, it is a secialized version of ChildNode.
template <typename ParentT, typename T>
requires std::is_same_v<RootBlock, ParentT>
class RootChildNode {
public:
protected:
  bool has_root_parent() const {
    return (bool)parent_of_root;
  }

  void set_root_parent_from_prior_instance() {
    auto &me = down_cast();
    assert(me.is_mutation_pending());
    auto pi = me.get_prior_instance();
    auto &prior = *pi->template cast<T>();
    ceph_assert(prior.parent_of_root);
    ceph_assert(me.pending_for_transaction);
    parent_of_root = prior.parent_of_root;
    TreeRootLinker<ParentT, T>::link_root(parent_of_root, &me);
    return;
  }

  void on_replace_prior() {
    set_root_parent_from_prior_instance();
  }

  void destroy() {
    assert(down_cast().is_btree_root());
    ceph_assert(parent_of_root);
    TreeRootLinker<ParentT, T>::unlink_root(parent_of_root);
  }
protected:
  // The parent of the root, e.g. RootBlock to the lba/backref tree
  TCachedExtentRef<ParentT> parent_of_root;

  void on_initial_write() {
    auto &me = down_cast();
    assert(me.is_btree_root());
    me.reset_parent_tracker();
  }
private:
  T& down_cast() {
    return *static_cast<T*>(this);
  }
  const T& down_cast() const {
    return *static_cast<const T*>(this);
  }
  template <typename, typename>
  friend class TreeRootLinker;
};

// The sharable linker from the ChildNodes to the same ParentNode
//
// The indirection of child.parent_tracker.parent is necessary
// because otherwise we'll need to update every child's parent
// upon commiting a mutated extent.
template <typename ParentT>
class parent_tracker_t
  : public boost::intrusive_ref_counter<
     parent_tracker_t<ParentT>, boost::thread_unsafe_counter> {
public:
  parent_tracker_t(TCachedExtentRef<ParentT> parent)
    : parent(parent) {}
  parent_tracker_t(ParentT* parent)
    : parent(parent) {}
  ~parent_tracker_t() {
    if (parent->my_tracker == this) {
      parent->my_tracker = nullptr;
    }
  }
  TCachedExtentRef<ParentT> get_parent() const {
    ceph_assert(parent);
    return parent;
  }
  void reset_parent(TCachedExtentRef<ParentT> p) {
    parent = p;
  }
  bool is_valid() const {
    return parent && parent->is_valid();
  }
private:
  TCachedExtentRef<ParentT> parent;
};

template <typename T>
std::ostream &operator<<(std::ostream &, const parent_tracker_t<T> &);

template <typename T>
using parent_tracker_ref = boost::intrusive_ptr<parent_tracker_t<T>>;

template <typename ParentT, typename key_t>
class BaseChildNode {
public:
  bool has_parent_tracker() const {
    return (bool)parent_tracker;
  }
  void reset_parent_tracker(parent_tracker_t<ParentT> *p = nullptr) {
    parent_tracker.reset(p);
  }
  bool is_parent_valid() const {
    return parent_tracker && parent_tracker->is_valid();
  }
  // this method should only be used for asserts and logs, because
  // the parent node might be stable writing and should "wait_io"
  // before further access
  TCachedExtentRef<ParentT> peek_parent_node() const {
    assert(parent_tracker);
    return parent_tracker->get_parent();
  }
  virtual key_t node_begin() const = 0;
protected:
  parent_tracker_ref<ParentT> parent_tracker;
  virtual bool _is_valid() const = 0;
  virtual bool _is_stable() const = 0;
  template <typename, typename, typename>
  friend class ParentNode;
};

// this is to avoid mistakenly copying pointers from
// copy sources when committing this lba node, because
// we rely on pointers' "nullness" to avoid copying
// pointers for updated values, and some lba mappings'
// aren't supposed to have children. At present, mappings
// that don't have children are reserved regions and
// indirect mapping.
template <typename T, typename node_key_t>
inline BaseChildNode<T, node_key_t>* get_reserved_ptr() {
  //TODO: using instant integers as invalid pointers may
  //	  not be a good practice.
  constexpr uint64_t reserved_ptr = std::numeric_limits<size_t>::max() - 15;
  return (BaseChildNode<T, node_key_t>*)reserved_ptr;
}

template <typename T, typename node_key_t>
bool is_reserved_ptr(BaseChildNode<T, node_key_t>* child) {
  return child == get_reserved_ptr<T, node_key_t>();
}

template <typename T, typename node_key_t>
bool is_valid_child_ptr(BaseChildNode<T, node_key_t>* child) {
  return child != nullptr && child != get_reserved_ptr<T, node_key_t>();
}

class ExtentTransViewRetriever {
public:
  template <typename T>
  get_child_ifut<T> get_extent_viewable_by_trans(
    Transaction &t,
    TCachedExtentRef<T> ext)
  {
    return get_extent_viewable_by_trans(t, CachedExtentRef(ext.get())
    ).si_then([](auto ext) {
      return ext->template cast<T>();
    });
  }
  virtual get_child_iertr::future<> maybe_wait_accessible(
    Transaction &, CachedExtent&) = 0;
  virtual bool is_viewable_extent_data_stable(Transaction &, CachedExtentRef) = 0;
  virtual bool is_viewable_extent_stable(Transaction &, CachedExtentRef) = 0;
  virtual ~ExtentTransViewRetriever() {}
protected:
  virtual get_child_iertr::future<CachedExtentRef> get_extent_viewable_by_trans(
    Transaction &t,
    CachedExtentRef extent) = 0;
};

// ParentNodes are nodes in the tree that have children,
// including leaf nodes that has other types of extents
// as the children, e.g. LBALeafNodes have logical extents
// as the children, so they are also ParentNodes.
template <
  typename T,
  typename node_key_t,
  typename Comparator>
class ParentNode {
  /*
   *
   * Nodes of a tree connect to their child nodes by pointers following
   * invariants below:
   *
   * 1. if nodes are stable:
   * 	a. parent points at the node's stable parent
   * 	b. prior_instance is empty
   * 	c. child pointers point at stable children. Child resolution is done
   * 	   directly via this array.
   * 	d. copy_sources is empty
   * 2. if nodes are mutation_pending:
   * 	a. parent is empty and needs to be fixed upon commit
   * 	b. prior_instance points to its stable version
   * 	c. child pointers are null except for initial_pending() children of
   * 	   this transaction. Child resolution is done by first checking this
   * 	   array, and then recursively resolving via the parent. We copy child
   * 	   pointers from parent on commit.
   * 	d. copy_sources is empty
   * 3. if nodes are initial_pending
   * 	a. parent points at its pending parent on this transaction (must exist)
   * 	b. prior_instance is empty or, if it's the result of rewrite, points to
   * 	   its stable predecessor
   * 	c. child pointers are null except for initial_pending() children of
   * 	   this transaction (live due to 3a below). Child resolution is done
   * 	   by first checking this array, and then recursively resolving via
   * 	   the correct copy_sources entry. We copy child pointers from copy_sources
   * 	   on commit.
   * 	d. copy_sources contains the set of stable nodes at the same tree-level(only
   * 	   its "prior_instance" if the node is the result of a rewrite), with which
   * 	   the lba range of this node overlaps.
   * 4. EXIST_CLEAN and EXIST_MUTATION_PENDING belong to 3 above (except that they
   * 	cannot be rewritten) because their parents must be mutated upon remapping.
   */
public:
  std::pair<bool, TCachedExtentRef<T>> resolve_transaction(
    Transaction &t, node_key_t key) {
    auto &me = down_cast();
    ceph_assert(me.is_valid());
    auto [viewable, state] = me.is_viewable_by_trans(t);
    if (viewable) {
      return {viewable, &me};
    }
    return {viewable, find_pending_version(t, key, state)};
  }

  template <typename ChildT>
  get_child_ret_t<T, ChildT> get_child(
    Transaction &t,
    ExtentTransViewRetriever &etvr,
    btreenode_pos_t pos,
    node_key_t key)
  {
    auto &me = down_cast();
    assert(children.capacity());
    assert(key == down_cast().iter_idx(pos).get_key());
    auto child = children[pos];
    ceph_assert(!is_reserved_ptr(child));
    if (is_valid_child_ptr(child)) {
      return etvr.get_extent_viewable_by_trans<ChildT>(
	t, static_cast<ChildT*>(child));
    } else if (me.is_pending()) {
      auto &sparent = me.get_stable_for_key(key);
      auto spos = sparent.lower_bound(key).get_offset();
      auto child = sparent.children[spos];
      if (is_valid_child_ptr(child)) {
	return etvr.get_extent_viewable_by_trans<ChildT>(
	  t, static_cast<ChildT*>(child));
      } else {
	return child_pos_t<T>(&sparent, spos);
      }
    } else {
      return child_pos_t<T>(&me, pos);
    }
  }

  void link_child(BaseChildNode<T, node_key_t>* child, btreenode_pos_t pos) {
    auto &me = down_cast();
    assert(pos < me.get_size());
    assert(pos < children.capacity());
    assert(child);
    ceph_assert(me.is_stable());
    assert(child->_is_stable());
    assert(!children[pos]);
    ceph_assert(is_valid_child_ptr(child));
    update_child_ptr(pos, child);
  }

  void insert_child_ptr(
    btreenode_pos_t offset,
    BaseChildNode<T, node_key_t>* child,
    btreenode_pos_t size = 0)
  {
    assert(child);
    auto &me = down_cast();
    if (size == 0) {
      size = me.get_size();
    }
    maybe_expand_children(size + 1);
    assert(size < children.capacity());
    auto raw_children = children.data();
    std::memmove(
      &raw_children[offset + 1],
      &raw_children[offset],
      (size - offset) * sizeof(BaseChildNode<T, node_key_t>*));
    children[offset] = child;
    if (!is_reserved_ptr(child)) {
      set_child_ptracker(child);
    }
  }

  void update_child_ptr(btreenode_pos_t pos, BaseChildNode<T, node_key_t>* child) {
    children[pos] = child;
    set_child_ptracker(child);
  }

protected:
  ParentNode(btreenode_pos_t capacity)
    : children(capacity, nullptr) {}
  ParentNode(const ParentNode &rhs)
    : children(rhs.children.capacity(), nullptr) {}
  void sync_children_capacity() {
    auto &me = down_cast();
    maybe_expand_children(me.get_size());
  }

  TCachedExtentRef<T> find_pending_version(
    Transaction &t, node_key_t key, CachedExtent::viewable_state_t &hint) {
    auto &me = down_cast();
    assert(me.is_stable());
    if (hint == CachedExtent::viewable_state_t::stable_become_pending) {
      auto mut_iter = me.mutation_pending_extents.find(
	t.get_trans_id(), trans_spec_view_t::cmp_t());
      assert(mut_iter != me.mutation_pending_extents.end());
      assert(copy_dests_by_trans.find(t.get_trans_id()) ==
	copy_dests_by_trans.end());
      return static_cast<T*>(&(*mut_iter));
    }
    ceph_assert(hint == CachedExtent::viewable_state_t::stable_become_retired);
    auto iter = copy_dests_by_trans.find(
      t.get_trans_id(), trans_spec_view_t::cmp_t());
    ceph_assert(iter != copy_dests_by_trans.end());
    auto &copy_dests = static_cast<copy_dests_t&>(*iter);
    auto it = copy_dests.dests_by_key.lower_bound(key);
    if (it == copy_dests.dests_by_key.end() || (*it)->get_begin() > key) {
      ceph_assert(it != copy_dests.dests_by_key.begin());
      --it;
    }
    ceph_assert((*it)->get_begin() <= key && key < (*it)->get_end());
    return *it;
  }

  void add_copy_dest(Transaction &t, TCachedExtentRef<T> dest) {
    ceph_assert(down_cast().is_stable());
    ceph_assert(dest->is_pending());
    auto tid = t.get_trans_id();
    auto iter = copy_dests_by_trans.lower_bound(
      tid, trans_spec_view_t::cmp_t());
    if (iter == copy_dests_by_trans.end() ||
	iter->pending_for_transaction != tid) {
      iter = copy_dests_by_trans.insert_before(
	iter, t.add_transactional_view<copy_dests_t>(t));
    }
    auto &copy_dests = static_cast<copy_dests_t&>(*iter);
    [[maybe_unused]] auto [it, inserted] =
      copy_dests.dests_by_key.insert(dest);
    assert(inserted || it->get() == dest.get());
  }

  void del_copy_dest(Transaction &t, TCachedExtentRef<T> dest) {
    auto iter = copy_dests_by_trans.find(
      t.get_trans_id(), trans_spec_view_t::cmp_t());
    ceph_assert(iter != copy_dests_by_trans.end());
    auto &copy_dests = static_cast<copy_dests_t&>(*iter);
    auto it = copy_dests.dests_by_key.find(dest);
    ceph_assert(it != copy_dests.dests_by_key.end());
    copy_dests.dests_by_key.erase(dest);
  }

  void set_child_ptracker(BaseChildNode<T, node_key_t> *child) {
    if (!this->my_tracker) {
      auto &me = down_cast();
      this->my_tracker = new parent_tracker_t(&me);
    }
    child->reset_parent_tracker(this->my_tracker);
  }

  void remove_child_ptr(btreenode_pos_t offset) {
    auto &me = down_cast();
    LOG_PREFIX(ParentNode::remove_child_ptr);
    auto raw_children = children.data();
    SUBTRACE(seastore_fixedkv_tree, "trans.{}, pos {}, total size {}, extent {}",
      me.pending_for_transaction,
      offset,
      me.get_size(),
      (void*)raw_children[offset]);
    // parent tracker of the child being removed will be
    // reset when the child is invalidated, so no need to
    // reset it here
    std::memmove(
      &raw_children[offset],
      &raw_children[offset + 1],
      (me.get_size() - offset - 1) * sizeof(BaseChildNode<T, node_key_t>*));
    maybe_shink_children();
  }

  void on_rewrite(Transaction &t, T &foreign_extent) {
    auto &me = down_cast();
    if (foreign_extent.is_stable()) {
      foreign_extent.add_copy_dest(t, &me);
      copy_sources.emplace(&foreign_extent);
    } else {
      ceph_assert(foreign_extent.is_mutation_pending());
      auto copy_source =
	foreign_extent.get_prior_instance()->template cast<T>();
      copy_source->add_copy_dest(t, &me);
      copy_sources.emplace(copy_source);
      children = std::move(foreign_extent.children);
      adjust_ptracker_for_children();
    }
  }

  void adjust_ptracker_for_children() {
    auto &me = down_cast();
    auto begin = children.begin();
    auto end = begin + me.get_size();
    ceph_assert(end <= children.end());
    for (auto it = begin; it != end; it++) {
      auto child = *it;
      if (is_valid_child_ptr(child)) {
	set_child_ptracker(child);
      }
    }
  }

  T& get_stable_for_key(node_key_t key) const {
    auto &me = down_cast();
    ceph_assert(me.is_pending());
    if (me.is_mutation_pending()) {
      return *me.get_prior_instance()->template cast<T>();
    } else {
      ceph_assert(!copy_sources.empty());
      auto it = copy_sources.upper_bound(key);
      it--;
      auto &copy_source = *it;
      ceph_assert(copy_source->is_in_range(key));
      return *copy_source;
    }
  }

  static void push_copy_sources(
    Transaction &t,
    T &dest,
    T &src)
  {
    ceph_assert(dest.is_initial_pending());
    if (src.is_stable()) {
      src.add_copy_dest(t, &dest);
      dest.copy_sources.emplace(&src);
    } else if (src.is_mutation_pending()) {
      auto copy_src =
	src.get_prior_instance()->template cast<T>();
      copy_src->add_copy_dest(t, &dest);
      dest.copy_sources.emplace(copy_src);
    } else {
      ceph_assert(src.is_initial_pending());
      for (auto &cs : src.copy_sources) {
	cs->add_copy_dest(t, &dest);
      }
      dest.copy_sources.insert(
	src.copy_sources.begin(),
	src.copy_sources.end());
    }
  }

  static void move_child_ptrs(
    T &dest,
    T &src,
    size_t dest_start,
    size_t src_start,
    size_t src_end)
  {
    std::memmove(
      dest.children.data() + dest_start,
      src.children.data() + src_start,
      (src_end - src_start) * sizeof(BaseChildNode<T, node_key_t>*));

    ceph_assert(src_start < src_end);
    ceph_assert(src.children.size() >= src_end);
    for (auto it = src.children.begin() + src_start;
	it != src.children.begin() + src_end;
	it++)
    {
      auto child = *it;
      if (is_valid_child_ptr(child)) {
	dest.set_child_ptracker(child);
      }
    }
  }

  void split_child_ptrs(
    Transaction &t,
    T &left,
    T &right)
  {
    auto &me = down_cast();
    assert(!left.my_tracker);
    assert(!right.my_tracker);
    btreenode_pos_t pivot = me.get_node_split_pivot();
    left.maybe_expand_children(pivot);
    right.maybe_expand_children(me.get_size() - pivot);
    if (me.is_pending()) {
      move_child_ptrs(left, me, 0, 0, pivot);
      move_child_ptrs(right, me, 0, pivot, me.get_size());
      my_tracker = nullptr;
    }
  }

  void adjust_copy_src_dest_on_split(
    Transaction &t,
    T &left,
    T &right)
  {
    auto &me = down_cast();
    if (me.is_initial_pending()) {
      for (auto &cs : copy_sources) {
	cs->del_copy_dest(t, &me);
      }
    }

    push_copy_sources(t, left, me);
    push_copy_sources(t, right, me);
  }

  void merge_child_ptrs(
    Transaction &t,
    T &left,
    T &right)
  {
    auto &me = down_cast();
    ceph_assert(!my_tracker);

    maybe_expand_children(left.get_size() + right.get_size());
    if (left.is_pending()) {
      move_child_ptrs(me, left, 0, 0, left.get_size());
      left.my_tracker = nullptr;
    }

    if (right.is_pending()) {
      move_child_ptrs(me, right, left.get_size(), 0, right.get_size());
      right.my_tracker = nullptr;
    }
  }

  void adjust_copy_src_dest_on_merge(
    Transaction &t,
    T &left,
    T &right)
  {
    auto &me = down_cast();

    if (left.is_initial_pending()) {
      for (auto &cs : left.copy_sources) {
	cs->del_copy_dest(t, &left);
      }
    }
    if (right.is_initial_pending()) {
      for (auto &cs : right.copy_sources) {
	cs->del_copy_dest(t, &right);
      }
    }
    push_copy_sources(t, me, left);
    push_copy_sources(t, me, right);
  }

  static void balance_child_ptrs(
    Transaction &t,
    T &left,
    T &right,
    uint32_t pivot_idx,
    T &replacement_left,
    T &replacement_right)
  {
    size_t l_size = left.get_size();
    size_t r_size = right.get_size();

    ceph_assert(pivot_idx != l_size && pivot_idx != r_size);
    replacement_left.maybe_expand_children(pivot_idx);
    replacement_right.maybe_expand_children(r_size + l_size - pivot_idx);

    assert(!replacement_left.my_tracker);
    assert(!replacement_right.my_tracker);
    if (pivot_idx < l_size) {
      // deal with left
      if (left.is_pending()) {
	move_child_ptrs(replacement_left, left, 0, 0, pivot_idx);
	move_child_ptrs(replacement_right, left, 0, pivot_idx, l_size);
	left.my_tracker = nullptr;
      }

      // deal with right
      if (right.is_pending()) {
	move_child_ptrs(replacement_right, right, l_size - pivot_idx, 0, r_size);
	right.my_tracker= nullptr;
      }
    } else {
      // deal with left
      if (left.is_pending()) {
	move_child_ptrs(replacement_left, left, 0, 0, l_size);
	left.my_tracker = nullptr;
      }

      // deal with right
      if (right.is_pending()) {
	move_child_ptrs(replacement_left, right, l_size, 0, pivot_idx - l_size);
	move_child_ptrs(replacement_right, right, 0, pivot_idx - l_size, r_size);
	right.my_tracker= nullptr;
      }
    }
  }

  static void adjust_copy_src_dest_on_balance(
    Transaction &t,
    T &left,
    T &right,
    uint32_t pivot_idx,
    T &replacement_left,
    T &replacement_right)
  {
    size_t l_size = left.get_size();

    if (left.is_initial_pending()) {
      for (auto &cs : left.copy_sources) {
	cs->del_copy_dest(t, &left);
      }
    }
    if (right.is_initial_pending()) {
      for (auto &cs : right.copy_sources) {
	cs->del_copy_dest(t, &right);
      }
    }

    if (pivot_idx < l_size) {
      // deal with left
      push_copy_sources(t, replacement_left, left);
      push_copy_sources(t, replacement_right, left);
      // deal with right
      push_copy_sources(t, replacement_right, right);
    } else {
      // deal with left
      push_copy_sources(t, replacement_left, left);
      // deal with right
      push_copy_sources(t, replacement_left, right);
      push_copy_sources(t, replacement_right, right);
    }
  }
#ifndef NDEBUG
  bool is_children_empty() const {
    for (auto it = children.begin();
	it != children.begin() + down_cast().get_size();
	it++) {
      if (is_valid_child_ptr(*it) && (*it)->_is_valid()) {
	return false;
      }
    }
    return true;
  }
#endif

  void set_children_from_prior_instance() {
    auto &me = down_cast();
    assert(me.get_prior_instance());
    auto &prior = *me.get_prior_instance()->template cast<T>();
    assert(prior.my_tracker || prior.is_children_empty());

    if (prior.my_tracker) {
      prior.my_tracker->reset_parent(&me);
      my_tracker = prior.my_tracker;
      // All my initial pending children is pointing to the original
      // tracker which has been dropped by the above line, so need
      // to adjust them to point to the new tracker
      adjust_ptracker_for_children();
    }
    assert(my_tracker || is_children_empty());
  }

  template<typename iter_t>
  btreenode_pos_t copy_children_from_stable_source(
    T &source,
    iter_t foreign_start_it,
    iter_t foreign_end_it,
    iter_t local_start_it) {
    auto &me = down_cast();
    auto foreign_it = foreign_start_it, local_it = local_start_it;
    while (foreign_it != foreign_end_it
	  && local_it.get_offset() < me.get_size())
    {
      auto &child = children[local_it.get_offset()];
      if (foreign_it.get_key() == local_it.get_key()) {
	// the foreign key is preserved
	if (!child) {
	  child = source.children[foreign_it.get_offset()];
	  // child can be either valid if present, nullptr if absent,
	  // or reserved ptr.
	}
	foreign_it++;
	local_it++;
      } else if (foreign_it.get_key() < local_it.get_key()) {
	// the foreign key has been removed, because, if it hasn't,
	// there must have been a local key before the one pointed
	// by the current "local_it" that's equal to this foreign key
	// and has pushed the foreign_it forward.
	foreign_it++;
      } else {
	// the local key must be a newly inserted one.
	local_it++;
      }
    }
    return local_it.get_offset();
  }

  void copy_children_from_stable_sources() {
    if (!copy_sources.empty()) {
      auto &me = down_cast();
      auto it = --copy_sources.upper_bound(me.get_begin());
      auto &cs = *it;
      btreenode_pos_t start_pos = cs->lower_bound(me.get_begin()).get_offset();
      if (start_pos == cs->get_size()) {
	it++;
	start_pos = 0;
      }
      btreenode_pos_t local_next_pos = 0;
      for (; it != copy_sources.end(); it++) {
	auto& copy_source = *it;
	auto end_pos = copy_source->get_size();
	if (copy_source->is_in_range(me.get_end())) {
	  end_pos = copy_source->upper_bound(me.get_end()).get_offset();
	}
	auto local_start_iter = me.iter_idx(local_next_pos);
	auto foreign_start_iter = copy_source->iter_idx(start_pos);
	auto foreign_end_iter = copy_source->iter_idx(end_pos);
	local_next_pos = copy_children_from_stable_source(
	  *copy_source, foreign_start_iter, foreign_end_iter, local_start_iter);
	if (end_pos != copy_source->get_size()) {
	  break;
	}
	start_pos = 0;
      }
    }
  }

  // for mutation pending and rewritten extents
  void take_children_from_prior_instance() {
    auto &me = down_cast();
    assert(me.is_mutation_pending() ? true : copy_sources.size() == 1);
    auto prior = me.get_prior_instance()->template cast<T>();
    assert(
      me.is_mutation_pending()
	? true
	: copy_sources.begin()->get() == prior.get());
    me.set_children_from_prior_instance();
    auto copied = me.copy_children_from_stable_source(
      *prior, prior->begin(), prior->end(), me.begin());
    ceph_assert(copied <= me.get_size());
  }

  // for inital pending extents created by tree node split/merge/balance
  void take_children_from_stable_sources() {
    copy_children_from_stable_sources();
    adjust_ptracker_for_children();
  }

  void prepare_commit() {
    auto &me = down_cast();
    if (me.is_initial_pending()) {
      if (me.is_rewrite()) {
	take_children_from_prior_instance();
      } else {
	take_children_from_stable_sources();
      }
      assert(me.validate_stable_children());
      me.copy_sources.clear();
    }
  }

#ifndef NDEBUG
  bool validate_stable_children() {
    LOG_PREFIX(FixedKVInternalNode::validate_stable_children);
    auto &me = down_cast();
    if (this->children.empty()) {
      return false;
    }

    for (auto i : me) {
      auto child = this->children[i.get_offset()];
      if (is_valid_child_ptr(child) && child->node_begin() != i.get_key()) {
	SUBERROR(seastore_fixedkv_tree,
	  "stable child not valid: child {}, key {}",
	  *dynamic_cast<CachedExtent*>(child),
	  i.get_key());
	ceph_abort();
	return false;
      }
    }
    return true;
  }
#endif

  void on_replace_prior() {
    auto &me = down_cast();
    ceph_assert(!me.is_rewrite());
    take_children_from_prior_instance();
    assert(me.validate_stable_children());
  }

  // children are considered stable if any of the following case is true:
  // 1. The child extent is absent in cache
  // 2. The child extent is (data) stable
  //
  // For reserved mappings, the return values are undefined.
  bool _is_child_stable(
    Transaction &t,
    ExtentTransViewRetriever &etvr,
    btreenode_pos_t pos,
    node_key_t key,
    bool data_only = false) const {
    auto &me = down_cast();
    assert(key == me.iter_idx(pos).get_key());
    auto child = this->children[pos];
    if (is_reserved_ptr(child)) {
      return true;
    } else if (is_valid_child_ptr(child)) {
      assert(dynamic_cast<CachedExtent*>(child)->is_logical());
      assert(
	dynamic_cast<CachedExtent*>(child)->is_pending_in_trans(t.get_trans_id())
	|| me.is_stable_ready());
      if (data_only) {
	return etvr.is_viewable_extent_data_stable(
	  t, dynamic_cast<CachedExtent*>(child));
      } else {
	return etvr.is_viewable_extent_stable(
	  t, dynamic_cast<CachedExtent*>(child));
      }
    } else if (me.is_pending()) {
      auto key = me.iter_idx(pos).get_key();
      auto &sparent = me.get_stable_for_key(key);
      auto spos = sparent.lower_bound(key).get_offset();
      auto child = sparent.children[spos];
      if (is_valid_child_ptr(child)) {
	assert(dynamic_cast<CachedExtent*>(child)->is_logical());
	if (data_only) {
	  return etvr.is_viewable_extent_data_stable(
	    t, dynamic_cast<CachedExtent*>(child));
	} else {
	  return etvr.is_viewable_extent_stable(
	    t, dynamic_cast<CachedExtent*>(child));
	}
      } else {
	return true;
      }
    } else {
      return true;
    }
  }

  parent_tracker_t<T>* my_tracker = nullptr;
private:
  T& down_cast() {
    return *static_cast<T*>(this);
  }
  const T& down_cast() const {
    return *static_cast<const T*>(this);
  }
  void maybe_expand_children(size_t size) {
    if constexpr (T::CHILD_VEC_UNIT) {
      if (size > children.capacity()) {
	children.resize(p2roundup(size, (size_t)T::CHILD_VEC_UNIT));
      }
    } else {
      // children.capacity() is static and assigned upon construction
      assert(size <= children.capacity());
    }
  }
  void maybe_shink_children() {
    if constexpr (T::CHILD_VEC_UNIT) {
      auto &me = down_cast();
      if (children.capacity() > T::CHILD_VEC_UNIT &&
	  me.get_size() < (children.capacity() / 3 /*should be parameterized*/)) {
	children.resize(p2roundup(me.get_size(), T::CHILD_VEC_UNIT));
	children.shrink_to_fit();
      }
    }
  }

  std::vector<BaseChildNode<T, node_key_t>*> children;
  std::set<TCachedExtentRef<T>, Comparator> copy_sources;

  // copy dests points from a stable node back to its pending nodes
  // having copy sources at the same tree level, it serves as a two-level index:
  // transaction-id then node-key to the pending node.
  //
  // The copy dest pointers must be symmetric to the copy source pointers.
  //
  // copy_dests_t will be automatically unregisterred upon transaction destruction,
  // see Transaction::views
  struct copy_dests_t : trans_spec_view_t {
    std::set<TCachedExtentRef<T>, Comparator> dests_by_key;
    copy_dests_t(Transaction &t) : trans_spec_view_t{t.get_trans_id()} {}
    ~copy_dests_t() {
      LOG_PREFIX(~copy_dests_t);
      SUBTRACE(seastore_fixedkv_tree, "copy_dests_t destroyed");
    }
  };

  using trans_view_set_t = trans_spec_view_t::trans_view_set_t;
  trans_view_set_t copy_dests_by_trans;
  template <typename, typename, typename>
  friend class ChildNode;
  template <typename>
  friend class parent_tracker_t;
  template <typename>
  friend class child_pos_t;
#ifdef UNIT_TESTS_BUILT
  template <typename, typename, typename, typename, typename, size_t>
  friend class FixedKVBtree;
#endif
};

// ChildNodes are non-root nodes in the tree or extents that
// have parents, e.g. LogicalCachedExtents have LBALeafnodes
// as the parents, so they are ChildNodes.
template <typename ParentT, typename T, typename key_t>
class ChildNode : public BaseChildNode<ParentT, key_t> {
public:
  using get_parent_node_iertr = get_child_iertr;
  using get_parent_node_ret =
    get_parent_node_iertr::future<TCachedExtentRef<ParentT>>;
  get_parent_node_ret get_parent_node(
    Transaction &t,
    ExtentTransViewRetriever &etvr)
  {
    auto &me = down_cast();
    if (this->has_parent_tracker()) {
      return this->_get_parent_node(t, etvr, me.get_begin());
    } else {
      assert(me.is_mutation_pending());
      auto prior = me.get_prior_instance()->template cast<T>();
      return prior->_get_parent_node(t, etvr, prior->get_begin());
    }
  }

protected:
  void on_invalidated() {
    this->reset_parent_tracker();
  }
  void take_parent_from_prior() {
    _take_parent_from_prior();
  }
  void on_replace_prior() {
    take_parent_from_prior();
  }
  // destroy() is allowed to be skipped for the pending extents,
  // because they will be destroyed altogether with their parents
  // upon transaction invalidation.
  void destroy() {
    assert(!down_cast().is_btree_root());
    assert(this->has_parent_tracker());
    auto off = get_parent_pos();
    auto parent = this->peek_parent_node();
    assert(parent->children[off] == &down_cast());
    parent->children[off] = nullptr;
  }
private:
  T& down_cast() {
    return *static_cast<T*>(this);
  }
  const T& down_cast() const {
    return *static_cast<const T*>(this);
  }

  get_parent_node_ret _get_parent_node(
    Transaction &t,
    ExtentTransViewRetriever &etvr,
    key_t key)
  {
    return etvr.maybe_wait_accessible(
      t, *this->peek_parent_node()
    ).si_then([&t, key, this] {
      auto parent = this->peek_parent_node();
      return parent->resolve_transaction(t, key).second;
    });
  }

  void _take_parent_from_prior() {
    auto &me = down_cast();
    assert(!me.is_btree_root());
    auto &prior = static_cast<T&>(*me.get_prior_instance());
    this->parent_tracker = prior.BaseChildNode<ParentT, key_t>::parent_tracker;
    assert(this->has_parent_tracker());
    auto off = get_parent_pos();
    auto parent = this->peek_parent_node();
    assert(me.get_prior_instance().get() ==
	   dynamic_cast<CachedExtent*>(parent->children[off]));
    parent->children[off] = &me;
  }

  btreenode_pos_t get_parent_pos() const {
    auto &me = down_cast();
    auto parent = this->peek_parent_node();
    assert(parent);
    //TODO: can this search be avoided?
    auto key = me.get_begin();
    auto iter = parent->lower_bound(key);
    if (iter.get_key() > key) {
      assert(iter != parent->end());
      iter--;
    }
    assert(iter.get_key() == me.get_begin());
    return iter.get_offset();
  }
  bool _is_valid() const final {
    return down_cast().is_valid();
  }
  bool _is_stable() const final {
    return down_cast().is_stable();
  }
  key_t node_begin() const final {
    return down_cast().get_begin();
  }
};

template <typename T>
std::ostream &operator<<(std::ostream &out, const parent_tracker_t<T> &tracker) {
  return out << "tracker_ptr=" << (void*)&tracker
	     << ", parent_ptr=" << (void*)tracker.get_parent().get();
}

} // namespace crimson::os::seastore
