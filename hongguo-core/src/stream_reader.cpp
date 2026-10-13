// SPDX-License-Identifier: GPL-3.0-only
#include "hongguo/stream_reader.hpp"

#include <algorithm>
#include <cstdio>
#include <regex>
#include <stdexcept>

#include "hongguo/vendor.hpp"

namespace hongguo {
namespace {

constexpr int64_t kHeadProbeBytes = 256 * 1024;
constexpr int64_t kWarmPrefixBytes = 1024 * 1024;
constexpr int kMaxReadLength = 1024 * 1024;

}  // namespace

StreamReader::StreamReader(Http& http, StreamInfo info)
    : http_(http), info_(std::move(info)) {}

StreamReader::RangeResult StreamReader::range(int64_t start, int64_t end) {
  if (start < 0 || end < start || end - start >= 8 * 1024 * 1024) {
    throw std::runtime_error("非法的分段请求范围");
  }

  const HeaderList headers = {
      {"User-Agent", std::string(vendor::kVideoUserAgent)},
      {"Referer", "https://novel.snssdk.com/"},
      {"Accept-Encoding", "identity"}};

  const HttpResponse response = http_.getRange(info_.url, headers, start, end);
  // 源站必须支持分段，否则无法边下边解
  if (response.status != 206) {
    throw std::runtime_error("视频服务器不支持分段读取（HTTP " +
                             std::to_string(response.status) + "）");
  }

  static const std::regex kContentRange(R"(bytes (\d+)-(\d+)/(\d+))");
  const std::string contentRange = response.header("Content-Range");
  std::smatch match;
  if (!std::regex_match(contentRange, match, kContentRange)) {
    throw std::runtime_error("视频分段响应异常");
  }
  const int64_t begin = std::stoll(match[1].str());
  const int64_t finish = std::stoll(match[2].str());
  const int64_t full = std::stoll(match[3].str());
  const int64_t last = std::min(end, full - 1);

  if (begin != start || finish != last || full <= 0) {
    throw std::runtime_error("视频分段范围不匹配");
  }
  if (total_ != 0 && full != total_) {
    throw std::runtime_error("视频资源已变化，请重试");
  }

  const int64_t want = last - start + 1;
  if (static_cast<int64_t>(response.body.size()) != want) {
    throw std::runtime_error("视频分段长度不符");
  }

  RangeResult result;
  result.bytes = response.body;
  result.total = full;
  return result;
}

std::string StreamReader::debugSampleDump(size_t count) const {
  if (!prepared_) return {};
  std::string out;
  const size_t n = std::min(count, cenc_.samples.size());
  out += "    SAMPLES=" + std::to_string(cenc_.samples.size()) + "\n";
  for (size_t i = 0; i < n; ++i) {
    const auto& s = cenc_.samples[i];
    out += "    #" + std::to_string(i) + " start=" + std::to_string(s.start) +
           " end=" + std::to_string(s.end) + " eoff=" +
           std::to_string(s.encryptedOffset) + " iv=" + toHex(s.iv) + "\n";
  }
  return out;
}

void StreamReader::prepare() {
  if (prepared_) return;

  const RangeResult first = range(0, kHeadProbeBytes - 1);
  total_ = first.total;
  prefix_ = first.bytes;

  const int64_t headerEnd = media_crypto::headerEnd(first.bytes, total_);
  Bytes header = first.bytes;
  if (headerEnd > static_cast<int64_t>(first.bytes.size())) {
    const RangeResult rest =
        range(static_cast<int64_t>(first.bytes.size()), headerEnd - 1);
    header.insert(header.end(), rest.bytes.begin(), rest.bytes.end());
  } else {
    header.resize(static_cast<size_t>(headerEnd));
  }

  cenc_ = media_crypto::prepare(header, info_.key);
  for (const media_crypto::Sample& sample : cenc_.samples) {
    if (sample.end > total_) throw std::runtime_error("视频样本超出资源大小");
  }
  prepared_ = true;
}

void StreamReader::warm() {
  prepare();
  const int64_t size = std::min<int64_t>(total_, kWarmPrefixBytes);
  if (static_cast<int64_t>(prefix_.size()) < size) {
    const RangeResult rest = range(static_cast<int64_t>(prefix_.size()), size - 1);
    prefix_.insert(prefix_.end(), rest.bytes.begin(), rest.bytes.end());
  }
}

Bytes StreamReader::read(int64_t start, int length) {
  if (length < 1 || length > kMaxReadLength || start < 0) {
    throw std::runtime_error("非法的读取参数");
  }
  prepare();
  if (start >= total_) return {};

  const int64_t end = std::min<int64_t>(total_, start + length);
  const Bytes& cached = prefix_;
  const int64_t available =
      std::max<int64_t>(0, std::min<int64_t>(end, static_cast<int64_t>(cached.size())) - start);

  Bytes raw;
  if (available > 0) {
    raw.assign(cached.begin() + static_cast<long>(start),
               cached.begin() + static_cast<long>(start + available));
  }
  if (start + available < end) {
    const RangeResult rest = range(start + available, end - 1);
    raw.insert(raw.end(), rest.bytes.begin(), rest.bytes.end());
  }

  // 解密始终按**绝对**偏移进行，因此前缀缓存可以直接复用
  return media_crypto::decrypt(raw, start, info_.key, cenc_);
}

}  // namespace hongguo
