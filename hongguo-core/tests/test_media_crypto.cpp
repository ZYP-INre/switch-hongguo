// SPDX-License-Identifier: GPL-3.0-only
//
// 移植自 hongguoTV 的 MediaCryptoTest.kt。
// 这些是合成用例（不是从网络抓的），覆盖了三件靠真实数据很难穷尽的事：
//   * CTR 解密在任意非对齐区间上的行为
//   * 子样本加密下的计数器延续
//   * 容器头部是否还原了**原始**编码格式（Node 版在这里是硬编码，会出错）
#include "harness.hpp"
#include "hongguo/media_crypto.hpp"

#include <stdexcept>
#include <string>

using namespace hongguo;
using namespace hongguo::media_crypto;

namespace {

// NIST SP 800-38A F.5.1 的 AES-128-CTR 标准向量
const Bytes kNistKey = fromHex("2b7e151628aed2a6abf7158809cf4f3c");
const Bytes kNistIv = fromHex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
const Bytes kNistPlain = fromHex(
    "6bc1bee22e409f96e93d7e117393172a"
    "ae2d8a571e03ac9c9eb76fac45af8e51"
    "30c81c46a35ce411e5fbc1191a0a52ef"
    "f69f2445df4f9b17ad2b417be66c3710");
const Bytes kNistCipher = fromHex(
    "874d6191b620e3261bef6864990db6ce"
    "9806f66b7970fdff8617187bb9fffdff"
    "5ae4df3edbd5d35e5b4f09020db03eab"
    "1e031dda2fbe03d1792170a0f3009cee");

Bytes box(const std::string& type, const Bytes& body) {
  Bytes out = be32(static_cast<uint32_t>(body.size() + 8));
  out.insert(out.end(), type.begin(), type.end());
  out.insert(out.end(), body.begin(), body.end());
  return out;
}

// 构造一个最小的加密 MP4：一个 trak，一个样本，aux 数据放在文件末尾的 free box 里。
// codec 可指定，用来验证头部还原的是原始编码而非硬编码值。
Bytes fixture(const std::string& codec = "avc1", uint8_t auxSize = 8) {
  const Bytes ftyp = box("ftyp", Bytes{'i', 's', 'o', 'm', '0', '0', '0', '0'});

  auto moov = [&](int32_t auxOffset) {
    const Bytes codecBytes(codec.begin(), codec.end());
    Bytes entryBody(78, 0x00);
    const Bytes sinf = box("sinf", box("frma", codecBytes));
    entryBody.insert(entryBody.end(), sinf.begin(), sinf.end());
    const Bytes entry = box("encv", entryBody);

    Bytes stsdBody = be32(1);
    stsdBody.insert(stsdBody.end(), entry.begin(), entry.end());
    const Bytes stsd = box("stsd", concat(Bytes(4, 0x00), stsdBody));

    const Bytes stsz = box("stsz", concat(Bytes(4, 0x00), be32(16), be32(1)));
    const Bytes stsc = box("stsc", concat(Bytes(4, 0x00), be32(1), be32(1), be32(1), be32(1)));
    const Bytes stco = box("stco", concat(Bytes(4, 0x00), be32(1), be32(4096)));

    Bytes saizBody(4, 0x00);
    saizBody.push_back(auxSize);
    const Bytes saizCount = be32(1);
    saizBody.insert(saizBody.end(), saizCount.begin(), saizCount.end());
    const Bytes saiz = box("saiz", saizBody);

    const Bytes saio = box("saio", concat(Bytes(4, 0x00), be32(1),
                                          be32(static_cast<uint32_t>(auxOffset))));

    const Bytes stbl =
        box("stbl", concat(concat(stsd, stsz, stsc, stco), concat(saiz, saio)));
    const Bytes minf = box("minf", stbl);
    const Bytes mdia = box("mdia", minf);
    // aux 数据就是末尾 free box 的 8 字节 body
    const Bytes freeBox = box("free", Bytes(kNistIv.begin(), kNistIv.begin() + 8));
    return box("moov", concat(box("trak", mdia), freeBox));
  };

  const Bytes initial = concat(ftyp, moov(0));
  return concat(ftyp, moov(static_cast<int32_t>(initial.size()) - 8));
}

}  // namespace

