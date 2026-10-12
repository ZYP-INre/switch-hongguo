// SPDX-License-Identifier: GPL-3.0-only
//
// 播放地址解密 + CENC 加密 MP4 的索引解析与分片解密。
//
// ⚠ 本模块以 hongguoTV 的 **Kotlin** 版（MediaCrypto.kt）为准，不是 Node 版。
//   Node 版有两处已知缺陷，Kotlin 版已修：
//     1. Node 把样本条目的 fourcc 硬编码改为 hvc1/mp4a；Kotlin 从 frma 还原原始编码
//        （AVC 与 HEVC 混用时前者会直接播不出来）
//     2. Node 不支持子样本（auxSize != 8）加密；Kotlin 支持并保留明文段
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hongguo/bytes.hpp"

namespace hongguo::media_crypto {

// 一段需要解密的样本（或子样本）。
// encryptedOffset 用于子样本：该样本内部第一个加密字节的偏移。
struct Sample {
  int64_t start = 0;
  int64_t end = 0;
  Bytes iv;                      // 16 字节
  int64_t encryptedOffset = 0;
};

struct Prepared {
  Bytes header;                 // 已还原 fourcc、抹掉 sinf 的容器头部
  std::vector<Sample> samples;  // 按 start 升序，区间互不重叠
};

// spade_a → 16 字节内容密钥。
Bytes deriveKey(const std::string& value);

// key_seed 解密播放地址。失败抛 std::runtime_error。
std::string decryptUrl(const std::string& value, const Bytes& seed);

// 由首个分片推算容器头部（ftyp + moov）的结束位置。
// 要求 moov 紧跟在 ftyp 之后且不超过 8MiB —— 红果资源满足此前提。
int64_t headerEnd(const Bytes& first, int64_t total);

// 解析 moov，建立样本索引并把容器头部改写成可被常规解复用器接受的形态。
// key 为空时只改写头部，不建索引（对应未加密资源）。
Prepared prepare(const Bytes& data, const std::optional<Bytes>& key);

// 解密 [start, start+data.size()) 区间。未落在加密样本上的字节原样返回。
Bytes decrypt(const Bytes& data, int64_t start, const std::optional<Bytes>& key,
              const Prepared& prepared);

}  // namespace hongguo::media_crypto
