#ifndef CHAKRA_FEEDER_V3_ET_FEEDER_NODE_H
#define CHAKRA_FEEDER_V3_ET_FEEDER_NODE_H

#include <functional>
#include <memory>
#include "common.h"
#include "et_def.pb.h"

namespace Chakra {
namespace FeederV3 {
class ETFeeder;

class ETFeederNode {
 public:
  ETFeederNode(ETFeeder& etfeeder, NodeId node_id)
      : feeder(etfeeder), node_id(node_id) {}

  /// The closed set of attribute names the typed accessors below use,
  /// generated from et_feeder_node_attr.h. Values of these attributes are
  /// memoized per node on first access (one scan of the protobuf attribute
  /// list instead of a string-compare scan per accessor call).
  enum class KnownAttr : int {
#define REGISTER_ATTR_WITH_DEFAULT(attr_name, system_default_value) attr_name,
#define REGISTER_ATTR(attr_name) attr_name,
#include "et_feeder_node_attr.h"
    kCount
  };
  static constexpr int kKnownAttrCount = static_cast<int>(KnownAttr::kCount);

  bool has_attr(const std::string& attr_name) const;

  const ChakraAttr get_attr_msg(const std::string& attr_name) const;
  bool get_attr_msg(const std::string& attr_name, const ChakraAttr** attr)
      const;

  ChakraAttr::ValueCase get_attr_type(const ChakraAttr& attr) const;

  template <typename T>
  T get_attr(
      const ChakraAttr& attr,
      const bool strict_type = DEFAULT_STRICT_TYPING) const;

  template <typename T>
  T get_attr(const std::string& attr_name, const T& default_value) const;

  template <typename T>
  T get_attr(const std::string& attr_name) const;

  // Memoized typed accessors for the KnownAttr set: same conversion
  // semantics (and same errors) as the string-keyed get_attr overloads,
  // reading from the per-node memo instead of rescanning the protobuf.
  template <typename T>
  T get_attr(KnownAttr attr, const T& default_value) const;

  template <typename T>
  T get_attr(KnownAttr attr) const;

#define REGISTER_ATTR_WITH_DEFAULT(attr_name, system_default_value)         \
  template <typename T>                                                     \
  T attr_name() const {                                                     \
    return this->get_attr<T>(KnownAttr::attr_name, (system_default_value)); \
  }                                                                         \
  template <typename T>                                                     \
  T attr_name(const T& default_value) const {                               \
    return this->get_attr<T>(KnownAttr::attr_name, default_value);          \
  }

#define REGISTER_ATTR(attr_name)                                   \
  template <typename T>                                            \
  T attr_name() const {                                            \
    return this->get_attr<T>(KnownAttr::attr_name);                \
  }                                                                \
  template <typename T>                                            \
  T attr_name(const T& default_value) const {                      \
    return this->get_attr<T>(KnownAttr::attr_name, default_value); \
  }

// please mod the following header to add any new attributes
#include "et_feeder_node_attr.h"

  NodeId id() const;
  std::string name() const;
  ChakraProtoMsg::NodeType type() const;
  uint64_t runtime() const;

  // old interface
  bool is_cpu_op() const;
  uint64_t num_ops() const;
  uint32_t tensor_loc() const;
  uint64_t tensor_size() const;
  ChakraProtoMsg::CollectiveCommType comm_type() const;
  uint32_t comm_priority() const;
  uint64_t comm_size() const;
  uint32_t comm_src() const;
  uint32_t comm_dst() const;
  uint32_t comm_tag() const;

  std::string get_inputs_values(const std::string& default_ = "") const;
  std::string get_inputs_shapes(const std::string& default_ = "") const;
  std::string get_inputs_types(const std::string& default_ = "") const;
  std::string get_outputs_values(const std::string& default_ = "") const;
  std::string get_outputs_shapes(const std::string& default_ = "") const;
  std::string get_outputs_types(const std::string& default_ = "") const;

 private:
  template <typename T>
  class _TypeConverter {
   public:
    template <typename F>
    static std::enable_if_t<std::is_same_v<F, T>, T> strict_converter(F value) {
      return value;
    }

    template <typename F>
    static std::enable_if_t<!std::is_same_v<F, T>, T> strict_converter(
        F value) {
      throw std::bad_cast();
    }

