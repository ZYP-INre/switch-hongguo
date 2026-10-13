// SPDX-License-Identifier: GPL-3.0-only
#include "hongguo/hongguo_ui.hpp"

#include <borealis.hpp>
#include <borealis/core/touch/tap_gesture.hpp>
#include <borealis/views/cells/cell_detail.hpp>

#include <algorithm>
#include <chrono>
#include <random>
#include <set>
#include <stdexcept>

#ifdef BUILTIN_NSP
#include "nspmini.hpp"
#endif

#include "view/mpv_core.hpp"
#include "view/recycling_grid.hpp"
#include "view/video_card.hpp"
#include "view/video_view.hpp"

namespace hongguo::ui {
namespace {

int64_t nowMillis() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// ⚠ 数据到位后必须手动给一次焦点。
//   Application::pushActivity 里 onContentAvailable() 先执行，之后才
//   giveFocus(getDefaultFocus()) —— 而那一刻列表还是空的，拿到 nullptr 就整个跳过，
//   currentFocus 停在已被覆盖的旧 activity 上，方向键从此没有反应。
void focusAfterLoad(brls::View* view) { brls::Application::giveFocus(view); }

// ---- 网格数据源：用 wiliwili 现成的视频卡片，自带封面加载 ----
// 支持追加（滚动到底续拉下一页），追加后调 grid->notifyDataChanged() 即可。
class SeriesSource : public RecyclingGridDataSource {
 public:
  using Select = std::function<void(const Series&)>;

  SeriesSource(Select onSelect) : onSelect_(std::move(onSelect)) {}

  void reset(std::vector<Series> items) {
    items_ = std::move(items);
    seen_.clear();
    for (const Series& s : items_) seen_.insert(s.id);
  }

  // 去重后追加，返回实际新增数量
  size_t append(const std::vector<Series>& more) {
    size_t added = 0;
    for (const Series& s : more) {
      if (!seen_.insert(s.id).second) continue;  // 已存在，跳过
      items_.push_back(s);
      ++added;
    }
    return added;
  }

  RecyclingGridItem* cellForRow(RecyclingGrid* recycler, size_t index) override {
    auto* item = (RecyclingGridItemVideoCard*)recycler->dequeueReusableCell("Cell");
    const Series& series = items_[index];
    // 封面 URL 已带尺寸参数，不能再拼 ImageHelper::h_ext（会拼坏）
    // 三个 string 参数会命中多个重载，给全参数消歧
    item->setCard(series.cover, series.title, series.badge,
                  std::string(), std::string(), std::string(), std::string());
    return item;
  }

  size_t getItemCount() override { return items_.size(); }
  void clearData() override { items_.clear(); seen_.clear(); }

  void onItemSelected(RecyclingGrid* recycler, size_t index) override {
    if (onSelect_ && index < items_.size()) onSelect_(items_[index]);
  }

 private:
  std::vector<Series> items_;
  std::set<std::string> seen_;
  Select onSelect_;
};

brls::DetailCell* makeCell(const std::string& title, const std::string& detail,
                           std::function<void()> onSelect) {
  auto* cell = new brls::DetailCell();
  cell->setText(title);
  if (!detail.empty()) cell->setDetailText(detail);
  if (onSelect) {
    cell->registerClickAction([onSelect](brls::View*) {
      onSelect();
      return true;
    });
    cell->addGestureRecognizer(new brls::TapGestureRecognizer(cell));
  }
  return cell;
}

brls::Header* makeHeader(const std::string& title) {
  auto* header = new brls::Header();
  header->setTitle(title);
  return header;
}

brls::Label* makeNote(const std::string& text) {
  auto* label = new brls::Label();
  label->setText(text);
  label->setFontSize(24);
  label->setWidthPercentage(100);
  label->setHorizontalAlign(brls::HorizontalAlign::CENTER);
  label->setMargins(40, 0, 40, 0);
  return label;
}

// 点开一部剧 → 分集列表
void openSeries(const Series& series) {
  brls::Application::pushActivity(new EpisodeActivity(series));
}

}  // namespace

// ================================================================ 内容层单例

struct Core::Impl {
  Http http;
  std::mt19937_64 engine{std::random_device{}()};
  std::uniform_real_distribution<double> distribution{0.0, 1.0};
  Signer signer;
  ContentRepo repo;

  Impl()
      : signer([] { return nowMillis(); },
               [this] { return distribution(engine); }),
        repo(http, signer) {
    httpGlobalInit();
  }

  std::unique_ptr<StreamReader> reader;
  std::unique_ptr<LocalProxy> proxy;

