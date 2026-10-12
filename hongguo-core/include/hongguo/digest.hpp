// SPDX-License-Identifier: GPL-3.0-only
//
// 一次性摘要：MD5 / SHA-512 / SM3。
//
// 为什么自己实现而不用 OpenSSL：见设计文档第 3 节。
// 简言之 —— 需要保证 macOS 与 devkitPro 两端逐字节一致，
// 不想把结果押在"目标平台的 OpenSSL 有没有编进 SM3"这种问题上。
#pragma once

#include <string>
#include <string_view>

#include "hongguo/bytes.hpp"

namespace hongguo {

Bytes md5(const Bytes& data);
Bytes sha512(const Bytes& data);
Bytes sm3(const Bytes& data);

inline Bytes md5(std::string_view text) {
  return md5(Bytes(text.begin(), text.end()));
}
inline Bytes sha512(std::string_view text) {
  return sha512(Bytes(text.begin(), text.end()));
}
inline Bytes sm3(std::string_view text) {
  return sm3(Bytes(text.begin(), text.end()));
}

// 小写十六进制。对齐 Node 的 crypto.createHash(...).digest('hex')。
std::string md5Hex(const Bytes& data);
std::string sha512Hex(const Bytes& data);

// 大写十六进制。POST 的 x-ss-stub 头要求大写。
std::string md5HexUpper(const Bytes& data);

}  // namespace hongguo
