// SPDX-License-Identifier: GPL-3.0-only
#include "hongguo/dash_reader.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <stdexcept>

#include "hongguo/aes.hpp"
#include "hongguo/media_crypto.hpp"

namespace hongguo {
namespace {

// 一次分片表解析允许的最大样本数。超过说明索引已损坏。
constexpr uint32_t kMaxSamples = 200000;
// 单个 moof 的体积上限。分片表的 moof 只有几 KB；超过说明偏移算错了，
// 继续按这个长度去拉只会白白吃掉一次大请求。
constexpr int64_t kMaxMoofBytes = 1 << 20;

int64_t asInt64(uint64_t value) { return static_cast<int64_t>(value); }

std::string asHex(const Bytes& data) {
  static const char* kDigits = "0123456789abcdef";
  std::string out;
  out.reserve(data.size() * 2);
  for (uint8_t byte : data) {
    out.push_back(kDigits[byte >> 4]);
    out.push_back(kDigits[byte & 0xf]);
  }
  return out;
}

// 16 位大端读取（bytes.hpp 只提供了 32/64 位版本）。
uint16_t readU16(const Bytes& data, size_t at) {
  if (at + 2 > data.size()) throw std::runtime_error("读取越界");
  return static_cast<uint16_t>((static_cast<uint16_t>(data[at]) << 8) | data[at + 1]);
}

struct Span {
  int64_t start = 0;
  int64_t end = 0;
};

bool parseBox(const Bytes& data, int64_t at, int64_t limit, Span& out, std::string* type) {
  if (at < 0 || at + 8 > limit) return false;
  uint64_t size = readU32BE(data, static_cast<size_t>(at));
  int64_t header = 8;
  if (size == 1) {
    if (at + 16 > limit) return false;
    size = readU64BE(data, static_cast<size_t>(at + 8));
    header = 16;
  } else if (size == 0) {
    size = static_cast<uint64_t>(limit - at);
  }
  if (size < static_cast<uint64_t>(header) || static_cast<int64_t>(size) > limit - at) {
    return false;
  }
  out.start = at;
  out.end = at + static_cast<int64_t>(size);
  if (type) *type = asciiAt(data, static_cast<size_t>(at + 4), 4);
  return true;
}

// 在 [start, limit) 内查找 box。与 media_crypto 一致：逐字节扫描，不假设对齐。
bool findSpan(const Bytes& data, const std::string& type, int64_t start, int64_t limit,
              Span& out) {
  if (type.empty()) return false;
  const char first = type[0];
  for (int64_t at = std::max<int64_t>(start, 0); at + 8 <= limit; ++at) {
    if (static_cast<char>(data[static_cast<size_t>(at + 4)]) != first) continue;
    std::string found;
    if (parseBox(data, at, limit, out, &found) && found == type) return true;
  }
  return false;
}

// 128 位大端计数器加一，与 media_crypto 的语义保持一致。
Bytes counterAdd(const Bytes& iv, uint64_t blocks) {
  unsigned __int128 value = 0;
  for (int i = 0; i < 16; ++i) value = (value << 8) | iv[static_cast<size_t>(i)];
  value += blocks;
  Bytes out(16);
  for (int i = 15; i >= 0; --i) {
    out[static_cast<size_t>(i)] = static_cast<uint8_t>(value & 0xff);
    value >>= 8;
  }
  return out;
}

}  // namespace

DashTrackReader::DashTrackReader(Http& http, DashTrack track, std::optional<Bytes> key)
    : http_(http), track_(std::move(track)), key_(std::move(key)) {}

Bytes DashTrackReader::fetch(int64_t start, int64_t end) const {
  if (end < start) return {};
  // 取 CDN 上的分片必须带站点来源；裸请求会被防盗链挡掉。
  const HeaderList headers = {
      {"Referer", "https://hongguoduanju.com/"},
      {"Origin", "https://hongguoduanju.com"},
      {"User-Agent",
       "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) "
       "Chrome/131.0.0.0 Safari/537.36"},
  };
  const HttpResponse response = http_.getRange(track_.url, headers, start, end);
  if (response.status != 200 && response.status != 206) {
    throw std::runtime_error("分片请求失败（HTTP " + std::to_string(response.status) + "）");
  }
  if (response.body.empty()) throw std::runtime_error("分片请求返回空");
  return response.body;
}

void DashTrackReader::prepare() {
  if (track_.initEnd <= 0) throw std::runtime_error("初始化段区间异常");

  // ⚠ init_range 是闭区间，且是**相对偏移**（从 0 开始）。
  const Bytes init = fetch(0, track_.initEnd);
  header_ = media_crypto::rewriteInitHeader(init);
  parseIndex();
}

void DashTrackReader::parseIndex() {
  segments_.clear();
  if (track_.indexEnd <= track_.indexStart) {
    // 没有索引段：退化成"从首个 moof 一路扫下去"，交给 runsFor 按需解析。
    if (track_.firstMoof > 0) {
      segments_.push_back(Segment{track_.firstMoof, track_.total - track_.firstMoof});
    }
    return;
  }

  const Bytes data = fetch(track_.indexStart, track_.indexEnd);
  Span sidx;
  if (!findSpan(data, "sidx", 0, static_cast<int64_t>(data.size()), sidx)) {
    throw std::runtime_error("分片索引缺失");
  }

  size_t at = static_cast<size_t>(sidx.start) + 8;
  const uint32_t version = readU32BE(data, at);
  at += 4;
  at += 4;  // reference_ID
  timescale_ = asInt64(readU32BE(data, at));
  at += 4;  // timescale
  int64_t firstOffset = 0;
  if (version == 0) {
    at += 4;  // earliest_presentation_time
    firstOffset = asInt64(readU32BE(data, at));
    at += 4;
  } else {
    at += 8;
    firstOffset = asInt64(readU64BE(data, at));
    at += 8;
  }
  if (at + 4 > static_cast<size_t>(sidx.end)) throw std::runtime_error("分片索引截断");
  at += 2;  // reserved
  // ⚠ reference_count 是 16 位，不是 32 位 —— 按 4 字节读会把后面两条引用当成数量。
  const uint32_t count = readU16(data, at);
  at += 2;
  if (count > kMaxSamples) throw std::runtime_error("分片数量异常");

  // ⚠ first_offset 是相对**索引段结束之后**那一个字节的偏移。
  //   play-info 给的 first_moof_range 就是它的绝对值，两者应当一致；
  //   不一致时以索引段自身的计算为准（那是容器里的第一手信息）。
  int64_t cursor = track_.indexEnd + 1 + firstOffset;

  for (uint32_t i = 0; i < count; ++i) {
    if (at + 12 > static_cast<size_t>(sidx.end)) throw std::runtime_error("分片索引截断");
    const uint32_t word = readU32BE(data, at);
    const bool hierarchical = (word & 0x80000000u) != 0;
    const int64_t size = static_cast<int64_t>(word & 0x7fffffffu);
    at += 12;
    if (hierarchical || size <= 0) continue;
    segments_.push_back(Segment{cursor, size});
    cursor += size;
  }
  if (segments_.empty()) throw std::runtime_error("分片索引为空");
}

std::vector<DashTrackReader::EncryptedRun> DashTrackReader::runsFor(
    const Segment& segment) const {
  // moof 在分片开头。先读 16 字节拿它的长度，再整段取回来 ——
  // 分片表（trun/senc）可能有几 KB，猜一个固定长度容易截断。
  // ⚠ 这里只读**长度字段**，不能要求整个 box 落在读回来的 16 字节里 ——
  //   moof 有几 KB，用"box 必须放得下"的判据会必然失败。
  const Bytes head = fetch(segment.start, segment.start + 15);
  uint64_t boxSize = readU32BE(head, 0);
  int64_t boxHeader = 8;
  if (boxSize == 1) {
    if (head.size() < 16) throw std::runtime_error("moof 长度字段截断");
    boxSize = readU64BE(head, 8);
    boxHeader = 16;
  } else if (boxSize == 0) {
    boxSize = static_cast<uint64_t>(segment.size);
  }
  if (boxSize < static_cast<uint64_t>(boxHeader) ||
      static_cast<int64_t>(boxSize) > kMaxMoofBytes) {
    throw std::runtime_error("moof 长度异常");
  }
  if (asciiAt(head, 4, 4) != "moof") {
    throw std::runtime_error("分片不是从 moof 开始（偏移算错了）");
  }
  const int64_t moofSize = static_cast<int64_t>(boxSize);
  const Bytes data = fetch(segment.start, segment.start + moofSize - 1);

  Span traf;
  if (!findSpan(data, "traf", 0, static_cast<int64_t>(data.size()), traf)) {
    throw std::runtime_error("分片缺少 traf");
  }

  // ---- tfhd：确定样本数据的基准偏移 ----
  Span tfhd;
  if (!findSpan(data, "tfhd", traf.start, traf.end, tfhd)) {
    throw std::runtime_error("分片缺少 tfhd");
  }
  size_t at = static_cast<size_t>(tfhd.start) + 8;
  const uint32_t tfhdFlags = readU32BE(data, at) & 0xffffff;
  at += 4 + 4;  // flags + track_ID
  int64_t base = segment.start;  // DASH 惯例：default-base-is-moof
  if ((tfhdFlags & 0x000001) != 0) {
    base = asInt64(readU64BE(data, at));
    at += 8;
  } else if ((tfhdFlags & 0x020000) == 0) {
    base = segment.start;
  }
  if ((tfhdFlags & 0x000002) != 0) at += 4;  // sample_description_index
  int64_t defaultSize = 0;
  if ((tfhdFlags & 0x000008) != 0) at += 4;  // default_sample_duration
  if ((tfhdFlags & 0x000010) != 0) {
    defaultSize = asInt64(readU32BE(data, at));
    at += 4;
  }

  // ---- senc：每个样本的 IV 与子样本布局 ----
  Span senc;
  const bool hasSenc = findSpan(data, "senc", traf.start, traf.end, senc);
  const uint32_t sencFlags = hasSenc ? (readU32BE(data, static_cast<size_t>(senc.start) + 8) & 0xffffff) : 0;
  const bool subsample = (sencFlags & 0x2) != 0;
  size_t sencAt = hasSenc ? static_cast<size_t>(senc.start) + 12 : 0;
  uint32_t sencCount = hasSenc ? readU32BE(data, sencAt) : 0;
  if (hasSenc) sencAt += 4;
  if (sencCount > kMaxSamples) throw std::runtime_error("加密样本数量异常");

  // ---- trun：每个样本的长度与数据偏移 ----
  Span trun;
  if (!findSpan(data, "trun", traf.start, traf.end, trun)) {
    throw std::runtime_error("分片缺少 trun");
  }
  size_t trunAt = static_cast<size_t>(trun.start) + 8;
  const uint32_t trunFlags = readU32BE(data, trunAt) & 0xffffff;
  trunAt += 4;
  const uint32_t sampleCount = readU32BE(data, trunAt);
  trunAt += 4;
  if (sampleCount > kMaxSamples) throw std::runtime_error("样本数量异常");
  // ⚠ 只有 flag 置位时字段才存在，不能按固定长度往下跳。
  if ((trunFlags & 0x000001) != 0) {
    const int32_t offset = static_cast<int32_t>(readU32BE(data, trunAt));
    base += offset;
    trunAt += 4;
  }
  if ((trunFlags & 0x000004) != 0) trunAt += 4;  // first_sample_flags

  std::vector<int64_t> sizes;
  sizes.reserve(sampleCount);
  size_t walk = trunAt;
  for (uint32_t i = 0; i < sampleCount; ++i) {
    if ((trunFlags & 0x000100) != 0) walk += 4;  // duration
    int64_t size = defaultSize;
    if ((trunFlags & 0x000200) != 0) {
      if (walk + 4 > static_cast<size_t>(trun.end)) throw std::runtime_error("trun 截断");
      size = asInt64(readU32BE(data, walk));
      walk += 4;
    }
    if ((trunFlags & 0x000400) != 0) walk += 4;  // flags
    if ((trunFlags & 0x000800) != 0) walk += 4;  // composition offset
    if (size <= 0) throw std::runtime_error("样本长度异常");
    sizes.push_back(size);
  }

  // ---- 按样本顺序展开加密区间 ----
  if (std::getenv("HG_DASH_DEBUG")) {
    std::fprintf(stderr, "[dash] seg=%lld size=%lld moofSize=%lld base=%lld trunFlags=0x%x "
                         "senc=%d subsample=%d count=%u defaultSize=%lld\n",
                 (long long)segment.start, (long long)segment.size, (long long)moofSize,
                 (long long)base, trunFlags, hasSenc ? 1 : 0, subsample ? 1 : 0, sampleCount,
                 (long long)defaultSize);
  }

  std::vector<EncryptedRun> runs;
  int64_t cursor = base;
  for (uint32_t i = 0; i < sampleCount; ++i) {
    const int64_t sampleStart = cursor;
    const int64_t sampleEnd = cursor + sizes[i];
    cursor = sampleEnd;

    if (!hasSenc || i >= sencCount) {
      // 没有 senc：整样本加密，IV 取 0（上游实际不会这么发）。
      runs.push_back(EncryptedRun{sampleStart, sampleEnd, Bytes(16, 0x00)});
      continue;
    }
    if (sencAt + 16 > static_cast<size_t>(senc.end)) throw std::runtime_error("senc 截断");
    Bytes iv(data.begin() + static_cast<int64_t>(sencAt),
             data.begin() + static_cast<int64_t>(sencAt) + 16);
    sencAt += 16;

    if (!subsample) {
      runs.push_back(EncryptedRun{sampleStart, sampleEnd, iv});
      continue;
    }
    if (sencAt + 2 > static_cast<size_t>(senc.end)) throw std::runtime_error("senc 截断");
    const uint16_t subs = static_cast<uint16_t>(readU16(data, sencAt));
    sencAt += 2;
    int64_t position = sampleStart;
    for (uint16_t k = 0; k < subs; ++k) {
      if (sencAt + 6 > static_cast<size_t>(senc.end)) throw std::runtime_error("子样本截断");
      const int64_t clear = static_cast<int64_t>(readU16(data, sencAt));
      const int64_t encrypted = asInt64(readU32BE(data, sencAt + 2));
      sencAt += 6;
      position += clear;
      if (encrypted > 0) runs.push_back(EncryptedRun{position, position + encrypted, iv});
      position += encrypted;
    }
  }
  if (std::getenv("HG_DASH_DEBUG")) {
    for (size_t i = 0; i < runs.size() && i < 4; ++i) {
      std::fprintf(stderr, "[dash]   run%zu %lld..%lld iv=%s\n", i,
                   (long long)runs[i].start, (long long)runs[i].end,
                   asHex(runs[i].iv).c_str());
    }
  }
  return runs;
}

Bytes DashTrackReader::read(int64_t start, int length) {
  if (start < 0 || length <= 0) return {};
  const int64_t end = std::min<int64_t>(start + length, track_.total);
  if (end <= start) return {};

  // ⚠ 先把请求区间**整段**取回来，再就地把加密段换成明文。
  //   只写"解出来的那几段"是不够的 —— 分片里有大量本来就不加密的字节
  //   （moof、mdat 头、子样本的明文段、尾部填充），漏掉它们文件就是一堆空洞。
  Bytes out = fetch(start, end - 1);

  // 头部是改写过的版本（编码格式已还原、sinf 已抹掉），必须覆盖掉原样的密文头。
  if (start < static_cast<int64_t>(header_.size())) {
    const int64_t stop = std::min<int64_t>(end, static_cast<int64_t>(header_.size()));
    for (int64_t i = start; i < stop; ++i) {
      const size_t index = static_cast<size_t>(i - start);
      if (index < out.size()) out[index] = header_[static_cast<size_t>(i)];
    }
  }

  if (!hasKey()) return out;

  for (const Segment& segment : segments_) {
    const int64_t segmentEnd = segment.start + segment.size;
    if (segmentEnd <= start || segment.start >= end) continue;

    const std::vector<EncryptedRun> runs = runsFor(segment);
    for (const EncryptedRun& run : runs) {
      const int64_t begin = std::max<int64_t>(start, run.start);
      const int64_t stop = std::min<int64_t>(end, run.end);
      if (begin >= stop) continue;

      const int64_t from = begin - start;
      if (from >= static_cast<int64_t>(out.size())) continue;
      const int64_t to = std::min<int64_t>(stop - start, static_cast<int64_t>(out.size()));

      const int64_t relative = begin - run.start;
      const size_t skip = static_cast<size_t>(relative % 16);
      const Bytes counter = counterAdd(run.iv, static_cast<uint64_t>(relative / 16));

      // aes128Ctr 要求 16 字节对齐：前面补 skip 个字节，丢掉结果里对应的前缀。
      Bytes chunk(skip, 0x00);
      chunk.insert(chunk.end(), out.begin() + static_cast<long>(from),
                   out.begin() + static_cast<long>(to));
      const Bytes decrypted = aes128Ctr(*key_, counter, chunk);
      for (size_t k = skip; k < decrypted.size(); ++k) {
        out[static_cast<size_t>(from + static_cast<int64_t>(k - skip))] = decrypted[k];
      }
    }
  }
  return out;
}

std::vector<DashTrackReader::SegmentRange> DashTrackReader::segmentRanges() const {
  std::vector<SegmentRange> out;
  out.reserve(segments_.size());
  for (const Segment& segment : segments_) out.push_back(SegmentRange{segment.start, segment.size});
  return out;
}

namespace {

// 组装一个 box：4 字节长度 + 4 字节类型 + 负载。
Bytes makeBox(const std::string& type, const Bytes& payload) {
  Bytes out;
  const uint64_t size = payload.size() + 8;
  out.push_back(static_cast<uint8_t>((size >> 24) & 0xff));
  out.push_back(static_cast<uint8_t>((size >> 16) & 0xff));
  out.push_back(static_cast<uint8_t>((size >> 8) & 0xff));
  out.push_back(static_cast<uint8_t>(size & 0xff));
  out.insert(out.end(), type.begin(), type.end());
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

bool locateBox(const Bytes& data, const std::string& type, int64_t start, int64_t end, Span& out) {
  return findSpan(data, type, start, end, out);
}

// 在缓冲里找 4 字节的 box 类型标记，返回首次出现的下标。
int64_t findIn(const Bytes& data, const std::string& tag) {
  if (tag.size() != 4 || data.size() < 4) return -1;
  for (size_t i = 0; i + 4 <= data.size(); ++i) {
    if (data[i] == tag[0] && data[i + 1] == tag[1] && data[i + 2] == tag[2] &&
        data[i + 3] == tag[3]) {
      return static_cast<int64_t>(i);
    }
  }
  return -1;
}

void patchU32(Bytes& data, int64_t at, uint32_t value) {
  if (at < 0 || at + 4 > static_cast<int64_t>(data.size())) return;
  for (int i = 0; i < 4; ++i) {
    data[static_cast<size_t>(at + i)] = static_cast<uint8_t>((value >> (8 * (3 - i))) & 0xff);
  }
}

uint32_t readU32At(const Bytes& data, int64_t at) {
  if (at < 0 || at + 4 > static_cast<int64_t>(data.size())) return 0;
  return readU32BE(data, static_cast<size_t>(at));
}

// 从 trak 里读出 track_ID。tkhd 的版本决定 creation/modification 是 4 还是 8 字节。
uint32_t trackIdOf(const Bytes& data, const Span& trak) {
  Span tkhd;
  if (!locateBox(data, "tkhd", trak.start, trak.end, tkhd)) return 0;
  const uint32_t version = data[static_cast<size_t>(tkhd.start) + 8];
  const int64_t fieldAt = tkhd.start + 8 + 4 + (version == 1 ? 16 : 8);
  return readU32At(data, fieldAt);
}

// mvhd 的 next_track_ID 在 box 末尾往前 4 字节（v0 与 v1 长度不同，这里按版本算）。
void patchNextTrackId(Bytes& moov, const Span& mvhd, uint32_t value) {
  const uint32_t version = moov[static_cast<size_t>(mvhd.start) + 8];
  const int64_t at = mvhd.start + 8 + 4 + (version == 1 ? 24 : 12) + 4 + 4 + 2 + 2 + 8 + 36 + 24;
  patchU32(moov, at, value);
}

}  // namespace

DashMuxer::DashMuxer(DashTrackReader& video, DashTrackReader* audio)
    : video_(video), audio_(audio) {}

void DashMuxer::prepare() {
  video_.prepare();
  if (audio_ != nullptr) audio_->prepare();

  const Bytes& vInit = video_.header();
  const Bytes& aInit = audio_ != nullptr ? audio_->header() : video_.header();

  // ---- 视频侧：ftyp / mvhd / trak / mvex ----
  Span ftyp;
  if (!locateBox(vInit, "ftyp", 0, static_cast<int64_t>(vInit.size()), ftyp)) {
    throw std::runtime_error("初始化段缺少 ftyp");
  }
  Span vMoov;
  if (!locateBox(vInit, "moov", ftyp.end, static_cast<int64_t>(vInit.size()), vMoov)) {
    throw std::runtime_error("初始化段缺少 moov");
  }
  Span vMvhd;
  if (!locateBox(vInit, "mvhd", vMoov.start, vMoov.end, vMvhd)) {
    throw std::runtime_error("moov 缺少 mvhd");
  }
  Span vTrak;
  if (!locateBox(vInit, "trak", vMoov.start, vMoov.end, vTrak)) {
    throw std::runtime_error("moov 缺少 trak");
  }
  Span vMvex;
  const bool hasMvex = locateBox(vInit, "mvex", vMoov.start, vMoov.end, vMvex);
  videoTrackId_ = trackIdOf(vInit, vTrak);
  if (videoTrackId_ == 0) videoTrackId_ = 1;

  Bytes moov = makeBox("mvhd", Bytes(vInit.begin() + vMvhd.start + 8, vInit.begin() + vMvhd.end));
  moov = concat(moov, Bytes(vInit.begin() + vTrak.start, vInit.begin() + vTrak.end));

  Bytes mvexPayload;
  if (hasMvex) {
    // 视频原来的 mvex（里面有 mehd 与 trex）整段留下。
    mvexPayload = Bytes(vInit.begin() + vMvex.start + 8, vInit.begin() + vMvex.end);
  }

  audioTrackId_ = 0;
  if (audio_ != nullptr) {
    Span aMoov;
    if (!locateBox(aInit, "moov", 0, static_cast<int64_t>(aInit.size()), aMoov)) {
      throw std::runtime_error("音频初始化段缺少 moov");
    }
    Span aTrak;
    if (!locateBox(aInit, "trak", aMoov.start, aMoov.end, aTrak)) {
      throw std::runtime_error("音频 moov 缺少 trak");
    }
    // ⚠ 两条轨各自独立生成，track_ID 都是 1 —— 合到一个 moov 里就会撞车。
    //   音频改成"视频 +1"，tkhd 里的那份也别落下。
    audioTrackId_ = static_cast<int>(videoTrackId_) + 1;

    Bytes aTrakBytes(aInit.begin() + aTrak.start, aInit.begin() + aTrak.end);
    Span aTkhd;
    if (locateBox(aTrakBytes, "tkhd", 0, static_cast<int64_t>(aTrakBytes.size()), aTkhd)) {
      const uint32_t version = aTrakBytes[static_cast<size_t>(aTkhd.start) + 8];
      patchU32(aTrakBytes, aTkhd.start + 8 + 4 + (version == 1 ? 16 : 8),
               static_cast<uint32_t>(audioTrackId_));
    }
    moov = concat(moov, aTrakBytes);

    Span aMvex;
    if (locateBox(aInit, "mvex", aMoov.start, aMoov.end, aMvex)) {
      Span aTrex;
      if (locateBox(aInit, "trex", aMvex.start, aMvex.end, aTrex)) {
        Bytes trex(aInit.begin() + aTrex.start, aInit.begin() + aTrex.end);
        patchU32(trex, 12, static_cast<uint32_t>(audioTrackId_));
        mvexPayload = concat(mvexPayload, trex);
      }
    }
  }

  if (!mvexPayload.empty()) moov = concat(moov, makeBox("mvex", mvexPayload));
  patchNextTrackId(moov, Span{0, static_cast<int64_t>(moov.size())},
                   static_cast<uint32_t>(videoTrackId_) + 2);

  // moov 的 mvhd 现在在偏移 0（刚组装出来的），上面按 box 起点 0 算的 next_track_ID。
  // ⚠ 这些 box 必须**再套一层 moov**。少了它 ffprobe 只会看到孤零零的 mvhd/trak，
  //   报 "moov atom not found"，而前面几个 box 拆开看全是对的 —— 很难一眼看出。
  header_ = concat(Bytes(vInit.begin() + ftyp.start, vInit.begin() + ftyp.end),
                   makeBox("moov", moov));

  // ---- 分片表：头部 + 视频/音频按序号交替 ----
  pieces_.clear();
  total_ = 0;
  Piece head;
  head.outStart = 0;
  head.length = static_cast<int64_t>(header_.size());
  head.source = 0;
  pieces_.push_back(head);
  total_ = head.length;

  const std::vector<DashTrackReader::SegmentRange> vSegments = video_.segmentRanges();
  std::vector<DashTrackReader::SegmentRange> aSegments;
  if (audio_ != nullptr) aSegments = audio_->segmentRanges();

  const size_t count = std::max(vSegments.size(), aSegments.size());
  for (size_t i = 0; i < count; ++i) {
    if (i < vSegments.size()) {
      Piece piece;
      piece.outStart = total_;
      piece.length = vSegments[i].size;
      piece.source = 1;
      piece.sourceOffset = vSegments[i].start;
      pieces_.push_back(piece);
      total_ += piece.length;
    }
    if (i < aSegments.size()) {
      Piece piece;
      piece.outStart = total_;
      piece.length = aSegments[i].size;
      piece.source = 2;
      piece.sourceOffset = aSegments[i].start;
      // 音频分片的 tfhd 里写着 track_ID，要改成改名后的那个。
      // 偏移在这里就扫出来 —— read() 拿到的可能只是分片中间的一小段。
      const Bytes headBytes = audio_->read(aSegments[i].start, 512);
      const int64_t found = findIn(headBytes, "tfhd");
      if (found >= 0) piece.patchAt = aSegments[i].start + found + 12;
      pieces_.push_back(piece);
      total_ += piece.length;
    }
  }
}

Bytes DashMuxer::read(int64_t start, int length) {
  if (start < 0 || length <= 0) return {};
  const int64_t end = std::min<int64_t>(start + length, total_);
  if (end <= start) return {};

  Bytes out(static_cast<size_t>(end - start), 0x00);
  for (const Piece& piece : pieces_) {
    const int64_t pieceEnd = piece.outStart + piece.length;
    if (pieceEnd <= start || piece.outStart >= end) continue;
    const int64_t begin = std::max<int64_t>(start, piece.outStart);
    const int64_t stop = std::min<int64_t>(end, pieceEnd);
    const int64_t from = begin - piece.outStart;
    const size_t destination = static_cast<size_t>(begin - start);

    if (piece.source == 0) {
      for (int64_t i = from; i < from + (stop - begin); ++i) {
        out[destination + static_cast<size_t>(i - from)] = header_[static_cast<size_t>(i)];
      }
      continue;
    }

    DashTrackReader& source = piece.source == 2 ? *audio_ : video_;
    const int64_t sourceStart = piece.sourceOffset + from;
    Bytes chunk = source.read(sourceStart, static_cast<int>(stop - begin));
    if (piece.patchAt >= 0 && audioTrackId_ > 0) {
      // track_ID 正好落在这次取回来的字节里才需要改；落在别处说明这次没取到它。
      for (int k = 0; k < 4; ++k) {
        const int64_t at = piece.patchAt + k;
        if (at >= sourceStart && at < sourceStart + static_cast<int64_t>(chunk.size())) {
          chunk[static_cast<size_t>(at - sourceStart)] =
              static_cast<uint8_t>((static_cast<uint32_t>(audioTrackId_) >> (8 * (3 - k))) & 0xff);
        }
      }
    }
    for (size_t i = 0; i < chunk.size(); ++i) {
      out[destination + i] = chunk[i];
    }
  }
  return out;
}

}  // namespace hongguo
