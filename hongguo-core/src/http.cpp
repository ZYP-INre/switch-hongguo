// SPDX-License-Identifier: GPL-3.0-only
#include "hongguo/http.hpp"

#include <curl/curl.h>

#include <cctype>
#include <chrono>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace hongguo {
namespace {

// ---- 连接复用 ----
//
// 原来每个请求都 curl_easy_init() 一个新 handle，用完即毁 —— 意味着每次 Range
// 都要重做一遍 TCP + TLS 握手。Switch 上一次握手几百毫秒，一个 10MB 的视频
// 拆成几十个 Range，光握手就十几秒，播放自然卡。
//
// curl_share 把连接缓存与 DNS 缓存跨 handle 共享，握手只做一次。
// 跨线程使用共享句柄必须提供 lock/unlock 回调，否则 libcurl 不保证安全。
// ⚠ 锁必须**按数据类型分开**，不能一把锁保护全部。
//   libcurl 在同一次操作里会依次锁住不同类型（例如先 CONNECT 再 DNS），
//   用单把非重入互斥量会当场死锁 —— 表现是请求永远挂住不回，界面一直"加载中"。
//   这里照 libcurl 官方示例：每种 curl_lock_data 各一把锁。
std::mutex gShareMutexes[CURL_LOCK_DATA_LAST];
CURLSH* gShare = nullptr;
std::once_flag gShareOnce;

void shareLock(CURL*, curl_lock_data data, curl_lock_access, void*) {
  if (data < CURL_LOCK_DATA_LAST) gShareMutexes[data].lock();
}
void shareUnlock(CURL*, curl_lock_data data, void*) {
  if (data < CURL_LOCK_DATA_LAST) gShareMutexes[data].unlock();
}

CURLSH* sharedCache() {
  std::call_once(gShareOnce, [] {
    gShare = curl_share_init();
    if (!gShare) return;
    curl_share_setopt(gShare, CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
    curl_share_setopt(gShare, CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);
    curl_share_setopt(gShare, CURLSHOPT_LOCKFUNC, shareLock);
    curl_share_setopt(gShare, CURLSHOPT_UNLOCKFUNC, shareUnlock);
  });
  return gShare;
}

constexpr int kMaxAttempts = 3;
constexpr int kConnectTimeoutSeconds = 20;

// 携带 curl 错误码的内部异常，供重试判定使用。
struct CurlFailure {
  CURLcode code;
  std::string message;
};

// 连接/握手阶段的瞬时故障才值得重试。
//
// 红果的第二跳 CDN（vas-lf-x.snssdk.com）实测握手常在 10 秒上下且偶发失败，
// 不放重试基本没法用；而 4xx/5xx 这类业务错误重试也没意义。
bool isTransient(CURLcode code) {
  switch (code) {
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
    case CURLE_PARTIAL_FILE:
      return true;
    default:
      return false;
  }
}

struct Sink {
  Bytes body;
  HeaderList headers;
};

size_t sinkBody(char* data, size_t size, size_t count, void* userdata) {
  auto* sink = static_cast<Sink*>(userdata);
  const size_t total = size * count;
  sink->body.insert(sink->body.end(), data, data + total);
  return total;
}

size_t sinkHeader(char* data, size_t size, size_t count, void* userdata) {
  auto* sink = static_cast<Sink*>(userdata);
  const size_t total = size * count;
  std::string line(data, total);
  while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
  const size_t colon = line.find(':');
  if (colon != std::string::npos) {
    std::string name = line.substr(0, colon);
    std::string value = line.substr(colon + 1);
    const size_t begin = value.find_first_not_of(" \t");
    value = begin == std::string::npos ? std::string() : value.substr(begin);
    sink->headers.emplace_back(std::move(name), std::move(value));
  }
  return total;
}

bool iequals(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(a[i])) !=
        std::tolower(static_cast<unsigned char>(b[i]))) {
      return false;
    }
  }
  return true;
}

}  // namespace

std::string HttpResponse::header(const std::string& name) const {
  for (const auto& item : headers) {
    if (iequals(item.first, name)) return item.second;
  }
  return {};
}

void httpGlobalInit() { curl_global_init(CURL_GLOBAL_DEFAULT); }

Http::Http() = default;
Http::~Http() = default;

