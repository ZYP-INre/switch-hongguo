// SPDX-License-Identifier: GPL-3.0-only
//
// 抖音系请求签名链。
//
// 设计要点：时钟与随机数**必须可注入**。签名里 ts 取当前秒、_rticket 取当前毫秒，
// gorgon / helios / medusa 各含随机数。若内部直接读系统时间，两个实现永远算不出
// 同一输入，"逐字节对拍"这条验收标准就无从谈起。
//
// 分支策略：强制 hashF13 的 branch 0（与 Kotlin 版一致，见设计文档 5.1 节）。
#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "hongguo/bytes.hpp"

namespace hongguo {

using HeaderList = std::vector<std::pair<std::string, std::string>>;

// 有序列值对。顺序直接进入签名，因此不可用 std::map 之类的有序容器替换。
using QueryValues = std::vector<std::pair<std::string, std::string>>;

// 百分号编码。等价于 Java URLEncoder.encode(v,"UTF-8") 再修掉 '+' 与 '~'，
// 也等价于 JS encodeURIComponent 再补转义 !'()。两者的不转义集合相同：
//   A-Z a-z 0-9 - _ . ~ *
std::string encodeQueryValue(std::string_view value);

// 拼成 a=1&b=2，键与值都做百分号编码。
std::string encodeQuery(const QueryValues& values);

class Signer {
 public:
  using Clock = std::function<int64_t()>;  // 毫秒
  using Rng = std::function<double()>;     // 期望 [0,1)

  Signer(Clock clock, Rng rng);

  struct Request {
    std::string url;
    HeaderList headers;  // 有序；直接照此下发
    std::optional<Bytes> body;
  };

  // 组装并签名一个请求。body 为空表示 GET。
  Request sign(const std::string& base, const QueryValues& values,
               std::optional<Bytes> body = std::nullopt);

  // ---- 以下仅为单测暴露，用于逐段对拍定位问题 ----

  static uint32_t getIv(uint32_t initial, const Bytes& data);
  static uint32_t sumMd5(const Bytes& data);

  int branch(const std::string& query, const std::optional<Bytes>& body, int ts) const;
  Bytes hashF13(const Bytes& querySm3, const Bytes& bodyMd5, int ts) const;

  std::string gorgon(const std::string& query, const std::optional<Bytes>& body,
                     int ts, int random) const;
  std::string helios(int ts);
  std::string medusa(const std::string& url, const std::optional<Bytes>& body, int ts);

 private:
  uint32_t nextRandom32();
  int nextRandomBelow(int bound);

  Clock clock_;
  Rng rng_;
};

}  // namespace hongguo
