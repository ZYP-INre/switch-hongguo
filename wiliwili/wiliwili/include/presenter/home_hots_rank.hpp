//
// Created by fang on 2022/7/8.
//

#pragma once

#include "bilibili.h"
#include "bilibili/result/home_hots_rank.h"

enum class RankType {
    UGC = 0,
    PGC,
};

class RankData {
public:
    RankData(const std::string& key, const RankType type, const int id, const std::string& extra = "all")
        : type(type), id(id), key(key), extra(extra) {}

    RankType type;
    int id;
    std::string key;
    std::string extra;
};

class HomeHotsRankRequest {
public:
    virtual void onHotsRankList(const bilibili::HotsRankVideoListResult& result, const std::string& note) {}
    virtual void onHotsRankPGCList(const bilibili::HotsRankPGCVideoListResult& result, const std::string& note) {}
    virtual void onError(const std::string& error) {}

    std::vector<RankData> rankList = {
        {"全站", RankType::UGC, 0},
        {"番剧", RankType::PGC, 1},
        {"国创", RankType::PGC, 4},
        {"纪录片", RankType::PGC, 3},
        {"电影", RankType::PGC, 2},
        {"电视剧", RankType::PGC, 5},
        {"动画", RankType::UGC, 1005},
        {"游戏", RankType::UGC, 1008},
        {"鬼畜", RankType::UGC, 1007},
        {"音乐", RankType::UGC, 1003},
        {"舞蹈", RankType::UGC, 1004},
        {"影视", RankType::UGC, 1001},
        {"娱乐", RankType::UGC, 1002},
        {"知识", RankType::UGC, 1010},
        {"科技数码", RankType::UGC, 1012},
        {"美食", RankType::UGC, 1020},
        {"汽车", RankType::UGC, 1013},
        {"时尚美妆", RankType::UGC, 1014},
        {"体育运动", RankType::UGC, 1018},
        {"动物", RankType::UGC, 1024},
    };

    std::vector<std::string> getRankList();

    void requestData(size_t index = 0);

    /// 主页 热门 排行榜 投稿视频
    void requestHotsRankVideoList(int rid, const std::string& type);

    /// 主页 热门 排行榜 官方
    void requestHotsRankPGCVideoList(int season_type, int day = 3);
};
