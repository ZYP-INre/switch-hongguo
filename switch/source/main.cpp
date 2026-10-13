// SPDX-License-Identifier: GPL-3.0-only
//
// 红果内容层 · Switch 诊断程序。
//
// 目的：在没有 UI 的情况下，证明内容层能在**真机**上跑通——
// 联网、签名、解密、起本地流服务这一整条链路。
//
// ⚠ 输出必须是纯 ASCII。
//   libnx 的 console 用的内置字体只覆盖 ASCII，中文会渲染成乱码，
//   那样打印出来的结果等于没打印。剧名是中文，所以这里**只打印数量、ID、
//   状态码这类英数信息**，足够定位问题。
//
// 这不是最终形态。UI 阶段完成后它就不再需要了。

#include <switch.h>

#include <cstdio>
#include <string>

#include "hongguo/content_repo.hpp"
#include "hongguo/digest.hpp"
#include "hongguo/http.hpp"
#include "hongguo/local_proxy.hpp"
#include "hongguo/stream_reader.hpp"

namespace {

using namespace hongguo;

// ⚠ 固定分集 ID，用于和桌面端对拍。
//   之前这里走「首页第一部 → 第1集」，而桌面端参考值是另一部剧算的 ——
//   两边根本不是同一个视频，密钥和样本当然对不上。
//   改这里时，桌面端必须用同一个 ID。
constexpr const char* kDemoEpisode = "7692469795035302936";

int64_t nowMillis() {
  return static_cast<int64_t>(armTicksToNs(armGetSystemTick()) / 1000000ull);
}

Signer productionSigner() {
  static u64 seed = 0;
  if (seed == 0) randomGet(&seed, sizeof(seed));
  return Signer([] { return nowMillis(); }, [] {
    // xorshift64*：分布够均匀即可，不要求密码学强度
    seed ^= seed >> 12;
    seed ^= seed << 25;
    seed ^= seed >> 27;
    return static_cast<double>((seed * 2685821657736338717ull) >> 11) /
           9007199254740992.0;
  });
}

// 每一步的结果都记下来，最后给一个总的结论。
struct Report {
  bool home = false;
  bool detail = false;
  bool stream = false;
  bool decrypt = false;
  bool proxy = false;
  std::string firstFailure;

