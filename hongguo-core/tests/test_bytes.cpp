// SPDX-License-Identifier: GPL-3.0-only
#include "harness.hpp"
#include "hongguo/bytes.hpp"

#include <stdexcept>

using namespace hongguo;

// 注意：HG_CHECK_EQ 是宏，参数里若含未加括号的花括号初始化列表，
// 其中的逗号会被预处理器当作参数分隔符。所有 Bytes{...} 都要写成 (Bytes{...})。

HG_TEST(bytes_hex_roundtrip) {
  const Bytes data = fromHex("00ff10a5");
  HG_CHECK_EQ(toHex(data), std::string("00ff10a5"));
  HG_CHECK_EQ(toHexUpper(data), std::string("00FF10A5"));
  HG_CHECK_EQ(fromHex(""), (Bytes{}));
}

HG_TEST(bytes_hex_rejects_odd_length) {
  bool threw = false;
  try {
    fromHex("abc");
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  HG_CHECK(threw);
}

HG_TEST(bytes_base64_encode) {
  HG_CHECK_EQ(toBase64(Bytes{'h', 'e', 'l', 'l', 'o'}), std::string("aGVsbG8="));
  HG_CHECK_EQ(toBase64(Bytes{}), std::string(""));
  // 三字节整组：无填充
  HG_CHECK_EQ(toBase64((Bytes{1, 2, 3})), std::string("AQID"));
  // 单字节余数：两个 '='
  HG_CHECK_EQ(toBase64((Bytes{0xff})), std::string("/w=="));
  // 两字节余数：一个 '='
  HG_CHECK_EQ(toBase64((Bytes{0xfb, 0xff})), std::string("+/8="));
}

HG_TEST(bytes_base64_decode_accepts_urlsafe) {
  HG_CHECK_EQ(fromBase64("aGVsbG8="), (Bytes{'h', 'e', 'l', 'l', 'o'}));
  // URL-safe 变体（Kotlin decode64 会把 -_ 换回 +/）
  HG_CHECK_EQ(fromBase64("-_8="), (Bytes{0xfb, 0xff}));
  HG_CHECK_EQ(fromBase64("AQID"), (Bytes{1, 2, 3}));
}

HG_TEST(bytes_base64_roundtrip_all_lengths) {
  // 覆盖 3n / 3n+1 / 3n+2 三种余数
  for (size_t n = 0; n < 10; ++n) {
    Bytes source(n);
    for (size_t i = 0; i < n; ++i) source[i] = static_cast<uint8_t>(i * 37 + 11);
    HG_CHECK_EQ(fromBase64(toBase64(source)), source);
  }
}

HG_TEST(bytes_endian_packing) {
  HG_CHECK_EQ(le32(0x01020304u), (Bytes{0x04, 0x03, 0x02, 0x01}));
  HG_CHECK_EQ(be32(0x01020304u), (Bytes{0x01, 0x02, 0x03, 0x04}));
  HG_CHECK_EQ(le64(0x0102030405060708ull),
              (Bytes{0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01}));
}

HG_TEST(bytes_endian_reading) {
  const Bytes data = fromHex("0102030405060708");
  HG_CHECK_EQ(readU32BE(data, 0), 0x01020304u);
  HG_CHECK_EQ(readU32LE(data, 0), 0x04030201u);
  HG_CHECK_EQ(readU32BE(data, 4), 0x05060708u);
  HG_CHECK_EQ(readU64BE(data, 0), 0x0102030405060708ull);
  // 越界读返回 0，调用方负责边界检查（对齐 Node 的用法）
  HG_CHECK_EQ(readU32BE(data, 6), 0u);
}

HG_TEST(bytes_rotations) {
  HG_CHECK_EQ(rol32(0x80000001u, 1), 0x00000003u);
  HG_CHECK_EQ(ror32(0x00000001u, 1), 0x80000000u);
  HG_CHECK_EQ(ror64(0x0000000000000001ull, 1), 0x8000000000000000ull);
  // 旋转 0 与旋转整圈应回到原值
  HG_CHECK_EQ(rol32(0xdeadbeefu, 0), 0xdeadbeefu);
  HG_CHECK_EQ(rol32(0xdeadbeefu, 32), 0xdeadbeefu);
  HG_CHECK_EQ(ror32(0xdeadbeefu, 64), 0xdeadbeefu);
}

HG_TEST(bytes_xor_variants) {
  // xorRepeat: 密钥循环使用
  HG_CHECK_EQ(xorRepeat(fromHex("0000"), fromHex("ff")), fromHex("ffff"));
  HG_CHECK_EQ(xorRepeat(fromHex("0f0f0f"), fromHex("ff00")), fromHex("f00ff0"));
  // xorZip: 取较短长度
  HG_CHECK_EQ(xorZip(fromHex("0f0f0f"), fromHex("ff00")), fromHex("f00f"));
}

HG_TEST(bytes_concat_helpers) {
  const Bytes a = fromHex("01"), b = fromHex("02"), c = fromHex("03");
  HG_CHECK_EQ(concat(a, b), fromHex("0102"));
  HG_CHECK_EQ(concat(a, b, c), fromHex("010203"));
  HG_CHECK_EQ(concat(a, b, c, fromHex("04")), fromHex("01020304"));
}

HG_TEST(bytes_ascii_at) {
  const Bytes data = fromHex("0000001866747970");  // 长度 + "ftyp"
  HG_CHECK_EQ(asciiAt(data, 4, 4), std::string("ftyp"));
  // 越界返回空串
  HG_CHECK_EQ(asciiAt(data, 6, 4), std::string(""));
}
