// SPDX-License-Identifier: GPL-3.0-only
#include "harness.hpp"
#include "hongguo/device.hpp"
#include "hongguo/protobuf.hpp"

using namespace hongguo;
namespace pb = hongguo::protobuf;

HG_TEST(protobuf_varint) {
  HG_CHECK_EQ(pb::varint(0), (Bytes{0x00}));
  HG_CHECK_EQ(pb::varint(1), (Bytes{0x01}));
  HG_CHECK_EQ(pb::varint(127), (Bytes{0x7f}));
  HG_CHECK_EQ(pb::varint(128), (Bytes{0x80, 0x01}));
  HG_CHECK_EQ(pb::varint(300), (Bytes{0xac, 0x02}));
  HG_CHECK_EQ(pb::varint(16383), (Bytes{0xff, 0x7f}));
  HG_CHECK_EQ(pb::varint(16384), (Bytes{0x80, 0x80, 0x01}));
}

HG_TEST(protobuf_zigzag) {
  HG_CHECK_EQ(pb::zigzag(0), (Bytes{0x00}));
  HG_CHECK_EQ(pb::zigzag(-1), (Bytes{0x01}));
  HG_CHECK_EQ(pb::zigzag(1), (Bytes{0x02}));
  HG_CHECK_EQ(pb::zigzag(-2), (Bytes{0x03}));
  HG_CHECK_EQ(pb::zigzag(2), (Bytes{0x04}));
  HG_CHECK_EQ(pb::zigzag(2147483647LL), pb::varint(4294967294ULL));
}

HG_TEST(protobuf_string_field_is_skipped_when_empty) {
  // 空字符串 → 字段整体省略
  HG_CHECK_EQ(pb::fieldString(3, ""), (Bytes{}));
  // 非空 → tag(3<<3|2)=0x1a、长度、内容
  HG_CHECK_EQ(pb::fieldString(3, "8662"),
              (Bytes{0x1a, 0x04, 0x38, 0x36, 0x36, 0x32}));
}

HG_TEST(protobuf_empty_bytes_field_is_kept) {
  // ⚠ 与空字符串不同：原实现用 `value === ''` 判空，该判断对 Buffer 恒为假，
  //   因此空字节数组仍会编码出「tag + 长度 0」。
  HG_CHECK_EQ(pb::fieldBytes(3, Bytes{}), (Bytes{0x1a, 0x00}));
  HG_CHECK_EQ(pb::fieldMessage(3, Bytes{}), (Bytes{0x1a, 0x00}));
}

HG_TEST(protobuf_sint_field) {
  // tag(1<<3)=0x08，值 zigzag(1)=2
  HG_CHECK_EQ(pb::fieldSint(1, 1), (Bytes{0x08, 0x02}));
  // zigzag(-1)=1
  HG_CHECK_EQ(pb::fieldSint(1, -1), (Bytes{0x08, 0x01}));
}

HG_TEST(protobuf_float_field_is_little_endian) {
  // tag(16<<3|5)=133 → 变长编码为 0x85 0x01，随后 4 字节小端 float
  const Bytes out = pb::fieldFloat(16, 1.0f);
  HG_CHECK_EQ(out.size(), 6u);
  HG_CHECK_EQ(out[0], static_cast<uint8_t>(0x85));
  HG_CHECK_EQ(out[1], static_cast<uint8_t>(0x01));
  // 1.0f 的 IEEE754 小端表示为 00 00 80 3f
  HG_CHECK_EQ(out[2], static_cast<uint8_t>(0x00));
  HG_CHECK_EQ(out[3], static_cast<uint8_t>(0x00));
  HG_CHECK_EQ(out[4], static_cast<uint8_t>(0x80));
  HG_CHECK_EQ(out[5], static_cast<uint8_t>(0x3f));
}

HG_TEST(deviceproto_matches_oracle) {
  // 40 项复合报文，一次覆盖 varint / zigzag / float / 嵌套 message 四条路径
  const auto rows = hgtest::tsv("proto.txt");
  HG_CHECK(!rows.empty());

  for (const auto& row : rows) {
    HG_CHECK(row.size() == 4);
    HG_CHECK_EQ(row[0], std::string("deviceproto"));
    const std::string got = toHex(deviceProto(row[1], row[2]));
    if (got != row[3]) {
      ::hgtest::fail("deviceproto " + row[1] +
                     "\n    实得: " + got + "\n    期望: " + row[3]);
    }
  }
}