  void fail(const char* step, const std::string& why) {
    if (firstFailure.empty()) {
      firstFailure = std::string(step) + ": " + why;
    }
  }
};

void rule() { printf("----------------------------------------\n"); }

// console 的内置字体渲染不了中文，而 hongguo-core 的异常信息全是中文。
// 把非 ASCII 字节以 \xNN 打出来 —— 纯 ASCII 控制台能显示，发回来也能直接解码。
// 错误信息是诊断里最有价值的东西，不能让它变成乱码。
void printError(const char* prefix, const std::string& message) {
  std::string out;
  for (const unsigned char ch : message) {
    if (ch >= 0x20 && ch < 0x7f) {
      out.push_back(static_cast<char>(ch));
    } else if (ch == '\n' || ch == '\r' || ch == '\t') {
      out.push_back(' ');
    } else {
      char buf[8];
      std::snprintf(buf, sizeof(buf), "\\x%02X", ch);
      out += buf;
    }
  }
  const size_t kWidth = 68;
  for (size_t i = 0; i < out.size(); i += kWidth) {
    printf("%s%s\n", i == 0 ? prefix : "        ", out.substr(i, kWidth).c_str());
  }
}

void runProbe(Report& report) {
  httpGlobalInit();
  Http http;
  ContentRepo repo(http, productionSigner());

  printf("HONGGUO PROBE  (content layer on Switch)\n");
  rule();

  // ---- 1. home ----
  printf("[1] HOME\n");
  CatalogPage home;
  try {
    home = repo.home(1, ContentType::Short);
    printf("    OK  items=%zu\n", home.items.size());
    report.home = true;
  } catch (const std::exception& e) {
    printError("    FAIL  ", e.what());
    report.fail("home", e.what());
    return;
  }
  if (home.items.empty()) {
    printf("    EMPTY - cannot continue\n");
    report.fail("home", "empty list");
    return;
  }
  // 剧名是中文，打出来会是乱码，只打 ID
  printf("    first id=%s  badge_len=%zu\n", home.items[0].id.c_str(),
         home.items[0].badge.size());

  // ---- 2. detail ----
  printf("[2] DETAIL\n");
  std::string episodeId = kDemoEpisode;
  try {
    const Detail detail = repo.detail(home.items[0].id);
    printf("    OK  episodes=%zu\n", detail.episodes.size());
    report.detail = true;
  } catch (const std::exception& e) {
    printError("    FAIL  ", e.what());
    report.fail("detail", e.what());
    return;
  }
  printf("    使用固定分集 id=%s（与桌面端一致）\n", episodeId.c_str());

  // ---- 3. 播放解析（签名 + 两层解密）----
  printf("[3] STREAM RESOLVE  (signed POST + 2-layer decrypt)\n");
  StreamInfo info;
  try {
    info = repo.stream(episodeId);
    printf("    OK  quality=%s  encrypted=%s\n", info.quality.c_str(),
           info.key ? "yes" : "no");
    printf("    url_len=%zu\n", info.url.size());
    report.stream = true;
  } catch (const std::exception& e) {
    printError("    FAIL  ", e.what());
    report.fail("stream", e.what());
    return;
  }

  // ---- 4. 拉取并解密 ----
  printf("[4] DECRYPT  (range fetch + CENC)\n");
  try {
    StreamReader reader(http, info);
    reader.prepare();
    printf("    total=%lld bytes\n", static_cast<long long>(reader.total()));

    const Bytes head = reader.read(0, 4096);
    const std::string fourcc = asciiAt(head, 4, 4);
    printf("    container fourcc='%s'  %s\n", fourcc.c_str(),
           fourcc == "ftyp" ? "OK" : "BAD");
    if (fourcc != "ftyp") {
      report.fail("decrypt", "bad container header");
      return;
    }

    // ⚠ 头部 4 字节是**未加密**区域，验它说明不了解密对不对。
    //   这里读一段**加密的视频数据**并打印指纹，与桌面端对拍 ——
    //   解密若在 ARM 上有偏差，mpv 拿到的就是乱码，表现为 0/0 不播。
    // 逐段打印中间值，用来判断是「密钥派生」「样本索引」还是「CTR 解密」出了偏差
    printf("    EP=%s\n", info.episodeId.c_str());
    printf("    QUALITY=%s\n", info.quality.c_str());
    printf("    SPADE_A len=%zu first32=%.32s\n", info.rawKey.size(), info.rawKey.c_str());
    printf("    SPADE_A len=%zu first32=%.32s\n", info.rawKey.size(), info.rawKey.c_str());
    if (info.key) {
      printf("    KEY len=%zu hex=%s\n", info.key->size(), toHex(*info.key).c_str());
    } else {
      printf("    KEY 无（资源未加密）\n");
    }
    {
      printf("%s", reader.debugSampleDump(3).c_str());
    }
    {
      const int64_t off = 2 * 1024 * 1024;   // 2MB 处，远在 moov 之后，必为加密样本
      if (reader.total() > off + 1024 * 1024) {
        const Bytes mid = reader.read(off, 1024 * 1024);
        printf("    DATA@2MB len=%zu md5=%s\n", mid.size(), md5Hex(mid).c_str());
        printf("    DATA@2MB head16=%s\n", toHex(Bytes(mid.begin(), mid.begin()+16)).c_str());
      } else {
        printf("    (资源过小，跳过加密区取样)\n");
      }
    }
    report.decrypt = true;

    // ---- 5. 本地代理 ----
    printf("[5] LOCAL PROXY\n");
    LocalProxy proxy;
    const std::string url = proxy.start(reader);
    printf("    listening: %s\n", url.c_str());
    printf("%s", proxy.selfTest().c_str());
    report.proxy = true;
  } catch (const std::exception& e) {
    printError("    FAIL  ", e.what());
    report.fail("decrypt", e.what());
    return;
  }
}

}  // namespace

int main(int argc, char** argv) {
  consoleInit(NULL);

  const Result rc = socketInitializeDefault();
  if (R_FAILED(rc)) {
    printf("socket init failed: 0x%08x\n", rc);
    consoleExit(NULL);
    return 1;
  }

  Report report;
  runProbe(report);

  rule();
  if (report.home && report.detail && report.stream && report.decrypt && report.proxy) {
    printf("RESULT: ALL PASS\n");
    printf("content layer works on this Switch.\n");
  } else {
    printf("RESULT: FAILED\n");
    printf("  home=%s detail=%s stream=%s decrypt=%s proxy=%s\n",
           report.home ? "ok" : "NO", report.detail ? "ok" : "NO",
           report.stream ? "ok" : "NO", report.decrypt ? "ok" : "NO",
           report.proxy ? "ok" : "NO");
    if (!report.firstFailure.empty()) {
      printError("  first failure -> ", report.firstFailure);
    }
  }
  rule();
  printf("Press + to exit.\n");

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  PadState pad;
  padInitializeDefault(&pad);
  while (appletMainLoop()) {
    padUpdate(&pad);
    if (padGetButtonsDown(&pad) & HidNpadButton_Plus) break;
    consoleUpdate(NULL);
  }

  socketExit();
  consoleExit(NULL);
  return 0;
}
