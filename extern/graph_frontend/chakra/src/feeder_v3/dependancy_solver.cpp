#include "dependancy_solver.h"
#include <iostream>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>
using namespace Chakra::FeederV3;

void _DependancyLayer::add_node(
    const NodeId& node,
    const std::unordered_set<NodeId>& parents) {
  this->dirty = true;
  this->_helper_allocate_bucket(node);
  for (auto& parent : parents) {
    this->_helper_allocate_bucket(parent);
    this->child_map_parent[node].insert(parent);
    this->parent_map_child[parent].insert(node);
  }
}

void _DependancyLayer::add_node_children(
    const NodeId& node,
    const std::unordered_set<NodeId>& children) {
  this->dirty = true;
  this->_helper_allocate_bucket(node);
  for (auto& child : children) {
    this->_helper_allocate_bucket(child);
    this->child_map_parent[child].insert(node);
    this->parent_map_child[node].insert(child);
  }
}

void _DependancyLayer::ensure_dense_ids() {
  if (!this->ids_.empty())
    return;
  this->ids_.reserve(this->child_map_parent.size());
  NodeId max_id = 0;
  for (const auto& it : this->child_map_parent) {
    this->ids_.push_back(it.first);
    if (it.first > max_id)
      max_id = it.first;
  }
  // Direct-index when the id space is dense enough (trace nodes are
  // typically numbered 0..N-1); hash map fallback for sparse ids.
  if (max_id < this->ids_.size() * 2 + 64) {
    this->direct_index_ = true;
    this->id_to_dense_vec_.assign(max_id + 1, UINT32_MAX);
    for (uint32_t i = 0; i < this->ids_.size(); ++i)
      this->id_to_dense_vec_[this->ids_[i]] = i;
  } else {
    this->id_to_dense_map_.reserve(this->ids_.size());
    for (uint32_t i = 0; i < this->ids_.size(); ++i)
      this->id_to_dense_map_.emplace(this->ids_[i], i);
  }
}

uint32_t _DependancyLayer::dense_of(NodeId node) const {
  if (this->direct_index_) {
    if (node < this->id_to_dense_vec_.size()) {
      const uint32_t d = this->id_to_dense_vec_[node];
      if (d != UINT32_MAX)
        return d;
    }
  } else {
    const auto it = this->id_to_dense_map_.find(node);
    if (it != this->id_to_dense_map_.end())
      return it->second;
  }
  throw std::runtime_error(
      "Node " + std::to_string(node) + " unknown to dependancy layer");
}

void _DependancyLayer::seal_edges() {
  this->ensure_dense_ids();
  const size_t n = this->ids_.size();
  this->child_off_.assign(n + 1, 0);
  this->child_idx_.clear();
  this->remaining_.assign(n, 0);
  for (uint32_t i = 0; i < n; ++i) {
    const NodeId id = this->ids_[i];
    // Freeze the children in the parent_map_child set's iteration order:
    // that is exactly the order the map-based finish_node visited them, so
    // downstream insertion sequences stay identical.
    const auto pmc_it = this->parent_map_child.find(id);
    if (pmc_it != this->parent_map_child.end()) {
      for (const NodeId child : pmc_it->second)
        this->child_idx_.push_back(this->dense_of(child));
    }
    this->child_off_[i + 1] = static_cast<uint32_t>(this->child_idx_.size());
    const auto cmp_it = this->child_map_parent.find(id);
    this->remaining_[i] = cmp_it == this->child_map_parent.end()
        ? 0
        : static_cast<uint32_t>(cmp_it->second.size());
  }
  // The maps are dead from here on; free their memory.
  this->child_map_parent = {};
  this->parent_map_child = {};
  this->sealed_ = true;
}

void _DependancyLayer::take_node(const NodeId& node) {
  if (this->dirty) {
    throw std::runtime_error(
        "dependancy layer is dirty, resolve_dependancy_free_nodes should be called first");
  }
  if (!this->sealed_)
    this->seal_edges();
  // Per-node hot path: validate via the mutation's own return value instead
  // of a separate find per set (erase/insert report presence), halving the
  // hash probes. Same mutations, same errors on corrupt state.
  if (this->dependancy_free_nodes.erase(node) == 0) {
    throw std::runtime_error(
        "Node " + std::to_string(node) +
        " is not dependancy free or already taken/released");
  }
  if (!this->ongoing_nodes.insert(node).second) {
    throw std::runtime_error("Node is already taken");
  }
}

