// SPDX-License-Identifier: GPL-3.0-only
//
// 移植自 hongguoTV 的 ContentRepository.kt。
// 这里只做"取数据 + 映射字段"，不做任何播放与解密 —— 那是 stream_reader 的事。
#include "hongguo/content_repo.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>

#include "hongguo/media_crypto.hpp"
#include "hongguo/vendor.hpp"

namespace hongguo {
namespace {

using Json = nlohmann::json;

constexpr int kMaxPage = 100;

std::string trim(const std::string& value) {
  const size_t begin = value.find_first_not_of(" \t\r\n");
  if (begin == std::string::npos) return {};
  const size_t end = value.find_last_not_of(" \t\r\n");
  return value.substr(begin, end - begin + 1);
}

// 对应 Kotlin 的 JSONObject.str(key)：null 视为空串，其余取字符串形式并 trim。
std::string jstr(const Json& object, const std::string& key) {
  if (!object.is_object()) return {};
  const auto it = object.find(key);
  if (it == object.end() || it->is_null()) return {};
  if (it->is_string()) return trim(it->get<std::string>());
  if (it->is_boolean()) return it->get<bool>() ? "true" : "false";
  if (it->is_number_integer()) return std::to_string(it->get<int64_t>());
  if (it->is_number_unsigned()) return std::to_string(it->get<uint64_t>());
  if (it->is_number_float()) {
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%g", it->get<double>());
    return trim(buffer);
  }
  return {};
}

int jint(const Json& object, const std::string& key, int fallback = 0) {
  if (!object.is_object()) return fallback;
  const auto it = object.find(key);
  if (it == object.end() || it->is_null()) return fallback;
  if (it->is_number_integer()) return static_cast<int>(it->get<int64_t>());
  if (it->is_number_unsigned()) return static_cast<int>(it->get<uint64_t>());
  if (it->is_number_float()) return static_cast<int>(it->get<double>());
  if (it->is_string()) {
    try {
      return std::stoi(it->get<std::string>());
    } catch (...) {
      return fallback;
    }
  }
  return fallback;
}

bool jbool(const Json& object, const std::string& key, bool fallback = false) {
  if (!object.is_object()) return fallback;
  const auto it = object.find(key);
  if (it == object.end() || it->is_null()) return fallback;
  if (it->is_boolean()) return it->get<bool>();
  return fallback;
}

const Json* jobj(const Json& object, const std::string& key) {
  if (!object.is_object()) return nullptr;
  const auto it = object.find(key);
  if (it == object.end() || !it->is_object()) return nullptr;
  return &(*it);
}

const Json* jarr(const Json& object, const std::string& key) {
  if (!object.is_object()) return nullptr;
  const auto it = object.find(key);
  if (it == object.end() || !it->is_array()) return nullptr;
  return &(*it);
}

// 对应 Kotlin 的 cleanUrl：还原被转义的斜杠与 & 。
std::string cleanUrl(std::string value) {
  struct Replacement {
    const char* from;
    const char* to;
    bool ignoreCase;
  };
  static const Replacement kRules[] = {
      {"\\/", "/", false}, {"\\u002F", "/", true},
      {"\\u0026", "&", false}, {"&amp;", "&", false}};
  for (const auto& rule : kRules) {
    const std::string from(rule.from);
    const std::string to(rule.to);
    if (!rule.ignoreCase) {
      size_t at = 0;
      while ((at = value.find(from, at)) != std::string::npos) {
        value.replace(at, from.size(), to);
        at += to.size();
      }
      continue;
    }
    // 大小写不敏感替换
    std::string lower = value;
    for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    std::string lowerFrom = from;
    for (char& ch : lowerFrom) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    size_t at = 0;
    while ((at = lower.find(lowerFrom, at)) != std::string::npos) {
      value.replace(at, from.size(), to);
      lower.replace(at, from.size(), to);
      at += to.size();
    }
  }
  return value;
}

bool isSeriesId(const std::string& id) {
  if (id.empty() || id.size() > 30) return false;
  for (const char ch : id) {
    if (ch < '0' || ch > '9') return false;
  }
  return true;
}

const Json& videoData(const Json& item) {
  const Json* nested = jobj(item, "video_data");
  return nested ? *nested : item;
}

// 对应 Kotlin 的 card()
Series card(const Json& item) {
  const Json& source = videoData(item);
  const int count = [&] {
    const int direct = jint(source, "episode_cnt", -1);
    if (direct >= 0) return direct;
    const Json* info = jobj(source, "series_episode_info");
    return info ? jint(*info, "episode_cnt", 0) : 0;
  }();

  const Json* tags = jarr(source, "tags");
  if (!tags) tags = jarr(source, "category_list");

  std::string title = jstr(source, "series_name");
  if (title.empty()) {
    title = jstr(source, "series_title");
    if (title.empty()) title = jstr(item, "name");
  }

  std::string badge = jstr(source, "episode_right_text");
  if (badge.empty() && count > 0) badge = "全 " + std::to_string(count) + " 集";

  std::string tagText;
  if (tags) {
    const size_t take = std::min<size_t>(tags->size(), 5);
    for (size_t i = 0; i < take; ++i) {
      if (i != 0) tagText += " · ";
      const Json& tag = (*tags)[i];
      if (tag.is_object()) {
        tagText += jstr(tag, "name");
      } else if (tag.is_string()) {
        tagText += trim(tag.get<std::string>());
      }
    }
  }

  Series out;
  out.id = jstr(source, "series_id");
  out.title = title;
  out.cover = cleanUrl(jstr(source, "series_cover"));
  out.description = jstr(source, "series_intro");
  out.badge = badge;
  out.tags = tagText;
  return out;
}

// 去掉 id 非法或标题为空的条目，并按 id 去重。
std::vector<Series> valid(const std::vector<Series>& rows) {
  std::vector<Series> out;
  std::set<std::string> seen;
  for (const Series& row : rows) {
    if (!isSeriesId(row.id) || trim(row.title).empty()) continue;
    if (!seen.insert(row.id).second) continue;
    out.push_back(row);
  }
  return out;
}

// 设备指纹 → 有序请求参数
QueryValues deviceValues() {
  QueryValues out;
  out.reserve(vendor::kDeviceCount);
  for (size_t i = 0; i < vendor::kDeviceCount; ++i) {
    out.emplace_back(std::string(vendor::kDevice[i].key),
                     std::string(vendor::kDevice[i].value));
  }
  return out;
}

// 有序列值对：已存在的键就地覆盖，新键追加 —— 与 Kotlin 的 LinkedHashMap 语义一致。
class OrderedValues {
 public:
  void set(const std::string& key, const std::string& value) {
    for (auto& item : items_) {
      if (item.first == key) {
        item.second = value;
        return;
      }
    }
    items_.emplace_back(key, value);
  }
  const QueryValues& items() const { return items_; }

