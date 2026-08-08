#include "et_feeder_node.h"
#include "et_feeder.h"

using namespace Chakra::FeederV3;

std::shared_ptr<const ChakraNode> ETFeederNode::get_chakra_node() const {
  if (this->chakra_node.expired()) {
    auto node = this->feeder.get_raw_chakra_node(this->node_id);
    this->chakra_node = node;
    return node;
  }
  return this->chakra_node.lock();
}

const char* ETFeederNode::known_attr_name(const KnownAttr attr) {
  static constexpr const char* kNames[] = {
#define REGISTER_ATTR_WITH_DEFAULT(attr_name, system_default_value) #attr_name,
#define REGISTER_ATTR(attr_name) #attr_name,
#include "et_feeder_node_attr.h"
  };
  return kNames[static_cast<int>(attr)];
}

const ETFeederNode::AttrMemo& ETFeederNode::memo() const {
  if (this->memo_) {
    return *this->memo_;
  }
  // One pass over the raw message: capture the node facts and the raw value
  // of every known attribute (first occurrence wins, like the linear scans
  // this replaces). The graph is static and readonly, so this is immutable.
  auto memo = std::make_unique<AttrMemo>();
  const auto node = this->get_chakra_node();
  memo->name = node->name();
  memo->runtime = node->duration_micros();
  memo->type = node->type();
  for (const auto& attr : node->attr()) {
    int idx = -1;
    for (int i = 0; i < kKnownAttrCount; ++i) {
      if (attr.name() == known_attr_name(static_cast<KnownAttr>(i))) {
        idx = i;
        break;
      }
    }
    if (idx < 0 || memo->attrs[idx].present) {
      continue;
    }
    RawAttrVal& val = memo->attrs[idx];
    val.kind = attr.value_case();
    val.present = true;
    switch (attr.value_case()) {
      case ChakraAttr::kDoubleVal:
        val.num.d = attr.double_val();
        break;
      case ChakraAttr::kFloatVal:
        val.num.f = attr.float_val();
        break;
      case ChakraAttr::kInt32Val:
        val.num.i32 = attr.int32_val();
        break;
      case ChakraAttr::kInt64Val:
        val.num.i64 = attr.int64_val();
        break;
      case ChakraAttr::kUint32Val:
        val.num.u32 = attr.uint32_val();
        break;
      case ChakraAttr::kUint64Val:
        val.num.u64 = attr.uint64_val();
        break;
      case ChakraAttr::kSint32Val:
        val.num.i32 = attr.sint32_val();
        break;
      case ChakraAttr::kSint64Val:
        val.num.i64 = attr.sint64_val();
        break;
      case ChakraAttr::kFixed32Val:
        val.num.u32 = attr.fixed32_val();
        break;
      case ChakraAttr::kFixed64Val:
        val.num.u64 = attr.fixed64_val();
        break;
      case ChakraAttr::kSfixed32Val:
        val.num.i32 = attr.sfixed32_val();
        break;
      case ChakraAttr::kSfixed64Val:
        val.num.i64 = attr.sfixed64_val();
        break;
      case ChakraAttr::kBoolVal:
        val.num.b = attr.bool_val();
        break;
      case ChakraAttr::kStringVal:
        val.str = attr.string_val();
        break;
      case ChakraAttr::kBytesVal:
        val.str = attr.bytes_val();
        break;
      default:
        // List or unset kinds: presence is recorded; conversion on access
        // throws the same invalid_argument as the unmemoized path.
        break;
    }
  }
  this->memo_ = std::move(memo);
  return *this->memo_;
}

bool ETFeederNode::has_attr(const std::string& attr_name) const {
  // Known attributes answer from the memo; arbitrary names fall back to
  // scanning the raw message.
  for (int i = 0; i < kKnownAttrCount; ++i) {
    if (attr_name == known_attr_name(static_cast<KnownAttr>(i))) {
      return this->memo().attrs[i].present;
    }
  }
  const auto node = this->get_chakra_node();
  for (auto& attr : node->attr())
    if (attr.name() == attr_name)
      return true;
  return false;
}

