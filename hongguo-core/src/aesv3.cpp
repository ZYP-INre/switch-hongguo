// SPDX-License-Identifier: GPL-3.0-only
//
// AesV3 移植。逐行对照 hongguoTV 的 AesV3.kt / hongguo.cjs。
// 各处私有的表与常量均来自 vendor.hpp（由神谕生成）。
#include "hongguo/aesv3.hpp"

#include <stdexcept>

#include "hongguo/vendor.hpp"

namespace hongguo {
namespace {

// 每 wordSize 一组：con（行列置换）、order（列内置换顺序）、init（初始字）
constexpr uint8_t kCon[4][4] = {
    {1, 0, 2, 3}, {2, 0, 3, 1}, {0, 1, 3, 2}, {1, 0, 2, 3}};

constexpr uint8_t kOrder[4][16] = {
    {0, 9, 14, 11, 4, 13, 2, 7, 8, 1, 6, 15, 12, 5, 10, 3},
    {0, 9, 14, 15, 4, 13, 2, 7, 8, 1, 6, 3, 12, 5, 10, 11},
    {0, 9, 14, 7, 4, 13, 2, 11, 8, 1, 6, 3, 12, 5, 10, 15},
    {0, 9, 14, 11, 4, 13, 2, 7, 8, 1, 6, 15, 12, 5, 10, 3}};

constexpr uint32_t kInit[4] = {0xca025ddcu, 0x823dc546u, 0xc9420583u,
                               0xc298225fu};

// 算术右移。原实现跑在 JS 的 int32 上（`init` 的最高位为 1，是负数），
// 这里必须保持同样的有符号语义 —— 用逻辑右移会得到完全不同的轮常量。
int32_t arithmeticShiftRight(int32_t value, int count) {
  // C++20 起明确规定有符号右移为算术右移；各主流编译器一直如此。
  return value >> count;
}

}  // namespace

AesV3::AesV3(const Bytes& key, uint32_t khronos) {
  if (key.size() != 16) {
    throw std::invalid_argument("AesV3 需要 16 字节密钥");
  }

  wordSize_ = static_cast<int>(khronos & 3u);
  box_ = vendor::kSbox + wordSize_ * 256;
  con_ = {kCon[wordSize_][0], kCon[wordSize_][1], kCon[wordSize_][2],
          kCon[wordSize_][3]};
  for (int i = 0; i < 16; ++i) order_[i] = kOrder[wordSize_][i];

  const uint32_t init = kInit[wordSize_];

  // initial = init 的四个小端字；mk = initial XOR key（密钥循环使用）
  Bytes initial;
  for (int i = 0; i < 4; ++i) {
    const Bytes word = le32(init);
    initial.insert(initial.end(), word.begin(), word.end());
  }
  Bytes expanded = xorRepeat(initial, key);
  expanded.resize(48, 0x00);

  int rounds = 8;
  for (int i = 4; i < 12; ++i) {
    const size_t at = static_cast<size_t>(4 * (i - 1));
    uint8_t a = expanded[at];
    uint8_t b = expanded[at + 1];
    uint8_t c = expanded[at + 2];
    uint8_t d = expanded[at + 3];
    if ((i & 3) == 0) {
      const uint32_t shifted =
          static_cast<uint32_t>(arithmeticShiftRight(static_cast<int32_t>(init),
                                                     rounds & 24));
      const uint8_t t = static_cast<uint8_t>((shifted ^ box_[b]) & 0xffu);
      b = box_[c];
      c = box_[d];
      d = box_[a];
      a = t;
    }
    rounds += 2;
    expanded[at + 4] = static_cast<uint8_t>(a ^ expanded[at - 12]);
    expanded[at + 5] = static_cast<uint8_t>(b ^ expanded[at - 11]);
    expanded[at + 6] = static_cast<uint8_t>(c ^ expanded[at - 10]);
    expanded[at + 7] = static_cast<uint8_t>(d ^ expanded[at - 9]);
  }

  for (int r = 0; r < 12; ++r) {
    for (int j = 0; j < 4; ++j) {
      keys_[static_cast<size_t>(r)][static_cast<size_t>(j)] =
          expanded[static_cast<size_t>(r * 4 + j)];
    }
  }
}

AesV3::Rows AesV3::matrix(const Bytes& value) {
  Rows rows{};
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = 0; j < 4; ++j) {
      const size_t at = i * 4 + j;
      rows[i][j] = at < value.size() ? value[at] : 0;
    }
  }
  return rows;
}

Bytes AesV3::flatten(const Rows& rows) {
  Bytes out;
  out.reserve(16);
  for (const Row& row : rows) {
    out.insert(out.end(), row.begin(), row.end());
  }
  return out;
}

void AesV3::addCon(Rows& rows, int keyOffset) const {
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = 0; j < 4; ++j) {
      rows[i][j] = static_cast<uint8_t>(
          rows[i][j] ^ keys_[static_cast<size_t>(keyOffset) + i][con_[j]]);
    }
  }
}

void AesV3::substitute(Rows& rows) const {
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = 0; j < 4; ++j) rows[i][j] = box_[rows[i][j]];
  }
  const Rows old = rows;
  for (size_t i = 0; i < 4; ++i) rows[i] = old[con_[i]];
}

void AesV3::shift(Rows& rows) const {
  const Bytes flat = flatten(rows);
  for (size_t i = 0; i < 16; ++i) {
    rows[i / 4][i % 4] = flat[order_[i]];
  }
}

