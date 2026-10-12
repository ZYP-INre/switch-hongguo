// 红果短剧客户端实现 —— 免签名网页接口部分
// 逻辑参照：hongguoTV ContentRepository.kt（GPL-3.0）+ probe-hongguo.mjs（已真机验证）
// TODO(M3): AppSearch / AppPlayUrl 的签名链（Signer.kt 移植）
#include <nlohmann/json.hpp>
#include <cpr/cpr.h>
#include "hongguo.h"
#include "api.h"

namespace hongguo {

static const char* kUA =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
    "(KHTML, like Gecko) Chrome/126.0 Safari/537.36";

// ---------- 内部工具 ----------

std::string HongguoClient::cleanUrl(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '\\' && i + 1 < s.size()) {
            char n = s[i + 1];
            if (n == '/') { out += '/'; i++; continue; }
            if (n == 'u' && i + 5 < s.size()) {
                std::string hex = s.substr(i + 2, 4);
                // \u002F -> /  \u0026 -> &
                if (hex == "002F" || hex == "002f") { out += '/'; i += 5; continue; }
                if (hex == "0026") { out += '&'; i += 5; continue; }
            }
        }
        out += s[i];
    }
    // &amp; -> &
    size_t pos;
    while ((pos = out.find("&amp;")) != std::string::npos) out.replace(pos, 5, "&");
    return out;
}

std::string HongguoClient::extractRouterData(const std::string& html) {
    static const std::string kMarker = "_ROUTER_DATA";
    size_t marker = html.find(kMarker);
    if (marker == std::string::npos) throw std::runtime_error("页面缺少内容数据");
    size_t first = html.find('{', marker);
    if (first == std::string::npos) throw std::runtime_error("页面数据格式错误");

    int depth = 0;
    bool inQuote = false, escaped = false;
    for (size_t i = first; i < html.size(); i++) {
        char ch = html[i];
        if (inQuote) {
            if (escaped) escaped = false;
            else if (ch == '\\') escaped = true;
            else if (ch == '"') inQuote = false;
            continue;
        }
        if (ch == '"') inQuote = true;
        else if (ch == '{' || ch == '[') depth++;
        else if (ch == '}' || ch == ']') {
            if (--depth == 0) return html.substr(first, i - first + 1);
        }
    }
    throw std::runtime_error("页面数据不完整");
}

static void loadRouter(const std::string& url, nlohmann::json& data) {
    cpr::Response r = cpr::Get(cpr::Url{url}, cpr::Header{{"User-Agent", kUA}, {"Accept-Language", "zh-CN,zh;q=0.9"}});
    if (r.error) throw std::runtime_error("网络错误: " + r.error.message);
    if (r.status_code != 200) throw std::runtime_error("HTTP " + std::to_string(r.status_code));
    data = nlohmann::json::parse(HongguoClient::extractRouterData(r.text));
}

/// loaderData 结构兼容两种键名（category_page / category_$）
static const nlohmann::json* sectionOf(const nlohmann::json& data) {
    if (data.contains("loaderData")) {
        const auto& ld = data.at("loaderData");
        for (const char* key : {"category_page", "category_$"}) {
            if (ld.contains(key)) return &ld.at(key);
        }
    }
    return nullptr;
}

static std::string s(const nlohmann::json& j, const char* key) {
    if (j.is_object() && j.contains(key) && j.at(key).is_string()) return j.at(key).get<std::string>();
    return "";
}

static std::string tagText(const nlohmann::json& j) {
    const nlohmann::json* arr = nullptr;
    for (const char* key : {"tags", "category_list"}) {
        if (j.contains(key) && j.at(key).is_array()) { arr = &j.at(key); break; }
    }
    if (!arr) return "";
    std::string out;
    for (size_t i = 0; i < arr->size() && i < 5; i++) {
        std::string name = (*arr)[i].is_object() ? s((*arr)[i], "name") : (*arr)[i].get<std::string>();
        if (name.empty()) continue;
        if (!out.empty()) out += " · ";
        out += name;
    }
    return out;
}

static SeriesCard cardFrom(const nlohmann::json& item) {
    // episode_cnt 可能直接在节点上，也可能在 series_episode_info 里
    int64_t cnt = 0;
    if (item.contains("episode_cnt") && item.at("episode_cnt").is_number())
        cnt = item.at("episode_cnt").get<int64_t>();
    if (cnt == 0 && item.contains("series_episode_info") && item.at("series_episode_info").is_object()
        && item.at("series_episode_info").contains("episode_cnt"))
        cnt = item.at("series_episode_info").at("episode_cnt").get<int64_t>();

    SeriesCard c;
    c.series_id   = s(item, "series_id");
    c.series_name = s(item, "series_name");
    if (c.series_name.empty()) c.series_name = s(item, "series_title");
    c.cover       = HongguoClient::cleanUrl(s(item, "series_cover"));
    c.intro       = s(item, "series_intro");
    std::string right = s(item, "episode_right_text");
    c.badge = !right.empty() ? right : (cnt > 0 ? ("全 " + std::to_string(cnt) + " 集") : "");
    c.tags  = tagText(item);
    return c;
}

