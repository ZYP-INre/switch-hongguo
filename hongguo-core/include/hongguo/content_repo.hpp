// SPDX-License-Identifier: GPL-3.0-only
//
// 内容编排。移植自 hongguoTV 的 ContentRepository.kt。
//
// 上游改版时**只需要改这个文件** —— 接口形态、字段名、解析路径全部集中在此。
#pragma once

#include <map>
#include <stdexcept>
#include <string>

#include "hongguo/http.hpp"
#include "hongguo/model.hpp"
#include "hongguo/signer.hpp"

namespace hongguo {

// 搜索结果过期（漫剧翻页时上游未给游标）。
class SearchSessionExpired : public std::runtime_error {
 public:
  SearchSessionExpired() : std::runtime_error("搜索结果已过期，请从第 1 页重新搜索") {}
};

class ContentRepo {
 public:
  ContentRepo(Http& http, Signer signer);

  CatalogPage home(int page = 1, ContentType type = ContentType::Short);

  // 分类浏览（网站的「热门推荐」流）。page 从 1 开始。
  CatalogPage browse(int page, Category category);

  // 排行榜。仅 RealDrama / ComicDrama 有。
  CatalogPage ranking(int page, Category category);
  CatalogPage search(const std::string& keyword, int page = 1,
                     ContentType type = ContentType::Short);
  Detail detail(const std::string& seriesId);

  // ⚠ 新播放通道（2026-10-15 起）**同时需要剧集 ID 与分集 ID**：
  //   play-info 会校验两者是否属于同一部剧，只给分集 ID 直接 403。
  StreamInfo stream(const std::string& seriesId, const std::string& episodeId,
                    int maxQuality = 1080);

  // 把清晰度描述换算成数值（如 "1080P" → 1080、"1920x1080" → 1080）。
  static int qualityNumber(const std::string& value);

  // 从 HTML 里抠出 `_ROUTER_DATA` 后跟的 JSON 对象文本。
  // 用花括号配平扫描，正确处理字符串内的括号与转义。
  static std::string extractRouterJson(const std::string& html);

 private:
  struct SearchCursor {
    std::string searchId;
    std::string passback;
    int offset = 0;
    bool hasOffset = false;
    int64_t updatedAt = 0;
  };

  std::string text(const std::string& url, const Signer::Request* signedRequest);

  // 分类页与首页的数据结构相同，共用这一份解析
  CatalogPage parseCategorySection(int page, const std::string& url);

  // 排行榜：key 形如 rank_hot-real-drama/page
  CatalogPage parseRanking(int page, const std::string& url, const std::string& rankKey);

  CatalogPage comicRanking(int page);
  CatalogPage appSearch(const std::string& keyword, int page, ContentType type);

  Http& http_;
  Signer signer_;
  std::map<std::string, SearchCursor> searchCursors_;
};

}  // namespace hongguo