void _DependancyLayer::finish_node(
    const NodeId& node,
    std::vector<NodeId>* freed) {
  if (this->dirty) {
    throw std::runtime_error(
        "dependancy layer is dirty, resolve_dependancy_free_nodes should be called first");
  }
  if (this->ongoing_nodes.erase(node) == 0) {
    throw std::runtime_error("Node is not taken");
  }
  // finish_node is only reachable after take_node (which seals), so the CSR
  // is live here: one dense lookup for the node, then a refcount decrement
  // per child -- no per-child hash traffic, no map erases.
  const uint32_t d = this->dense_of(node);
  const uint32_t begin = this->child_off_[d];
  const uint32_t end = this->child_off_[d + 1];
  for (uint32_t k = begin; k < end; ++k) {
    const uint32_t child = this->child_idx_[k];
    if (--this->remaining_[child] == 0) {
      const NodeId child_id = this->ids_[child];
      this->dependancy_free_nodes.insert(child_id);
      if (freed != nullptr) {
        freed->push_back(child_id);
      }
    }
  }
}

void _DependancyLayer::push_back_node(const NodeId& node) {
  if (this->dirty) {
    throw std::runtime_error(
        "dependancy layer is dirty, resolve_dependancy_free_nodes should be called first");
  }
  if (this->ongoing_nodes.erase(node) == 0) {
    throw std::runtime_error("Node is not taken");
  }
  this->dependancy_free_nodes.insert(node);
}

void _DependancyLayer::resolve_dependancy_free_nodes() {
  if ((!this->dependancy_free_nodes.empty()) || (!this->ongoing_nodes.empty()))
    throw std::runtime_error(
        "resolve_dependancy_free_nodes after initialization is not supported yet!");
  for (auto& it : this->child_map_parent) {
    auto& node = it.first;
    auto& parents = it.second;
    if (parents.empty())
      this->dependancy_free_nodes.insert(node);
  }
  if (this->dependancy_free_nodes.empty())
    throw std::runtime_error(
        "No dependancy free nodes found, there might be deadlocks");
  this->dirty = false;
}

void _DependancyLayer::capture_pristine() {
  if (this->dirty) {
    throw std::runtime_error(
        "capture_pristine requires a sealed layer (call resolve_dependancy_free_nodes first)");
  }
  if (!this->ongoing_nodes.empty()) {
    throw std::runtime_error(
        "capture_pristine must run before any node is taken");
  }
  // Record, as compact arrays, exactly what the map-based reset() used to
  // rebuild: it copied child_map_parent, then iterated the copy inverting
  // edges into a fresh parent_map_child and seeding the roots in copy
  // order. Perform that dance once here on scratch containers (the copy's
  // and the fresh sets' iteration orders are what downstream consumers
  // observed), freeze the result, and drop the scratch. reset() is then a
  // cheap array install with bit-identical insertion sequences.
  this->ensure_dense_ids();
  const auto scratch_cmp = this->child_map_parent; // the copy, as before
  std::unordered_map<NodeId, std::unordered_set<NodeId>> scratch_pmc;
  this->pristine_seed_.clear();
  for (const auto& it : scratch_cmp) {
    scratch_pmc.try_emplace(it.first);
    for (const auto& parent : it.second) {
      scratch_pmc[parent].insert(it.first);
    }
    if (it.second.empty()) {
      this->pristine_seed_.push_back(it.first);
    }
  }
  const size_t n = this->ids_.size();
  this->pristine_child_off_.assign(n + 1, 0);
  this->pristine_child_idx_.clear();
  this->pristine_remaining_.assign(n, 0);
  for (uint32_t i = 0; i < n; ++i) {
    const NodeId id = this->ids_[i];
    const auto pmc_it = scratch_pmc.find(id);
    if (pmc_it != scratch_pmc.end()) {
      for (const NodeId child : pmc_it->second)
        this->pristine_child_idx_.push_back(this->dense_of(child));
    }
    this->pristine_child_off_[i + 1] =
        static_cast<uint32_t>(this->pristine_child_idx_.size());
    const auto cmp_it = scratch_cmp.find(id);
    this->pristine_remaining_[i] = cmp_it == scratch_cmp.end()
        ? 0
        : static_cast<uint32_t>(cmp_it->second.size());
  }
  this->pristine_captured = true;
}

void _DependancyLayer::reset() {
  if (!this->pristine_captured) {
    throw std::runtime_error("reset without a prior capture_pristine");
  }
  if (!this->ongoing_nodes.empty() || !this->dependancy_free_nodes.empty()) {
    throw std::runtime_error(
        "reset requires a fully-consumed graph (drained iteration boundary)");
  }
  // Install the pristine replica (copies: reset can run again at the next
  // iteration boundary). Seed insertion order matches the map-based rebuild.
  this->child_off_ = this->pristine_child_off_;
  this->child_idx_ = this->pristine_child_idx_;
  this->remaining_ = this->pristine_remaining_;
  this->sealed_ = true;
  this->child_map_parent = {};
  this->parent_map_child = {};
  for (const NodeId node : this->pristine_seed_) {
    this->dependancy_free_nodes.insert(node);
  }
  if (this->dependancy_free_nodes.empty()) {
    throw std::runtime_error(
        "No dependancy free nodes found after reset, there might be deadlocks");
  }
  this->dirty = false;
}

