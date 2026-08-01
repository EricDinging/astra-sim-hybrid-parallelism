#ifndef CHAKRA_FEEDER_V3_DEPENDANCY_SOLVER_H
#define CHAKRA_FEEDER_V3_DEPENDANCY_SOLVER_H

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
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
  void finish_node(const NodeId& node);
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
   * maps and re-derive the dependancy-free roots. Requires a prior
   * capture_pristine() and a fully-consumed graph (no ongoing and no
   * dependancy-free nodes), i.e. a drained iteration boundary.
   */
  void reset();

  const std::unordered_set<NodeId>& get_dependancy_free_nodes() const;
  const std::unordered_set<NodeId>& get_ongoing_nodes() const;
  const std::unordered_set<NodeId>& get_children(NodeId node) const;
  const std::unordered_set<NodeId>& get_parents(NodeId node) const;

 private:
  std::unordered_map<NodeId, std::unordered_set<NodeId>> child_map_parent;
  std::unordered_map<NodeId, std::unordered_set<NodeId>> parent_map_child;
  std::unordered_set<NodeId> dependancy_free_nodes;
  std::unordered_set<NodeId> ongoing_nodes;
  // Pristine node->parents edges captured by capture_pristine(); empty until
  // then. parent_map_child is re-derived from it on reset() by inversion.
  std::unordered_map<NodeId, std::unordered_set<NodeId>>
      pristine_child_map_parent;
  bool pristine_captured = false;
  bool dirty = true;
  void _helper_allocate_bucket(NodeId node_id);
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
  void finish_node(const NodeId& node);
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
