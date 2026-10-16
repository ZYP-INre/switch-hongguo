// SPDX-License-Identifier: GPL-3.0-only
#include "hongguo/local_proxy.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <mutex>
#include <vector>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace hongguo {
namespace {

// ⚠ 128KB，刻意偏小。
//
// wiliwili 给 mpv 设了 network-timeout=5（见 video_view.cpp 的 genExtraUrlParam），
// 意思是「5 秒收不到数据就放弃」。而本代理是收到请求才去 CDN 拉、拉完解密再吐，
// 块越大首字节越晚 —— 1MB 的块要现拉 768KB，在 Switch 上轻易超过 5 秒，
// mpv 就会直接放弃，表现为进度 0/0、有画面框但不播。
//
// 用 128KB：前两块正好命中 prepare() 预取的 256KB 前缀缓存，**立刻就能发出去**，
// 之后每块只需拉 128KB，远低于超时阈值。连接已复用，多几次请求的代价可以接受。
constexpr size_t kChunkSize = 128 * 1024;

// 返回 false 表示对端已经断开。
//
// ⚠ 这个返回值必须被用起来。播放器读够了就关连接，而我们这边还在按 128KB
//   一段段去 CDN 拉数据 —— 不因为写失败而停下的话，每条被抛弃的连接都会
//   继续跑完整段（十几 MB），线程与文件描述符只增不减，
//   最后 accept 拿到 EMFILE，内核把新连接直接 RST，表现就是
//   "端口还在监听却 Connection refused"。
bool writeAll(int fd, const char* data, size_t size) {
  size_t sent = 0;
  while (sent < size) {
    const ssize_t written = ::send(fd, data + sent, size - sent, 0);
    if (written <= 0) return false;
    sent += static_cast<size_t>(written);
  }
  return true;
}

bool writeAll(int fd, const std::string& text) { return writeAll(fd, text.data(), text.size()); }

// 读到请求头结束（\r\n\r\n）为止。
std::string readRequest(int fd) {
  std::string buffer;
  char chunk[1024];
  while (buffer.find("\r\n\r\n") == std::string::npos && buffer.size() < 16384) {
    const ssize_t got = ::recv(fd, chunk, sizeof(chunk), 0);
    if (got <= 0) break;
    buffer.append(chunk, static_cast<size_t>(got));
  }
  return buffer;
}

std::string lower(std::string value) {
  for (char& ch : value) {
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  }
  return value;
}

// 解析 "bytes=start-end" / "bytes=start-" / "bytes=-suffix"
bool parseRange(const std::string& value, int64_t total, int64_t& start, int64_t& end) {
  if (value.rfind("bytes=", 0) != 0) return false;
  const std::string spec = value.substr(6);
  const size_t dash = spec.find('-');
  if (dash == std::string::npos) return false;

  const std::string left = spec.substr(0, dash);
  const std::string right = spec.substr(dash + 1);

  if (left.empty()) {
    // 后缀区间：最后 N 字节
    if (right.empty()) return false;
    const int64_t suffix = std::stoll(right);
    if (suffix <= 0) return false;
    start = std::max<int64_t>(0, total - suffix);
    end = total - 1;
    return true;
  }

  start = std::stoll(left);
  if (right.empty()) {
    end = total - 1;
  } else {
    end = std::stoll(right);
  }
  end = std::min(end, total - 1);
  return start <= end;
}

}  // namespace

LocalProxy::LocalProxy() = default;

LocalProxy::~LocalProxy() { stop(); }

std::string LocalProxy::start(StreamReader& reader) {
  if (running_) {
    return "http://127.0.0.1:" + std::to_string(port_) + "/stream";
  }
  reader_ = &reader;

  bindListener();
  running_ = true;
  worker_ = std::thread([this] { acceptLoop(); });
  return "http://127.0.0.1:" + std::to_string(port_) + "/stream";
}

