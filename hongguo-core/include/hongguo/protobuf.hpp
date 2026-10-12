// SPDX-License-Identifier: GPL-3.0-only
//
// 手工 protobuf 编码器。只做编码，不做解析 —— 签名只需要把结构体拼成字节。
//
// 移植来源：hongguo.cjs 的 varint / zigzag / protoField / proto 四个函数。
// 刻意保持与之一一对应的函数形态，便于逐字段对拍。
#pragma once

#include <cstdint>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

#include "hongguo/bytes.hpp"

namespace hongguo::protobuf {

// 基 128 变长整数。
Bytes varint(uint64_t value);

// ZigZag 编码：把有符号数映射为非负。
Bytes zigzag(int64_t value);

// ---- 生成单个字段的编码 ----
//
// 与原实现一致：值为空字符串时字段**整体省略**（返回空）。
// 注意空字符串与空字节数组待遇不同 —— 原实现用 `value === ''` 判空，
// 该判断对 Buffer 恒为假，故空字节数组仍会编码出「tag + 长度 0」。

Bytes fieldString(int tag, std::string_view value);
Bytes fieldBytes(int tag, const Bytes& value);
Bytes fieldMessage(int tag, const Bytes& body);
Bytes fieldSint(int tag, int64_t value);
Bytes fieldInt(int tag, uint64_t value);
Bytes fieldFloat(int tag, float value);

// ---- 按顺序拼接 ----

Bytes encode(std::initializer_list<Bytes> fields);
Bytes encode(const std::vector<Bytes>& fields);

}  // namespace hongguo::protobuf