const std::unordered_set<NodeId>& _DependancyLayer::get_dependancy_free_nodes()
    const {
  return this->dependancy_free_nodes;
}

const std::unordered_set<NodeId>& _DependancyLayer::get_children(
    NodeId node) const {
  if (this->sealed_) {
    throw std::logic_error(
        "get_children is a load-time API; edges are sealed after the first take_node");
  }
  return this->parent_map_child.at(node);
}

const std::unordered_set<NodeId>& _DependancyLayer::get_parents(
    NodeId node) const {
  if (this->sealed_) {
    throw std::logic_error(
        "get_parents is a load-time API; edges are sealed after the first take_node");
  }
  return this->child_map_parent.at(node);
}

const std::unordered_set<NodeId>& _DependancyLayer::get_ongoing_nodes() const {
  return this->ongoing_nodes;
}

void _DependancyLayer::_helper_allocate_bucket(NodeId node_id) {
  // Single probe per map: try_emplace default-constructs the bucket iff
  // absent (identical semantics to the old find + operator[]).
  this->child_map_parent.try_emplace(node_id);
  this->parent_map_child.try_emplace(node_id);
}

void DependancyResolver::add_node(const ChakraNode& node) {
  NodeId node_id = node.id();
  std::unordered_set<NodeId> parents, enabled_parents;

  for (auto& parent : node.data_deps()) {
    if (this->enable_data_deps)
      enabled_parents.insert(parent);
    parents.insert(parent);
  }

  this->data_dependancy.add_node(node_id, parents);
  parents.clear();

  for (auto& parent : node.ctrl_deps()) {
    if (this->enable_ctrl_deps)
      enabled_parents.insert(parent);
    parents.insert(parent);
  }
  this->ctrl_dependancy.add_node(node_id, parents);
  parents.clear();

  this->enabled_dependancy.add_node(node_id, enabled_parents);
}

// Runtime state transitions touch only the enabled layer: after the
// load-time graph_sanity_check nothing ever reads the data/ctrl layers
// (every runtime query -- get_dependancy_free_nodes, get_ongoing_nodes,
// get_children -- routes to enabled), and free_load_time_layers() drops
// them right after construction.
void DependancyResolver::take_node(const NodeId& node) {
  this->enabled_dependancy.take_node(node);
}

void DependancyResolver::push_back_node(const NodeId& node) {
  this->enabled_dependancy.push_back_node(node);
}

void DependancyResolver::finish_node(
    const NodeId& node,
    std::vector<NodeId>* freed) {
  this->enabled_dependancy.finish_node(node, freed);
}

void DependancyResolver::resolve_dependancy_free_nodes() {
  this->data_dependancy.resolve_dependancy_free_nodes();
  this->ctrl_dependancy.resolve_dependancy_free_nodes();
  this->enabled_dependancy.resolve_dependancy_free_nodes();
}

void DependancyResolver::capture_pristine() {
  // Only the enabled layer is consumed at runtime (see take/finish above),
  // so only it needs a pristine snapshot for reset(). The data/ctrl layers
  // may already be freed by free_load_time_layers() at this point.
  this->enabled_dependancy.capture_pristine();
}

void DependancyResolver::reset() {
  this->enabled_dependancy.reset();
}

void DependancyResolver::free_load_time_layers() {
  // The data/ctrl layers exist only to build the enabled layer and to run
  // the load-time graph_sanity_check; nothing reads them afterwards. Free
  // their edge maps to cut per-feeder memory.
  this->data_dependancy = _DependancyLayer();
  this->ctrl_dependancy = _DependancyLayer();
}

const std::unordered_set<NodeId>& DependancyResolver::
    get_dependancy_free_nodes() const {
  return this->enabled_dependancy.get_dependancy_free_nodes();
}

const std::unordered_set<NodeId>& DependancyResolver::get_ongoing_nodes()
    const {
  return this->enabled_dependancy.get_ongoing_nodes();
}

const _DependancyLayer& DependancyResolver::get_data_dependancy() const {
  return this->data_dependancy;
}

const _DependancyLayer& DependancyResolver::get_ctrl_dependancy() const {
  return this->ctrl_dependancy;
}

const _DependancyLayer& DependancyResolver::get_enabled_dependancy() const {
  return this->enabled_dependancy;
}

_DependancyLayer& DependancyResolver::get_data_dependancy_mut() {
  return this->data_dependancy;
}

_DependancyLayer& DependancyResolver::get_ctrl_dependancy_mut() {
  return this->ctrl_dependancy;
}

_DependancyLayer& DependancyResolver::get_enabled_dependancy_mut() {
  return this->enabled_dependancy;
}
