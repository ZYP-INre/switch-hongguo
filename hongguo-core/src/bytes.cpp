// SPDX-License-Identifier: GPL-3.0-only
#include "hongguo/bytes.hpp"

#include <stdexcept>

namespace hongguo {
namespace {

constexpr char kHexLower[] = "0123456789abcdef";

int hexNibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

constexpr char kB64Alphabet[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

int b64Value(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+' || c == '-') return 62;
  if (c == '/' || c == '_') return 63;
  return -1;
}

}  // namespace

Bytes fromHex(std::string_view hex) {
  if (hex.size() % 2 != 0) {
    throw std::invalid_argument("fromHex: 十六进制字符串长度为奇数");
  }
  Bytes out;
  out.reserve(hex.size() / 2);
  for (size_t i = 0; i < hex.size(); i += 2) {
    const int high = hexNibble(hex[i]);
    const int low = hexNibble(hex[i + 1]);
    if (high < 0 || low < 0) {
      throw std::invalid_argument("fromHex: 非法十六进制字符");
    }
    out.push_back(static_cast<uint8_t>((high << 4) | low));
  }
  return out;
}

std::string toHex(const Bytes& value) {
  std::string out;
  out.reserve(value.size() * 2);
  for (const uint8_t byte : value) {
    out.push_back(kHexLower[byte >> 4]);
    out.push_back(kHexLower[byte & 0x0f]);
  }
  return out;
}

std::string toHexUpper(const Bytes& value) {
  std::string out = toHex(value);
  for (char& c : out) {
    if (c >= 'a' && c <= 'f') c = static_cast<char>(c - 'a' + 'A');
  }
  return out;
}

Bytes fromBase64(std::string_view text) {
  Bytes out;
  out.reserve(text.size() / 4 * 3);
  int acc = 0;
  int bits = 0;
  for (const char c : text) {
    if (c == '=' || c == '\n' || c == '\r') continue;
    const int v = b64Value(c);
    if (v < 0) throw std::invalid_argument("fromBase64: 非法 base64 字符");
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out.push_back(static_cast<uint8_t>((acc >> bits) & 0xff));
    }
  }
  return out;
}

std::string toBase64(const Bytes& value) {
  std::string out;
  out.reserve((value.size() + 2) / 3 * 4);
  size_t i = 0;
  for (; i + 3 <= value.size(); i += 3) {
    const uint32_t group = (static_cast<uint32_t>(value[i]) << 16) |
                           (static_cast<uint32_t>(value[i + 1]) << 8) |
                           static_cast<uint32_t>(value[i + 2]);
    out.push_back(kB64Alphabet[(group >> 18) & 0x3f]);
    out.push_back(kB64Alphabet[(group >> 12) & 0x3f]);
    out.push_back(kB64Alphabet[(group >> 6) & 0x3f]);
    out.push_back(kB64Alphabet[group & 0x3f]);
  }
  const size_t rest = value.size() - i;
  if (rest == 1) {
    const uint32_t group = static_cast<uint32_t>(value[i]) << 16;
    out.push_back(kB64Alphabet[(group >> 18) & 0x3f]);
    out.push_back(kB64Alphabet[(group >> 12) & 0x3f]);
    out.push_back('=');
    out.push_back('=');
  } else if (rest == 2) {
    const uint32_t group = (static_cast<uint32_t>(value[i]) << 16) |
                           (static_cast<uint32_t>(value[i + 1]) << 8);
    out.push_back(kB64Alphabet[(group >> 18) & 0x3f]);
    out.push_back(kB64Alphabet[(group >> 12) & 0x3f]);
    out.push_back(kB64Alphabet[(group >> 6) & 0x3f]);
    out.push_back('=');
  }
  return out;
}

namespace {

Bytes packU32(uint32_t value, bool little) {
  Bytes out(4);
  for (int i = 0; i < 4; ++i) {
    const int shift = little ? i * 8 : (3 - i) * 8;
    out[static_cast<size_t>(i)] = static_cast<uint8_t>((value >> shift) & 0xff);
  }
  return out;
}

Bytes packU64(uint64_t value, bool little) {
  Bytes out(8);
  for (int i = 0; i < 8; ++i) {
    const int shift = little ? i * 8 : (7 - i) * 8;
    out[static_cast<size_t>(i)] = static_cast<uint8_t>((value >> shift) & 0xff);
  }
  return out;
}

uint32_t unpackU32(const Bytes& data, size_t at, bool little) {
  if (at + 4 > data.size()) return 0;
  uint32_t out = 0;
  for (int i = 0; i < 4; ++i) {
    const uint32_t byte = data[at + static_cast<size_t>(i)];
    out |= little ? (byte << (i * 8)) : (byte << ((3 - i) * 8));
  }
  return out;
}

uint64_t unpackU64(const Bytes& data, size_t at, bool little) {
  if (at + 8 > data.size()) return 0;
  uint64_t out = 0;
  for (int i = 0; i < 8; ++i) {
    const uint64_t byte = data[at + static_cast<size_t>(i)];
    out |= little ? (byte << (i * 8)) : (byte << ((7 - i) * 8));
  }
  return out;
}

}  // namespace

Bytes le32(uint32_t value) { return packU32(value, true); }
Bytes be32(uint32_t value) { return packU32(value, false); }
Bytes le64(uint64_t value) { return packU64(value, true); }
Bytes be64(uint64_t value) { return packU64(value, false); }

uint32_t readU32LE(const Bytes& data, size_t at) { return unpackU32(data, at, true); }
uint32_t readU32BE(const Bytes& data, size_t at) { return unpackU32(data, at, false); }
uint64_t readU64LE(const Bytes& data, size_t at) { return unpackU64(data, at, true); }
uint64_t readU64BE(const Bytes& data, size_t at) { return unpackU64(data, at, false); }

uint32_t rol32(uint32_t value, int count) {
  const int n = count & 31;
  if (n == 0) return value;
  return (value << n) | (value >> (32 - n));
}

uint32_t ror32(uint32_t value, int count) {
  const int n = count & 31;
  if (n == 0) return value;
  return (value >> n) | (value << (32 - n));
}

uint64_t ror64(uint64_t value, int count) {
  const int n = count & 63;
  if (n == 0) return value;
  return (value >> n) | (value << (64 - n));
}

Bytes concat(const Bytes& a, const Bytes& b) {
  Bytes out;
  out.reserve(a.size() + b.size());
  out.insert(out.end(), a.begin(), a.end());
  out.insert(out.end(), b.begin(), b.end());
  return out;
}

Bytes concat(const Bytes& a, const Bytes& b, const Bytes& c) {
  return concat(concat(a, b), c);
}

Bytes concat(const Bytes& a, const Bytes& b, const Bytes& c, const Bytes& d) {
  return concat(concat(a, b), concat(c, d));
}

Bytes concat(const Bytes& a, const Bytes& b, const Bytes& c, const Bytes& d,
             const Bytes& e) {
  return concat(concat(a, b), concat(c, d), e);
}

Bytes xorRepeat(const Bytes& a, const Bytes& b) {
  if (b.empty()) throw std::invalid_argument("xorRepeat: 异或密钥为空");
  Bytes out(a.size());
  for (size_t i = 0; i < a.size(); ++i) {
    out[i] = static_cast<uint8_t>(a[i] ^ b[i % b.size()]);
  }
  return out;
}

Bytes xorZip(const Bytes& a, const Bytes& b) {
  const size_t n = a.size() < b.size() ? a.size() : b.size();
  Bytes out(n);
  for (size_t i = 0; i < n; ++i) {
    out[i] = static_cast<uint8_t>(a[i] ^ b[i]);
  }
  return out;
}

std::string asciiAt(const Bytes& data, size_t at, size_t length) {
  if (at + length > data.size()) return {};
  return std::string(reinterpret_cast<const char*>(data.data() + at), length);
}

}  // namespace hongguo
