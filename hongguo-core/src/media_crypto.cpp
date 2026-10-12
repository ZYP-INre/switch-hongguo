// SPDX-License-Identifier: GPL-3.0-only
//
// 移植自 hongguoTV 的 MediaCrypto.kt。ISO-BMFF 的解析刻意写成"读到哪查到哪"，
// 每一处读取都带边界判断 —— 上游数据是外部输入，宁可报错也不能越界。
#include "hongguo/media_crypto.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "hongguo/aes.hpp"
#include "hongguo/digest.hpp"

namespace hongguo::media_crypto {
namespace {

// 一次索引读取的上限。超过这个数量说明索引已损坏，继续分配只会吃掉内存。
constexpr uint32_t kMaxIndexCount = 200000;
// moov 允许的最大体积（headerEnd 的约束）。
constexpr int64_t kMaxHeaderBytes = 8 * 1024 * 1024;

struct Box {
  int64_t start = 0;
  int64_t end = 0;
  int64_t body = 0;
  std::string type;
};

int64_t asInt64(uint64_t value) { return static_cast<int64_t>(value); }

// 解析 [at, limit) 处的一个 box。不合法返回 false。
bool readBox(const Bytes& data, int64_t at, int64_t limit, Box& out) {
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
  if (size < static_cast<uint64_t>(header) ||
      asInt64(size) > limit - at) {
    return false;
  }
  out.start = at;
  out.end = at + asInt64(size);
  out.body = at + header;
  out.type = asciiAt(data, static_cast<size_t>(at + 4), 4);
  return true;
}

// 在 [start, limit) 内查找指定类型的 box。对齐无关 —— 上游确实存在
// 非 8 字节对齐的容器，因此逐字节扫描。
bool findBox(const Bytes& data, const std::string& type, int64_t start, int64_t limit,
             Box& out) {
  if (type.empty()) return false;
  const char first = type[0];
  for (int64_t at = start; at + 8 <= limit; ++at) {
    if (static_cast<char>(data[static_cast<size_t>(at + 4)]) != first) continue;
    Box candidate;
    if (readBox(data, at, limit, candidate) && candidate.type == type) {
      out = candidate;
      return true;
    }
  }
  return false;
}

// 读取"数量"字段，带完整性校验。
uint32_t readCount(const Bytes& data, int64_t at, int64_t limit) {
  if (at + 4 > limit) throw std::runtime_error("索引越界");
  const uint32_t value = readU32BE(data, static_cast<size_t>(at));
  if (value > kMaxIndexCount) throw std::runtime_error("索引数量过大");
  return value;
}

// 128 位大端计数器加一。
Bytes counterAdd(const Bytes& iv, uint64_t blocks) {
  unsigned __int128 value = 0;
  for (int i = 0; i < 16; ++i) value = (value << 8) | iv[static_cast<size_t>(i)];
  value += blocks;  // 模 2^128 自然回绕
  Bytes out(16);
  for (int i = 15; i >= 0; --i) {
    out[static_cast<size_t>(i)] = static_cast<uint8_t>(value & 0xff);
    value >>= 8;
  }
  return out;
}

// 把一个 4 字节 fourcc 写回 header。
void writeFourcc(Bytes& header, int64_t at, const Bytes& fourcc) {
  if (at < 0 || at + 4 > static_cast<int64_t>(header.size()) || fourcc.size() != 4) {
    return;
  }
  for (int i = 0; i < 4; ++i) header[static_cast<size_t>(at + i)] = fourcc[static_cast<size_t>(i)];
}

}  // namespace

// ------------------------------------------------------------ 内容密钥派生

Bytes deriveKey(const std::string& value) {
  const Bytes raw = fromBase64(value);
  if (raw.size() < 3) throw std::runtime_error("内容密钥格式异常");

  const int64_t span = static_cast<int64_t>(raw.size()) -
                       (raw[0] ^ raw[1] ^ raw[2]) + 47;
  const int64_t length = std::min<int64_t>(span, static_cast<int64_t>(raw.size()) - 1);
  if (length < 33) throw std::runtime_error("内容密钥长度异常");

  Bytes work(raw.begin() + 1, raw.begin() + 1 + length);
  uint32_t a = 85;
  uint32_t b = 246;
  for (size_t i = 0; i < work.size(); ++i) {
    const uint32_t old = work[i];
    const uint32_t previous = (i & 1) != 0 ? a : b;
    if ((i & 1) != 0) {
      a = old;
    } else {
      b = old;
    }
    const int popcount = __builtin_popcount(static_cast<unsigned>(i));
    work[i] = static_cast<uint8_t>((-21 - popcount + static_cast<int>(previous ^ old)) & 0xff);
  }

  const std::string hexText(work.begin() + 1, work.begin() + 33);
  return fromHex(hexText);
}

// ------------------------------------------------------------ 播放地址解密

std::string decryptUrl(const std::string& value, const Bytes& seed) {
  const Bytes raw = fromBase64(value);
  if (raw.size() < 20 || raw[0] != 0xa8 || raw[2] != 0x01 || raw[3] != 0x00) {
    throw std::runtime_error("播放地址格式异常");
  }

  const Bytes constants = fromHex(
      "4dd4c2e6b83162090e52b3c7a6733ba41cb2462b829ab58a196b39db57177524"
      "f49baf7f08e8d68d26a72e37c1a95a2f1f05a51892aef2949732b62a38aadd58");

  const Bytes inner = sha512(seed);
  const Bytes material = concat(inner, constants);
  const Bytes h = sha512(material);

  const Bytes key(h.begin(), h.begin() + 16);
  const Bytes iv(h.begin() + 16, h.begin() + 32);

  const size_t cipherLength = (raw.size() - 4) / 16 * 16;
  const Bytes cipher(raw.begin() + 4, raw.begin() + 4 + static_cast<int64_t>(cipherLength));
  Bytes plain = aes128CbcDecrypt(key, iv, cipher);

  size_t end = plain.size();
  if (!plain.empty()) {
    const uint8_t padding = plain.back();
    if (padding >= 1 && padding <= 16 && padding <= end) end -= padding;
  }
  while (end > 0 && plain[end - 1] == 0x00) --end;

  return std::string(plain.begin(), plain.begin() + static_cast<int64_t>(end));
}

// ------------------------------------------------------------ 容器头部定位

int64_t headerEnd(const Bytes& first, int64_t total) {
  if (first.size() < 16 || asciiAt(first, 4, 4) != "ftyp") {
    throw std::runtime_error("不支持的视频容器");
  }
  const uint64_t ftypSize = readU32BE(first, 0);
  if (ftypSize < 8 || asInt64(ftypSize) + 8 > static_cast<int64_t>(first.size()) ||
      asciiAt(first, static_cast<size_t>(ftypSize) + 4, 4) != "moov") {
    throw std::runtime_error("视频索引不在文件开头");
  }
  const uint64_t moovSize = readU32BE(first, static_cast<size_t>(ftypSize));
  const int64_t end = asInt64(ftypSize) + asInt64(moovSize);
  if (moovSize < 8 || end > total || end > kMaxHeaderBytes) {
    throw std::runtime_error("视频索引大小异常");
  }
  return end;
}

// -------------------------------------------------------------- 样本索引

Prepared prepare(const Bytes& data, const std::optional<Bytes>& key) {
  if (!key || key->empty()) {
    return Prepared{data, {}};
  }

  Box moov;
  if (!readBox(data, asInt64(readU32BE(data, 0)), static_cast<int64_t>(data.size()), moov) ||
      moov.type != "moov") {
    throw std::runtime_error("视频索引不完整");
  }

  std::vector<Sample> samples;
  Bytes header = data;

  int64_t cursor = moov.body;
  while (cursor < moov.end) {
    Box track;
    if (!readBox(data, cursor, moov.end, track)) {
      throw std::runtime_error("视频轨道格式异常");
    }
    cursor = track.end;
    if (track.type != "trak") continue;

    Box stbl;
    if (!findBox(data, "stbl", track.body, track.end, stbl)) continue;
    Box saiz;
    if (!findBox(data, "saiz", stbl.body, stbl.end, saiz)) continue;
    Box saio;
    if (!findBox(data, "saio", stbl.body, stbl.end, saio)) {
      throw std::runtime_error("视频加密索引缺失");
    }
    Box stsz;
    if (!findBox(data, "stsz", stbl.body, stbl.end, stsz)) {
      throw std::runtime_error("样本索引缺失");
    }
    Box stsc;
    if (!findBox(data, "stsc", stbl.body, stbl.end, stsc)) {
      throw std::runtime_error("块索引缺失");
    }
    Box chunks;
    if (!findBox(data, "stco", stbl.body, stbl.end, chunks) &&
        !findBox(data, "co64", stbl.body, stbl.end, chunks)) {
      throw std::runtime_error("块偏移缺失");
    }

    const uint32_t sampleCount = readCount(data, stsz.body + 8, stsz.end);
    const uint64_t defaultSize = readU32BE(data, static_cast<size_t>(stsz.body + 4));
    if (defaultSize == 0 &&
        stsz.body + 12 + static_cast<int64_t>(sampleCount) * 4 > stsz.end) {
      throw std::runtime_error("样本索引截断");
    }
    std::vector<int64_t> sizes(sampleCount);
    for (uint32_t i = 0; i < sampleCount; ++i) {
      sizes[i] = defaultSize > 0
                     ? asInt64(defaultSize)
                     : asInt64(readU32BE(data, static_cast<size_t>(stsz.body + 12) + i * 4));
    }

    const uint32_t chunkCount = readCount(data, chunks.body + 4, chunks.end);
    const int width = chunks.type == "co64" ? 8 : 4;
    if (chunks.body + 8 + static_cast<int64_t>(chunkCount) * width > chunks.end) {
      throw std::runtime_error("块偏移截断");
    }
    std::vector<int64_t> offsets(chunkCount);
    for (uint32_t i = 0; i < chunkCount; ++i) {
      const size_t at = static_cast<size_t>(chunks.body + 8) + static_cast<size_t>(i) * static_cast<size_t>(width);
      offsets[i] = width == 8 ? asInt64(readU64BE(data, at)) : asInt64(readU32BE(data, at));
    }

    const uint32_t entryCount = readCount(data, stsc.body + 4, stsc.end);
    if (stsc.body + 8 + static_cast<int64_t>(entryCount) * 12 > stsc.end || entryCount == 0) {
      throw std::runtime_error("块索引截断");
    }
    std::vector<std::pair<uint32_t, uint32_t>> entries(entryCount);
    for (uint32_t i = 0; i < entryCount; ++i) {
      entries[i] = {readU32BE(data, static_cast<size_t>(stsc.body + 8) + i * 12),
                    readU32BE(data, static_cast<size_t>(stsc.body + 12) + i * 12)};
    }

    // saiz / saio 的 flags 低位可能插入 8 字节的 type+parameter 对
    const int64_t sizeBase =
        saiz.body + 4 + ((readU32BE(data, static_cast<size_t>(saiz.body)) & 1) != 0 ? 8 : 0);
    if (sizeBase + 5 > saiz.end) throw std::runtime_error("辅助索引截断");
    const uint8_t auxDefault = data[static_cast<size_t>(sizeBase)];
    const uint32_t auxCount = readCount(data, sizeBase + 1, saiz.end);
    if (auxCount != sampleCount ||
        (auxDefault == 0 && sizeBase + 5 + static_cast<int64_t>(sampleCount) > saiz.end)) {
      throw std::runtime_error("辅助索引数量不一致");
    }
    std::vector<int64_t> auxSizes(sampleCount);
    for (uint32_t i = 0; i < sampleCount; ++i) {
      auxSizes[i] = auxDefault > 0 ? auxDefault
                                   : data[static_cast<size_t>(sizeBase + 5) + i];
    }

    const int64_t offsetBase =
        saio.body + 4 + ((readU32BE(data, static_cast<size_t>(saio.body)) & 1) != 0 ? 8 : 0);
    const int offsetWidth = data[static_cast<size_t>(saio.body)] == 1 ? 8 : 4;
    if (readCount(data, offsetBase, saio.end) != 1 ||
        offsetBase + 4 + offsetWidth > saio.end) {
      throw std::runtime_error("不支持的辅助索引布局");
    }
    int64_t aux = offsetWidth == 8
                      ? asInt64(readU64BE(data, static_cast<size_t>(offsetBase + 4)))
                      : asInt64(readU32BE(data, static_cast<size_t>(offsetBase + 4)));
    int64_t auxTotal = 0;
    for (const int64_t size : auxSizes) auxTotal += size;
    const int64_t auxEnd = aux + auxTotal;
    if (aux < 0 || auxEnd > static_cast<int64_t>(data.size())) {
      throw std::runtime_error("辅助数据超出索引范围");
    }

    uint32_t sampleIndex = 0;
    size_t entry = 0;
    for (size_t chunk = 0; chunk < offsets.size(); ++chunk) {
      while (entry + 1 < entries.size() &&
             entries[entry + 1].first <= static_cast<uint32_t>(chunk) + 1) {
        ++entry;
      }
      const uint32_t perChunk = entries[entry].second;
      if (perChunk > sampleCount - sampleIndex ||
          entries[entry].first > static_cast<uint32_t>(chunk) + 1) {
        throw std::runtime_error("样本与块数量不一致");
      }
      int64_t position = offsets[chunk];
      for (uint32_t k = 0; k < perChunk; ++k) {
        const int64_t length = sizes[sampleIndex];
        const int64_t auxSize = auxSizes[sampleIndex];
        if (position < 0 || length <= 0 ||
            position > std::numeric_limits<int64_t>::max() - length || auxSize < 8) {
          throw std::runtime_error("样本索引异常");
        }
        Bytes iv(data.begin() + aux, data.begin() + aux + 8);
        iv.resize(16, 0x00);

        if (auxSize == 8) {
          samples.push_back(Sample{position, position + length, iv, 0});
        } else {
          if (auxSize < 10) throw std::runtime_error("不支持的加密样本格式");
          const uint32_t subs = (static_cast<uint32_t>(data[static_cast<size_t>(aux + 8)]) << 8) |
                                data[static_cast<size_t>(aux + 9)];
          if (auxSize != 10 + static_cast<int64_t>(subs) * 6) {
            throw std::runtime_error("加密子样本索引异常");
          }
          int64_t offset = position;
          int64_t encrypted = 0;
          for (uint32_t sub = 0; sub < subs; ++sub) {
            const size_t at = static_cast<size_t>(aux + 10) + static_cast<size_t>(sub) * 6;
            const uint32_t clear = (static_cast<uint32_t>(data[at]) << 8) | data[at + 1];
            const uint32_t size = readU32BE(data, at + 2);
            offset += clear;
            if (offset + size > position + length) {
              throw std::runtime_error("加密子样本越界");
            }
            if (size > 0) samples.push_back(Sample{offset, offset + size, iv, encrypted});
            encrypted += size;
            offset += size;
          }
          if (offset != position + length) {
            throw std::runtime_error("加密子样本长度异常");
          }
        }
        position += length;
        aux += auxSize;
        ++sampleIndex;
      }
    }
    if (sampleIndex != sampleCount) throw std::runtime_error("视频样本不完整");

    // 还原原始编码格式：从 frma 取回 codec，把 encv/enca 改回去，并抹掉 sinf
    int64_t at = stbl.body;
    while (at + 8 <= stbl.end) {
      Box sinf;
      if (!findBox(data, "sinf", at, stbl.end, sinf)) break;
      Box frma;
      if (!findBox(data, "frma", sinf.body, sinf.end, frma)) {
        throw std::runtime_error("原始编码格式缺失");
      }
      if (frma.body + 4 > frma.end) throw std::runtime_error("原始编码格式无效");
      const Bytes original(data.begin() + frma.body, data.begin() + frma.body + 4);
      for (int64_t entryAt = sinf.start - 1; entryAt >= stbl.body; --entryAt) {
        const std::string type = asciiAt(data, static_cast<size_t>(entryAt), 4);
        if (type == "encv" || type == "enca") {
          writeFourcc(header, entryAt, original);
          break;
        }
      }
      writeFourcc(header, sinf.start + 4, Bytes{'f', 'r', 'e', 'e'});
      std::fill(header.begin() + static_cast<int64_t>(sinf.body),
                header.begin() + static_cast<int64_t>(sinf.end), 0x00);
      at = sinf.end;
    }
  }

  if (samples.empty()) throw std::runtime_error("视频加密索引不可用");
  std::sort(samples.begin(), samples.end(),
            [](const Sample& a, const Sample& b) { return a.start < b.start; });
  for (size_t i = 1; i < samples.size(); ++i) {
    if (samples[i - 1].end > samples[i].start) {
      throw std::runtime_error("视频样本重叠");
    }
  }
  return Prepared{header, samples};
}

// ---------------------------------------------------------------- 分片解密

Bytes decrypt(const Bytes& data, int64_t start, const std::optional<Bytes>& key,
              const Prepared& prepared) {
  if (start < 0) throw std::invalid_argument("start 不能为负");

  Bytes result = data;
  const int64_t end = start + static_cast<int64_t>(data.size());

  if (start < static_cast<int64_t>(prepared.header.size())) {
    const int64_t stop = std::min<int64_t>(end, static_cast<int64_t>(prepared.header.size()));
    for (int64_t i = start; i < stop; ++i) {
      result[static_cast<size_t>(i - start)] = prepared.header[static_cast<size_t>(i)];
    }
  }
  if (!key || key->empty()) return result;

  const std::vector<Sample>& samples = prepared.samples;
  size_t low = 0;
  size_t high = samples.size();
  while (low < high) {
    const size_t middle = (low + high) / 2;
    if (samples[middle].end <= start) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }

  for (size_t i = low; i < samples.size(); ++i) {
    const Sample& sample = samples[i];
    if (sample.start >= end) break;
    const int64_t begin = std::max<int64_t>(start, sample.start);
    const int64_t stop = std::min<int64_t>(end, sample.end);
    if (begin >= stop) continue;

    const int64_t relative = begin - sample.start + sample.encryptedOffset;
    const size_t skip = static_cast<size_t>(relative % 16);
    const Bytes counter = counterAdd(sample.iv, static_cast<uint64_t>(relative / 16));

    Bytes chunk(skip, 0x00);
    chunk.insert(chunk.end(), result.begin() + (begin - start), result.begin() + (stop - start));
    const Bytes decrypted = aes128Ctr(*key, counter, chunk);

    for (size_t k = skip; k < decrypted.size(); ++k) {
      result[static_cast<size_t>(begin - start) + (k - skip)] = decrypted[k];
    }
  }
  return result;
}

}  // namespace hongguo::media_crypto