void AesV3::shiftCon(Rows& rows) const {
  const Rows old = rows;
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = 0; j < 4; ++j) rows[i][j] = old[i][con_[j]];
  }
}

AesV3::Block AesV3::block(const Bytes& value) const {
  Rows rows = matrix(value);
  addCon(rows, 0);

  for (int round = 1; round < 3; ++round) {
    substitute(rows);
    shift(rows);
    if (round == 1) {
      shiftCon(rows);
      // aesMix：与标准 AES 的 MixColumns 同形，但在私有的行列布局上运行
      for (size_t i = 0; i < 4; ++i) {
        const uint8_t t = static_cast<uint8_t>(rows[0][i] ^ rows[1][i] ^
                                               rows[2][i] ^ rows[3][i]);
        const uint8_t u = rows[0][i];
        const auto xt = [](uint8_t x) -> uint8_t {
          return static_cast<uint8_t>((x << 1) ^ ((x & 128) ? 0x1b : 0));
        };
        rows[0][i] = static_cast<uint8_t>(rows[0][i] ^ t ^ xt(static_cast<uint8_t>(rows[0][i] ^ rows[1][i])));
        rows[1][i] = static_cast<uint8_t>(rows[1][i] ^ t ^ xt(static_cast<uint8_t>(rows[1][i] ^ rows[2][i])));
        rows[2][i] = static_cast<uint8_t>(rows[2][i] ^ t ^ xt(static_cast<uint8_t>(rows[2][i] ^ rows[3][i])));
        rows[3][i] = static_cast<uint8_t>(rows[3][i] ^ t ^ xt(static_cast<uint8_t>(rows[3][i] ^ u)));
      }
    }
    addCon(rows, round * 4);
  }

  // 与 keys[4..7] 逐元素异或
  for (size_t i = 0; i < 4; ++i) {
    for (size_t j = 0; j < 4; ++j) {
      rows[i][j] = static_cast<uint8_t>(rows[i][j] ^ keys_[4 + i][j]);
    }
  }

  Block out{};
  const Bytes flat = flatten(rows);
  for (size_t i = 0; i < 16; ++i) out[i] = flat[i];
  return out;
}

Bytes AesV3::encrypt(const Bytes& source, const Bytes& iv) const {
  if (source.size() < 248) {
    throw std::invalid_argument("AesV3::encrypt 需要至少 248 字节输入");
  }
  if (iv.size() != 16) throw std::invalid_argument("AesV3 需要 16 字节 IV");

  // 1) 从原文固定比特位抽出 31 字节"明文字"
  Bytes plaintext(32, 0x00);
  for (int i = 0; i < 31; ++i) {
    const size_t at = static_cast<size_t>(i) * 8;
    uint8_t value = 0;
    value |= static_cast<uint8_t>((source[at] >> 4) & 2);
    value |= static_cast<uint8_t>(source[at + 1] & 64);
    value |= static_cast<uint8_t>((source[at + 2] >> 2) & 1);
    value |= static_cast<uint8_t>((source[at + 3] << 3) & 128);
    value |= static_cast<uint8_t>((source[at + 4] >> 1) & 4);
    value |= static_cast<uint8_t>((source[at + 5] << 3) & 16);
    value |= static_cast<uint8_t>((source[at + 6] << 5) & 32);
    value |= static_cast<uint8_t>((source[at + 7] >> 4) & 8);
    plaintext[static_cast<size_t>(i)] = value;
  }
  plaintext[31] = 1;

  // 2) 两轮派生，得到 32 字节密钥（CBC 式串联）
  Bytes key;
  key.reserve(32);
  Bytes previous = iv;
  for (size_t at = 0; at < plaintext.size(); at += 16) {
    const Bytes chunk(plaintext.begin() + static_cast<long>(at),
                      plaintext.begin() + static_cast<long>(at + 16));
    const Bytes value = xorZip(chunk, previous);
    const Block derivedBlock = block(value);
    const Bytes derived(derivedBlock.begin(), derivedBlock.end());
    key.insert(key.end(), derived.begin(), derived.end());
    previous = derived;
  }

  // 3) 把这 31 字节密钥的比特嵌回原文的固定比特位
  Bytes out = source;
  for (int i = 0; i < 31; ++i) {
    const size_t at = static_cast<size_t>(i) * 8;
    const uint8_t k = key[static_cast<size_t>(i)];
    out[at] = static_cast<uint8_t>((out[at] & 0xdf) | ((k << 4) & 32));
    out[at + 1] = static_cast<uint8_t>((out[at + 1] & 0xbf) | (k & 64));
    out[at + 2] = static_cast<uint8_t>((out[at + 2] & 0xfb) | ((k << 2) & 4));
    out[at + 3] = static_cast<uint8_t>((out[at + 3] & 0xef) | ((k >> 3) & 16));
    out[at + 4] = static_cast<uint8_t>((out[at + 4] & 0xf7) | ((k + k) & 8));
    out[at + 5] = static_cast<uint8_t>((out[at + 5] & 0xfd) | ((k >> 3) & 2));
    out[at + 6] = static_cast<uint8_t>((out[at + 6] & 0xfe) | ((k >> 5) & 1));
    out[at + 7] = static_cast<uint8_t>((out[at + 7] & 0x7f) | ((k << 4) & 128));
  }

  Bytes result;
  result.reserve(out.size() + 1);
  result.push_back(key.back());
  result.insert(result.end(), out.begin(), out.end());
  return result;
}

}  // namespace hongguo
