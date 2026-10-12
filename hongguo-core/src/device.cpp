// SPDX-License-Identifier: GPL-3.0-only
#include "hongguo/device.hpp"

#include "hongguo/protobuf.hpp"

namespace hongguo {
namespace pb = protobuf;

// 逐项对照 hongguo.cjs 的 deviceProto()。字段顺序即语义，不可重排。
//
// 关于 version：上游 Node 版签名是 deviceProto(deviceId, version)，
// 但 version 从未进入字段表；Kotlin 版干脆没有这个参数。此处保留形参
// 以对齐上游签名，便于日后上游真的启用它时能对上。
Bytes deviceProto(std::string_view deviceId, std::string_view version) {
  (void)version;  // 上游未使用
  return pb::encode({
      pb::fieldSint(1, 1),
      pb::fieldSint(2, 2),
      pb::fieldString(3, "8662"),
      pb::fieldString(4, deviceId),
      pb::fieldString(5, "Ai6svO3PyrwDOUSmO6ZcResxu"),
      pb::fieldString(6, "!noperm!"),
      pb::fieldSint(7, -888888),
      pb::fieldSint(8, -888888),
      pb::fieldSint(9, 3),
      pb::fieldSint(10, -888888),
      pb::fieldString(11, "!notset!"),
      pb::fieldString(12, "Asia/Shanghai,8"),
      pb::fieldString(13, "zh_CN"),
      pb::fieldSint(14, 4),
      pb::fieldFloat(16, 255.24993896484375f),
      pb::fieldFloat(17, 35.58599090576172f),
      pb::fieldFloat(18, 3.467449188232422f),
      pb::fieldFloat(19, 3.467449188232422f),
      pb::fieldFloat(20, 255.1754913330078f),
      pb::fieldFloat(21, 42.17544174194336f),
      pb::fieldString(22, "16"),
      pb::fieldSint(23, 41),
      pb::fieldSint(24, 36),
      pb::fieldSint(25, 1728388016635LL),
      pb::fieldSint(26, 1728388016635LL),
      pb::fieldSint(27, 1728388016635LL),
      pb::fieldSint(28, 1728388016637LL),
      pb::fieldSint(29, -1),
      pb::fieldString(30, "25053RT47C"),
      pb::fieldString(31, "Redmi"),
      pb::fieldString(32, "25053RT47C"),
      pb::fieldString(33, "25053RT47C"),
      pb::fieldString(34, "Xiaomi"),
      pb::fieldString(35, "Redmi"),
      pb::fieldString(36, "Redmi"),
      pb::fieldSint(38, 31),
  });
}

}  // namespace hongguo
