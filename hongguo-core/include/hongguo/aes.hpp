// SPDX-License-Identifier: GPL-3.0-only
//
// 标准 AES-128。只实现红果链路真正用到的两个模式：
//   * CBC 解密 —— 解播放地址（MediaCrypto.decryptUrl）
//   * CTR      —— 解视频本体（MediaCrypto.decrypt）
//
// 红果链路里所有 AES 密钥都是 16 字节，故只做 128 位。
#pragma once

#include "hongguo/bytes.hpp"

namespace hongguo {

// AES-128-CBC 解密，NoPadding。data 长度必须是 16 的整数倍。
Bytes aes128CbcDecrypt(const Bytes& key, const Bytes& iv, const Bytes& data);

// AES-128-CTR 变换。CTR 是对合运算，加密与解密是同一个调用。
//
// 计数器语义：整个 128 位块按大端加一 —— 与 Node 的 aes-128-ctr 一致。
Bytes aes128Ctr(const Bytes& key, const Bytes& iv, const Bytes& data);

}  // namespace hongguo
