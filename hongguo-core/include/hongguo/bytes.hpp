// SPDX-License-Identifier: GPL-3.0-only
//
// 字节工具。移植自 hongguoTV 的 Bytes.kt 与 hongguo.cjs。
// 本文件不含任何业务逻辑，也不依赖任何其他模块 —— 可单独单测。
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace hongguo {

using Bytes = std::vector<uint8_t>;

// ---- 十六进制 ----

// 解析十六进制字符串。长度为奇数时抛 std::invalid_argument。
Bytes fromHex(std::string_view hex);

// 小写十六进制。
std::string toHex(const Bytes& value);

// 大写十六进制（POST 的 x-ss-stub 要求大写）。
std::string toHexUpper(const Bytes& value);

// ---- Base64 ----

// 标准 base64 解码；'-' / '_' 按 URL-safe 变体接受（对应 Kotlin 的 decode64）。
Bytes fromBase64(std::string_view text);

// 标准 base64 编码（带 '=' 填充）。
std::string toBase64(const Bytes& value);

// ---- 端序 ----

Bytes le32(uint32_t value);
Bytes be32(uint32_t value);
Bytes le64(uint64_t value);
Bytes be64(uint64_t value);

// 越界读返回 0 —— 对齐 Node 的 Buffer.readUInt32LE 在调用方已做过边界检查的用法。
uint32_t readU32LE(const Bytes& data, size_t at);
uint32_t readU32BE(const Bytes& data, size_t at);
uint64_t readU64LE(const Bytes& data, size_t at);
uint64_t readU64BE(const Bytes& data, size_t at);

// ---- 位运算 ----

uint32_t rol32(uint32_t value, int count);
uint32_t ror32(uint32_t value, int count);
uint64_t ror64(uint64_t value, int count);

// ---- 拼接 / 异或 ----

Bytes concat(const Bytes& a, const Bytes& b);
Bytes concat(const Bytes& a, const Bytes& b, const Bytes& c);
Bytes concat(const Bytes& a, const Bytes& b, const Bytes& c, const Bytes& d);
Bytes concat(const Bytes& a, const Bytes& b, const Bytes& c, const Bytes& d,
             const Bytes& e);

// a[i] ^ b[i % b.size()]。b 为空时抛 std::invalid_argument。
// 对应 Kotlin 的 xor() 与 Node 的 xorBytes()。
Bytes xorRepeat(const Bytes& a, const Bytes& b);

// 逐字节异或，长度取两者较短者。对应 Node 的 bxor()。
Bytes xorZip(const Bytes& a, const Bytes& b);

// ---- 杂项 ----

// 把可打印的字节序列当字符串取用（用于 ftyp / moov 这类 fourcc 比较）。
std::string asciiAt(const Bytes& data, size_t at, size_t length);

}  // namespace hongguo