uint16_t LocalProxy::bindListener() {
  // 对端提前断开时不要用 SIGPIPE 打死整个进程
  ::signal(SIGPIPE, SIG_IGN);

  listenFd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listenFd_ < 0) throw std::runtime_error("无法创建监听套接字");

  int reuse = 1;
  ::setsockopt(listenFd_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

  sockaddr_in address{};
  address.sin_family = AF_INET;
  address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 只监听回环，不对外暴露
  address.sin_port = 0;                                // 由内核分配空闲端口

  if (::bind(listenFd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    throw std::runtime_error("无法绑定回环端口");
  }
  // ⚠ backlog 不能小。每条连接都带 Connection: close，播放器一次会打开好几条
  //   （视频、音频、seek 各一条），队列一满内核对新连接就直接回 RST ——
  //   表现是"本地端口明明还在监听，却 Connection refused"。
  if (::listen(listenFd_, 128) < 0) {
    ::close(listenFd_);
    listenFd_ = -1;
    throw std::runtime_error("监听失败");
  }

  sockaddr_in bound{};
  socklen_t length = sizeof(bound);
  if (::getsockname(listenFd_, reinterpret_cast<sockaddr*>(&bound), &length) == 0) {
    port_ = ntohs(bound.sin_port);
  }

  return port_;
}

std::string LocalProxy::startMux(DashMuxer& muxer) {
  if (running_) return "http://127.0.0.1:" + std::to_string(port_) + "/stream";
  muxer_ = &muxer;
  bindListener();
  running_ = true;
  worker_ = std::thread([this] { acceptLoop(); });
  return "http://127.0.0.1:" + std::to_string(port_) + "/stream";
}

void LocalProxy::stop() {
  if (!running_.exchange(false)) return;

  if (listenFd_ >= 0) {
    ::shutdown(listenFd_, SHUT_RDWR);
    ::close(listenFd_);
    listenFd_ = -1;
  }
  if (worker_.joinable()) worker_.join();   // 等 accept 线程退出

  // ⚠ 必须把**每条连接**的线程也等回来，再让调用方销毁 reader。
  //   之前只 join 了 accept 线程，连接线程还在用 reader 时它就被 reset() 了 ——
  //   快速切换剧集/剧时最容易撞上：上一部的连接没断，新的一次 closeStream
  //   就把 reader 拆掉，直接 use-after-free，表现为卡死、退不出、崩溃。
  std::vector<std::thread> pending;
  {
    std::lock_guard<std::mutex> guard(workersMutex_);
    pending.swap(workers_);
  }
  for (std::thread& t : pending) {
    if (t.joinable()) t.join();
  }
}

void LocalProxy::acceptLoop() {
  while (running_) {
    const int client = ::accept(listenFd_, nullptr, nullptr);
    if (client < 0) {
      if (std::getenv("HG_PROXY_DEBUG")) {
        std::fprintf(stderr, "[proxy] accept 失败: %s\n", std::strerror(errno));
      }
      if (!running_) break;
      continue;
    }
    if (std::getenv("HG_PROXY_DEBUG")) {
      std::fprintf(stderr, "[proxy] 接受连接 fd=%d\n", client);
    }
#ifdef SO_NOSIGPIPE
    int on = 1;
    ::setsockopt(client, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#endif
    // ⚠ 必须设发送超时。
    //   对端（mpv）若不读数据，socket 缓冲区写满后 send() 会永久阻塞；
    //   而 stop() 要等这条连接的线程结束 —— 于是整个 stop() 卡住、界面冻死。
    //   有超时后 send 最多等这么久就返回错误，线程得以退出。
    struct timeval sendTimeout {};
    sendTimeout.tv_sec = 5;
    ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &sendTimeout, sizeof(sendTimeout));
    // ⚠ 每个连接必须独立线程处理。
    //   mpv 经常同时开多条连接（读 moov、读数据、seek 时另开）。
    //   原来在 accept 循环里同步处理，第一条连接不结束第二条就进不来 ——
    //   mpv 等不到响应，表现就是进度 0/0、一直转圈。
    {
      std::lock_guard<std::mutex> guard(workersMutex_);
      workers_.emplace_back([this, client] {
        // ⚠ 必须兜住异常。分片拉取失败、容器解析异常都会从这里冒出来，
        //   而线程里漏出去的异常是 std::terminate —— 整个进程当场消失，
        //   表现就是"播几秒后连本地端口都连不上了"，极难定位。
        try {
          handleConnection(client);
        } catch (const std::exception& error) {
          std::fprintf(stderr, "[proxy] 连接处理失败：%s\n", error.what());
        } catch (...) {
          std::fprintf(stderr, "[proxy] 连接处理失败：未知异常\n");
        }
        ::close(client);
      });
    }
  }
}

void LocalProxy::handleConnection(int client) {
  connections_.fetch_add(1);
  const std::string request = readRequest(client);
  if (request.empty()) return;

  const size_t lineEnd = request.find("\r\n");
  const std::string requestLine =
      request.substr(0, lineEnd == std::string::npos ? request.size() : lineEnd);

  std::string method = "GET";
  std::string target = "/stream";
  {
    const size_t space = requestLine.find(' ');
    if (space != std::string::npos) {
      method = requestLine.substr(0, space);
      const size_t stop = requestLine.find(' ', space + 1);
      const size_t end = stop == std::string::npos ? requestLine.size() : stop;
      if (end > space + 1) target = requestLine.substr(space + 1, end - space - 1);
    }
  }
  if (const size_t query = target.find('?'); query != std::string::npos) {
    target = target.substr(0, query);
  }

  // 取 Range 头
  std::string rangeValue;
  {
    size_t cursor = 0;
    while (cursor < request.size()) {
      const size_t end = request.find("\r\n", cursor);
      if (end == std::string::npos) break;
      const std::string line = request.substr(cursor, end - cursor);
      cursor = end + 2;
      if (line.empty()) break;
      const size_t colon = line.find(':');
      if (colon == std::string::npos) continue;
      if (lower(line.substr(0, colon)) == "range") {
        const size_t begin = line.find_first_not_of(" \t", colon + 1);
        if (begin != std::string::npos) rangeValue = line.substr(begin);
      }
    }
  }
  const bool head = method == "HEAD";
  if (std::getenv("HG_PROXY_DEBUG")) {
    std::fprintf(stderr, "[proxy] %s %s range=[%s]\n", method.c_str(), target.c_str(),
                 rangeValue.c_str());
  }

  // ---- 合成流模式（新通道）：两条 DASH 轨拼成的一条普通 fMP4 ----
  if (muxer_ != nullptr) {
    const int64_t total = muxer_->total();
    if (total <= 0) {
      writeAll(client, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n");
      return;
    }
    int64_t start = 0;
    int64_t end = total - 1;
    if (!rangeValue.empty() && !parseRange(rangeValue, total, start, end)) {
      const std::string body = "range not satisfiable";
      writeAll(client, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                           std::to_string(total) + "\r\nContent-Length: " +
                           std::to_string(body.size()) + "\r\n\r\n" + body);
      return;
    }
    serveRange(client, head, total, "video/mp4", readerMutex_,
               [this](int64_t at, int count) { return muxer_->read(at, count); }, start, end,
               !rangeValue.empty());
    return;
  }

  // ---- 单流模式（旧通道）----
  if (reader_ == nullptr) {
    writeAll(client, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n");
    return;
  }
  const int64_t total = reader_->total();
  if (total <= 0) {
    writeAll(client, "HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\n\r\n");
    return;
  }

  int64_t start = 0;
  int64_t end = total - 1;
  const bool ranged = !rangeValue.empty() && parseRange(rangeValue, total, start, end);
  if (!rangeValue.empty() && !ranged) {
    const std::string body = "range not satisfiable";
    writeAll(client, "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */" +
                         std::to_string(total) + "\r\nContent-Length: " +
                         std::to_string(body.size()) + "\r\n\r\n" + body);
    return;
  }
  serveRange(client, head, total, "video/mp4", readerMutex_,
             [this](int64_t at, int count) { return reader_->read(at, count); }, start, end,
             ranged);
}

void LocalProxy::serveRange(int client, bool head, int64_t total, const std::string& contentType,
                            std::mutex& mutex,
                            const std::function<Bytes(int64_t, int)>& read, int64_t start,
                            int64_t end, bool ranged) {
  const int64_t length = end - start + 1;
  std::string header = ranged ? "HTTP/1.1 206 Partial Content\r\n" : "HTTP/1.1 200 OK\r\n";
  header += "Content-Type: " + contentType + "\r\n";
  header += "Accept-Ranges: bytes\r\n";
  header += "Content-Length: " + std::to_string(length) + "\r\n";
  if (ranged) {
    header += "Content-Range: bytes " + std::to_string(start) + "-" + std::to_string(end) + "/" +
              std::to_string(total) + "\r\n";
  }
  header += "Connection: close\r\n\r\n";
  writeAll(client, header);
  if (head) return;

  for (int64_t at = start; at <= end;) {
    if (!running_) break;  // 收到停止信号就尽快退出，缩短 stop() 的等待
    const int chunk = static_cast<int>(std::min<int64_t>(kChunkSize, end - at + 1));
    Bytes piece;
    {
      // 读动作串行：StreamReader 有前缀缓存与共享连接，多线程同时读会互相踩。
      // DASH 的两条轨各用各的锁，视频与音频可以真正并行。
      std::lock_guard<std::mutex> guard(mutex);
      piece = read(at, chunk);
    }
    if (piece.empty()) {
      if (std::getenv("HG_PROXY_DEBUG")) {
        std::fprintf(stderr, "[proxy] 读到空块，提前结束 at=%lld\n", (long long)at);
      }
      break;
    }
    servedBytes_.fetch_add(piece.size());
    if (!writeAll(client, reinterpret_cast<const char*>(piece.data()), piece.size())) {
      if (std::getenv("HG_PROXY_DEBUG")) {
        std::fprintf(stderr, "[proxy] 对端断开 at=%lld\n", (long long)at);
      }
      break;
    }
    at += static_cast<int64_t>(piece.size());
  }
}

std::string LocalProxy::selfTest() {
  if (!running_) throw std::runtime_error("代理尚未启动");
  const int64_t total = reader_->total();

  Http http;
  const std::string base = "http://127.0.0.1:" + std::to_string(port_) + "/stream";

  std::string report;
  // 覆盖：开头、中间非对齐点、末尾、后缀区间
  struct Probe {
    const char* label;
    int64_t start;
    int64_t end;  // -1 表示到末尾
    bool suffix;
  };
  const Probe probes[] = {
      {"起始 256KiB", 0, 256 * 1024 - 1, false},
      {"中间非对齐", 1 << 20, (1 << 20) + 65535, false},
      {"末尾 64KiB", std::max<int64_t>(0, total - 65536), -1, false},
  };

  for (const Probe& probe : probes) {
    if (probe.start >= total) continue;
    const HttpResponse response =
        http.getRange(base, {}, probe.start, probe.end < 0 ? -1 : probe.end);

    const std::string contentRange = response.header("Content-Range");
    const int64_t wantStart = probe.start;
    const int64_t wantEnd = probe.end < 0 ? total - 1 : std::min(probe.end, total - 1);
    const int64_t wantLength = wantEnd - wantStart + 1;

    const bool ok = response.status == 206 &&
                    static_cast<int64_t>(response.body.size()) == wantLength &&
                    contentRange == "bytes " + std::to_string(wantStart) + "-" +
                                        std::to_string(wantEnd) + "/" +
                                        std::to_string(total);

    // 与直连 reader 的结果逐字节比对 —— 证明代理没有引入偏差
    bool identical = ok;
    if (ok) {
      const Bytes direct = reader_->read(wantStart, static_cast<int>(wantLength));
      identical = direct == response.body;
    }

    report += "  ";
    report += ok ? (identical ? "✓ " : "✗ ") : "✗ ";
    report += probe.label;
    report += "  HTTP " + std::to_string(response.status) + "，" +
              std::to_string(response.body.size()) + " 字节";
    if (!ok) report += "（期望 " + std::to_string(wantLength) + " 字节，" + contentRange + "）";
    else if (!identical) report += "（与直连结果不一致）";
    report += "\n";
  }

  // 后缀区间是 mpv 读 moov 时常用的形态，单独验一次
  {
    const HttpResponse response = http.getRange(base, {}, total - 4096, -1);
    const bool ok = response.status == 206 && response.body.size() == 4096;
    report += std::string("  ") + (ok ? "✓ " : "✗ ") + "末尾 Range 读取  HTTP " +
              std::to_string(response.status) + "，" + std::to_string(response.body.size()) +
              " 字节\n";
  }

  return report;
}

}  // namespace hongguo