 private:
  QueryValues items_;
};

// 合并请求头：后者按名覆盖前者（大小写不敏感），对应 OkHttp 的 .header() 语义。
HeaderList mergeHeaders(const HeaderList& base, const HeaderList& override) {
  auto same = [](const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      if (std::tolower(static_cast<unsigned char>(a[i])) !=
          std::tolower(static_cast<unsigned char>(b[i]))) {
        return false;
      }
    }
    return true;
  };
  HeaderList out;
  for (const auto& item : base) {
    bool shadowed = false;
    for (const auto& other : override) {
      if (same(item.first, other.first)) {
        shadowed = true;
        break;
      }
    }
    if (!shadowed) out.push_back(item);
  }
  for (const auto& item : override) out.push_back(item);
  return out;
}

// 读取 HTML 属性值中的实体引用
std::string decodeEntities(std::string value) {
  struct Entity {
    const char* from;
    const char* to;
  };
  static const Entity kEntities[] = {{"&quot;", "\""}, {"&#34;", "\""}, {"&apos;", "'"},
                                     {"&#39;", "'"},  {"&lt;", "<"},   {"&gt;", ">"},
                                     {"&amp;", "&"}};
  for (const auto& entity : kEntities) {
    const std::string from(entity.from);
    const std::string to(entity.to);
    size_t at = 0;
    while ((at = value.find(from, at)) != std::string::npos) {
      value.replace(at, from.size(), to);
      at += to.size();
    }
  }
  return value;
}

// 从 HTML 里取出所有 <script> 标签的属性表。
// 只做属性扫描，不建 DOM —— 我们从不执行页面脚本，只需读它的 JSON 载荷。
struct ScriptTag {
  std::map<std::string, std::string> attrs;
};

std::vector<ScriptTag> scanScriptTags(const std::string& html) {
  std::vector<ScriptTag> out;
  size_t at = 0;
  while (true) {
    const size_t open = html.find("<script", at);
    if (open == std::string::npos) break;
    const size_t tagEnd = html.find('>', open);
    if (tagEnd == std::string::npos) break;
    const std::string tag = html.substr(open, tagEnd - open);

    ScriptTag script;
    size_t cursor = 7;  // 跳过 "<script"
    while (cursor < tag.size()) {
      while (cursor < tag.size() &&
             (std::isspace(static_cast<unsigned char>(tag[cursor])) || tag[cursor] == '/')) {
        ++cursor;
      }
      const size_t nameStart = cursor;
      while (cursor < tag.size() && tag[cursor] != '=' && tag[cursor] != '>' &&
             !std::isspace(static_cast<unsigned char>(tag[cursor]))) {
        ++cursor;
      }
      if (cursor == nameStart) break;
      const std::string name = tag.substr(nameStart, cursor - nameStart);
      while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor]))) ++cursor;
      if (cursor >= tag.size() || tag[cursor] != '=') {
        script.attrs[name] = "";
        continue;
      }
      ++cursor;
      while (cursor < tag.size() && std::isspace(static_cast<unsigned char>(tag[cursor]))) ++cursor;
      if (cursor >= tag.size()) break;
      const char quote = tag[cursor];
      std::string value;
      if (quote == '"' || quote == '\'') {
        ++cursor;
        const size_t close = tag.find(quote, cursor);
        value = tag.substr(cursor, close == std::string::npos ? std::string::npos : close - cursor);
        cursor = close == std::string::npos ? tag.size() : close + 1;
      } else {
        const size_t close = tag.find_first_of(" \t\r\n>", cursor);
        value = tag.substr(cursor, close == std::string::npos ? std::string::npos : close - cursor);
        cursor = close == std::string::npos ? tag.size() : close;
      }
      script.attrs[name] = decodeEntities(value);
    }
    out.push_back(std::move(script));
    at = tagEnd;
  }
  return out;
}

