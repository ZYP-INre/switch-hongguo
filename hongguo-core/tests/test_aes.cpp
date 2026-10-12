// SPDX-License-Identifier: GPL-3.0-only
#include "harness.hpp"
#include "hongguo/aes.hpp"
#include "hongguo/aesv3.hpp"

#include <stdexcept>

using namespace hongguo;

// 标准 AES 的向量由 Node crypto 生成；AesV3 的向量由神谕生成（它是私有变体）。

HG_TEST(aes_matches_node_crypto) {
  const auto rows = hgtest::tsv("aes.txt");
  HG_CHECK(!rows.empty());

  for (const auto& row : rows) {
    HG_CHECK(row.size() == 5);
    const std::string& op = row[0];
    const Bytes key = fromHex(row[1]);
    const Bytes iv = fromHex(row[2]);
    const Bytes data = fromHex(row[3]);
    const std::string expected = row[4];

    std::string got;
    if (op == "ctr") {
      got = toHex(aes128Ctr(key, iv, data));
    } else if (op == "cbc") {
      got = toHex(aes128CbcDecrypt(key, iv, data));
    } else {
      HG_CHECK(false);
    }

    if (got != expected) {
      ::hgtest::fail("向量 " + op + " 长度 " + std::to_string(data.size()) +
                     "\n    实得: " + got + "\n    期望: " + expected);
    }
  }
}

HG_TEST(aes_ctr_is_an_involution) {
  // CTR 加密即解密：同一调用两次应还原
  const Bytes key = fromHex("000102030405060708090a0b0c0d0e0f");
  const Bytes iv = fromHex("101112131415161718191a1b1c1d1e1f");
  const Bytes plain = fromHex("00112233445566778899aabbccddeeff0011");
  HG_CHECK_EQ(aes128Ctr(key, iv, aes128Ctr(key, iv, plain)), plain);
}

HG_TEST(aes_ctr_counter_advances_big_endian) {
  // 计数器跨字节进位：IV 末字节为 0xff 时，第二块必须用进位后的计数器
  const Bytes key = fromHex("00000000000000000000000000000000");
  const Bytes iv = fromHex("000000000000000000000000000000ff");
  const Bytes data(32, 0x00);
  const Bytes stream = aes128Ctr(key, iv, data);
  // 与"两块分别用 iv 和 iv+1 加密"应完全一致
  const Bytes first = aes128Ctr(key, iv, Bytes(16, 0x00));
  const Bytes second = aes128Ctr(key, fromHex("00000000000000000000000000000100"),
                                 Bytes(16, 0x00));
  Bytes expected = first;
  expected.insert(expected.end(), second.begin(), second.end());
  HG_CHECK_EQ(stream, expected);
}

HG_TEST(aes_rejects_bad_key_or_iv) {
  bool threw = false;
  try {
    aes128Ctr(fromHex("0011"), fromHex("00000000000000000000000000000000"), Bytes(16, 0));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  HG_CHECK(threw);

  threw = false;
  try {
    aes128CbcDecrypt(fromHex("00000000000000000000000000000000"), fromHex("00"), Bytes(16, 0));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  HG_CHECK(threw);
}

HG_TEST(aesv3_matches_oracle) {
  const auto rows = hgtest::tsv("aesv3.txt");
  HG_CHECK(!rows.empty());

  for (const auto& row : rows) {
    HG_CHECK(row.size() == 6);
    HG_CHECK_EQ(row[0], std::string("aesv3"));

    const Bytes key = fromHex(row[1]);
    const Bytes iv = fromHex(row[2]);
    const uint32_t khronos = static_cast<uint32_t>(std::stoul(row[3]));
    const Bytes data = fromHex(row[4]);
    const std::string expected = row[5];

    const std::string got = toHex(AesV3(key, khronos).encrypt(data, iv));
    if (got != expected) {
      ::hgtest::fail("aesv3 khronos=" + row[3] +
                     "\n    实得: " + got + "\n    期望: " + expected);
    }
  }
}

HG_TEST(aesv3_rejects_short_input) {
  bool threw = false;
  try {
    AesV3(fromHex("00000000000000000000000000000000"), 1)
        .encrypt(Bytes(100, 0), Bytes(16, 0));
  } catch (const std::invalid_argument&) {
    threw = true;
  }
  HG_CHECK(threw);
}

HG_TEST(aesv3_output_shape) {
  // 输出 = 1 字节密钥末位 + 输入原文
  const Bytes key = fromHex("f1593376766ea98d34f31b057a9d5be4");
  const Bytes iv = fromHex("1fe109a4125283f418de9e051a969e12");
  const Bytes data(288, 0x5a);
  const Bytes out = AesV3(key, 1800500000).encrypt(data, iv);
  HG_CHECK_EQ(out.size(), data.size() + 1);
}
