// SPDX-License-Identifier: GPL-3.0-only
//
// hgctl —— 内容层的命令行入口。
//
// 两个用途：
//   1. 本机验证：直连上游，确认接口是活的、我们的用法是对的
//   2. 样例代码：将来接进 wiliwili 时，照着这里的调用顺序写即可

#include <chrono>
#include <thread>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <vector>

#include "hongguo/content_repo.hpp"
#include "hongguo/http.hpp"
#include "hongguo/model.hpp"
#include "hongguo/signer.hpp"
#include "hongguo/dash_reader.hpp"
#include "hongguo/stream_reader.hpp"
#include "hongguo/local_proxy.hpp"

namespace {

using namespace hongguo;

int64_t nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 生产环境的时钟与随机源。测试时换成固定值即可复现。
Signer productionSigner() {
  static std::mt19937_64 engine(std::random_device{}());
  static std::uniform_real_distribution<double> distribution(0.0, 1.0);
  return Signer([] { return nowMillis(); },
                []( ) { return distribution(engine); });
}

void printSeries(const Series& series, const char* indent = "  ") {
  std::printf("%s%s  %s\n", indent, series.id.c_str(), series.title.c_str());
  if (!series.badge.empty()) std::printf("%s    标  %s\n", indent, series.badge.c_str());
  if (!series.tags.empty()) std::printf("%s    类  %s\n", indent, series.tags.c_str());
  if (!series.cover.empty()) {
    std::printf("%s    图  %.90s%s\n", indent, series.cover.c_str(),
                series.cover.size() > 90 ? "…" : "");
  }
}

void printUsage() {
  std::printf(
      "hgctl —— 红果内容层命令行\n"
      "\n"
      "用法：\n"
      "  hgctl home [--comic] [--page N]        首页/分类\n"
      "  hgctl search <关键词> [--comic] [--page N]\n"
      "  hgctl detail <剧集ID>                  详情与分集\n"
      "  hgctl play <剧集ID> <分集ID>           解析播放地址\n"
      "  hgctl fetch <剧集ID> <分集ID> <输出文件> [--bytes N]\n"
      "                                         下载并解密一段，写盘\n"
      "  hgctl stream <剧集ID> <分集ID> [--seconds N]  起本地代理并自测 Range\n"
      "  hgctl dashdump <剧集ID> <分集ID> <video|audio> <输出文件> [--bytes N]\n"
      "  hgctl dashserve <剧集ID> <分集ID> [--seconds N]\n"
      "\n"
      "示例：\n"
      "  hgctl home\n"
      "  hgctl search 短剧\n"
      "  hgctl detail 7391000000000000000\n");
}

struct Args {
  std::vector<std::string> positional;
  bool comic = false;
  int page = 1;
  int64_t bytes = 1 << 20;
  int seconds = 12;
};

Args parseArgs(int argc, char** argv) {
  Args args;
  for (int i = 0; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--comic") {
      args.comic = true;
    } else if (arg == "--page" && i + 1 < argc) {
      args.page = std::atoi(argv[++i]);
    } else if (arg == "--bytes" && i + 1 < argc) {
      args.bytes = std::atoll(argv[++i]);
    } else if (arg == "--seconds" && i + 1 < argc) {
      args.seconds = std::atoi(argv[++i]);
    } else if (!arg.empty() && arg[0] == '-') {
      throw std::runtime_error("未知参数: " + arg);
    } else {
      args.positional.push_back(arg);
    }
  }
  return args;
}