// ---------------------------------------------------------- 排行榜 HTML 解析
//
// 榜单页的数据**不在 JSON 里**：loaderData 的 content 是空的，
// 真正的内容由客户端脚本解析，服务端只渲染成 HTML。
// 而且脚本标签里的 routerDataFnName 两个榜还不一样（真人榜 "p"、漫剧榜 "s"），
// 靠它判断并不可靠。所以直接读渲染好的 <article>。
//
// 元素上的 class 名带随机哈希后缀（pc-title-LHxn_J），因此一律按前缀匹配。

std::string stripTags(const std::string& html) {
  std::string out;
  bool inTag = false;
  for (const char ch : html) {
    if (ch == '<') {
      inTag = true;
    } else if (ch == '>') {
      inTag = false;
    } else if (!inTag) {
      out.push_back(ch);
    }
  }
  return trim(out);
}

// 取第一个匹配 <tag ...class="prefix...">TEXT</tag> 里的 TEXT
std::string textByClass(const std::string& html, const std::string& tag,
                        const std::string& classPrefix) {
  const std::regex pattern("<" + tag + "[^>]*class=\"" + classPrefix + "[^\"]*\"[^>]*>(.*?)</" +
                           tag + ">");
  std::smatch match;
  if (std::regex_search(html, match, pattern)) return stripTags(match[1].str());
  return {};
}

std::string decodeBasicEntities(std::string value) {
  struct Entity { const char* from; const char* to; };
  static const Entity kEntities[] = {{"&amp;", "&"}, {"&lt;", "<"}, {"&gt;", ">"},
                                     {"&quot;", "\""}, {"&#39;", "'"}, {"&apos;", "'"}};
  for (const auto& entity : kEntities) {
    const std::string from(entity.from);
    const std::string to(entity.to);
    size_t at = 0;
    while ((at = value.find(from, at)) != std::string::npos) {
      value.replace(at, from.size(), to);
      at += to.size();
    }
  }
  return value;
}

