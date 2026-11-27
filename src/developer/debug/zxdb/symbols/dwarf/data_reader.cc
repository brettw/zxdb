// Copyright 2026 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/developer/debug/zxdb/symbols/dwarf/data_reader.h"

#include <string.h>

#include <algorithm>

namespace zxdb {

void DataReader::set_limit(uint64_t limit) {
  limit_ = std::min(limit_, limit);
  ok_ = ok_ && offset_ <= limit_;
}

void DataReader::Seek(uint64_t offset) {
  if (!ok_ || offset > limit_) {
    ok_ = false;
    return;
  }
  offset_ = offset;
}

uint64_t DataReader::UlebSlow() {
  const uint8_t* p = data_.data() + offset_;
  auto result = ReadUleb(p, data_.data() + limit_);
  if (!result) {
    ok_ = false;
    return 0;
  }
  offset_ = p - data_.data();
  return *result;
}

int64_t DataReader::Sleb() {
  if (!Check(1))
    return 0;
  const uint8_t* p = data_.data() + offset_;
  auto result = ReadSleb(p, data_.data() + limit_);
  if (!result) {
    ok_ = false;
    return 0;
  }
  offset_ = p - data_.data();
  return *result;
}

uint64_t DataReader::Offset(size_t offset_size) {
  if (offset_size == 4 || offset_size == 8)
    return Fixed(offset_size);
  ok_ = false;
  return 0;
}

uint64_t DataReader::Address(size_t address_size) {
  if (address_size == 1 || address_size == 2 || address_size == 4 || address_size == 8)
    return Fixed(address_size);
  ok_ = false;
  return 0;
}

std::string_view DataReader::CString() {
  if (!Check(1))
    return {};
  const uint8_t* start = data_.data() + offset_;
  const auto* nul = static_cast<const uint8_t*>(memchr(start, 0, remaining()));
  if (!nul) {
    ok_ = false;
    return {};
  }
  size_t length = nul - start;
  offset_ += length + 1;
  return {reinterpret_cast<const char*>(start), length};
}

std::span<const uint8_t> DataReader::Bytes(uint64_t size) {
  if (!Check(size))
    return {};
  auto result = data_.subspan(offset_, size);
  offset_ += size;
  return result;
}

DataReader::UnitLength DataReader::InitialLength() {
  uint64_t start = offset_;
  uint64_t length = Fixed(4);
  if (!ok_)
    return {};
  if (length < 0xfffffff0)
    return {length, 4};
  if (length == 0xffffffff) {
    length = Fixed(8);
    if (ok_)
      return {length, 8};
  }
  offset_ = start;
  ok_ = false;
  return {};
}

}  // namespace zxdb
