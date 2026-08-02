#ifndef CHAKRA_FEEDER_V3_COMMON_H
#define CHAKRA_FEEDER_V3_COMMON_H
#include <cstdint>
#include "et_def.pb.h"

namespace Chakra {
namespace FeederV3 {
using NodeId = uint64_t;
using ETFeederId = uint64_t;
using ChakraNode = ChakraProtoMsg::Node;
using ChakraGlobalMetadata = ChakraProtoMsg::GlobalMetadata;
using ChakraAttr = ChakraProtoMsg::AttributeProto;

constexpr static bool ALLOW_IMPLICIT_INTEGER_CONVERSION = true;
constexpr static bool ALLOW_IMPLICIT_FLOAT_CONVERSION = true;
constexpr static bool ALLOW_IMPLICIT_INTEGER_TO_FLOAT_CONVERSION = true;
constexpr static bool ALLOW_IMPLICIT_FLOAT_TO_INTEGER_CONVERSION = false;
constexpr static bool NO_IMPLICIT_CONVERSION = false;
constexpr static bool DEFAULT_STRICT_TYPING = false;

// Capacity (in nodes) of the process-wide raw-ChakraNode cache shared by
// every rank feeder. Sized to hold the live working set of a saturated
// 16x16x16 multi-tenant run (4096 concurrent rank traces x hundreds of
// nodes): the previous 16384 thrashed there -- every miss is a file
// seek/read + protobuf parse + an eviction destroying another node, ~10% of
// runtime (fix measured at -22% wall on the 16^3 probe, byte-identical).
// Worst-case memory is capacity x ~1 KB/node; override with the
// CHAKRA_NODE_CACHE_SIZE env var to trade RSS vs re-parse rate (e.g. for
// hosts packing many concurrent sims).
constexpr static size_t DEFAULT_ETFEEDER_CACHE_SIZE = 4194304;
constexpr static bool RESOLVE_DATA_DEPS = true;
constexpr static bool RESOLVE_CTRL_DEPS = true;

constexpr static size_t DEFAULT_PROTOBUF_BUFFER_SIZE = 16384;

} // namespace FeederV3
} // namespace Chakra

#endif