// 从一段 <article> 里抠出一部剧
bool parseRankArticle(const std::string& segment, Series& out, RankPosition& position) {
  static const std::regex kId("aria-labelledby=\"rank-title-(\\d+)\"");
  std::smatch match;
  if (!std::regex_search(segment, match, kId)) return false;
  out.id = match[1].str();

  // 标题：<h2 id="rank-title-N" class="pc-title-...">标题</h2>
  const std::regex kTitle("id=\"rank-title-" + out.id + "\"[^>]*>(.*?)</h2>");
  if (std::regex_search(segment, match, kTitle)) {
    out.title = decodeBasicEntities(stripTags(match[1].str()));
  }
  if (out.title.empty()) return false;

  // 封面：<img class="pc-cover-..." src="...">
  const std::regex kCover("<img[^>]*class=\"pc-cover-[^\"]*\"[^>]*src=\"([^\"]+)\"");
  if (std::regex_search(segment, match, kCover)) out.cover = match[1].str();
  if (out.cover.empty()) {
    const std::regex kAnyImg("<img[^>]*src=\"(https://[^\"]+novel-pic[^\"]*)\"");
    if (std::regex_search(segment, match, kAnyImg)) out.cover = match[1].str();
  }

  // 热度：<p class="pc-metrics-...">...</p>（内含 svg，需要剥标签）
  out.badge = textByClass(segment, "p", "pc-metrics-");

  // 名次：<span class="pc-badge-number-...">1</span>
  const std::regex kRank("class=\"pc-badge-number-[^\"]*\">(\\d+)<");
  if (std::regex_search(segment, match, kRank)) {
    position.rank = std::stoi(match[1].str());
  }
  position.heat = out.badge;

  // 题材：<p class="pc-categories-..."><span>爱情</span>...</p>
  const std::string categories = textByClass(segment, "p", "pc-categories-");
  // textByClass 会把多个 <span> 连在一起，用 · 分隔更好读
  out.tags = categories;

  if (out.badge.empty() && position.rank) {
    out.badge = "No." + std::to_string(*position.rank);
  }
  return true;
}

}  // namespace

std::string toString(ContentType type) {
  return type == ContentType::Comic ? "comic" : "short";
}

std::string toSlug(Category category) {
  switch (category) {
    case Category::ComicDrama: return "comic-drama";
    case Category::AiDrama:    return "ai-drama";
    case Category::RealDrama:
    default:                   return "real-drama";
  }
}

std::string toLabel(Category category) {
  switch (category) {
    case Category::ComicDrama: return "漫剧";
    case Category::AiDrama:    return "AI 剧";
    case Category::RealDrama:
    default:                   return "短剧";
  }
}

bool hasRanking(Category category) { return category != Category::AiDrama; }

// ------------------------------------------------------ _ROUTER_DATA 抠取

std::string ContentRepo::extractRouterJson(const std::string& html) {
  const size_t marker = html.find("_ROUTER_DATA");
  if (marker == std::string::npos) throw std::runtime_error("页面没有内容数据");
  const size_t first = html.find('{', marker);
  if (first == std::string::npos) throw std::runtime_error("页面数据格式错误");

  int depth = 0;
  bool quoted = false;
  bool escaped = false;
  for (size_t i = first; i < html.size(); ++i) {
    const char ch = html[i];
    if (quoted) {
      if (escaped) {
        escaped = false;
      } else if (ch == '\\') {
        escaped = true;
      } else if (ch == '"') {
        quoted = false;
      }
      continue;
    }
    if (ch == '"') {
      quoted = true;
    } else if (ch == '{' || ch == '[') {
      ++depth;
    } else if (ch == '}' || ch == ']') {
      --depth;
      if (depth == 0) return html.substr(first, i - first + 1);
    }
  }
  throw std::runtime_error("页面数据不完整");
}

// ---------------------------------------------------------------- 清晰度

int ContentRepo::qualityNumber(const std::string& value) {
  static const std::regex kLabel("2160|1440|1080|720|576|540|480|360");
  std::smatch match;
  if (std::regex_search(value, match, kLabel)) {
    return std::stoi(match.str());
  }
  static const std::map<std::string, int> kByWidth = {
      {"1920", 1080}, {"1280", 720}, {"1024", 576}, {"854", 480}, {"640", 360}};
  const auto it = kByWidth.find(value);
  if (it != kByWidth.end()) return it->second;
  if (value.size() >= 3 && value.size() <= 4 &&
      value.find_first_not_of("0123456789") == std::string::npos) {
    return std::stoi(value);
  }
  return 0;
}

// ------------------------------------------------------------------ 构造

ContentRepo::ContentRepo(Http& http, Signer signer)
    : http_(http), signer_(std::move(signer)) {}

// ------------------------------------------------------------------ 取文本

std::string ContentRepo::text(const std::string& url, const Signer::Request* signedRequest) {
  const HeaderList base = {
      {"User-Agent", std::string(vendor::kVideoUserAgent)},
      {"Accept-Language", "zh-CN,zh;q=0.9"}};

  HeaderList headers = base;
  std::optional<Bytes> body;
  if (signedRequest) {
    headers = mergeHeaders(base, signedRequest->headers);
    body = signedRequest->body;
  }

  const HttpResponse response = body ? http_.post(url, headers, *body) : http_.get(url, headers);
  if (response.status != 200) {
    throw std::runtime_error("内容请求失败（HTTP " + std::to_string(response.status) + "）");
  }
  if (response.body.size() > 16u * 1024u * 1024u) {
    throw std::runtime_error("内容响应过大");
  }
  return std::string(response.body.begin(), response.body.end());
}