// ---------- 免签名接口 ----------

void HongguoClient::getHomeRecommend(int page, const std::function<void(std::vector<SeriesCard>)>& callback,
                                     const ErrorCallback& error) {
    cpr::async([page, callback, error]() {
        try {
            std::string url = Api::WebCategory + "?tab=1&sort_type=1";
            if (page > 1) url += "&page=" + std::to_string(page);
            nlohmann::json data;
            loadRouter(url, data);
            const nlohmann::json* section = sectionOf(data);
            const nlohmann::json* rows = nullptr;
            if (section) {
                for (const char* key : {"recommendList", "categoryData"}) {
                    if (section->contains(key) && section->at(key).is_array()) { rows = &section->at(key); break; }
                    if (section->contains(key) && section->at(key).is_object()
                        && section->at(key).contains("recommendList")) {
                        rows = &section->at(key).at("recommendList");
                        break;
                    }
                }
            }
            if (!rows) throw std::runtime_error("首页数据结构已变化");
            std::vector<SeriesCard> out;
            for (const auto& item : *rows) {
                SeriesCard c = cardFrom(item);
                if (!c.series_id.empty() && !c.series_name.empty()) out.push_back(std::move(c));
            }
            callback(std::move(out));
        } catch (const std::exception& e) {
            error(e.what());
        }
    });
}

void HongguoClient::getSeriesDetail(const std::string& series_id,
                                    const std::function<void(SeriesDetail)>& callback, const ErrorCallback& error) {
    cpr::async([series_id, callback, error]() {
        try {
            nlohmann::json data;
            loadRouter(Api::WebDetail + "?series_id=" + series_id, data);
            if (!data.contains("loaderData") || !data.at("loaderData").contains("detail_page")
                || !data.at("loaderData").at("detail_page").contains("seriesDetail"))
                throw std::runtime_error("剧集不存在或接口已变化");
            const auto& se = data.at("loaderData").at("detail_page").at("seriesDetail");

            SeriesDetail d;
            d.series_id   = s(se, "series_id").empty() ? series_id : s(se, "series_id");
            d.series_name = s(se, "series_name");
            d.cover       = cleanUrl(s(se, "series_cover"));
            d.intro       = s(se, "series_intro");
            if (se.contains("vid_list") && se.at("vid_list").is_array()) {
                for (const auto& vid : se.at("vid_list"))
                    if (vid.is_number()) d.vid_list.push_back(vid.get<uint64_t>());
            }
            d.episode_total = (int64_t)d.vid_list.size();
            if (d.vid_list.empty()) throw std::runtime_error("剧集分集暂不可用");
            callback(std::move(d));
        } catch (const std::exception& e) {
            error(e.what());
        }
    });
}

void HongguoClient::searchSeries(const std::string& keyword,
                                 const std::function<void(std::vector<SeriesCard>)>& callback,
                                 const ErrorCallback& error) {
    // TODO(M3): 优先走 Api::AppSearch + Signer；当前使用网页搜索回落
    cpr::async([keyword, callback, error]() {
        try {
            nlohmann::json data;
            loadRouter(Api::WebSearch + "?query=" + cpr::util::urlEncode(keyword), data);
            // 网页搜索结构与首页一致：loaderData.search_page.recommendList
            const nlohmann::json* rows = nullptr;
            if (data.contains("loaderData")) {
                for (auto it = data.at("loaderData").begin(); it != data.at("loaderData").end(); ++it) {
                    if (it.value().is_object() && it.value().contains("recommendList")) {
                        rows = &it.value().at("recommendList");
                        break;
                    }
                }
            }
            std::vector<SeriesCard> out;
            if (rows) {
                for (const auto& item : *rows) {
                    SeriesCard c = cardFrom(item);
                    if (!c.series_id.empty()) out.push_back(std::move(c));
                }
            }
            callback(std::move(out));
        } catch (const std::exception& e) {
            error(e.what());
        }
    });
}

// ---------- 签名接口（M3） ----------

void HongguoClient::getEpisodeSource(const std::string& series_id, uint64_t vid,
                                     const std::function<void(EpisodeSource)>& callback, const ErrorCallback& error) {
    // TODO(M3) 实现链路：
    //   1. 移植 Signer.kt：SM3/MD5/protobuf/RC4变体/AES -> x-gorgon 等 6 个头
    //   2. POST Api::AppPlayUrl -> fallback_api -> video_list 按 quality 过滤
    //   3. key_seed 解密真实 URL；spade_a 派生 AES key
    //   4. mongoose 起本地解密代理，返回 stream_url 给 mpv
    (void)series_id; (void)vid;
    if (error) error("播放地址接口需要签名链，M3 里程碑实现");
}

}  // namespace hongguo
