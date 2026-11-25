// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DATA_READER_H_
#define SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DATA_READER_H_

#include <stddef.h>
#include <stdint.h>

#include <optional>
#include <span>
#include <string_view>

namespace zxdb {

// These helpers decode from a raw pointer in hot loops such as the line program decoder.
// For LEB128, the caller supplies the end pointer. On failure, the LEB128 helpers return
// std::nullopt or false and may advance p.

// Both LEB128 readers reject truncated values and values that don't fit in 64 bits.
// They accept extra bytes if those bytes only extend the value.
inline std::optional<uint64_t> ReadUleb(const uint8_t*& p, const uint8_t* end) {
  if (p < end && !(*p & 0x80))
    return *p++;

  uint64_t result = 0;
  unsigned shift = 0;
  while (p < end) {
    uint8_t byte = *p++;
    uint64_t slice = byte & 0x7f;
    if (shift >= 63 && (shift == 63 ? slice > 1 : slice != 0))
      return std::nullopt;
    if (shift < 64) {
      result |= slice << shift;
      shift += 7;
    }
    if (!(byte & 0x80))
      return result;
  }
  return std::nullopt;
}

inline std::optional<int64_t> ReadSleb(const uint8_t*& p, const uint8_t* end) {
  uint64_t result = 0;
  unsigned shift = 0;
  while (p < end) {
    uint8_t byte = *p++;
    uint64_t slice = byte & 0x7f;
    if (shift >= 63) {
      uint64_t extension = (result >> 63) ? 0x7f : 0;
      if (shift == 63 ? slice != 0 && slice != 0x7f : slice != extension)
        return std::nullopt;
    }
    if (shift < 64) {
      result |= slice << shift;
      shift += 7;
    }
    if (!(byte & 0x80)) {
      if (shift < 64 && (byte & 0x40))
        result |= ~uint64_t{0} << shift;
      return static_cast<int64_t>(result);
    }
  }
  return std::nullopt;
}

// Skip a value with the same checks as the readers. Most values are one byte.
inline bool SkipUleb(const uint8_t*& p, const uint8_t* end) { return ReadUleb(p, end).has_value(); }

inline bool SkipSleb(const uint8_t*& p, const uint8_t* end) {
  if (p < end && !(*p & 0x80)) {
    p++;
    return true;
  }
  return ReadSleb(p, end).has_value();
}

// The caller checks the bounds and supplies a size from 1 to 8.
inline uint64_t ReadFixed(const uint8_t* p, size_t size, bool little_endian) {
  uint64_t result = 0;
  for (size_t i = 0; i < size; i++) {
    size_t shift = little_endian ? i : size - 1 - i;
    result |= static_cast<uint64_t>(p[i]) << (8 * shift);
  }
  return result;
}

// Reads section bytes up to a limit. A failed read or cursor move leaves the offset unchanged,
// sets a sticky error, and causes later reads to return zero or an empty view.
class DataReader {
 public:
  struct UnitLength {
    uint64_t length = 0;
    uint8_t offset_size = 0;
  };

  DataReader(std::span<const uint8_t> data, bool little_endian)
      : data_(data), limit_(data.size()), little_endian_(little_endian) {}

  bool ok() const { return ok_; }
  uint64_t offset() const { return offset_; }
  uint64_t remaining() const { return ok_ ? limit_ - offset_ : 0; }

  // A larger limit has no effect. A limit below the current offset sets the sticky error.
  void set_limit(uint64_t limit);
  void Seek(uint64_t offset);
  void Skip(uint64_t size) {
    if (Check(size))
      offset_ += size;
  }

  uint64_t Fixed(size_t size) {
    if (size == 0 || size > 8 || !Check(size)) {
      ok_ = false;
      return 0;
    }
    uint64_t result = ReadFixed(data_.data() + offset_, size, little_endian_);
    offset_ += size;
    return result;
  }

  uint64_t Uleb();
  int64_t Sleb();
  void SkipUleb() { Uleb(); }
  void SkipSleb() { Sleb(); }

  uint64_t Offset(size_t offset_size);
  uint64_t Address(size_t address_size);
  std::string_view CString();
  std::span<const uint8_t> Bytes(uint64_t size);

  // Reads the length field only. The caller checks whether the unit fits in the remaining bytes.
  UnitLength InitialLength();

 private:
  bool Check(uint64_t size) {
    if (!ok_ || size > limit_ - offset_) {
      ok_ = false;
      return false;
    }
    return true;
  }

  std::span<const uint8_t> data_;
  uint64_t offset_ = 0;
  uint64_t limit_;
  bool little_endian_;
  bool ok_ = true;
};

}  // namespace zxdb

#endif  // SRC_DEVELOPER_DEBUG_ZXDB_SYMBOLS_DWARF_DATA_READER_H_