// ---------------------------------------------------------------- 首页

// 分类页与首页用的是同一套数据结构，解析集中在这里。
CatalogPage ContentRepo::parseCategorySection(int page, const std::string& url) {
  const Json loader = Json::parse(extractRouterJson(text(url, nullptr)))["loaderData"];
  const Json* section = jobj(loader, "category_page");
  if (!section) section = jobj(loader, "category_$");
  if (!section) throw std::runtime_error("首页数据结构已变化");

  const Json* rows = jarr(*section, "recommendList");
  if (!rows) {
    const Json* categoryData = jobj(*section, "categoryData");
    if (categoryData) rows = jarr(*categoryData, "recommendList");
  }
  if (!rows) throw std::runtime_error("首页数据暂不可用");

  std::vector<Series> items;
  items.reserve(rows->size());
  for (const Json& row : *rows) items.push_back(card(row));
  items = valid(items);

  const Json* pagination = jobj(*section, "pagination");
  const int lastPage = pagination ? jint(*pagination, "totalPages", 100) : 100;

  CatalogPage out;
  out.items = std::move(items);
  out.hasMore = !out.items.empty() && page < std::min(lastPage, kMaxPage);
  return out;
}

CatalogPage ContentRepo::home(int page, ContentType type) {
  if (page < 1 || page > kMaxPage) throw std::runtime_error("页码超出范围");
  if (type == ContentType::Comic) return comicRanking(page);

  // 旧入口重定向到 /category/real-drama，这里直接用规范路径
  std::string url = std::string(vendor::kSiteBase) + "/category/real-drama";
  if (page > 1) url += "?page=" + std::to_string(page);
  return parseCategorySection(page, url);
}

CatalogPage ContentRepo::browse(int page, Category category) {
  if (page < 1 || page > kMaxPage) throw std::runtime_error("页码超出范围");
  std::string url = std::string(vendor::kSiteBase) + "/category/" + toSlug(category);
  if (page > 1) url += "?page=" + std::to_string(page);
  return parseCategorySection(page, url);
}

CatalogPage ContentRepo::ranking(int page, Category category) {
  if (page < 1 || page > kMaxPage) throw std::runtime_error("页码超出范围");
  if (!hasRanking(category)) throw std::runtime_error("该分类没有排行榜");
  const std::string slug = toSlug(category);
  const std::string base = std::string(vendor::kSiteBase) + "/rank/hot-" + slug;
  return parseRanking(page, base, "rank_hot-" + slug + "/page");
}

// ---------------------------------------------------------------- 搜索

CatalogPage ContentRepo::search(const std::string& keyword, int page, ContentType type) {
  if (keyword.empty() || keyword.size() > 80 || page < 1 || page > kMaxPage) {
    throw std::runtime_error("搜索参数不合法");
  }
  // 官网搜索不区分内容类型，漫剧一律走 App 接口
  if (type == ContentType::Comic) return appSearch(keyword, page, type);

  try {
    const CatalogPage result = appSearch(keyword, page, type);
    if (!result.items.empty() || !result.hasMore) return result;
  } catch (const SearchSessionExpired&) {
    throw;
  } catch (...) {
    // 回落到官网搜索；下方若仍失败会抛错，不会伪装成"空结果"
  }

  const std::string url = std::string(vendor::kSiteBase) + "/search/" +
                          encodeQueryValue(keyword) + "?page=" + std::to_string(page);
  const Json loader = Json::parse(extractRouterJson(text(url, nullptr)))["loaderData"];
  const Json* section = jobj(loader, "search_(keyword)/page");
  if (!section) section = jobj(loader, "search_page");
  if (!section) throw std::runtime_error("搜索数据暂不可用");

  const Json* rows = jarr(*section, "searchList");
  if (!rows) throw std::runtime_error("搜索数据结构已变化");

  std::vector<Series> items;
  items.reserve(rows->size());
  for (const Json& row : *rows) items.push_back(card(row));
  items = valid(items);

  CatalogPage out;
  out.items = std::move(items);
  out.hasMore = !out.items.empty() && page < kMaxPage;
  return out;
}