  // 浏览页当前那一批剧，「换一部」从这儿随机挑
  std::vector<Series> pool;
  std::mt19937 rng{std::random_device{}()};
};

Core::Core() : impl_(std::make_unique<Impl>()) {}
Core::~Core() = default;
Core& Core::instance() {
  static Core core;
  return core;
}
ContentRepo& Core::repo() { return impl_->repo; }
Http& Core::http() { return impl_->http; }

std::string Core::openStream(const std::string& episodeId, std::string& quality) {
  closeStream();
  const StreamInfo info = impl_->repo.stream(episodeId);
  quality = info.quality;
  impl_->reader = std::make_unique<StreamReader>(impl_->http, info);
  // 用 warm() 而非 prepare()：前者会把 1MB 前缀都预取到本地。
  // 只 prepare() 的话，起播后头几秒每次读取都要现去 CDN 拉，表现为一开始就卡。
  impl_->reader->warm();
  impl_->proxy = std::make_unique<LocalProxy>();
  return impl_->proxy->start(*impl_->reader);
}

void Core::setPool(std::vector<Series> items) { impl_->pool = std::move(items); }

Series Core::randomFromPool(const std::string& excludeId) {
  if (impl_->pool.empty()) return {};
  std::vector<size_t> candidates;
  for (size_t i = 0; i < impl_->pool.size(); ++i) {
    if (impl_->pool[i].id != excludeId) candidates.push_back(i);
  }
  if (candidates.empty()) return {};
  std::uniform_int_distribution<size_t> pick(0, candidates.size() - 1);
  return impl_->pool[candidates[pick(impl_->rng)]];
}

void Core::closeStream() {
  if (impl_->proxy) {
    impl_->proxy->stop();
    impl_->proxy.reset();
  }
  impl_->reader.reset();
}

// ================================================================== 浏览页

struct BrowseActivity::Impl {
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
  bool loading = false;
  Category category = Category::RealDrama;
  bool ranking = false;
  int page = 1;
  bool hasMore = false;
  std::vector<Series> items;
  SeriesSource* source = nullptr;   // 网格持有所有权
};

BrowseActivity::BrowseActivity() : impl_(std::make_unique<Impl>()) {}
BrowseActivity::~BrowseActivity() { *impl_->alive = false; }

void BrowseActivity::onContentAvailable() {
  this->grid->registerCell("Cell", []() {
    return RecyclingGridItemVideoCard::create();
  });

  struct Mode {
    const char* label;
    Category category;
    bool ranking;
  };
  static const Mode kModes[] = {
      {"短剧", Category::RealDrama, false}, {"漫剧", Category::ComicDrama, false},
      {"AI剧", Category::AiDrama, false},   {"短剧榜", Category::RealDrama, true},
      {"漫剧榜", Category::ComicDrama, true}};

  for (const Mode& mode : kModes) {
    const Category category = mode.category;
    const bool ranking = mode.ranking;
    auto* button = new brls::Button();
    button->setText(mode.label);
    button->setMargins(0, 8, 0, 8);
    button->registerClickAction([this, category, ranking](brls::View*) {
      this->load(category, ranking);
      return true;
    });
    button->addGestureRecognizer(new brls::TapGestureRecognizer(button));
    this->modes->addView(button);
  }

  // 换一批：直接替换列表，不追加。
  // 之前用无限滚动，条目越堆越多、后面必然卡顿，而且用户并不需要"看得更远"，
  // 只需要"看更多不同的"。
  {
    auto* button = new brls::Button();
    button->setText("换一批");
    button->setMargins(0, 24, 0, 8);
    button->registerClickAction([this](brls::View*) {
      this->refresh();
      return true;
    });
    button->addGestureRecognizer(new brls::TapGestureRecognizer(button));
    this->modes->addView(button);
  }

  // 装到 Switch 主页（把 .nro 变成主页上的游戏图标）。
  // 依赖构建时打开 -DBUILTIN_NSP=ON，那会把 forwarder 的 .nsp 打进 romfs。
#ifdef BUILTIN_NSP
  if (brls::Application::getPlatform()->isApplicationMode()) {
    auto* button = new brls::Button();
    button->setText("装到主页");
    button->setMargins(0, 24, 0, 8);
    button->registerClickAction([](brls::View*) {
      auto* dialog = new brls::Dialog("把「红果短剧」装到 Switch 主页？");
      dialog->addButton("取消", [] {});
      dialog->addButton("安装", [] {
        brls::Application::blockInputs();
        mini::InstallSD("romfs:/nsp_forwarder.nsp");
        const unsigned long long titleId = mini::GetTitleID();
        appletRequestLaunchApplication(titleId, nullptr);
      });
      dialog->open();
      return true;
    });
    button->addGestureRecognizer(new brls::TapGestureRecognizer(button));
    this->modes->addView(button);
  }
#endif

  // 搜索入口
  {
    auto* button = new brls::Button();
    button->setText("搜索");
    button->setMargins(0, 24, 0, 0);
    button->registerClickAction([](brls::View*) {
      brls::Application::pushActivity(new SearchActivity());
      return true;
    });
    button->addGestureRecognizer(new brls::TapGestureRecognizer(button));
    this->modes->addView(button);
  }

  this->load(Category::RealDrama, false);
}

void BrowseActivity::load(Category category, bool ranking) {
  if (impl_->loading) return;
  impl_->loading = true;
  impl_->category = category;
  impl_->ranking = ranking;
  impl_->page = 1;

  if (this->status) this->status->setText("正在加载…");

  auto alive = impl_->alive;
  const std::string label = toLabel(category) + (ranking ? "排行榜" : "推荐");

  brls::async([this, alive, category, ranking, label] {
    std::string error;
    CatalogPage page;
    try {
      page = ranking ? Core::instance().repo().ranking(1, category)
                     : Core::instance().repo().browse(1, category);
    } catch (const std::exception& e) {
      error = e.what();
    }

    brls::sync([this, alive, page, error, label] {
      if (!*alive) return;
      impl_->loading = false;

      if (!error.empty()) {
        if (this->status) this->status->setText("加载失败");
        this->grid->setError(error);
        return;
      }
      if (page.items.empty()) {
        if (this->status) this->status->setText("没有内容");
        this->grid->setEmpty("这一栏暂时没有内容");
        return;
      }

      impl_->hasMore = false;
      impl_->items = page.items;
      Core::instance().setPool(impl_->items);   // 供播放页「换一部」随机挑
      if (this->status) {
        this->status->setText(label + " · " + std::to_string(page.items.size()) + " 部");
      }
      auto* source = new SeriesSource([](const Series& s) { openSeries(s); });
      source->reset(impl_->items);
      impl_->source = source;
      this->grid->setDataSource(source);
      focusAfterLoad(this->grid);
    });
  });
}

void BrowseActivity::refresh() {
  if (impl_->loading) return;
  impl_->loading = true;

  // 换一个随机页给用户"新的一批"。同页重复进没意义。
  static std::mt19937 rng{std::random_device{}()};
  std::uniform_int_distribution<int> pick(2, 20);
  const int page = pick(rng);

  auto alive = impl_->alive;
  const Category category = impl_->category;
  const bool ranking = impl_->ranking;
  const std::string label = toLabel(category) + (ranking ? "排行榜" : "推荐");

  if (this->status) this->status->setText("正在换一批…");

  brls::async([this, alive, category, ranking, page, label] {
    std::string error;
    CatalogPage result;
    try {
      result = ranking ? Core::instance().repo().ranking(1, category)
                       : Core::instance().repo().browse(page, category);
    } catch (const std::exception& e) {
      error = e.what();
    }

    brls::sync([this, alive, result, error, label] {
      if (!*alive) return;
      impl_->loading = false;

      if (!error.empty()) {
        if (this->status) this->status->setText("换一批失败");
        return;
      }
      if (result.items.empty()) {
        if (this->status) this->status->setText("没有内容");
        return;
      }

      // 替换而非追加
      impl_->hasMore = false;
      impl_->items = result.items;
      Core::instance().setPool(impl_->items);
      if (this->status) {
        this->status->setText(label + " · " + std::to_string(impl_->items.size()) + " 部");
      }
      auto* source = new SeriesSource([](const Series& s) { openSeries(s); });
      source->reset(impl_->items);
      impl_->source = source;
      this->grid->setDataSource(source);
      focusAfterLoad(this->grid);
    });
  });
}

// ================================================================== 搜索页

struct SearchActivity::Impl {
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
  std::string keyword;
  SeriesSource* source = nullptr;
};

SearchActivity::SearchActivity() : impl_(std::make_unique<Impl>()) {}
SearchActivity::~SearchActivity() { *impl_->alive = false; }

void SearchActivity::onContentAvailable() {
  this->grid->registerCell("Cell", []() {
    return RecyclingGridItemVideoCard::create();
  });

  // Y 键随时重新输入（与 wiliwili 搜索页一致的手感）
  this->registerAction("输入关键词", brls::ControllerButton::BUTTON_Y,
                       [this](brls::View*) {
                         this->prompt();
                         return true;
                       });

  this->prompt();
}

void SearchActivity::prompt() {
  brls::Application::getImeManager()->openForText(
      [this](const std::string& text) {
        if (!text.empty()) this->doSearch(text);
      },
      "搜索短剧", "输入剧名或关键词", 40, impl_->keyword, 0);
}

void SearchActivity::doSearch(const std::string& keyword) {
  impl_->keyword = keyword;
  if (this->status) this->status->setText("正在搜索「" + keyword + "」…");

  auto alive = impl_->alive;
  brls::async([this, alive, keyword] {
    std::string error;
    CatalogPage page;
    try {
      page = Core::instance().repo().search(keyword, 1, ContentType::Short);
    } catch (const std::exception& e) {
      error = e.what();
    }

    brls::sync([this, alive, page, error, keyword] {
      if (!*alive) return;

      if (!error.empty()) {
        if (this->status) this->status->setText("搜索失败");
        this->grid->setError(error);
        return;
      }
      if (page.items.empty()) {
        if (this->status) this->status->setText("没有找到「" + keyword + "」");
        this->grid->setEmpty("没有找到相关短剧");
        return;
      }
      if (this->status) {
        this->status->setText("「" + keyword + "」找到 " +
                              std::to_string(page.items.size()) + " 部");
      }
      auto* source = new SeriesSource([](const Series& s) { openSeries(s); });
      source->reset(std::move(page.items));
      impl_->source = source;
      this->grid->setDataSource(source);
      focusAfterLoad(this->grid);
    });
  });
}

// ================================================================== 分集列表

struct EpisodeActivity::Impl {
  Series series;
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
  explicit Impl(Series series) : series(std::move(series)) {}
};

EpisodeActivity::EpisodeActivity(Series series)
    : impl_(std::make_unique<Impl>(std::move(series))) {}
EpisodeActivity::~EpisodeActivity() { *impl_->alive = false; }

void EpisodeActivity::onContentAvailable() {
  this->list->addView(makeHeader(impl_->series.title));
  this->list->addView(makeNote("正在加载分集…"));

  auto alive = impl_->alive;
  const std::string seriesId = impl_->series.id;
  const std::string seriesTitle = impl_->series.title;

  brls::async([this, alive, seriesId, seriesTitle] {
    std::string error;
    std::vector<std::string> episodes;
    try {
      episodes = Core::instance().repo().detail(seriesId).episodes;
    } catch (const std::exception& e) {
      error = e.what();
    }

    brls::sync([this, alive, episodes, error, seriesTitle] {
      if (!*alive) return;
      this->list->clearViews();
      this->list->addView(makeHeader(seriesTitle));

      if (!error.empty()) {
        this->list->addView(makeNote("加载分集失败：\n" + error));
        return;
      }
      if (episodes.empty()) {
        this->list->addView(makeNote("这部剧没有分集"));
        return;
      }
      // 分集已经拿到了，直接传给播放页，省一次请求
      const Series series = impl_->series;
      for (size_t i = 0; i < episodes.size(); ++i) {
        const std::string title = "第 " + std::to_string(i + 1) + " 集";
        const size_t index = i;
        this->list->addView(makeCell(title, "", [series, episodes, index] {
          brls::Application::pushActivity(new PlayerActivity(series, episodes, index));
        }));
      }
      focusAfterLoad(this->list);
    });
  });
}

// ==================================================================== 播放页

struct PlayerActivity::Impl {
  Series series;
  std::vector<std::string> episodes;
  size_t index = 0;
  int rotation = 0;                                   // 0 / 90 / 180 / 270
  std::shared_ptr<bool> alive = std::make_shared<bool>(true);
  MPVEvent::Subscription eventSub = MPVEvent::Subscription();
  bool subscribed = false;

};

PlayerActivity::PlayerActivity(Series series, std::vector<std::string> episodes,
                               size_t startIndex)
    : impl_(std::make_unique<Impl>()) {
  impl_->series = std::move(series);
  impl_->episodes = std::move(episodes);
  impl_->index = startIndex;
}

PlayerActivity::~PlayerActivity() {
  *impl_->alive = false;
  if (impl_->subscribed) {
    MPV_E->unsubscribe(impl_->eventSub);   // 不注销的话，回调会在 activity 销毁后触发
  }
}

void PlayerActivity::willDisappear(bool resetState) {
  if (video) video->stop();            // 否则旧剧会继续响
  Core::instance().closeStream();      // 否则本地代理一直占着端口
  brls::Activity::willDisappear(resetState);
}

// 一集播完：还有下一集就接着放，否则提示
void PlayerActivity::onEpisodeEnded() {
  if (impl_->index + 1 < impl_->episodes.size()) {
    this->playIndex(impl_->index + 1);
    return;
  }
  if (status) status->setText(impl_->series.title + " · 已播完最后一集  (Y 换一部)");
  brls::Application::notify("已播完最后一集，按 Y 换一部");
}

void PlayerActivity::playIndex(size_t index) {
  if (index >= impl_->episodes.size()) return;
  impl_->index = index;

  const std::string episodeId = impl_->episodes[index];
  const std::string seriesTitle = impl_->series.title;
  const size_t shown = index + 1;

  if (status) {
    status->setText(seriesTitle + " · 第 " + std::to_string(shown) + " 集 · 解析中…");
  }
  if (video) video->showLoading();

  auto alive = impl_->alive;
  brls::async([this, alive, episodeId, seriesTitle, shown] {
    std::string error;
    std::string quality;
    std::string url;
    try {
      url = Core::instance().openStream(episodeId, quality);
    } catch (const std::exception& e) {
      error = e.what();
    }

    brls::sync([this, alive, url, quality, error, seriesTitle, shown] {
      if (!*alive) return;
      if (!error.empty()) {
        if (video) video->hideLoading();
        if (status) status->setText("播放失败：" + error);
        return;
      }
      brls::Logger::info("hongguo: 第{}集 {}", shown, url);
      if (status) {
        status->setText(seriesTitle + " · 第 " + std::to_string(shown) + " 集 ·  " +
                        quality + "   X 旋转   Y 换一部");
      }
      if (video) {
        video->hideLoading();
        video->setUrl(url);        // 换集就是换 URL，mpv 直接接上
        video->showOSD(true);
      }
    });
  });
}

// 载入某部剧并从头播（用于「换一部」）
void PlayerActivity::startSeries(Series series) {
  impl_->series = series;
  impl_->episodes.clear();
  impl_->index = 0;
  if (status) status->setText(series.title + " · 正在取分集…");
  if (video) video->showLoading();

  auto alive = impl_->alive;
  const std::string seriesId = series.id;
  const std::string seriesTitle = series.title;

  brls::async([this, alive, seriesId, seriesTitle] {
    std::string error;
    std::vector<std::string> episodes;
    try {
      episodes = Core::instance().repo().detail(seriesId).episodes;
    } catch (const std::exception& e) {
      error = e.what();
    }

    brls::sync([this, alive, episodes, error, seriesTitle] {
      if (!*alive) return;
      if (!error.empty()) {
        if (video) video->hideLoading();
        if (status) status->setText("取分集失败：" + error);
        return;
      }
      if (episodes.empty()) {
        if (video) video->hideLoading();
        if (status) status->setText("这部剧没有分集");
        return;
      }
      impl_->episodes = std::move(episodes);
      this->playIndex(0);
    });
  });
}

void PlayerActivity::onContentAvailable() {
  // X：旋转画面。短剧是 9:16，Switch 屏是 16:9，直着播左右必留大片黑边。
  this->registerAction("旋转画面", brls::ControllerButton::BUTTON_X,
                       [this](brls::View*) {
                         impl_->rotation = (impl_->rotation + 90) % 360;
                         MPVCore::instance().command_async(
                             "set", "video-rotate", std::to_string(impl_->rotation));
                         brls::Application::notify(
                             "画面旋转 " + std::to_string(impl_->rotation) + "°");
                         return true;
                       });

  // Y：随机换一部别的剧
  this->registerAction("换一部", brls::ControllerButton::BUTTON_Y,
                       [this](brls::View*) {
                         const Series next =
                             Core::instance().randomFromPool(impl_->series.id);
                         if (next.id.empty()) {
                           brls::Application::notify("没有可切换的剧");
                           return true;
                         }
                         brls::Application::notify("换一部：" + next.title);
                         this->startSeries(next);
                         return true;
                       });

  // 一集播完自动接下一集
  auto alive = impl_->alive;
  impl_->eventSub = MPV_E->subscribe([this, alive](MpvEventEnum event) {
    if (event != MpvEventEnum::END_OF_FILE) return;
    if (!*alive) return;
    this->onEpisodeEnded();
  });
  impl_->subscribed = true;

  if (impl_->episodes.empty()) {
    // 从首页直接点进来的情形：先取分集，再从第一集开始
    this->startSeries(impl_->series);
  } else {
    this->playIndex(impl_->index);
  }
}

}  // namespace hongguo::ui
