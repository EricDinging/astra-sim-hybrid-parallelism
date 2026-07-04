/******************************************************************************
This source code is licensed under the MIT license found in the
LICENSE file in the root directory of this source tree.
*******************************************************************************/

#ifndef __ASTRASIM_SCHEDULING_CHAKRATRACE_HH__
#define __ASTRASIM_SCHEDULING_CHAKRATRACE_HH__

#include <cstdint>
#include <istream>
#include <string>

namespace AstraSim {
namespace Scheduling {

// Minimal length-delimited protobuf reader for Chakra .et files. Kept as inline
// / template functions in this header so multiple TUs (AffinityMatrix,
// DurationEstimator) can share it without the ODR violation in protobuf_util.h
// (which puts a std::mutex in a header, emitting a strong definition in every
// TU that includes it).

inline bool read_varint32(std::istream& f, uint32_t& value) {
    uint8_t byte = 0;
    value = 0;
    int8_t shift = 0;
    while (f.read(reinterpret_cast<char*>(&byte), 1)) {
        value |= static_cast<uint32_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            return true;
        }
        shift += 7;
        if (shift > 28) {
            return false;
        }
    }
    return false;
}

// Read one length-delimited message: varint32(size) + serialized bytes.
// Returns false at clean EOF or on a malformed/short read.
template <typename T> bool read_message(std::istream& f, T& msg) {
    if (f.eof()) {
        return false;
    }
    uint32_t size = 0;
    if (!read_varint32(f, size)) {
        return false;
    }
    std::string buf(size, '\0');
    f.read(buf.data(), static_cast<std::streamsize>(size));
    if (f.gcount() != static_cast<std::streamsize>(size)) {
        // Truncated file: fail cleanly instead of parsing a zero-padded
        // buffer.
        return false;
    }
    return msg.ParseFromArray(buf.data(), static_cast<int>(size));
}

}  // namespace Scheduling
}  // namespace AstraSim

#endif