CatalogPage ContentRepo::appSearch(const std::string& keyword, int page, ContentType type) {
  OrderedValues values;
  for (const auto& item : deviceValues()) values.set(item.first, item.second);
  for (const char* key : {"version_code", "manifest_version_code",
                          "update_version_code", "pv_player"}) {
    values.set(key, "72232");
  }
  values.set("version_name", "7.2.2.32");
  values.set("query", keyword);
  values.set("count", "20");
  values.set("offset", std::to_string((page - 1) * 20));
  values.set("tab_type", std::to_string(type == ContentType::Comic ? 19 : 11));
  values.set("bookshelf_search_plan", "4");
  values.set("use_correct", "true");
  values.set("user_is_login", "0");

  // 游标键：类型 + 关键词 + 上一页。漫剧没有游标就无法翻页（上游语义如此）。
  const std::string cursorKey =
      toString(type) + "\x1f" + keyword + "\x1f" + std::to_string(page - 1);
  const auto found = searchCursors_.find(cursorKey);
  if (type == ContentType::Comic && page > 1 && found == searchCursors_.end()) {
    throw SearchSessionExpired();
  }
  if (found != searchCursors_.end()) {
    if (!found->second.searchId.empty()) values.set("search_id", found->second.searchId);
    if (!found->second.passback.empty()) values.set("passback", found->second.passback);
    if (found->second.hasOffset && found->second.offset >= 0) {
      values.set("offset", std::to_string(found->second.offset));
    }
  }

  const Signer::Request signedRequest =
      signer_.sign(std::string(vendor::kAppSearchApi), values.items());
  const Json data = Json::parse(text(signedRequest.url, &signedRequest));
  if (jint(data, "code", -1) != 0) throw std::runtime_error("搜索接口暂不可用");

  const Json* tabs = jarr(data, "search_tabs");
  if (!tabs) {
    const Json* inner = jobj(data, "data");
    if (inner) tabs = jarr(*inner, "search_tabs");
  }
  if (!tabs) throw std::runtime_error("搜索格式异常");

  const int wantTab = type == ContentType::Comic ? 19 : 11;
  const Json* tab = nullptr;
  for (const Json& candidate : *tabs) {
    if (jint(candidate, "tab_type", -1) == wantTab) {
      tab = &candidate;
      break;
    }
  }
  if (!tab) {
    throw std::runtime_error(toString(type) == "comic" ? "漫剧搜索暂不可用" : "短剧搜索暂不可用");
  }

  std::vector<Series> list;
  const Json* cells = jarr(*tab, "data");
  if (cells) {
    for (const Json& cell : *cells) {
      std::vector<const Json*> rows = {&cell};
      // lynx_data 里可能还嵌着一层 cell_data
      const std::string lynx = jstr(cell, "lynx_data");
      if (!lynx.empty()) {
        try {
          const Json nested = Json::parse(lynx);
          if (const Json* inner = jarr(nested, "cell_data")) {
            for (const Json& row : *inner) rows.push_back(&row);
          }
        } catch (...) {
          // lynx_data 不是合法 JSON 是常态，忽略即可
        }
      }
      for (const Json* row : rows) {
        const Json* videos = jarr(*row, "video_data");
        if (!videos || videos->empty()) continue;
        const Json& video = (*videos)[0];

        std::string id = jstr(video, "series_id");
        if (id.empty()) id = jstr(*row, "book_id");
        if (id.empty()) id = jstr(*row, "search_result_id");

        const Json* highlight = jobj(*row, "search_high_light");
        const Json* highlightTitle = highlight ? jobj(*highlight, "title") : nullptr;
        std::string title = highlightTitle ? jstr(*highlightTitle, "text") : "";
        if (title.empty()) title = jstr(video, "title");
        if (title.empty()) title = jstr(*row, "cell_name");

        const int count = jint(video, "episode_cnt", 0);
        std::string cover = jstr(video, "cover");
        if (cover.empty()) cover = jstr(video, "cover_url");

        Series series;
        series.id = id;
        series.title = title;
        series.cover = cleanUrl(cover);
        series.description = jstr(video, "video_desc");
        series.badge = count > 0 ? "全 " + std::to_string(count) + " 集"
                                 : jstr(video, "rec_text");
        series.tags = jstr(video, "sub_title");
        list.push_back(std::move(series));
      }
    }
  }

  SearchCursor next;
  next.searchId = jstr(*tab, "search_id");
  next.passback = jstr(*tab, "passback");
  const std::string nextOffset = jstr(*tab, "next_offset");
  if (!nextOffset.empty()) {
    try {
      next.offset = std::stoi(nextOffset);
      next.hasOffset = true;
    } catch (...) {
      next.hasOffset = false;
    }
  }
  searchCursors_[toString(type) + "\x1f" + keyword + "\x1f" + std::to_string(page)] = next;
  while (searchCursors_.size() > 100) searchCursors_.erase(searchCursors_.begin());

  CatalogPage out;
  out.items = valid(list);
  out.hasMore = jbool(*tab, "has_more", false) && page < kMaxPage;
  return out;
}