int run(int argc, char** argv) {
  if (argc < 2) {
    printUsage();
    return 2;
  }

  const std::string command = argv[1];
  const Args args = parseArgs(argc - 2, argv + 2);

  httpGlobalInit();
  Http http;
  ContentRepo repo(http, productionSigner());

  const ContentType type = args.comic ? ContentType::Comic : ContentType::Short;

  if (command == "home") {
    const CatalogPage page = repo.home(args.page, type);
    std::printf("首页（%s，第 %d 页）：%zu 部\n", toString(type).c_str(), args.page,
                page.items.size());
    for (const Series& series : page.items) printSeries(series);
    std::printf("还有更多：%s\n", page.hasMore ? "是" : "否");
    if (page.ranking) {
      std::printf("榜单共 %d 页，更新时间：%s\n", page.ranking->totalPages,
                  page.ranking->updatedText.c_str());
    }
    return 0;
  }

  if (command == "search") {
    if (args.positional.empty()) throw std::runtime_error("search 需要关键词");
    const std::string keyword = args.positional[0];
    const CatalogPage page = repo.search(keyword, args.page, type);
    std::printf("搜索「%s」（%s，第 %d 页）：%zu 条\n", keyword.c_str(),
                toString(type).c_str(), args.page, page.items.size());
    for (const Series& series : page.items) printSeries(series);
    std::printf("还有更多：%s\n", page.hasMore ? "是" : "否");
    return 0;
  }

  if (command == "detail") {
    if (args.positional.empty()) throw std::runtime_error("detail 需要剧集 ID");
    const Detail detail = repo.detail(args.positional[0]);
    std::printf("剧集：%s（%s）\n", detail.series.title.c_str(), detail.series.id.c_str());
    if (!detail.series.badge.empty()) {
      std::printf("  标  %s\n", detail.series.badge.c_str());
    }
    if (!detail.series.description.empty()) {
      std::printf("  简介 %s\n", detail.series.description.c_str());
    }
    std::printf("分集 %zu 集：\n", detail.episodes.size());
    const size_t show = std::min<size_t>(detail.episodes.size(), 8);
    for (size_t i = 0; i < show; ++i) {
      std::printf("  %2zu  %s\n", i + 1, detail.episodes[i].c_str());
    }
    if (detail.episodes.size() > show) {
      std::printf("  …（余 %zu 集）\n", detail.episodes.size() - show);
    }
    return 0;
  }

  if (command == "play") {
    if (args.positional.size() < 2) throw std::runtime_error("play 需要剧集 ID 与分集 ID");
    const StreamInfo info = repo.stream(args.positional[0], args.positional[1]);
    std::printf("清晰度：%s\n", info.quality.c_str());
    std::printf("加密：%s\n", info.key ? "是（密钥已派生）" : "否");
    if (info.dash) {
      const auto dump = [](const char* label, const DashTrack& track) {
        std::printf("%s init 0-%lld  index %lld-%lld  moof %lld  共 %lld 字节\n", label,
                    static_cast<long long>(track.initEnd),
                    static_cast<long long>(track.indexStart),
                    static_cast<long long>(track.indexEnd),
                    static_cast<long long>(track.firstMoof),
                    static_cast<long long>(track.total));
        std::printf("     %.140s%s\n", track.url.c_str(), track.url.size() > 140 ? "…" : "");
      };
      std::printf("通道：DASH（视频/音频分轨）\n");
      dump("视频", info.video);
      if (!info.audio.url.empty()) dump("音频", info.audio);
    } else {
      std::printf("地址：%.160s%s\n", info.url.c_str(), info.url.size() > 160 ? "…" : "");
    }
    return 0;
  }

  if (command == "fetch") {
    if (args.positional.size() < 3) throw std::runtime_error("fetch 需要剧集 ID、分集 ID 与输出文件");
    const StreamInfo info = repo.stream(args.positional[0], args.positional[1]);
    StreamReader reader(http, info);
    reader.prepare();
    std::printf("资源总大小：%lld 字节，清晰度 %s\n",
                static_cast<long long>(reader.total()), info.quality.c_str());
    const int64_t want = std::min<int64_t>(args.bytes, reader.total());
    Bytes collected;
    for (int64_t at = 0; at < want;) {
      const int chunk = static_cast<int>(std::min<int64_t>(1 << 20, want - at));
      const Bytes piece = reader.read(at, chunk);
      if (piece.empty()) break;
      collected.insert(collected.end(), piece.begin(), piece.end());
      at += static_cast<int64_t>(piece.size());
    }
    std::FILE* file = std::fopen(args.positional[1].c_str(), "wb");
    if (!file) throw std::runtime_error("无法写入 " + args.positional[2]);
    std::fwrite(collected.data(), 1, collected.size(), file);
    std::fclose(file);
    std::printf("已写出 %zu 字节 → %s\n", collected.size(), args.positional[2].c_str());
    return 0;
  }

  if (command == "dashserve") {
    // 把一集的两条 DASH 轨合成一条 fMP4，起本地代理，供 mpv/ffprobe 直接播。
    if (args.positional.size() < 2) throw std::runtime_error("dashserve 需要剧集 ID 与分集 ID");
    const StreamInfo info = repo.stream(args.positional[0], args.positional[1]);
    if (!info.dash) throw std::runtime_error("当前通道不是 DASH");
    DashTrackReader video(http, info.video, info.key);
    std::unique_ptr<DashTrackReader> audio;
    if (!info.audio.url.empty()) audio = std::make_unique<DashTrackReader>(http, info.audio, info.key);
    DashMuxer muxer(video, audio.get());
    muxer.prepare();
    LocalProxy proxy;
    const std::string url = proxy.startMux(muxer);
    std::printf("本地地址：%s\n", url.c_str());
    std::printf("合成后共 %lld 字节（头部 %lld，分片 %zu 个）\n",
                static_cast<long long>(muxer.total()),
                static_cast<long long>(muxer.headerSize()), muxer.pieceCount());
    std::printf("（%d 秒后退出，期间可用 ffprobe \"%s\" 验收）\n", args.seconds, url.c_str());
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(args.seconds));
    return 0;
  }

  if (command == "dashdump") {
    // 把某一条 DASH 轨解密后整条写出，供 ffprobe 验收 —— 这是在没有播放器的
    // 环境下唯一能证明"分片索引 + 就地解密"确实正确的办法。
    if (args.positional.size() < 4) {
      throw std::runtime_error("dashdump 需要剧集 ID、分集 ID、轨（video/audio）与输出文件");
    }
    const StreamInfo info = repo.stream(args.positional[0], args.positional[1]);
    if (!info.dash) throw std::runtime_error("当前通道不是 DASH");
    const std::string which = args.positional[2];
    const DashTrack& track = which == "audio" ? info.audio : info.video;
    if (track.url.empty()) throw std::runtime_error("该轨不存在");
    DashTrackReader reader(http, track, info.key);
    reader.prepare();
    const int64_t total = reader.total();
    const int64_t want = args.bytes > 0 ? std::min<int64_t>(args.bytes, total) : total;
    std::printf("轨=%s 共 %lld 字节（分片 %zu 个），解密写出前 %lld 字节…\n", which.c_str(),
                static_cast<long long>(total), reader.segmentCount(),
                static_cast<long long>(want));
    std::FILE* file = std::fopen(args.positional[3].c_str(), "wb");
    if (!file) throw std::runtime_error("无法写入 " + args.positional[3]);
    const int64_t kChunk = 1 << 20;
    for (int64_t at = 0; at < want;) {
      const int length = static_cast<int>(std::min<int64_t>(kChunk, want - at));
      const Bytes piece = reader.read(at, length);
      if (piece.empty()) break;
      std::fwrite(piece.data(), 1, piece.size(), file);
      at += static_cast<int64_t>(piece.size());
      std::printf("\r  %lld / %lld", static_cast<long long>(at), static_cast<long long>(want));
      std::fflush(stdout);
    }
    std::fclose(file);
    std::printf("\n已写出 → %s\n", args.positional[3].c_str());
    return 0;
  }

  if (command == "stream") {
    if (args.positional.size() < 2) throw std::runtime_error("stream 需要剧集 ID 与分集 ID");
    const StreamInfo info = repo.stream(args.positional[0], args.positional[1]);
    StreamReader reader(http, info);
    reader.prepare();
    LocalProxy proxy;
    const std::string url = proxy.start(reader);
    std::printf("本地地址：%s\n", url.c_str());
    std::printf("资源总大小：%lld 字节，清晰度 %s\n",
                static_cast<long long>(reader.total()), info.quality.c_str());
    std::printf("自测 Range（%d 秒后退出）…\n", args.seconds);
    const std::string report = proxy.selfTest();
    std::printf("%s", report.c_str());
    std::printf("提示：可用 mpv \"%s\" 直接播放。\n", url.c_str());
    return 0;
  }

  throw std::runtime_error("未知命令: " + command);
}

}  // namespace

int main(int argc, char** argv) {
  try {
    return run(argc, argv);
  } catch (const std::exception& error) {
    std::fprintf(stderr, "错误：%s\n", error.what());
    return 1;
  }
}