const ChakraAttr ETFeederNode::get_attr_msg(
    const std::string& attr_name) const {
  const auto node = this->get_chakra_node();
  for (auto& attr : node->attr())
    if (attr.name() == attr_name)
      return attr;
  throw std::runtime_error(
      "Attribute " + attr_name + " not found in node " +
      std::to_string(this->node_id) +
      " feeder->id=" + std::to_string(this->feeder._operator_id));
}

bool ETFeederNode::get_attr_msg(
    const std::string& attr_name,
    const ChakraAttr** attr) const {
  const auto node = this->get_chakra_node();
  for (auto& iter_attr : node->attr())
    if (iter_attr.name() == attr_name) {
      *attr = &iter_attr;
      return true;
    }
  return false;
}

ChakraAttr::ValueCase ETFeederNode::get_attr_type(
    const ChakraAttr& attr) const {
  return attr.value_case();
}

NodeId ETFeederNode::id() const {
  // node_id is the key the node was looked up with (== protobuf node.id());
  // no need to fetch the whole protobuf message.
  return this->node_id;
}

std::string ETFeederNode::name() const {
  return this->memo().name;
}

ChakraProtoMsg::NodeType ETFeederNode::type() const {
  return this->memo().type;
}

uint64_t ETFeederNode::runtime() const {
  return this->memo().runtime;
}

bool ETFeederNode::is_cpu_op() const {
  return this->is_cpu_op<bool>();
}

uint64_t ETFeederNode::num_ops() const {
  return this->num_ops<uint64_t>();
}

uint32_t ETFeederNode::tensor_loc() const {
  return this->tensor_loc<uint32_t>();
}

uint64_t ETFeederNode::tensor_size() const {
  return this->tensor_size<uint64_t>();
}

ChakraProtoMsg::CollectiveCommType ETFeederNode::comm_type() const {
  return static_cast<ChakraProtoMsg::CollectiveCommType>(
      this->comm_type<uint64_t>());
}

uint32_t ETFeederNode::comm_priority() const {
  return this->comm_priority<uint32_t>();
}

uint64_t ETFeederNode::comm_size() const {
  return this->comm_size<uint64_t>();
}

uint32_t ETFeederNode::comm_src() const {
  return this->comm_src<uint32_t>();
}

uint32_t ETFeederNode::comm_dst() const {
  return this->comm_dst<uint32_t>();
}

uint32_t ETFeederNode::comm_tag() const {
  return this->comm_tag<uint32_t>();
}

std::string ETFeederNode::get_inputs_values(const std::string& default_) const {
  auto node = this->get_chakra_node();
  if (node->has_inputs()) {
    return node->inputs().values();
  }
  return default_;
}

std::string ETFeederNode::get_inputs_shapes(const std::string& default_) const {
  auto node = this->get_chakra_node();
  if (node->has_inputs()) {
    return node->inputs().shapes();
  }
  return default_;
}

std::string ETFeederNode::get_inputs_types(const std::string& default_) const {
  auto node = this->get_chakra_node();
  if (node->has_inputs()) {
    return node->inputs().types();
  }
  return default_;
}

std::string ETFeederNode::get_outputs_values(
    const std::string& default_) const {
  auto node = this->get_chakra_node();
  if (node->has_outputs()) {
    return node->outputs().values();
  }
  return default_;
}

std::string ETFeederNode::get_outputs_shapes(
    const std::string& default_) const {
  auto node = this->get_chakra_node();
  if (node->has_outputs()) {
    return node->outputs().shapes();
  }
  return default_;
}

std::string ETFeederNode::get_outputs_types(const std::string& default_) const {
  auto node = this->get_chakra_node();
  if (node->has_outputs()) {
    return node->outputs().types();
  }
  return default_;
}