// ---------------------------------------------------------------- 详情

Detail ContentRepo::detail(const std::string& seriesId) {
  if (!isSeriesId(seriesId)) throw std::runtime_error("剧集 ID 不合法");

  const std::string url =
      std::string(vendor::kSiteBase) + "/detail?series_id=" + seriesId;
  const Json loader = Json::parse(extractRouterJson(text(url, nullptr)))["loaderData"];
  const Json* detailPage = jobj(loader, "detail_page");
  const Json* series = detailPage ? jobj(*detailPage, "seriesDetail") : nullptr;
  if (!series) throw std::runtime_error("剧集详情暂不可用");

  const Json* raw = jarr(*series, "vid_list");
  if (!raw) throw std::runtime_error("分集暂不可用");

  std::vector<std::string> episodes;
  episodes.reserve(raw->size());
  for (const Json& entry : *raw) {
    episodes.push_back(entry.is_string() ? entry.get<std::string>() : jstr(entry, "vid"));
  }
  if (episodes.empty()) throw std::runtime_error("分集数据异常");
  std::set<std::string> seen;
  for (const std::string& episode : episodes) {
    if (!isSeriesId(episode) || !seen.insert(episode).second) {
      throw std::runtime_error("分集数据异常");
    }
  }

  Detail out;
  out.series = card(*series);
  out.series.id = seriesId;
  out.episodes = std::move(episodes);
  return out;
}

// ---------------------------------------------------------------- 播放解析

StreamInfo ContentRepo::stream(const std::string& episodeId, int maxQuality) {
  if (!isSeriesId(episodeId)) throw std::runtime_error("分集 ID 不合法");

  const std::string payload =
      "{\"biz_param\":{\"detail_page_version\":0,\"device_level\":3,"
      "\"disable_digg_stat\":false,\"need_all_video_definition\":true,"
      "\"need_mp4_align\":false,\"use_os_player\":false,\"use_server_dns\":false,"
      "\"video_platform\":1024},\"mixed_video_id_map\":{\"1004\":[\"" +
      episodeId + "\"]}}";

  const Bytes body(payload.begin(), payload.end());
  const Signer::Request signedRequest =
      signer_.sign(std::string(vendor::kVideoApi), deviceValues(), body);
  const Json data = Json::parse(text(signedRequest.url, &signedRequest));
  const int code = jint(data, "Code", jint(data, "code", 0));
  if (code != 0) {
    // 把上游的原始错误带出来。"播放接口暂不可用"这种笼统说法在排查时毫无用处 ——
    // 上游报 110001（未知异常）还是参数错误，处理方式完全不同。
    std::string message = jstr(data, "Message");
    if (message.empty()) message = jstr(data, "message");
    if (message.empty()) {
      const Json* base = jobj(data, "BaseResp");
      if (base) message = jstr(*base, "StatusMessage");
    }
    throw std::runtime_error("播放接口返回错误 code=" + std::to_string(code) +
                             (message.empty() ? "" : "（" + message + "）"));
  }

  const Json* map = jobj(data, "data");
  if (!map) throw std::runtime_error("播放信息为空");

  const Json* entry = jobj(*map, episodeId);
  if (!entry) {
    for (auto it = map->begin(); it != map->end() && !entry; ++it) {
      if (it->is_object()) entry = &(*it);
    }
  }
  if (!entry) throw std::runtime_error("播放信息为空");

  Json model;
  const Json* direct = jobj(*entry, "video_model");
  if (direct) {
    model = *direct;
  } else {
    const std::string text = jstr(*entry, "video_model");
    if (text.empty()) throw std::runtime_error("播放信息为空");
    model = Json::parse(text);
  }

  std::string fallback;
  const auto raw = model.find("fallback_api");
  if (raw != model.end()) {
    if (raw->is_object()) {
      fallback = jstr(*raw, "fallback_api");
    } else if (raw->is_array() && !raw->empty()) {
      fallback = (*raw)[0].is_string() ? (*raw)[0].get<std::string>() : "";
    } else if (raw->is_string()) {
      const std::string text = raw->get<std::string>();
      try {
        fallback = jstr(Json::parse(text), "fallback_api");
      } catch (...) {
        fallback = text;
      }
    }
  }
  if (fallback.rfind("https://", 0) != 0) throw std::runtime_error("播放地址格式异常");

  const Json infoRoot = Json::parse(text(fallback, nullptr));
  const Json* videoInfo = jobj(infoRoot, "video_info");
  const Json* info = videoInfo ? jobj(*videoInfo, "data") : nullptr;
  if (!info) throw std::runtime_error("没有播放资源");

  const Json* videoList = jobj(*info, "video_list");
  if (!videoList) throw std::runtime_error("没有可用清晰度");

  struct Candidate {
    int quality = 0;
    std::string key;
    const Json* item = nullptr;
  };
  std::vector<Candidate> rows;
  for (auto it = videoList->begin(); it != videoList->end(); ++it) {
    if (!it->is_object()) continue;
    const std::string mainUrl = jstr(*it, "main_url");
    if (mainUrl.empty()) continue;
    const std::string codec = [&] {
      std::string value = jstr(*it, "codec_type");
      for (char& ch : value) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      return value;
    }();
    // ByteVC2 资源在标准 MP4 解复用器下只有音频没有视频轨，必须排除
    if (codec == "bytevc2" || codec == "bvc2") continue;

    std::string label = jstr(*it, "quality_desc");
    if (label.empty()) label = jstr(*it, "height");
    if (label.empty()) label = jstr(*it, "vheight");
    if (label.empty()) label = jstr(*it, "quality");
    if (label.empty()) label = it.key();
    rows.push_back(Candidate{qualityNumber(label), it.key(), &(*it)});
  }
  // 清晰度降序；同档时按键名升序，保证结果可复现
  std::sort(rows.begin(), rows.end(), [](const Candidate& a, const Candidate& b) {
    if (a.quality != b.quality) return a.quality > b.quality;
    return a.key < b.key;
  });

  const Candidate* selected = nullptr;
  for (const Candidate& candidate : rows) {
    if (candidate.quality > 0 && candidate.quality <= maxQuality) {
      selected = &candidate;
      break;
    }
  }
  if (!selected && !rows.empty()) selected = &rows.back();
  if (!selected) throw std::runtime_error("没有可播放的清晰度，请稍后再试或选择其他剧集");

  std::string url = cleanUrl(jstr(*selected->item, "main_url"));
  const std::string seedText = jstr(*info, "key_seed");
  if (!seedText.empty()) {
    url = media_crypto::decryptUrl(url, fromBase64(seedText));
  }
  if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) {
    throw std::runtime_error("播放地址无效");
  }

  StreamInfo out;
  out.url = std::move(url);
  const std::string spade = jstr(*selected->item, "spade_a");
  if (!spade.empty()) out.key = media_crypto::deriveKey(spade);
  if (selected->quality > 0) {
    out.quality = std::to_string(selected->quality) + "P";
    if (selected->quality > maxQuality) out.quality += "（兼容资源）";
  } else {
    out.quality = "自动";
  }
  return out;
}

