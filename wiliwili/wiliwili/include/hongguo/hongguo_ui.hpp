// SPDX-License-Identifier: GPL-3.0-only
//
// 红果短剧的界面层。自包含：不碰 wiliwili 原有的 B 站代码，
// 只在 main.cpp 换一行入口；加 -DHONGGUO_UI=OFF 即可编回原版。
//
// 浏览页 = 顶部一排模式按钮（分类 / 排行榜）+ 下方封面网格。
#pragma once

#include <memory>
#include <string>

#include <borealis/core/activity.hpp>
#include <borealis/core/bind.hpp>
#include <borealis/core/box.hpp>
#include <borealis/views/label.hpp>

#include "hongguo/content_repo.hpp"
#include "hongguo/local_proxy.hpp"
#include "hongguo/model.hpp"
#include "hongguo/stream_reader.hpp"
#include "utils/event_helper.hpp"

class VideoView;
class RecyclingGrid;

namespace hongguo::ui {

// ---------------------------------------------------------------- 内容层单例
class Core {
 public:
  static Core& instance();

  ContentRepo& repo();
  Http& http();

  // 为某集开一个本地流，返回可直接喂给播放器的地址。失败抛 std::runtime_error。
  std::string openStream(const std::string& episodeId, std::string& quality);
  void closeStream();


  // 浏览页当前那一批剧。播放页的「换一部」从这里随机挑。
  void setPool(std::vector<Series> items);
  // 随机挑一部 id 不等于 excludeId 的；池子为空返回空 Series。
  Series randomFromPool(const std::string& excludeId);

 private:
  Core();
  ~Core();
  Core(const Core&) = delete;
  Core& operator=(const Core&) = delete;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ------------------------------------------------------------------ 浏览页
class BrowseActivity : public brls::Activity {
 public:
  CONTENT_FROM_XML_RES("activity/hongguo_browse.xml");

  BrowseActivity();
  ~BrowseActivity() override;

  void onContentAvailable() override;

 private:
  // 切换栏目：分类浏览 或 排行榜
  void load(Category category, bool ranking);
  // 「换一批」：换一页，**替换**掉当前列表（不追加）
  void refresh();

  struct Impl;
  std::unique_ptr<Impl> impl_;

  BRLS_BIND(brls::Box, modes, "hongguo/modes");
  BRLS_BIND(RecyclingGrid, grid, "hongguo/grid");
  BRLS_BIND(brls::Label, status, "hongguo/status");
};

// ------------------------------------------------------------------ 搜索页
class SearchActivity : public brls::Activity {
 public:
  CONTENT_FROM_XML_RES("activity/hongguo_search.xml");

  SearchActivity();
  ~SearchActivity() override;

  void onContentAvailable() override;

 private:
  void prompt();                                    // 唤起系统键盘
  void doSearch(const std::string& keyword);        // 执行搜索

  BRLS_BIND(RecyclingGrid, grid, "hongguo/grid");
  BRLS_BIND(brls::Label, status, "hongguo/status");

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// ------------------------------------------------------------------ 分集列表
class EpisodeActivity : public brls::Activity {
 public:
  CONTENT_FROM_XML_RES("activity/hongguo_list.xml");

  explicit EpisodeActivity(Series series);
  ~EpisodeActivity() override;

  void onContentAvailable() override;

 private:
  BRLS_BIND(brls::Box, list, "hongguo/list");

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

// -------------------------------------------------------------------- 播放页
class PlayerActivity : public brls::Activity {
 public:
  CONTENT_FROM_XML_RES("activity/hongguo_player.xml");

  // episodes 为空表示"自己去取"（首页直接点进来预览第一集的情形）；
  // 从分集列表进来时已经拿到了，直接传，省一次请求。
  explicit PlayerActivity(Series series, std::vector<std::string> episodes = {},
                          size_t startIndex = 0);
  ~PlayerActivity() override;

  void onContentAvailable() override;

  // 退出时必须停播放、关本地代理，否则「退出后还在响、再点新剧就卡住」
  void willDisappear(bool resetState = false) override;

 private:
  void startSeries(Series series);   // 载入某部剧并从第 startIndex 集开始
  void playIndex(size_t index);      // 切到第 index 集
  void onEpisodeEnded();             // 一集播完 → 自动下一集

 private:
  BRLS_BIND(VideoView, video, "hongguo/video");
  BRLS_BIND(brls::Label, status, "hongguo/status");

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace hongguo::ui
