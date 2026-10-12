// 红果短剧数据结构（对齐 wiliwili 的 nlohmann 反序列化风格）
#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace hongguo {

/// 剧集卡片（首页/分类/搜索共用）
/// 对应网页版 _ROUTER_DATA 里 recommendList 的 video_data 字段
class SeriesCard {
public:
    std::string series_id;   // 剧集唯一ID（等价 wiliwili 的 bvid 角色）
    std::string series_name;
    std::string cover;       // 封面 URL（已清理转义）
    std::string intro;       // 简介
    std::string badge;       // 角标文本，如 "全 69 集"
    std::string tags;        // 标签串，如 "爱情 · 古风爱情 · 先婚后爱"
};

/// 剧集详情 + 分集
class SeriesDetail {
public:
    std::string series_id;
    std::string series_name;
    std::string cover;
    std::string intro;
    int64_t episode_total{};
    std::vector<uint64_t> vid_list;  // 分集 ID 列表，顺序即集数顺序
};

/// 单集播放信息（M3 签名链完成后填充）
class EpisodeSource {
public:
    uint64_t vid{};
    /// 经过本地解密代理后供 mpv 播放的地址，形如 http://127.0.0.1:PORT/hgstream?vid=xxx
    std::string stream_url;
    int64_t duration_ms{};
};

}  // namespace hongguo
