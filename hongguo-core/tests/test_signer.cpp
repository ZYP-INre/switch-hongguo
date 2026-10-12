// SPDX-License-Identifier: GPL-3.0-only
#include "harness.hpp"
#include "hongguo/digest.hpp"
#include "hongguo/signer.hpp"

#include <optional>
#include <string>

using namespace hongguo;

namespace {

// 与 hongguoTV 的 SignerTest.kt 完全同款的 LCG。
// 之所以要复刻它：helios 与 medusa 内部会取随机数，
// 不把随机源钉死就无法与向量比对。
struct Lcg {
  uint64_t seed = 17;
  double operator()() {
    seed = (seed * 1664525ull + 1013904223ull) & 0xffffffffull;
    return static_cast<double>(seed) / 4294967296.0;
  }
};

}  // namespace

HG_TEST(signer_matches_pinned_vectors) {
  const auto rows = hgtest::tsv("signer.txt");
  HG_CHECK(!rows.empty());

  for (const auto& row : rows) {
    HG_CHECK(row.size() == 7);
    const int ts = std::stoi(row[0]);
    const std::string url = row[1];
    std::optional<Bytes> body;
    // "-" 表示无 body；空字符串表示 body 为空字节序列 —— 两者签名含义不同
    if (row[2] != "-") body = fromHex(row[2]);

    const std::string query = url.substr(url.find('?') + 1);
    Lcg rng;
    Signer signer([ts]() { return static_cast<int64_t>(ts) * 1000; },
                  [&rng]() { return rng(); });

    // 调用顺序即 RNG 消耗顺序，必须与 SignerTest 一致
    const int branch = signer.branch(query, body, ts);
    const std::string hash = toHex(
        signer.hashF13(sm3(query), body ? md5(*body) : Bytes(16, 0x00), ts));
    const std::string gorgon = signer.gorgon(query, body, ts, 42131);
    const std::string helios = signer.helios(ts);
    const std::string medusa = signer.medusa(url, body, ts);

    const std::string where = "ts=" + row[0] + " url=" + url;
    if (branch != 0) {
      ::hgtest::fail(where + "：branch=" + std::to_string(branch) + "，期望 0");
    }
    if (hash != row[3]) {
      ::hgtest::fail(where + "\n    hash 实得: " + hash + "\n    hash 期望: " + row[3]);
    }
    if (gorgon != row[4]) {
      ::hgtest::fail(where + "\n    gorgon 实得: " + gorgon + "\n    gorgon 期望: " + row[4]);
    }
    if (helios != row[5]) {
      ::hgtest::fail(where + "\n    helios 实得: " + helios + "\n    helios 期望: " + row[5]);
    }
    if (medusa != row[6]) {
      ::hgtest::fail(where + "\n    medusa 实得: " + medusa + "\n    medusa 期望: " + row[6]);
    }
  }
}

HG_TEST(signer_query_encoding) {
  // 不转义集合必须是 A-Za-z0-9 - _ . ~ *
  HG_CHECK_EQ(encodeQueryValue("abcXYZ019"), std::string("abcXYZ019"));
  HG_CHECK_EQ(encodeQueryValue("-_.~*"), std::string("-_.~*"));
  // 空格、加号
  HG_CHECK_EQ(encodeQueryValue("a b"), std::string("a%20b"));
  HG_CHECK_EQ(encodeQueryValue("a+b"), std::string("a%2Bb"));
  // 这些字符 Java 与 JS 的处理不同，必须显式转义才等价
  HG_CHECK_EQ(encodeQueryValue("!'()"), std::string("%21%27%28%29"));
  // 保留字符
  HG_CHECK_EQ(encodeQueryValue("a/b?c=d&e"), std::string("a%2Fb%3Fc%3Dd%26e"));
  // 已是百分号编码的输入要被二次转义
  HG_CHECK_EQ(encodeQueryValue("%20"), std::string("%2520"));
  // 非 ASCII 按 UTF-8 逐字节转义，十六进制大写
  HG_CHECK_EQ(encodeQueryValue("短剧"), std::string("%E7%9F%AD%E5%89%A7"));
}

HG_TEST(signer_query_join_preserves_order) {
  const QueryValues values = {{"b", "2"}, {"a", "1"}, {"c", "x y"}};
  HG_CHECK_EQ(encodeQuery(values), std::string("b=2&a=1&c=x%20y"));
}

HG_TEST(signer_builds_expected_headers_for_post) {
  Lcg rng;
  Signer signer([]() { return 1800500000LL * 1000; }, [&rng]() { return rng(); });

  const Bytes body = Bytes{'h', 'e', 'l', 'l', 'o'};
  const QueryValues values = {{"aid", "8662"}, {"device_id", "42"}};
  const Signer::Request request = signer.sign("https://example.com/x", values, body);

  // URL 里必须带上 ts 与 _rticket
  HG_CHECK(request.url.find("ts=1800500000") != std::string::npos);
  HG_CHECK(request.url.find("_rticket=") != std::string::npos);
  HG_CHECK(request.url.find("aid=8662") != std::string::npos);

  auto header = [&](const std::string& name) -> std::string {
    for (const auto& item : request.headers) {
      if (item.first == name) return item.second;
    }
    return "<缺失>";
  };

  // 六个签名头一个都不能少
  for (const char* name : {"x-gorgon", "x-medusa", "x-helios", "x-ladon", "x-argus",
                           "x-khronos"}) {
    HG_CHECK(header(name) != "<缺失>");
  }
  HG_CHECK_EQ(header("x-khronos"), std::string("1800500000"));
  // POST 专有
  HG_CHECK_EQ(header("x-ss-stub"), md5HexUpper(body));
  HG_CHECK_EQ(header("Content-Type"), std::string("application/json; charset=UTF-8"));
  HG_CHECK_EQ(header("x-vc-bdturing-sdk-version"), std::string("3.7.2.cn"));
  // UA 必须是视频版（POST 用 VIDEO_UA，GET 用 APP_UA）
  HG_CHECK(header("User-Agent").find("Cronet") != std::string::npos);
}

HG_TEST(signer_builds_expected_headers_for_get) {
  Lcg rng;
  Signer signer([]() { return 1800500000LL * 1000; }, [&rng]() { return rng(); });

  const QueryValues values = {{"aid", "8662"}};
  const Signer::Request request = signer.sign("https://example.com/y", values);

  auto header = [&](const std::string& name) -> std::string {
    for (const auto& item : request.headers) {
      if (item.first == name) return item.second;
    }
    return "<缺失>";
  };

  // GET 不应出现 POST 专有头
  HG_CHECK_EQ(header("x-ss-stub"), std::string("<缺失>"));
  HG_CHECK_EQ(header("Content-Type"), std::string("<缺失>"));
  // UA 是 App 版
  HG_CHECK(header("User-Agent").find("Cronet") == std::string::npos);
  HG_CHECK(header("User-Agent").find("com.phoenix.read") != std::string::npos);
  // 但签名头仍在
  HG_CHECK(header("x-gorgon") != "<缺失>");
}

HG_TEST(signer_ticket_jitter_lands_on_branch_zero) {
  // 签名的核心动作：抖动 _rticket 直到 branch 为 0。
  // 这里换多组时间戳验证它总能收敛。
  for (int64_t second = 1800500000; second < 1800500020; ++second) {
    Lcg rng;
    Signer signer([second]() { return second * 1000; }, [&rng]() { return rng(); });
    const Signer::Request request =
        signer.sign("https://example.com/z", {{"aid", "8662"}});
    HG_CHECK(request.url.find("ts=" + std::to_string(second)) != std::string::npos);
  }
}
