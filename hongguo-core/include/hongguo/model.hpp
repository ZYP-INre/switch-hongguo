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

// 一次可播放的流。key 为空表示资源未加密。
struct StreamInfo {
  std::string url;
  std::optional<Bytes> key;
  std::string quality;  // 如「1080P」
};

}  // namespace hongguo
