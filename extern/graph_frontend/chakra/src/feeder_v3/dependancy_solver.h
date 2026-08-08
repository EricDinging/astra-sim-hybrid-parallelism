#ifndef CHAKRA_FEEDER_V3_DEPENDANCY_SOLVER_H
#define CHAKRA_FEEDER_V3_DEPENDANCY_SOLVER_H

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common.h"
#include "et_def.pb.h"

namespace Chakra {
namespace FeederV3 {

class _DependancyLayer {
 public:
  _DependancyLayer() = default;
  ~_DependancyLayer() {
    this->child_map_parent.clear();
    this->parent_map_child.clear();
    this->dependancy_free_nodes.clear();
    this->ongoing_nodes.clear();
  }
  /**
   * @brief The node has three possible states in a process of resolving
   * dependancy
   *  1. Pending, which means this node is not processed yet, and might be taken
   * if all its parents released.
   *  2. Taken, which means this node is taken by a process, but still in
   * progress. It shouldnt be taken again by other process.
   *  3. Finished(Not in Graph), which means this node is finished.
   *     The child of it may be released if all its parents is finished.
   *  Finished --add--> Pending --take--> Taken --finish--> Finished
   *  Taken --push_back--> Pending
   */
  void add_node(const NodeId& node, const std::unordered_set<NodeId>& parents);
  void add_node_children(
      const NodeId& node,
      const std::unordered_set<NodeId>& children);
  void take_node(const NodeId& node);
  // If `freed` is non-null, the children that became dependancy-free by this
  // finish are appended to it (exactly the nodes inserted into
  // dependancy_free_nodes by this call).
  void finish_node(const NodeId& node, std::vector<NodeId>* freed = nullptr);
  void push_back_node(const NodeId& node);
  void resolve_dependancy_free_nodes();

  /**
   * @brief Snapshot the (sealed, not-yet-consumed) edge structure so the
   * layer can later be rewound with reset(). finish_node() consumes the
   * graph destructively, so the snapshot must be taken before the first
   * finish -- right after resolve_dependancy_free_nodes(). Opt-in: callers
   * that never reset (throwaway scan feeders, one-shot runs) skip the
   * memory cost of the copy.
   */
  void capture_pristine();

  /**
   * @brief Rewind the layer to the captured pristine state: restore the edge
   * structure and re-derive the dependancy-free roots. Requires a prior
   * capture_pristine() and a fully-consumed graph (no ongoing and no
   * dependancy-free nodes), i.e. a drained iteration boundary.
   */
  void reset();

  const std::unordered_set<NodeId>& get_dependancy_free_nodes() const;
  const std::unordered_set<NodeId>& get_ongoing_nodes() const;
  // Edge queries are load-time-only APIs (sanity checks, tests): they read
  // the edge maps, which are frozen into the sealed CSR at the first
  // take_node() and then freed. Calling them after that throws.
  const std::unordered_set<NodeId>& get_children(NodeId node) const;
  const std::unordered_set<NodeId>& get_parents(NodeId node) const;

 private:
  std::unordered_map<NodeId, std::unordered_set<NodeId>> child_map_parent;
  std::unordered_map<NodeId, std::unordered_set<NodeId>> parent_map_child;
  std::unordered_set<NodeId> dependancy_free_nodes;
  std::unordered_set<NodeId> ongoing_nodes;
  bool pristine_captured = false;
  bool dirty = true;
  void _helper_allocate_bucket(NodeId node_id);

  // --- Sealed edge structure -------------------------------------------
  // The maps above exist only while the graph is being built and scanned by
  // load-time code. At the first take_node() the edges are frozen into a
  // CSR (children lists per node, in the exact iteration order of the
  // parent_map_child sets, so the sequence of insertions into
  // dependancy_free_nodes is bit-for-bit the same as the map-based
  // finish_node produced) plus a remaining-parent refcount per node, and
  // the maps are freed. finish_node then costs one hash probe for the node
  // and O(1) array work per child, instead of a hash find + set erase per
  // child and two map erases per node.
  bool sealed_ = false;
  std::vector<NodeId> ids_; // dense index -> node id
  // node id -> dense index: direct vector when ids are dense (the common
  // case: trace nodes are numbered 0..N-1), hash map fallback otherwise
  bool direct_index_ = false;
  std::vector<uint32_t> id_to_dense_vec_;
  std::unordered_map<NodeId, uint32_t> id_to_dense_map_;
  std::vector<uint32_t> child_off_; // CSR offsets, size ids_.size()+1
  std::vector<uint32_t> child_idx_; // children as dense indices
  std::vector<uint32_t> remaining_; // live parent count per dense node
  // Pristine replica recorded by capture_pristine(): the CSR and seed order
  // that the map-based reset() would have produced, so reset() is a cheap
  // array install instead of a map copy + rebuild.
  std::vector<uint32_t> pristine_child_off_;
  std::vector<uint32_t> pristine_child_idx_;
  std::vector<uint32_t> pristine_remaining_;
  std::vector<NodeId> pristine_seed_;

  void ensure_dense_ids();
  void seal_edges();
  uint32_t dense_of(NodeId node) const;
};

class DependancyResolver {
 public:
  DependancyResolver(bool enable_data_deps, bool enable_ctrl_deps)
      : enable_data_deps(enable_data_deps), enable_ctrl_deps(enable_ctrl_deps) {
    if (!enable_data_deps)
      if (!enable_ctrl_deps)
        throw std::runtime_error(
            "Should not create a dependancy resolver that resolves neither data nor control dependancy");
  }
  void add_node(const ChakraNode& node);
  void take_node(const NodeId& node);
  void push_back_node(const NodeId& node);
  // See _DependancyLayer::finish_node: `freed` optionally collects the
  // children this finish made dependancy-free.
  void finish_node(const NodeId& node, std::vector<NodeId>* freed = nullptr);
  void resolve_dependancy_free_nodes();
  // See _DependancyLayer::capture_pristine()/reset(): snapshot the sealed
  // graph, and rewind to it at a drained iteration boundary (avoids
  // re-parsing the trace to rebuild the resolver). Only the enabled layer
  // is snapshotted/rewound: it is the only layer consumed at runtime.
  void capture_pristine();
  void reset();
  // Drop the data/ctrl layers once load-time sanity checks are done; they
  // are never read afterwards (all runtime queries route to the enabled
  // layer). Saves memory at high rank counts.
  void free_load_time_layers();

  const std::unordered_set<NodeId>& get_dependancy_free_nodes() const;
  const std::unordered_set<NodeId>& get_ongoing_nodes() const;
  const _DependancyLayer& get_data_dependancy() const;
  const _DependancyLayer& get_ctrl_dependancy() const;
  const _DependancyLayer& get_enabled_dependancy() const;

  // Warning: It is user's responsibility to make sure different layer's
  // dependancy are consistent.
  _DependancyLayer& get_data_dependancy_mut();
  _DependancyLayer& get_ctrl_dependancy_mut();
  _DependancyLayer& get_enabled_dependancy_mut();

 private:
  bool enable_data_deps;
  bool enable_ctrl_deps;
  _DependancyLayer data_dependancy;
  _DependancyLayer ctrl_dependancy;
  _DependancyLayer enabled_dependancy;
};

} // namespace FeederV3
} // namespace Chakra

#endif
