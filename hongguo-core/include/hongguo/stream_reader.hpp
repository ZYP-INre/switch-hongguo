// SPDX-License-Identifier: GPL-3.0-only
//
// 按区间拉取并解密视频流。移植自 hongguoTV 的 RemoteVideo.kt。
//
// 这里没有 socket，也不起服务 —— 只负责"给我 [start, start+length) 的明文"。
// 对外提供 HTTP 是 local_proxy 的事，两者刻意分开：代理出问题时不必怀疑密码学。
#pragma once

#include <cstdint>
#include <string>

#include "hongguo/http.hpp"
#include "hongguo/media_crypto.hpp"
#include "hongguo/model.hpp"

namespace hongguo {

class StreamReader {
 public:
  StreamReader(Http& http, StreamInfo info);

  // 拉首个 256KiB 分片，解析 moov，建立样本索引。可重复调用。
  void prepare();

  // prepare() 之上再预热 1MiB 前缀，减少起播时的往返。
  void warm();

  // 读取并解密区间。返回的字节数可能少于请求量（到达文件末尾时）。
  Bytes read(int64_t start, int length);

  int64_t total() const { return total_; }
  bool hasKey() const { return info_.key.has_value() && !info_.key->empty(); }
  const StreamInfo& info() const { return info_; }

 private:
  struct RangeResult {
    Bytes bytes;
    int64_t total = 0;
  };

  RangeResult range(int64_t start, int64_t end);

  Http& http_;
  StreamInfo info_;
  int64_t total_ = 0;
  bool prepared_ = false;
  media_crypto::Prepared cenc_;
  Bytes prefix_;
};

}  // namespace hongguo
