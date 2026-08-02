#ifndef CHAKRA_FEEDER_V3_ET_FEEDER_H
#define CHAKRA_FEEDER_V3_ET_FEEDER_H

#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <tuple>
#include <unordered_set>
#include <vector>
#include "cache.h"
#include "common.h"
#include "dependancy_solver.h"
#include "et_def.pb.h"
#include "et_feeder_node.h"

namespace std {
template <>
struct hash<
    std::tuple<Chakra::FeederV3::ETFeederId, Chakra::FeederV3::NodeId>> {
  // splitmix64-style finalizer per field (same pattern as
  // CallbackTracker::KeyHash). The previous hash(a) ^ (hash(b) << 1) with
  // identity std::hash collapsed to a few thousand distinct values under a
  // multi-million-entry cache, producing long collision chains. Hash choice
  // only affects bucket placement, never map contents.
  static size_t mix64(uint64_t x) noexcept {
    x += 0x9e3779b97f4a7c15ULL;
    x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ULL;
    x = (x ^ (x >> 27)) * 0x94d049bb133111ebULL;
    return static_cast<size_t>(x ^ (x >> 31));
  }
  size_t operator()(
      const std::tuple<Chakra::FeederV3::ETFeederId, Chakra::FeederV3::NodeId>&
          k) const noexcept {
    return mix64(std::get<0>(k)) ^
        mix64(std::get<1>(k) ^ 0x9e3779b97f4a7c15ULL);
  }
};
} // namespace std

namespace Chakra {
namespace FeederV3 {

class ETFeeder {
 public:
  ChakraGlobalMetadata global_metadata;
  ETFeeder(const std::string& file_path)
      // Local deviation from upstream: dropped std::ios::app. chakra_file is a
      // read-only std::ifstream (no code writes to it), but std::ios::app makes
      // the underlying open() request write permission, so opening a read-only
      // trace (e.g. root-owned, generated in Docker) fails despite an access()
      // R_OK precheck passing. app has no read-side effect, so removing it is
      // safe and lets read-only traces open.
      : chakra_file(file_path, std::ios::binary | std::ios::in),
        _operator_id(_operator_id_cnt++),
        dependancy_resolver(RESOLVE_DATA_DEPS, RESOLVE_CTRL_DEPS) {
    if (!chakra_file.is_open())
      throw std::runtime_error("Failed to open file " + file_path);
    this->build_index_dependancy_cache();
    this->graph_sanity_check(); // make sure graph is sane
    // After the sanity check only the enabled layer is ever read; drop the
    // data/ctrl layers to save memory (matters at thousands of concurrent
    // rank feeders).
    this->dependancy_resolver.free_load_time_layers();
  }

  ~ETFeeder() {
    // no explict cache release. Will be kicked-out natually.
    node_obj_cache_.clear();
    index_map.clear();
    chakra_file.close();
  }

  DependancyResolver& getDependancyResolver() {
    return dependancy_resolver;
  }

  // Opt-in support for rewinding the feeder at iteration boundaries instead
  // of re-parsing the trace: capture right after construction (graph sealed,
  // nothing consumed yet), reset at a drained boundary. The trace file,
  // index_map, and the (feeder-id-keyed) node cache all stay valid across
  // resets, so a reset is pure in-memory work.
  void capture_pristine_dependancy() {
    dependancy_resolver.capture_pristine();
  }
  void reset_dependancy() {
    dependancy_resolver.reset();
  }

  std::shared_ptr<ETFeederNode> lookupNode(const NodeId& node_id);
  bool hasNodesToIssue();
  std::shared_ptr<ETFeederNode> getNextIssuableNode();
  void pushBackIssuableNode(const NodeId& node_id);
  void freeChildrenNodes(const NodeId& node_id);

  // Destructively traverse all nodes (topological order as produced by
  // dependency resolver) and collect unique communicator group ids from
  // nodes that define a pg_name attribute. Order of returned ids follows
  // first encounter order during traversal; traversal invalidates the
  // feeder state for further issuing.
  std::vector<int> traverse_comm_group();

  // legacy interface
  void addNode(std::shared_ptr<ETFeederNode> node);
  void removeNode(const NodeId& node_id);

 private:
  std::ifstream chakra_file;

  static uint64_t _operator_id_cnt;
  uint64_t _operator_id;

  // shared global cache for storing chakra msgs.
  static Cache<std::tuple<ETFeederId, NodeId>, ChakraNode> _node_cache;

  std::unordered_map<NodeId, std::streampos> index_map;
  // Per-feeder node-object cache: keeps ETFeederNode memo fields alive
  // across lookupNode calls. Single-threaded simulator, so no lock (see
  // cache.h note).
  std::unordered_map<NodeId, std::shared_ptr<ETFeederNode>> node_obj_cache_;
  DependancyResolver dependancy_resolver;

  void build_index_dependancy_cache();
  std::shared_ptr<const ChakraNode> get_raw_chakra_node(NodeId node_id);
  friend class ETFeederNode;

  void graph_sanity_check();
};

} // namespace FeederV3
using ETFeeder = FeederV3::ETFeeder;
} // namespace Chakra

#endif
