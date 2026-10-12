# 红果短剧 Switch 版 —— hongguo API 层

基于 wiliwili（GPL-3.0）骨架的红果短剧内容层。本目录是**新增的 API 层**，用于替换 wiliwili 的 B站 API（`wiliwili/include/api/bilibili/` + `wiliwili/source/api/`）。

总体方案见 `../hongguo-switch-plan.md`。

## 目录结构

```
hongguo-switch/api/hongguo/
├── api.h                    # 端点常量（网页版免签名 + App 版需签名）
├── hongguo.h                # HongguoClient 声明（对齐 BilibiliClient 回调风格）
├── hongguo.cpp              # 免签名接口实现：首页/详情/网页搜索
└── result/
    └── hongguo_result.h     # 数据结构：SeriesCard / SeriesDetail / EpisodeSource
```

## 接口验证状态（2026-10-11）

| 接口 | 签名 | 状态 |
| --- | --- | --- |
| 首页/分类 `WebCategory` | 免签名 | ✅ JS 原型真机验证通过（24 部剧） |
| 详情分集 `WebDetail` | 免签名 | ✅ JS 原型真机验证通过（69 集） |
| 网页搜索 `WebSearch` | 免签名 | ⏳ 结构未验证，代码已按首页同构实现 |
| App 搜索 `AppSearch` | 抖音系签名链 | 🔜 M3（Signer.kt 移植） |
| 播放地址 `AppPlayUrl` | 抖音系签名链 + CENC 解密 | 🔜 M3（Signer + 本地 mongoose 代理） |

## 接入 wiliwili 的步骤

1. 把 `api/hongguo/` 放入 `wiliwili/include/`（与 `api/bilibili/` 并列）
2. `wiliwili/source/api/` 新增对 `hongguo.cpp` 的编译引用（CMakeLists 已含 source/api 通配则免改）
3. 改造首页（`fragment/home_recommends.cpp`）：
   - `BILI::get_recommend` → `hongguo::HongguoClient::getHomeRecommend`
   - 卡片字段：`r.pic → r.cover`、`r.title → r.series_name`、`r.owner.name → r.badge`
   - 去重键：`r.cid → r.series_id`
   - `Intent::openBV(bvid)` → 新增 `Intent::openSeries(series_id)` 打开详情 Activity
4. M3 后接入播放：`getEpisodeSource` 返回本地代理地址，交给 `mpv_core` 加载

## 关键设计

- **免签名通道**：红果官网网页接口无需任何签名，只需 GET + 从 HTML 抠 `_ROUTER_DATA` JSON（`extractRouterData` 括号配对扫描）
- **本地解密代理**：视频流是 CENC 加密 MP4，客户端内用 wiliwili 自带的 mongoose 起本地线程做 Range 拉取+解密，mpv 播 `http://127.0.0.1/...`
- **签名链**：`hongguoTV/kotlin-tv/core/.../Signer.kt`（172 行）是完整移植蓝本，含 SM3/MD5/protobuf/RC4 变体/AES
- **许可**：GPL-3.0（wiliwili、hongguoTV 均为 GPL-3.0，衍生须同协议开源）

## 参考文件位置

- 接口逆向：`hongguoTV/kotlin-tv/core/src/main/kotlin/com/hongguotv/core/`（Signer/MediaCrypto/ContentRepository）
- 服务端参考：`hongguoTV/server/provider.mjs`
- JS 原型（已验证）：`probe-hongguo.mjs`
- wiliwili 源码快照：`wili-src/`（关键文件）+ `wiliwili-yoga.zip`（若完整）
