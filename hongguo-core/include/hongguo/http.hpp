// SPDX-License-Identifier: GPL-3.0-only
//
// libcurl 薄封装。整个项目唯一的外部依赖（除内置的 JSON 解析器外）。
// 选 libcurl 的理由：macOS 系统自带，Switch 有 switch-curl，两端都不必自带 TLS 栈。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "hongguo/bytes.hpp"
#include "hongguo/signer.hpp"  // HeaderList

namespace hongguo {

struct HttpResponse {
  int status = 0;
  HeaderList headers;
  Bytes body;

  // 大小写不敏感地取响应头。缺失返回空串。
  std::string header(const std::string& name) const;
};

class Http {
 public:
  Http();
  ~Http();

  Http(const Http&) = delete;
  Http& operator=(const Http&) = delete;

  HttpResponse get(const std::string& url, const HeaderList& headers,
                   int64_t timeoutSeconds = 30);

  HttpResponse post(const std::string& url, const HeaderList& headers,
                    const Bytes& body, int64_t timeoutSeconds = 30);

  // 闭区间范围请求。end 传 -1 表示"直到文件末尾"（对应 `bytes=start-`）。
  HttpResponse getRange(const std::string& url, const HeaderList& headers,
                        int64_t start, int64_t end, int64_t timeoutSeconds = 30);

 private:
  // 带重试的外层入口。瞬时故障（连接/握手超时）会自动重试。
  HttpResponse perform(const std::string& url, const HeaderList& headers,
                       const Bytes* body, const std::string* range,
                       int64_t timeoutSeconds);

  // 单次请求，失败时抛出内部异常以便按错误码决定是否重试。
  HttpResponse performOnce(const std::string& url, const HeaderList& headers,
                           const Bytes* body, const std::string* range,
                           int64_t timeoutSeconds);
};

// 全局初始化/清理（libcurl 的 curl_global_init）。进程内调用一次即可。
void httpGlobalInit();

}  // namespace hongguo