    template <typename F>
    static T flagged_implicit_converter(F value) {
      if constexpr (std::is_integral_v<T>) {
        // integer to integer
        if constexpr (
            ALLOW_IMPLICIT_INTEGER_CONVERSION && std::is_integral_v<F>)
          return static_cast<T>(value);
        // float to integer
        if constexpr (
            ALLOW_IMPLICIT_FLOAT_TO_INTEGER_CONVERSION &&
            std::is_floating_point_v<F>)
          return static_cast<T>(value);
      } else if constexpr (std::is_floating_point_v<T>) {
        // float to float
        if constexpr (
            ALLOW_IMPLICIT_FLOAT_CONVERSION && std::is_floating_point_v<F>)
          return static_cast<T>(value);
        // integer to float
        if constexpr (
            ALLOW_IMPLICIT_INTEGER_TO_FLOAT_CONVERSION && std::is_integral_v<F>)
          return static_cast<T>(value);
      }
      return strict_converter(value);
    }

    template <typename F>
    static std::enable_if_t<std::is_convertible_v<F, T>, T> implicit_converter(
        F value) {
      return static_cast<T>(value);
    }

    template <typename F>
    static std::enable_if_t<!std::is_convertible_v<F, T>, T> implicit_converter(
        F value) {
      throw std::bad_cast();
    }
  };
  // ETFeederNode only store minimal thing to reduce memory usage.
  ETFeeder& feeder;
  NodeId node_id;
  mutable std::weak_ptr<const ChakraNode> chakra_node;

  /// One known attribute's raw value: the protobuf value_case plus the
  /// scalar bits (or the string for string/bytes kinds). Conversion to the
  /// caller's type happens on access via the same converter switch as the
  /// unmemoized path, so results and errors are identical.
  struct RawAttrVal {
    ChakraAttr::ValueCase kind = ChakraAttr::VALUE_NOT_SET;
    bool present = false;
    union {
      double d;
      float f;
      int32_t i32;
      int64_t i64;
      uint32_t u32;
      uint64_t u64;
      bool b;
    } num = {0.0};
    std::string str; // kStringVal / kBytesVal only
  };

  /// Immutable node facts captured in one pass over the protobuf message
  /// (the graph is static and readonly): the accessors on this class are
  /// called several times per node visit, and each used to re-lock the raw
  /// node cache and linearly rescan the attribute list with string
  /// compares. Built lazily on first access.
  struct AttrMemo {
    std::string name;
    uint64_t runtime = 0; // duration_micros
    ChakraProtoMsg::NodeType type = ChakraProtoMsg::COMP_NODE;
    RawAttrVal attrs[kKnownAttrCount];
  };
  mutable std::unique_ptr<AttrMemo> memo_;

  const AttrMemo& memo() const;
  static const char* known_attr_name(KnownAttr attr);
  template <typename T>
  T convert_raw(const RawAttrVal& val) const;