// ---------------------------------------------------------------- 漫剧榜单

CatalogPage ContentRepo::comicRanking(int page) {
  return parseRanking(page, std::string(vendor::kSiteBase) + "/rank/hot-comic-drama",
                      "rank_hot-comic-drama/page");
}

CatalogPage ContentRepo::parseRanking(int page, const std::string& baseUrl,
                                      const std::string& rankKey) {
  (void)rankKey;  // 解析走渲染版 HTML，不再依赖脚本标签里的 key
  const std::string url = baseUrl + "?page=" + std::to_string(page);
  const std::string html = text(url, nullptr);

  std::vector<Series> items;
  std::map<std::string, RankPosition> positions;

  // 按 <article 切段，逐段解析
  static const std::string kOpen = "<article";
  static const std::string kClose = "</article>";
  size_t cursor = 0;
  while (true) {
    const size_t begin = html.find(kOpen, cursor);
    if (begin == std::string::npos) break;
    const size_t finish = html.find(kClose, begin);
    if (finish == std::string::npos) break;
    cursor = finish + kClose.size();

    Series series;
    RankPosition position;
    if (!parseRankArticle(html.substr(begin, cursor - begin), series, position)) continue;
    if (!positions.count(series.id)) positions[series.id] = position;
    items.push_back(std::move(series));
  }

  items = valid(items);
  if (items.empty()) {
    throw std::runtime_error("榜单页没有解析出任何条目（页面结构可能已变化）");
  }

  // 只保留仍然存在的剧目的名次
  std::map<std::string, RankPosition> kept;
  for (const auto& entry : positions) {
    for (const Series& series : items) {
      if (series.id == entry.first) {
        kept.insert(entry);
        break;
      }
    }
  }

  // 榜单页固定一屏，没有翻页
  CatalogPage out;
  out.items = std::move(items);
  out.hasMore = false;
  out.ranking = ComicRanking{std::move(kept), 1, ""};
  return out;
}

}  // namespace hongguo