HG_TEST(media_ctr_vectors_across_every_unaligned_range) {
  // 对每个起点 × 每个长度都验一遍。CTR 的问题几乎全出在非对齐和跨块上，
  // 只测"整块对齐"的用例是测不出来的。
  Bytes source(23, 0x66);
  source.insert(source.end(), kNistCipher.begin(), kNistCipher.end());
  source.insert(source.end(), 12, 0x77);
  Bytes expected(23, 0x66);
  expected.insert(expected.end(), kNistPlain.begin(), kNistPlain.end());
  expected.insert(expected.end(), 12, 0x77);

  Prepared prepared;
  prepared.samples.push_back(Sample{23, 87, kNistIv, 0});

  size_t checked = 0;
  for (size_t start = 0; start < source.size(); ++start) {
    const size_t maxLength = std::min<size_t>(29, source.size() - start);
    for (size_t length = 1; length <= maxLength; ++length) {
      const Bytes slice(source.begin() + static_cast<long>(start),
                        source.begin() + static_cast<long>(start + length));
      const Bytes want(expected.begin() + static_cast<long>(start),
                       expected.begin() + static_cast<long>(start + length));
      const Bytes got = decrypt(slice, static_cast<int64_t>(start), kNistKey, prepared);
      if (got != want) {
        ::hgtest::fail("start=" + std::to_string(start) + " length=" +
                       std::to_string(length) + "\n    实得: " +
                       hgtest::describe(got) + "\n    期望: " + hgtest::describe(want));
      }
      ++checked;
    }
  }
  HG_CHECK(checked > 2000);
}

HG_TEST(media_subsamples_preserve_clear_bytes_and_continue_ctr) {
  Bytes source = {1, 2, 3};
  source.insert(source.end(), kNistCipher.begin(), kNistCipher.begin() + 21);
  source.push_back(4);
  source.push_back(5);
  source.insert(source.end(), kNistCipher.begin() + 21, kNistCipher.end());

  Bytes expected = {1, 2, 3};
  expected.insert(expected.end(), kNistPlain.begin(), kNistPlain.begin() + 21);
  expected.push_back(4);
  expected.push_back(5);
  expected.insert(expected.end(), kNistPlain.begin() + 21, kNistPlain.end());

  Prepared prepared;
  prepared.samples.push_back(Sample{3, 24, kNistIv, 0});
  // 第二个子样本落在同一 CTR 密钥流的第 21 字节处
  prepared.samples.push_back(Sample{26, 69, kNistIv, 21});

  HG_CHECK_EQ(decrypt(source, 0, kNistKey, prepared), expected);

  const Bytes slice(expected.begin() + 22, expected.begin() + 47);
  const Bytes inputSlice(source.begin() + 22, source.begin() + 47);
  HG_CHECK_EQ(decrypt(inputSlice, 22, kNistKey, prepared), slice);
}

HG_TEST(media_header_restores_original_codec) {
  // 关键回归：Node 版会把 encv 硬编码改成 hvc1。若原片是 AVC，改完就播不了。
  const Bytes data = fixture("avc1");
  const Prepared prepared = prepare(data, kNistKey);

  const std::string header(prepared.header.begin(), prepared.header.end());
  HG_CHECK(header.find("avc1") != std::string::npos);
  HG_CHECK(header.find("encv") == std::string::npos);
  HG_CHECK(header.find("sinf") == std::string::npos);

  HG_CHECK_EQ(prepared.samples.size(), 1u);
  HG_CHECK_EQ(prepared.samples[0].start, static_cast<int64_t>(4096));
  HG_CHECK_EQ(prepared.samples[0].end, static_cast<int64_t>(4112));
  HG_CHECK_EQ(headerEnd(data, 5000), static_cast<int64_t>(data.size()));
}

HG_TEST(media_header_preserves_hevc_codec_too) {
  const Prepared prepared = prepare(fixture("hvc1"), kNistKey);
  const std::string header(prepared.header.begin(), prepared.header.end());
  HG_CHECK(header.find("hvc1") != std::string::npos);
  HG_CHECK(header.find("encv") == std::string::npos);
}

HG_TEST(media_truncated_auxiliary_data_fails_closed) {
  // aux 声明 16 字节，但 free box 只给了 8 字节 —— 必须报错而不是读越界
  bool threw = false;
  try {
    prepare(fixture("avc1", 16), kNistKey);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  HG_CHECK(threw);
}

HG_TEST(media_oversized_moov_rejected) {
  // moov 超过 8MiB 上限必须拒绝（headerEnd 的硬约束）
  bool threw = false;
  try {
    const Bytes data = concat(box("ftyp", Bytes{'i', 's', 'o', 'm', '0', '0', '0', '0'}),
                              be32(9000000), Bytes{'m', 'o', 'o', 'v'});
    headerEnd(data, 10000000);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  HG_CHECK(threw);
}

HG_TEST(media_rejects_moov_not_at_file_start) {
  // 索引不在文件头时直接报错，而不是静默出错
  bool threw = false;
  try {
    const Bytes data = concat(box("ftyp", Bytes{'i', 's', 'o', 'm', '0', '0', '0', '0'}),
                              box("free", Bytes(32, 0x00)));
    headerEnd(data, 100000);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  HG_CHECK(threw);
}

HG_TEST(media_without_key_returns_data_untouched) {
  const Bytes data = fixture("avc1");
  const Prepared prepared = prepare(data, std::nullopt);
  HG_CHECK(prepared.samples.empty());
  HG_CHECK_EQ(decrypt(data, 0, std::nullopt, prepared), data);
}