  std::shared_ptr<const ChakraNode> get_chakra_node() const;
};

template <typename T>
T ETFeederNode::get_attr(const ChakraAttr& attr, const bool strict_type) const {
  {
    // change to implicit if user prefer here.
    auto cvt = [&](auto value) -> T {
      if (strict_type) {
        return _TypeConverter<T>::strict_converter(value);
      } else {
        return _TypeConverter<T>::flagged_implicit_converter(value);
      }
    };
    switch (attr.value_case()) {
      case ChakraAttr::kDoubleVal:
        return cvt(attr.double_val());
      case ChakraAttr::kFloatVal:
        return cvt(attr.float_val());
      case ChakraAttr::kInt32Val:
        return cvt(attr.int32_val());
      case ChakraAttr::kInt64Val:
        return cvt(attr.int64_val());
      case ChakraAttr::kUint32Val:
        return cvt(attr.uint32_val());
      case ChakraAttr::kUint64Val:
        return cvt(attr.uint64_val());
      case ChakraAttr::kSint32Val:
        return cvt(attr.sint32_val());
      case ChakraAttr::kSint64Val:
        return cvt(attr.sint64_val());
      case ChakraAttr::kFixed32Val:
        return cvt(attr.fixed32_val());
      case ChakraAttr::kFixed64Val:
        return cvt(attr.fixed64_val());
      case ChakraAttr::kSfixed32Val:
        return cvt(attr.sfixed32_val());
      case ChakraAttr::kSfixed64Val:
        return cvt(attr.sfixed64_val());
      case ChakraAttr::kBoolVal:
        return cvt(attr.bool_val());
      case ChakraAttr::kStringVal:
        return cvt(attr.string_val());
      case ChakraAttr::kBytesVal:
        return cvt(attr.bytes_val());
      default:
        // TODO: support list types
        // TODO: maybe let use register their own converter for complicate
        // types?
        throw std::invalid_argument(
            "Attribute type not supported, for list types please handle them manually");
    }
  }
}

template <typename T>
T ETFeederNode::get_attr(const std::string& attr_name, const T& default_value)
    const {
  // option 1 or 3: user provide default value or systemwise default value
  // Single scan via the pointer overload: avoids the previous has_attr scan
  // plus a second scan and a by-value ChakraAttr copy in get_attr_msg.
  const ChakraAttr* attr = nullptr;
  if (this->get_attr_msg(attr_name, &attr)) {
    return this->get_attr<T>(*attr, DEFAULT_STRICT_TYPING);
  }
  return default_value;
}

template <typename T>
T ETFeederNode::get_attr(const std::string& attr_name) const {
  // option 2: throw complaints
  const ChakraAttr* attr = nullptr;
  if (this->get_attr_msg(attr_name, &attr)) {
    return this->get_attr<T>(*attr, DEFAULT_STRICT_TYPING);
  }
  throw std::runtime_error(
      "Attribute " + attr_name + " not found in node " +
      std::to_string(this->node_id) +
      " feeder->id=" + std::to_string(this->feeder._operator_id));
}
template <>
inline ChakraAttr ETFeederNode::get_attr<ChakraAttr>(
    const ChakraAttr& attr,
    const bool /*strict_type*/) const {
  return attr;
}

// Mirrors get_attr(const ChakraAttr&) case for case, reading the stored raw
// value instead of the protobuf message. Same converters, same throws.
template <typename T>
T ETFeederNode::convert_raw(const RawAttrVal& val) const {
  auto cvt = [&](auto value) -> T {
    if (DEFAULT_STRICT_TYPING) {
      return _TypeConverter<T>::strict_converter(value);
    } else {
      return _TypeConverter<T>::flagged_implicit_converter(value);
    }
  };
  switch (val.kind) {
    case ChakraAttr::kDoubleVal:
      return cvt(val.num.d);
    case ChakraAttr::kFloatVal:
      return cvt(val.num.f);
    case ChakraAttr::kInt32Val:
      return cvt(val.num.i32);
    case ChakraAttr::kInt64Val:
      return cvt(val.num.i64);
    case ChakraAttr::kUint32Val:
      return cvt(val.num.u32);
    case ChakraAttr::kUint64Val:
      return cvt(val.num.u64);
    case ChakraAttr::kSint32Val:
      return cvt(val.num.i32);
    case ChakraAttr::kSint64Val:
      return cvt(val.num.i64);
    case ChakraAttr::kFixed32Val:
      return cvt(val.num.u32);
    case ChakraAttr::kFixed64Val:
      return cvt(val.num.u64);
    case ChakraAttr::kSfixed32Val:
      return cvt(val.num.i32);
    case ChakraAttr::kSfixed64Val:
      return cvt(val.num.i64);
    case ChakraAttr::kBoolVal:
      return cvt(val.num.b);
    case ChakraAttr::kStringVal:
    case ChakraAttr::kBytesVal:
      return cvt(val.str);
    default:
      throw std::invalid_argument(
          "Attribute type not supported, for list types please handle them manually");
  }
}

template <typename T>
T ETFeederNode::get_attr(const KnownAttr attr, const T& default_value) const {
  const RawAttrVal& val = this->memo().attrs[static_cast<int>(attr)];
  if (!val.present) {
    return default_value;
  }
  return this->convert_raw<T>(val);
}

template <typename T>
T ETFeederNode::get_attr(const KnownAttr attr) const {
  const RawAttrVal& val = this->memo().attrs[static_cast<int>(attr)];
  if (!val.present) {
    throw std::runtime_error(
        "Attribute " + std::string(known_attr_name(attr)) +
        " not found in node " + std::to_string(this->node_id) +
        " feeder->id=" + std::to_string(this->feeder._operator_id));
  }
  return this->convert_raw<T>(val);
}

// The ChakraAttr-typed accessor (e.g. get_attr_msg-style uses via the
// template machinery) cannot be served from the raw memo -- fall back to
// the protobuf scan, same as before.
template <>
inline ChakraAttr ETFeederNode::get_attr<ChakraAttr>(
    const KnownAttr attr) const {
  return this->get_attr<ChakraAttr>(std::string(known_attr_name(attr)));
}
template <>
inline ChakraAttr ETFeederNode::get_attr<ChakraAttr>(
    const KnownAttr attr,
    const ChakraAttr& default_value) const {
  const ChakraAttr* found = nullptr;
  if (this->get_attr_msg(known_attr_name(attr), &found)) {
    return *found;
  }
  return default_value;
}

} // namespace FeederV3
using ETFeederNode = FeederV3::ETFeederNode;
} // namespace Chakra

#endif
