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
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <functional>

#include "hongguo/dash_reader.hpp"
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

  // DASH 模式：把两条轨合成的一条 fMP4 当普通单流喂出去。
  // （目标机的 ffmpeg 没有 dash 解复用器，MPD 那条路走不通 —— 见 DashMuxer 的注释。）
  std::string startMux(DashMuxer& muxer);

  void stop();

  // 通过回环地址自测若干 Range 请求，返回可读报告。
  // 目的是在没有人肉播放器的情况下，也能证明"这一层是对的"。
  std::string selfTest();

  uint16_t port() const { return port_; }

  // 诊断用：收到了多少条连接、回了多少字节。
  // 真机上 mpv 若一直 0/0，先看这两个数 —— 都是 0 说明 mpv 根本没连上代理。
  uint64_t connectionCount() const { return connections_.load(); }
  uint64_t servedBytes() const { return servedBytes_.load(); }

 private:
  void acceptLoop();
  void handleConnection(int client);

  // 起监听并返回端口。start / startDash 共用。
  uint16_t bindListener();
  // 把一条连接的请求按 [start, end] 区间吐出去。
  //
  // ⚠ mutex 可以为 nullptr：合成模式下音频与视频是**两条独立**的轨，各自有缓存、
  //   各自上锁，音视频读因此能真正并行；再套一把全局锁只会让它们互相排队。
  //   单流模式（StreamReader）没有内部锁，仍然必须传 &readerMutex_。
  void serveRange(int client, bool head, int64_t total, const std::string& contentType,
                  std::mutex* mutex, const std::function<Bytes(int64_t, int)>& read,
                  int64_t start, int64_t end, bool ranged);

  // 连接之间并发，但对 reader 的读取必须串行
  std::mutex readerMutex_;
  // 每条连接一个线程。必须由 stop() 全部 join 之后再让调用方销毁 reader ——
  // 否则线程还在用 reader 时它已被释放，就是 use-after-free。
  std::mutex workersMutex_;
  std::vector<std::thread> workers_;

  StreamReader* reader_ = nullptr;
  DashMuxer* muxer_ = nullptr;
  int listenFd_ = -1;
  uint16_t port_ = 0;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> connections_{0};
  std::atomic<uint64_t> servedBytes_{0};
  std::thread worker_;
};

}  // namespace hongguo
