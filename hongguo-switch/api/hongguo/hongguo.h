// 红果短剧客户端（对齐 wiliwili BilibiliClient 的异步回调模式）
// 许可：GPL-3.0（衍生自 wiliwili 与 hongguoTV，两者均为 GPL-3.0）
#pragma once

#include <functional>
#include <string>
#include "result/hongguo_result.h"

namespace hongguo {

using ErrorCallback = const std::function<void(const std::string&)>;

class HongguoClient {
public:
    /// 首页/分类推荐（免签名网页接口）
    /// page 从 1 开始；回调在 cpr 异步线程触发，UI 更新需自行 brls::Threading::sync
    static void getHomeRecommend(
        int page,
        const std::function<void(std::vector<SeriesCard>)>& callback,
        const ErrorCallback& error);

    /// 剧集详情 + 分集列表（免签名网页接口）
    static void getSeriesDetail(
        const std::string& series_id,
        const std::function<void(SeriesDetail)>& callback,
        const ErrorCallback& error);

    /// 搜索（免签名网页搜索，M3 完成签名链后优先走 App 接口）
    static void searchSeries(
        const std::string& keyword,
        const std::function<void(std::vector<SeriesCard>)>& callback,
        const ErrorCallback& error);

    /// 单集播放地址（需签名链 + 解密，M3 实现）
    /// 返回的 stream_url 指向内置本地解密代理
    static void getEpisodeSource(
        const std::string& series_id,
        uint64_t vid,
        const std::function<void(EpisodeSource)>& callback,
        const ErrorCallback& error);

    // ---- 内部工具，公开以便单测 ----

    /// 从 HTML 中抠取 _ROUTER_DATA 的 JSON（括号配对扫描，容忍字符串内的引号转义）
    static std::string extractRouterData(const std::string& html);

    /// 清理 JSON 里转义的 URL：\/ \u002F \u0026 &amp;
    static std::string cleanUrl(const std::string& s);
};

}  // namespace hongguo