HttpResponse Http::perform(const std::string& url, const HeaderList& headers,
                           const Bytes* body, const std::string* range,
                           int64_t timeoutSeconds) {
  std::string lastError = "网络请求失败";
  for (int attempt = 0; attempt < kMaxAttempts; ++attempt) {
    if (attempt != 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(300 * attempt));
    }
    try {
      return performOnce(url, headers, body, range, timeoutSeconds);
    } catch (const CurlFailure& failure) {
      lastError = failure.message;
      if (!isTransient(failure.code)) break;
    }
  }
  throw std::runtime_error(lastError);
}

HttpResponse Http::performOnce(const std::string& url, const HeaderList& headers,
                               const Bytes* body, const std::string* range,
                               int64_t timeoutSeconds) {
  CURL* handle = curl_easy_init();
  if (!handle) throw CurlFailure{CURLE_FAILED_INIT, "curl 初始化失败"};

  curl_slist* list = nullptr;
  char error[CURL_ERROR_SIZE] = {0};

  struct Cleanup {
    CURL* handle;
    curl_slist* list;
    ~Cleanup() {
      if (list) curl_slist_free_all(list);
      if (handle) curl_easy_cleanup(handle);
    }
  } cleanup{handle, list};

  Sink sink;
  curl_easy_setopt(handle, CURLOPT_URL, url.c_str());
  curl_easy_setopt(handle, CURLOPT_ERRORBUFFER, error);
  curl_easy_setopt(handle, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(handle, CURLOPT_MAXREDIRS, 5L);
  curl_easy_setopt(handle, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");  // 自动协商并解压
  curl_easy_setopt(handle, CURLOPT_TCP_KEEPALIVE, 1L);
  if (CURLSH* share = sharedCache()) curl_easy_setopt(handle, CURLOPT_SHARE, share);
  curl_easy_setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
  curl_easy_setopt(handle, CURLOPT_WRITEFUNCTION, sinkBody);
  curl_easy_setopt(handle, CURLOPT_WRITEDATA, &sink);
  curl_easy_setopt(handle, CURLOPT_HEADERFUNCTION, sinkHeader);
  curl_easy_setopt(handle, CURLOPT_HEADERDATA, &sink);
  curl_easy_setopt(handle, CURLOPT_TIMEOUT, static_cast<long>(timeoutSeconds));
  curl_easy_setopt(handle, CURLOPT_CONNECTTIMEOUT, static_cast<long>(kConnectTimeoutSeconds));

  for (const auto& item : headers) {
    list = curl_slist_append(list, (item.first + ": " + item.second).c_str());
  }
  if (range) {
    list = curl_slist_append(list, ("Range: " + *range).c_str());
  }
  curl_easy_setopt(handle, CURLOPT_HTTPHEADER, list);
  cleanup.list = list;

  if (body) {
    curl_easy_setopt(handle, CURLOPT_POST, 1L);
    curl_easy_setopt(handle, CURLOPT_POSTFIELDS,
                     reinterpret_cast<const char*>(body->data()));
    curl_easy_setopt(handle, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(body->size()));
  }

  const CURLcode code = curl_easy_perform(handle);
  if (code != CURLE_OK) {
    const std::string detail =
        error[0] ? std::string(error) : std::string(curl_easy_strerror(code));
    throw CurlFailure{code, "网络请求失败: " + detail};
  }

  HttpResponse response;
  long status = 0;
  curl_easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
  response.status = static_cast<int>(status);
  response.headers = std::move(sink.headers);
  response.body = std::move(sink.body);
  return response;
}

HttpResponse Http::get(const std::string& url, const HeaderList& headers,
                       int64_t timeoutSeconds) {
  return perform(url, headers, nullptr, nullptr, timeoutSeconds);
}

HttpResponse Http::post(const std::string& url, const HeaderList& headers,
                        const Bytes& body, int64_t timeoutSeconds) {
  return perform(url, headers, &body, nullptr, timeoutSeconds);
}

HttpResponse Http::getRange(const std::string& url, const HeaderList& headers,
                            int64_t start, int64_t end, int64_t timeoutSeconds) {
  const std::string range =
      end < 0 ? "bytes=" + std::to_string(start) + "-"
              : "bytes=" + std::to_string(start) + "-" + std::to_string(end);
  return perform(url, headers, nullptr, &range, timeoutSeconds);
}

}  // namespace hongguo
