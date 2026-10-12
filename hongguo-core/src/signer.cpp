// SPDX-License-Identifier: GPL-3.0-only
//
// 签名链移植。逐段对照 hongguoTV 的 Signer.kt（其本身是 drpys 实现的移植），
// 并由 tools/oracle 与 Node 原实现逐字节对拍。
#include "hongguo/signer.hpp"

#include <algorithm>
#include <stdexcept>

#include "hongguo/aesv3.hpp"
#include "hongguo/device.hpp"
#include "hongguo/digest.hpp"
#include "hongguo/protobuf.hpp"
#include "hongguo/vendor.hpp"

namespace hongguo {
namespace {

namespace pb = protobuf;

// 8 位反转。对应 Node 的 reverseBits，也等价于 Integer.reverse(x) >>> 24。
uint8_t reverseBits8(uint8_t value) {
  uint8_t result = 0;
  for (int i = 0; i < 8; ++i) result = static_cast<uint8_t>((result << 1) | ((value >> i) & 1));
  return result;
}

// medusa 内层混淆。逐位操作用 int32 承载中间值，以对齐 JS/Kotlin 的
// 有符号 32 位语义 —— 用 uint8 会在 `~d2` 处丢掉高位而结果不同。
Bytes xmxorBytes(const Bytes& data, const Bytes& key) {
  const size_t n = data.size();
  Bytes encoded(n);

  for (size_t i = 0; i < n; ++i) {
    const size_t at = (i * 4) & 28;
    const int32_t d0 = key[at];
    const int32_t d1 = key[at + 1];
    const int32_t v = data[i];
    int32_t d2 = (((v << 4) | (v >> 4)) & 255) + d0;
    d2 = (~d2) ^ d1;
    d2 = (((d2 & 255) << 3) | ((d2 & 255) >> 5)) & 255;
    d2 = (d2 + d1) & 255;
    d2 = (d2 ^ d0) & 255;
    encoded[n - i - 1] = static_cast<uint8_t>((~d2) & 255);
  }

  const int32_t last = encoded[n - 1] ^ encoded[n - 2];
  const int32_t first = encoded[0];
  encoded[0] = static_cast<uint8_t>((~last + first) & 255);
  encoded[1] = static_cast<uint8_t>(
      ((encoded[0] ^ encoded[n - 1] ^ 254) + encoded[1]) & 255);
  encoded[2] = static_cast<uint8_t>(
      (encoded[2] + ((last - first) ^ (((encoded[1] << 3) | (encoded[1] >> 5)) & 255) ^ 2)) &
      255);
  for (size_t i = 0; i + 4 < n; ++i) {
    const int32_t temp = (((encoded[i + 2] << 3) | (encoded[i + 2] >> 5)) & 255) ^
                         encoded[i + 1] ^ static_cast<int32_t>(i + 3);
    encoded[i + 3] = static_cast<uint8_t>((~temp + encoded[i + 3]) & 255);
  }
  encoded[n - 1] = static_cast<uint8_t>(encoded[n - 1] ^ encoded[n - 2]);

  int32_t sum = 0;
  for (size_t i = 1; i < n; ++i) sum += encoded[i];
  encoded[0] = static_cast<uint8_t>(((encoded[0] ^ encoded[1]) + sum) & 255);
  return encoded;
}

// hashF13 branch 0 的六组循环参数（按 ivV0 选取）
constexpr int kIvV1List[6] = {static_cast<int>(0xc4a78580u), static_cast<int>(0xb3c0fd39u),
                              static_cast<int>(0xc58c5686u), static_cast<int>(0xc9aa3ba7u),
                              static_cast<int>(0xf5a7adf2u), static_cast<int>(0x963c2ed1u)};

constexpr int kChosen[6][11] = {
    {101, 5, 7, 6, 3, 2, 1, 0, 5, 4, 3},  {96, 0, 6, 7, 5, 3, 2, 1, 5, 4, 4},
    {96, 7, 6, 2, 1, 4, 0, 5, 4, 3, 5},   {99, 3, 6, 2, 4, 5, 1, 0, 0, 7, 6},
    {96, 0, 5, 6, 7, 3, 1, 2, 5, 4, 4},   {100, 2, 0, 3, 5, 4, 6, 7, 2, 1, 5}};

constexpr uint32_t kInitial[8] = {0x7aba4fc8u, 0x67166507u, 0x6403fa00u,
                                  0x340f512fu, 984304912u, 3005047866u,
                                  2874125293u, 2152413264u};

}  // namespace

// ---------------------------------------------------------------- 百分号编码

std::string encodeQueryValue(std::string_view value) {
  static const char* kHexUpperDigits = "0123456789ABCDEF";
  std::string out;
  out.reserve(value.size());
  for (const unsigned char ch : value) {
    const bool unreserved = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                            (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' ||
                            ch == '.' || ch == '~' || ch == '*';
    if (unreserved) {
      out.push_back(static_cast<char>(ch));
      continue;
    }
    // 非 ASCII 一律按 UTF-8 逐字节转义；入参已是 UTF-8 字节序列。
    out.push_back('%');
    out.push_back(kHexUpperDigits[ch >> 4]);
    out.push_back(kHexUpperDigits[ch & 0x0f]);
  }
  return out;
}

std::string encodeQuery(const QueryValues& values) {
  std::string out;
  for (size_t i = 0; i < values.size(); ++i) {
    if (i != 0) out.push_back('&');
    out += encodeQueryValue(values[i].first);
    out.push_back('=');
    out += encodeQueryValue(values[i].second);
  }
  return out;
}

// -------------------------------------------------------------------- 基础件

uint32_t Signer::getIv(uint32_t initial, const Bytes& data) {
  uint32_t value = initial;
  for (size_t i = 0; i < data.size(); ++i) {
    if ((i & 1) == 0) {
      value = (value >> 4) ^ value ^ (value << 6) ^ data[i];
    } else {
      value = ~((value >> 7) ^ value ^ (data[i] | (value << 12)));
    }
  }
  return value;
}

uint32_t Signer::sumMd5(const Bytes& data) {
  uint32_t check = 0x20220420u;
  for (int i = 0; i < 12; ++i) {
    const uint32_t temp =
        (i & 1) == 0 ? (check >> 3) ^ check : (check >> 5) ^ check;
    if ((i & 1) == 0) {
      check = static_cast<uint32_t>(data[static_cast<size_t>(i)]) ^ (check << 7);
    } else {
      check = ~(static_cast<uint32_t>(data[static_cast<size_t>(i)]) | (check << 11));
    }
    check = check ^ temp;
  }
  return (check | 4u) ^ 0x1000000u;
}

int Signer::branch(const std::string& query, const std::optional<Bytes>& body,
                   int ts) const {
  const Bytes querySm3 = sm3(query);
  const Bytes bodyMd5 = body ? md5(*body) : Bytes(16, 0x00);
  const uint32_t iv = getIv(getIv(getIv(0x20230928u, querySm3), bodyMd5),
                            le32(static_cast<uint32_t>(ts)));
  const uint32_t low = iv & 15u;
  return static_cast<int>(low % 3u);
}

Bytes Signer::hashF13(const Bytes& querySm3, const Bytes& bodyMd5, int ts) const {
  const uint32_t tsWord = static_cast<uint32_t>(ts);
  const uint32_t iv = getIv(getIv(getIv(0x20230928u, querySm3), bodyMd5),
                            le32(tsWord));
  const uint32_t low = iv & 15u;
  const int ivV0 = static_cast<int>((low * 171u) >> 9);
  if (low % 3u != 0) {
    throw std::logic_error("hashF13: 只实现了 branch 0（见设计文档 5.1）");
  }

  const int ivV1 = kIvV1List[ivV0];
  const uint32_t countV1 = static_cast<uint32_t>(ivV1 + ts + 1) & 255u;
  const uint32_t countV2 = static_cast<uint32_t>(ivV1 + ts);

  // 查表按 countV1 旋转
  uint32_t tt[64];
  for (int i = 0; i < 64; ++i) tt[i] = ror32(vendor::kHashTable[i], static_cast<int>(countV1));

  const uint32_t shift = (countV2 + 2u) & 7u;
  const uint8_t seedBytes[4] = {0xfa, 0x45, 0x61, 0xd7};
  Bytes pad;
  for (const uint8_t value : seedBytes) {
    const uint32_t combined = static_cast<uint32_t>(value) |
                              (static_cast<uint32_t>(value) << 8);
    pad.push_back(static_cast<uint8_t>((combined >> shift) & 255u));
  }

  Bytes data = concat(querySm3, bodyMd5, le32(tsWord), pad,
                      fromHex("00000000000001a0"));

  std::vector<uint32_t> di;
  di.reserve(128);
  for (size_t i = 0; i < data.size(); i += 4) di.push_back(readU32BE(data, i));

  for (int i = 0; i < 112; ++i) {
    const uint32_t x = di[static_cast<size_t>(i) + 1];
    const uint32_t y = di[static_cast<size_t>(i) + 14];
    const uint32_t r1 = rol32(x, 14) ^ rol32(x, 25) ^ (x >> 3);
    const uint32_t r2 = rol32(y, 13) ^ rol32(y, 15) ^ (y >> 10);
    di.push_back(di[static_cast<size_t>(i)] + di[static_cast<size_t>(i) + 9] + r1 + r2);
  }

  uint32_t init[8];
  for (int i = 0; i < 8; ++i) init[i] = ror32(kInitial[i], static_cast<int>(countV2 & 31u));

  uint32_t d[8];
  for (int i = 0; i < 8; ++i) d[i] = init[i];

  const int* chosen = kChosen[ivV0];
  for (int i = 0; i < chosen[0]; ++i) {
    const uint32_t base = static_cast<uint32_t>(ivV1 + i);
    const uint32_t n0 = di[base & 127u];
    const uint32_t n1 = ((d[chosen[3]] ^ d[chosen[4]]) & d[chosen[1]]) ^ d[chosen[3]];
    const uint32_t n2 = rol32(d[chosen[1]], 26) ^ rol32(d[chosen[1]], 21) ^
                        rol32(d[chosen[1]], 7);
    const uint32_t n4 = n0 + n1 + n2 + tt[base & 63u] + d[chosen[5]];
    const uint32_t n5 = rol32(d[chosen[2]], 30) ^ rol32(d[chosen[2]], 19) ^
                        rol32(d[chosen[2]], 10);
    const uint32_t n6 = (d[chosen[2]] & d[chosen[6]]) |
                        ((d[chosen[2]] | d[chosen[6]]) & d[chosen[7]]);
    const uint32_t old = d[chosen[9]];

    // d.unshift(d.pop())：d 恒为 8 项，故等价于"取末项插到队首"
    const uint32_t tail = d[7];
    for (int k = 7; k > 0; --k) d[k] = d[k - 1];
    d[0] = tail;

    d[chosen[10]] = n5 + n6 + n4;
    d[chosen[8]] = old + n4;
  }

  Bytes ret;
  ret.reserve(32);
  for (int i = 0; i < 8; ++i) {
    const Bytes word = be32(d[i] + init[i]);
    ret.insert(ret.end(), word.begin(), word.end());
  }

  Bytes folded(ret.begin(), ret.begin() + 16);
  for (size_t i = 0; i < 16; ++i) folded[i] = static_cast<uint8_t>(ret[i] ^ ret[i + 16]);

  return concat(folded, le32(sumMd5(folded)));
}

std::string Signer::gorgon(const std::string& query, const std::optional<Bytes>& body,
                           int ts, int random) const {
  const uint32_t randomWord = static_cast<uint32_t>(random);
  Bytes input = md5(query);
  input.resize(4);
  const Bytes bodyMd5 = body ? md5(*body) : Bytes(16, 0x00);
  input.insert(input.end(), bodyMd5.begin(), bodyMd5.begin() + 4);
  input.resize(4 + 4 + 4);
  const Bytes sdk = le32(67503104u);
  input.insert(input.end(), sdk.begin(), sdk.end());
  const Bytes tsBytes = be32(static_cast<uint32_t>(ts));
  input.insert(input.end(), tsBytes.begin(), tsBytes.end());

  const uint8_t key[8] = {0x4a, 0x40, 0x16,
                          static_cast<uint8_t>((randomWord >> 8) & 255u), 0x47, 0x6c,
                          0x01, static_cast<uint8_t>(randomWord & 255u)};

  uint8_t s[256];
  for (int i = 0; i < 256; ++i) s[i] = static_cast<uint8_t>(i);
  uint32_t j = 0;
  // ⚠ 这是非标准 RC4：密钥调度只做 s[i] = s[j]，**不交换**。
  //   照教科书补上 swap 会导致签名全错。原实现（Node 与 Kotlin）都是如此。
  for (uint32_t i = 0; i < 256; ++i) {
    j = (j + s[i] + key[i % 8]) & 255u;
    s[i] = s[j];
  }

  Bytes out(input.size());
  j = 0;
  for (size_t n = 0; n < input.size(); ++n) {
    const uint32_t i = static_cast<uint32_t>(n) + 1;
    j += s[i & 255u];
    const uint8_t y = s[j & 255u];
    s[i & 255u] = y;
    out[n] = static_cast<uint8_t>(input[n] ^ s[(y + y) & 255u]);
  }

  for (size_t i = 0; i < out.size(); ++i) {
    const uint8_t value = static_cast<uint8_t>(((out[i] >> 4) | (out[i] << 4)) & 255u);
    const uint8_t next = out[i + 1 < out.size() ? i + 1 : 0];
    const uint8_t reversed = reverseBits8(static_cast<uint8_t>((next ^ value) & 255u));
    out[i] = static_cast<uint8_t>(~(reversed ^ 20u));
  }

  Bytes result = fromHex("8404");
  result.push_back(static_cast<uint8_t>(randomWord & 255u));
  result.push_back(static_cast<uint8_t>((randomWord >> 8) & 255u));
  result.push_back(0x40);
  result.push_back(0x01);
  result.insert(result.end(), out.begin(), out.end());
  return toHex(result);
}

std::string Signer::helios(int ts) {
  const uint32_t randomWord = nextRandom32();

  Bytes seed = le32(randomWord);
  const Bytes tag = {'8', '6', '6', '2'};
  seed.insert(seed.end(), tag.begin(), tag.end());
  // ascii = md5 摘要的十六进制字符串本身的字节（32 个 ASCII 字符）
  const std::string hexDigest = md5Hex(seed);
  const Bytes ascii(hexDigest.begin(), hexDigest.end());

  uint64_t words[4];
  for (int i = 0; i < 4; ++i) words[i] = readU64LE(ascii, static_cast<size_t>(i) * 8);

  uint64_t table[35];
  table[0] = words[0];
  uint64_t b0 = words[0];
  uint64_t b8 = words[1];
  // 队列初值为 words[2]、words[3]
  uint64_t queue[64];
  size_t queueSize = 2;
  queue[0] = words[2];
  queue[1] = words[3];
  size_t queueHead = 0;

  for (int i = 0; i < 34; ++i) {
    uint64_t x8 = ror64(b8, 8) + b0;
    x8 = x8 ^ static_cast<uint64_t>(i);
    queue[queueHead + queueSize] = x8;
    ++queueSize;
    x8 = x8 ^ ror64(b0, 61);
    table[i + 1] = x8;
    b0 = x8;
    b8 = queue[queueHead];
    ++queueHead;
    --queueSize;
  }

  const std::string text = std::to_string(ts) + "-1588093228-8662";
  Bytes data(text.begin(), text.end());
  const size_t padding = 16 - (data.size() % 16);
  data.insert(data.end(), padding, static_cast<uint8_t>(padding));

  Bytes output = le32(randomWord);
  for (size_t at = 0; at < data.size(); at += 16) {
    uint64_t a = readU64LE(data, at);
    uint64_t b = readU64LE(data, at + 8);
    for (int i = 0; i < 34; ++i) {
      b = table[i] ^ (a + ror64(b, 8));
      a = b ^ ror64(a, 61);
    }
    const Bytes blockA = le64(a);
    const Bytes blockB = le64(b);
    output.insert(output.end(), blockA.begin(), blockA.end());
    output.insert(output.end(), blockB.begin(), blockB.end());
  }
  return toBase64(output);
}

std::string Signer::medusa(const std::string& url, const std::optional<Bytes>& body,
                           int ts) {
  const Bytes bodyMd5 = body ? md5(*body) : Bytes(16, 0x00);
  const std::string queryText = url.substr(url.find('?') + 1);
  const Bytes query(queryText.begin(), queryText.end());
  const Bytes querySm3 = sm3(query);
  const Bytes queryBodyTs = hashF13(querySm3, bodyMd5, static_cast<int>(ts));

  const Bytes nested = pb::encode({
      pb::fieldSint(1, 111), pb::fieldSint(2, 10), pb::fieldSint(3, 694367),
      pb::fieldSint(5, 586952199)});

  const uint32_t messageRand = nextRandom32();
  const int envLaunch = nextRandomBelow(21) + 100;
  const int envPid = nextRandomBelow(2000) + 10001;
  const std::string version(vendor::kDevice[7].value);  // device[7] = version_name

  const Bytes envInner = pb::encode({
      pb::fieldSint(1, clock_() / 1000), pb::fieldSint(2, -2), pb::fieldSint(4, 200)});
  const Bytes env = pb::encode({
      pb::fieldSint(1, envLaunch),
      pb::fieldSint(2, 146331399),
      pb::fieldSint(3, 146331396),
      pb::fieldSint(5, 7),
      pb::fieldString(6, "v04.06.04.03-bugfix"),
      pb::fieldSint(7, envPid),
      pb::fieldMessage(12, deviceProto(vendor::kDevice[1].value, version)),
      pb::fieldMessage(13, envInner),
      pb::fieldString(14, version)});

  Bytes queryHashInput = query;
  queryHashInput.insert(queryHashInput.end(), bodyMd5.begin(), bodyMd5.end());
  const Bytes noneBytes = {'n', 'o', 'n', 'e'};
  queryHashInput.insert(queryHashInput.end(), noneBytes.begin(), noneBytes.end());
  const Bytes queryHash = sm3(queryHashInput);

  const Bytes message = pb::encode({
      pb::fieldBytes(1, fromHex("f7e85ffad7d7dc3bd62ac87057cf6118")),
      pb::fieldSint(2, 3),
      pb::fieldSint(3, static_cast<int64_t>(messageRand)),
      pb::fieldString(4, "8662"),
      pb::fieldString(5, vendor::kDevice[1].value),
      pb::fieldString(6, "1588093228"),
      pb::fieldString(7, version),
      pb::fieldString(8, "v04.06.04-ml-android"),
      pb::fieldSint(9, 67503104),
      pb::fieldBytes(10, fromHex("4001000000000000")),
      pb::fieldSint(12, ts),
      pb::fieldBytes(13, queryBodyTs),
      pb::fieldBytes(14, Bytes(querySm3.begin(), querySm3.begin() + 6)),
      pb::fieldMessage(15, nested),
      pb::fieldString(16, "AXYQOS6n2m60x1fVZHIrH3iol"),
      pb::fieldSint(17, ts),
      pb::fieldBytes(19, queryHash),
      pb::fieldString(20, "none"),
      pb::fieldSint(21, 312),
      pb::fieldMessage(23, env),
      pb::fieldString(24, R"({"cmr":16777216,"cmr2":16777216,"un_h":1879194040,"vpn":0,"kd":0,"fkd":3672518972,"pd":-1872573247,"dyn":"","do":0,"tk":true})")});

  const uint32_t r = nextRandom32();
  const Bytes signKey = fromHex("8ebdfa3806ecc5cee79423e6029ed82540bc2218bb7eae"
                                "f71cb691f7aa8aa2f5");
  Bytes hashInput = signKey;
  const Bytes rBytes = le32(r);
  hashInput.insert(hashInput.end(), rBytes.begin(), rBytes.end());
  hashInput.insert(hashInput.end(), signKey.begin(), signKey.end());
  const Bytes hash = sm3(hashInput);

  const uint32_t d1 = (r >> 16) & 255u;
  const uint32_t seedValue =
      ~(((d1 << 11) | (r >> 24)) ^ (d1 >> 5) ^ d1);
  const Bytes seed = le32(seedValue);

  // transformed = reverse(prefix + xmxor(message, hash))
  Bytes xored = xmxorBytes(message, hash);
  Bytes transformed = fromHex("4001000000000000");
  transformed.insert(transformed.end(), xored.begin(), xored.end());
  std::reverse(transformed.begin(), transformed.end());
  for (size_t i = 0; i < transformed.size(); ++i) {
    transformed[i] = static_cast<uint8_t>(transformed[i] ^ seed[(~i) & 3u]);
  }

  const uint32_t check = ((static_cast<uint32_t>(querySm3[0]) & 63u) << 14) |
                         0x18000001u |
                         ((static_cast<uint32_t>(queryBodyTs[0]) & 63u) << 8);

  Bytes packed;
  packed.push_back(0x35);
  const Bytes randWord = le32(nextRandom32());
  packed.insert(packed.end(), randWord.begin(), randWord.end());
  const Bytes checkBytes = le32(check);
  packed.insert(packed.end(), checkBytes.begin(), checkBytes.end());
  packed.insert(packed.end(), transformed.begin(), transformed.end());
  packed.push_back(static_cast<uint8_t>((r >> 16) & 255u));
  packed.push_back(static_cast<uint8_t>((r >> 24) & 255u));

  const Bytes encrypted = AesV3(fromHex("f1593376766ea98d34f31b057a9d5be4"),
                                static_cast<uint32_t>(ts))
                              .encrypt(packed, fromHex("1fe109a4125283f418de9e051a969e12"));

  const Bytes versionBytes = fromHex("03000000f7e85ffad7d7dc3bd62ac87057cf6118");
  Bytes result;
  result.reserve(20 + 4 + encrypted.size());
  for (size_t i = 0; i < 20; i += 4) {
    const uint32_t word = readU32LE(versionBytes, i) ^ static_cast<uint32_t>(ts);
    const Bytes bytes = le32(word);
    result.insert(result.end(), bytes.begin(), bytes.end());
  }
  result.push_back(static_cast<uint8_t>(r & 255u));
  result.push_back(static_cast<uint8_t>((r >> 8) & 255u));
  result.push_back(0x00);
  result.push_back(0x01);
  result.insert(result.end(), encrypted.begin(), encrypted.end());
  return toBase64(result);
}

// ------------------------------------------------------------------ 随机数源

Signer::Signer(Clock clock, Rng rng) : clock_(std::move(clock)), rng_(std::move(rng)) {}

uint32_t Signer::nextRandom32() {
  // 对应 Kotlin: (random() * 4294967296.0).toLong().toInt()
  const double scaled = rng_() * 4294967296.0;
  return static_cast<uint32_t>(static_cast<int64_t>(scaled) & 0xffffffffLL);
}

int Signer::nextRandomBelow(int bound) {
  // 对应 Kotlin: (random() * bound).toInt()
  return static_cast<int>(rng_() * static_cast<double>(bound));
}

// ------------------------------------------------------------------ 请求组装

Signer::Request Signer::sign(const std::string& base, const QueryValues& values,
                             std::optional<Bytes> body) {
  const int64_t now = clock_();
  const int ts = static_cast<int>(now / 1000);

  for (int offset = 0; offset < 128; ++offset) {
    const int64_t ticket = now + offset;
    QueryValues withStamp = values;
    withStamp.emplace_back("ts", std::to_string(ts));
    withStamp.emplace_back("_rticket", std::to_string(ticket));
    const std::string query = encodeQuery(withStamp);

    if (branch(query, body, ts) != 0) continue;

    const std::string url = base + "?" + query;
    const int random = nextRandomBelow(65536);
    const std::string ticketText = std::to_string(ticket);

    Request request;
    request.url = url;
    request.body = body;

    const std::string userAgent(body ? vendor::kVideoUserAgent : vendor::kAppUserAgent);
    request.headers.emplace_back("User-Agent", userAgent);
    request.headers.emplace_back("Accept",
                                 "application/json; charset=utf-8,application/x-protobuf");
    request.headers.emplace_back("x-ss-req-ticket", ticketText);
    request.headers.emplace_back("x-tt-request-tag", "t=0;n=0");
    request.headers.emplace_back("sdk-version", "2");
    request.headers.emplace_back("passport-sdk-version", "50561");
    request.headers.emplace_back("x-xs-from-web", "0");
    request.headers.emplace_back("x-khronos", std::to_string(ts));
    request.headers.emplace_back("x-ladon", toBase64(be32(static_cast<uint32_t>(ts))));
    request.headers.emplace_back("x-argus", toBase64(le32(static_cast<uint32_t>(ts))));
    request.headers.emplace_back("x-gorgon", gorgon(query, body, ts, random));
    request.headers.emplace_back("x-helios", helios(ts));
    request.headers.emplace_back("x-medusa", medusa(url, body, ts));
    request.headers.emplace_back("x-tt-dt", "");

    if (body) {
      request.headers.emplace_back("Content-Type", "application/json; charset=UTF-8");
      request.headers.emplace_back("x-vc-bdturing-sdk-version", "3.7.2.cn");
      request.headers.emplace_back("x-ss-stub", md5HexUpper(*body));
    }
    return request;
  }
  throw std::runtime_error("无法生成请求签名：128 次票据抖动均落在非 0 分支");
}

}  // namespace hongguo
