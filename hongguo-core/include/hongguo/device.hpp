// SPDX-License-Identifier: GPL-3.0-only
//
// 设备指纹报文。medusa 签名会把它整体嵌进请求里。
//
// 40 项字段里混有 sint / float / string / 嵌套 message 四种编码，
// 是检验 protobuf 编码器最完整的单一素材。
#pragma once

#include <string_view>

#include "hongguo/bytes.hpp"

namespace hongguo {

Bytes deviceProto(std::string_view deviceId, std::string_view version);

}  // namespace hongguo
