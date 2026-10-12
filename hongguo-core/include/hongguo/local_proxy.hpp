// SPDX-License-Identifier: GPL-3.0-only
//
// 本地 HTTP 流服务。
//
// 为什么必须有这一层：红果视频是 CENC 加密的 MP4，mpv 没有能力在解复用**之前**
// 插入我们的解密逻辑。所以做法是进程内起一个 HTTP 服务，mpv 去播
// http://127.0.0.1:PORT/stream，这个服务收到 Range 请求后去 CDN 拉密文、
// 解密、再把明文吐回去。
//
// hongguoTV 在 Android 上走 VideoDataSource、在 Node 服务里走 /api/episodes/:id/stream，
// 是同一个思路。这里把它挪进进程内。
#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <thread>

#include "hongguo/http.hpp"
#include "hongguo/stream_reader.hpp"

namespace hongguo {

class LocalProxy {
 public:
  LocalProxy();
  ~LocalProxy();

  LocalProxy(const LocalProxy&) = delete;
  LocalProxy& operator=(const LocalProxy&) = delete;

  // 启动服务并返回可喂给 mpv 的地址。重复调用返回同一地址。
  std::string start(StreamReader& reader);

  void stop();

  // 通过回环地址自测若干 Range 请求，返回可读报告。
  // 目的是在没有人肉播放器的情况下，也能证明"这一层是对的"。
  std::string selfTest();

  uint16_t port() const { return port_; }

 private:
  void acceptLoop();
  void handleConnection(int client);

  StreamReader* reader_ = nullptr;
  int listenFd_ = -1;
  uint16_t port_ = 0;
  std::atomic<bool> running_{false};
  std::thread worker_;
};

}  // namespace hongguo
