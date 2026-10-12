// SPDX-License-Identifier: GPL-3.0-only
//
// AesV3 —— 字节系签名里的私有加密变体。
//
// 名字带 AES，但**不是 AES**：
//   * sbox 来自私有表，按 khronos & 3 取四分之一片
//   * 只有 2 轮，且轮内混列/行列置换的顺序与标准不同
//   * 轮常量与初始字都按 khronos 选表
//   * 不加密原文，而是把 31 字节派生密钥的比特"嵌"进原文的固定比特位
//
// 因此没有任何标准库可以替代，必须逐行对照原实现。
#pragma once

#include <array>
#include <cstdint>

#include "hongguo/bytes.hpp"

namespace hongguo {

class AesV3 {
 public:
  // key 必须 16 字节。khronos 决定使用哪一组私有表。
  AesV3(const Bytes& key, uint32_t khronos);

  // source 至少 248 字节（实现只触及前 248 字节）。
  // 返回 1 + source.size() 字节，首字节是派生密钥的最后一字节。
  Bytes encrypt(const Bytes& source, const Bytes& iv) const;

 private:
  using Row = std::array<uint8_t, 4>;
  using Rows = std::array<Row, 4>;
  using Block = std::array<uint8_t, 16>;

  static Rows matrix(const Bytes& value);
  static Bytes flatten(const Rows& rows);

  // 一轮变换，输入输出都是 16 字节
  Block block(const Bytes& value) const;
  void addCon(Rows& rows, int keyOffset) const;
  void substitute(Rows& rows) const;
  void shift(Rows& rows) const;
  void shiftCon(Rows& rows) const;

  int wordSize_;
  const uint8_t* box_;          // 指向 kSbox 中 256 字节的切片
  std::array<uint8_t, 4> con_;  // 行列置换用
  std::array<uint8_t, 16> order_;
  std::array<Row, 12> keys_;
};

}  // namespace hongguo
