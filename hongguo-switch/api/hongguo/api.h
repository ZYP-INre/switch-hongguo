// 红果短剧端点常量（Switch 客户端用）
// 分层参照 hongguoTV（GPL-3.0）ContentRepository.kt 的接口调研结论
#pragma once

namespace hongguo {

namespace Api {

/// 红果官网网页版（免签名，HTML 内嵌 _ROUTER_DATA）
const std::string _webBase = "https://hongguoduanju.com";

/// 首页/分类列表. tab: 1=最热, 4=新剧; sort_type: 1=人气; page 从 1 开始
const std::string WebCategory = _webBase + "/category";
/// 剧集详情+分集列表. ?series_id=xxx
const std::string WebDetail = _webBase + "/detail";
/// 网页版搜索（回落通道，作为签名搜索失败后的 fallback）
const std::string WebSearch = _webBase + "/search";

/// 红果 App 接口（需抖音系签名链，见 Signer 移植 M3）
const std::string _appBase    = "https://api5-normal-sinfonlinea.fqnovel.com";
const std::string _appBaseB   = "https://api5-normal-sinfonlineb.fqnovel.com";
/// App 搜索
const std::string AppSearch   = _appBase + "/reading/bookapi/search/tab/v";
/// 播放地址（POST JSON，返回 key_seed 加密 URL + spade_a 密钥材料）
const std::string AppPlayUrl  = _appBaseB + "/novel/player/multi_video_model/v1/";

}  // namespace Api
}  // namespace hongguo
