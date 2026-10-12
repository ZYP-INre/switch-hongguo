// SPDX-License-Identifier: GPL-3.0-only
#include "hongguo/protobuf.hpp"

#include <cstring>

namespace hongguo::protobuf {

Bytes varint(uint64_t value) {
  Bytes out;
  while (value > 127) {
    out.push_back(static_cast<uint8_t>((value & 127) | 128));
    value >>= 7;
  }
  out.push_back(static_cast<uint8_t>(value));
  return out;
}

Bytes zigzag(int64_t value) {
  const uint64_t magnitude =
      value < 0 ? static_cast<uint64_t>(-value) * 2 - 1
                : static_cast<uint64_t>(value) * 2;
  return varint(magnitude);
}

namespace {

// 原实现的 varint 走 BigInt，负数会落到 `out.push(Number(n))`，
// 经 Buffer 归一为低 8 位。这里照抄该行为 —— 虽然红果链路里不会出现。
Bytes varintSignedFallback(int64_t value) {
  return Bytes{static_cast<uint8_t>(static_cast<uint64_t>(value) & 0xff)};
}

Bytes delimited(int tag, const Bytes& body) {
  Bytes out = varint((static_cast<uint64_t>(tag) << 3) | 2);
  const Bytes length = varint(body.size());
  out.insert(out.end(), length.begin(), length.end());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

}  // namespace

Bytes fieldString(int tag, std::string_view value) {
  if (value.empty()) return {};  // 空字符串 → 字段省略
  return delimited(tag, Bytes(value.begin(), value.end()));
}

Bytes fieldBytes(int tag, const Bytes& value) {
  // 注意：空字节数组**不**省略（原实现用 `=== ''` 判空，对 Buffer 不成立）
  return delimited(tag, value);
}

Bytes fieldMessage(int tag, const Bytes& body) { return delimited(tag, body); }

Bytes fieldSint(int tag, int64_t value) {
  Bytes out = varint(static_cast<uint64_t>(tag) << 3);
  const Bytes body = zigzag(value);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

Bytes fieldInt(int tag, uint64_t value) {
  Bytes out = varint(static_cast<uint64_t>(tag) << 3);
  const Bytes body =
      value > static_cast<uint64_t>(INT64_MAX)
          ? varintSignedFallback(static_cast<int64_t>(value))
          : varint(value);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

Bytes fieldFloat(int tag, float value) {
  Bytes out = varint((static_cast<uint64_t>(tag) << 3) | 5);
  uint32_t bits = 0;
  std::memcpy(&bits, &value, sizeof(bits));
  const Bytes body = le32(bits);
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

Bytes encode(std::initializer_list<Bytes> fields) {
  Bytes out;
  for (const Bytes& field : fields) {
    out.insert(out.end(), field.begin(), field.end());
  }
  return out;
}

Bytes encode(const std::vector<Bytes>& fields) {
  Bytes out;
  for (const Bytes& field : fields) {
    out.insert(out.end(), field.begin(), field.end());
  }
  return out;
}

}  // namespace hongguo::protobuf
