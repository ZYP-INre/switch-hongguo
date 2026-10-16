// SPDX-License-Identifier: GPL-3.0-only
//
// 内容层的纯数据结构。不含逻辑，便于单独使用与测试。
#pragma once

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "hongguo/bytes.hpp"

namespace hongguo {

enum class ContentType { Comic, Short };

std::string toString(ContentType type);

// 网站上的内容分类。对应 URL 里的 slug：
//   /category/real-drama  真人短剧
//   /category/comic-drama 漫剧
//   /category/ai-drama    AI 剧
// 排行榜则走 /rank/hot-<slug>。
// 注意：这不是 ContentType —— 后者是 App 搜索接口的 tab 概念，两者不同源。
enum class Category { RealDrama, ComicDrama, AiDrama };

std::string toSlug(Category category);
std::string toLabel(Category category);

// 排行榜只覆盖前两类
bool hasRanking(Category category);

struct Series {
  std::string id;
  std::string title;
  std::string cover;
  std::string description;
  std::string badge;  // 如「全 80 集」
  std::string tags;   // 以 · 连接
};

struct Episode {
  std::string id;
  int number = 0;
  std::string title;
};

// 漫剧榜单里的位次与热度。
struct RankPosition {
  std::optional<int> rank;
  std::string heat;
};

struct ComicRanking {
  std::map<std::string, RankPosition> positions;
  int totalPages = 0;
  std::string updatedText;
};

struct CatalogPage {
  std::vector<Series> items;
  bool hasMore = false;
  std::optional<ComicRanking> ranking;
};

struct Detail {
  Series series;
  std::vector<std::string> episodes;  // 分集 ID，顺序即集数
};

// 新播放通道的一条 DASH 轨。
//
// ⚠ 这里的区间是**原始密文资源**上的字节区间，不是解密后重新排布的结果。
//   CENC 解密是就地等长的，所以明文与密文的偏移完全一致 ——
//   拿到这些数字就能直接对 CDN 发 Range 请求，也能原样转给播放器。
struct DashTrack {
  std::string url;
  std::string backupUrl;
  int64_t initEnd = 0;     // 初始化段是 [0, initEnd]（闭区间）
  int64_t indexStart = 0;  // sidx（分片索引）区间
  int64_t indexEnd = 0;
  int64_t firstMoof = 0;   // 首个 moof 的**起点**（即第一个分片的偏移），没有该字段时为 0
  int64_t total = 0;
  std::string codecType;   // 如 "h264"
  // 生成 MPD 用：ffmpeg 的 DASH 解复用器要据此推算媒体时长与带宽。
  double duration = 0;     // 秒
  int64_t bandwidth = 0;
  int width = 0;
  int height = 0;
};

// 一次可播放的流。key 为空表示资源未加密。
struct StreamInfo {
  std::string url;  // 旧通道：单文件渐进式 MP4
  std::optional<Bytes> key;
  std::string quality;  // 如「1080P」
  // 诊断用：派生密钥的原始输入（spade_a / player_access_key）与解析出的分集 ID。
  // 两端结果不一致时，先看这两个是否相同 —— 不同说明拿到的根本不是一个视频。
  std::string rawKey;
  std::string episodeId;

  // 新通道（2026-10-15 起）：视频与音频是两条独立的 DASH 轨，各自加密。
  // dash 为 true 时 url/key 不使用，改看 video/audio。
  bool dash = false;
  DashTrack video;
  DashTrack audio;
};

}  // namespace hongguo
