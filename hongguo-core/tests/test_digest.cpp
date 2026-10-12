// SPDX-License-Identifier: GPL-3.0-only
#include "harness.hpp"
#include "hongguo/digest.hpp"

using namespace hongguo;

// 向量由 Node 标准 crypto 生成（见 tools/oracle/gen-vectors.mjs）。
// 覆盖各算法的填充边界：MD5/SM3 块长 64（55/56/64），SHA-512 块长 128（111/112/128）。

HG_TEST(digest_matches_node_crypto) {
  const auto rows = hgtest::tsv("digest.txt");
  HG_CHECK(!rows.empty());

  size_t checked = 0;
  for (const auto& row : rows) {
    HG_CHECK(row.size() == 3);
    const std::string& op = row[0];
    const Bytes input = fromHex(row[1]);
    const std::string expected = row[2];

    std::string got;
    if (op == "md5") {
      got = md5Hex(input);
    } else if (op == "sha512") {
      got = sha512Hex(input);
    } else if (op == "sm3") {
      got = toHex(sm3(input));
    } else {
      HG_CHECK(false);  // 未知操作符：向量文件与测试脱节了
    }

    if (got != expected) {
      ::hgtest::fail("向量 " + op + " 输入长度 " + std::to_string(input.size()) +
                     "\n    实得: " + got + "\n    期望: " + expected);
    }
    ++checked;
  }
  HG_CHECK(checked == rows.size());
}

HG_TEST(digest_output_sizes) {
  HG_CHECK_EQ(md5(Bytes{}).size(), 16u);
  HG_CHECK_EQ(sha512(Bytes{}).size(), 64u);
  HG_CHECK_EQ(sm3(Bytes{}).size(), 32u);
}

HG_TEST(digest_known_empty_vectors) {
  // 三条空消息的经典值，作为"实现没跑偏"的兜底（不依赖向量文件）
  HG_CHECK_EQ(md5Hex(Bytes{}), std::string("d41d8cd98f00b204e9800998ecf8427e"));
  HG_CHECK_EQ(toHex(sm3(Bytes{})),
              std::string("1ab21d8355cfa17f8e61194831e81a8f22bec8c728fefb747ed035eb5082aa2b"));
}

HG_TEST(digest_hex_helpers) {
  const Bytes data = fromHex("616263");  // "abc"
  // MD5("abc") 的大写形式 —— x-ss-stub 需要大写
  HG_CHECK_EQ(md5HexUpper(data), std::string("900150983CD24FB0D6963F7D28E17F72"));
  HG_CHECK_EQ(md5Hex(data), std::string("900150983cd24fb0d6963f7d28e17f72"));
}
