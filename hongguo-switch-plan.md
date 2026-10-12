# 红果短剧 Switch 版开发方案（基于 wiliwili）

> 更新：2026-10-11 ｜ 状态：调研完成，进入环境搭建

## 1. 目标

做一个运行在 Nintendo Switch（大气层自制系统）上的红果短剧客户端，参照 wiliwili（B站第三方客户端）的架构，替换内容层为红果接口。

产物形态：`.nro` homebrew 文件，放 SD 卡 `switch/` 目录，hbmenu 启动。

## 2. 为什么选 wiliwili 做骨架

| 维度 | wiliwili 的价值 |
| --- | --- |
| 架构 | 三层分离：borealis 手柄导航 UI + nanovg 绘制（OpenGL/deko3d 后端）+ FFmpeg/MPV 播放 |
| 平台 | Switch 是一等公民，有硬解版本；同时支持 Mac/Win/Linux 桌面版 |
| 开发体验 | **桌面版可直接在 Mac 编译运行**，业务逻辑迭代不需要每步碰 Switch |
| 许可 | GPL-3.0。衍生作品公开发布时须同协议开源（自用无影响） |
| 参考 | UI 交互（方向键浏览、手柄播放控制、选集面板）与短剧 App 高度重合 |

## 3. 红果接口调研结论（来自 hongguoTV 逆向）

hongguoTV（GPL-3.0）已完成红果接口逆向，Kotlin 版 `Signer.kt` + `MediaCrypto.kt` 是完整可移植的实现。

### 3.1 接口分层

| 能力 | 端点 | 签名需求 |
| --- | --- | --- |
| 首页/分类列表 | `https://hongguoduanju.com/category?tab=1&sort_type=1&page=N`（网页版） | 免签名，从 HTML 抠 `_ROUTER_DATA` JSON |
| 剧集详情+分集 | `https://hongguoduanju.com/detail?series_id=ID`（网页版） | 免签名 |
| 搜索 | App 接口 `api5-normal-sinfonlinea.fqnovel.com/reading/bookapi/search/tab/v`，失败回落官网网页搜索 | 需完整签名 |
| 播放地址 | App 接口 `api5-normal-sinfonlineb.fqnovel.com/novel/player/multi_video_model/v1/`（POST JSON） | 需完整签名 |

### 3.2 签名链（移植重点，约 200~300 行 C++）

抖音系全家桶请求头：`x-gorgon` / `x-helios` / `x-medusa` / `x-khronos` / `x-ladon` / `x-argus`。
依赖的标准算法：SM3、MD5、SHA-512、AES-CBC、RC4 变体（gorgon 内部）、protobuf 手工编码器。
设备参数硬编码（Redmi 机型 + device_id），封装在 `VendorConstants`。

### 3.3 视频解密链

1. `key_seed` → SHA-512 双重哈希 + AES-CBC 解密出真实播放 URL
2. `spade_a` → 派生 16 字节 AES key
3. 视频本体是 CENC 加密 MP4：解析 `moov/saiz/saio/stsz` box 得到逐样本 IV 与加密区间，Range 拉取后分块解密

### 3.4 Switch 端播放方案

照搬 hongguoTV 已验证的**本地代理架构**：客户端内起一个轻量 HTTP 线程，负责"拉取 Range → 解密 → 提供 `http://127.0.0.1:PORT/stream`"，libmpv 直接播本地地址。libnx 支持 socket，此方案不依赖 mpv 内部解密机制，风险最低。

## 4. 改造清单（基于 yoga 分支源码逐文件确认）

wiliwili 源码结构（wiliwili/source/，135 个 C++ 文件）与红果版处置：

| wiliwili 模块 | 文件（yoga 分支） | 处置 |
| --- | --- | --- |
| API 端点常量 | `include/api/bilibili/api.h` | 替换为 `api/hongguo/api.h`（端点：网页版 category/detail + App 版 search/playurl） |
| 请求客户端 | `source/api/bilibili.cpp` + `BilibiliClient` | 仿写 `HongguoClient`：`HTTP::getResultAsync` 模板改为网页版 HTML 抠取 + App 版签名请求 |
| 数据结构 | `include/api/bilibili/result/home_result.h` 等 | 新增 `result/hongguo_result.h`：SeriesCard / SeriesDetail / PlayUrlResult，字段 series_id/series_name/series_cover/vid_list |
| 首页推荐 | `presenter/home_recommends.cpp` + `fragment/home_recommends.cpp` | 数据源换 HongguoClient，卡片字段映射：pic→series_cover、title→series_name、owner.name→badge、去重键 cid→series_id；`Intent::openBV(bvid)`→`openSeries(series_id)` |
| 详情/选集/播放 | `presenter/video_detail.cpp`（29.8KB）+ `activity/player_*.cpp` | 数据源换红果详情+播放地址接口；播放 URL 指向本地解密代理 |
| 主框架 | `activity/main_activity.cpp` | tab 增删：保留 首页/搜索/我的，删 直播/动态/收件箱 |
| 卡片/网格 UI | `view/video_card.cpp`、`view/recycling_grid.cpp`（含骨架屏/错误态） | 原样保留 |
| 播放器 | `view/mpv_core.cpp`（58.9KB） | 原样保留，输入本地代理流地址 |
| 竖屏适配 | 播放页布局 | 9:16 视频居中 + 海报模糊背景（borealis XML 布局） |
| 删除 | 弹幕（danmaku_core 37KB）、直播（live_core 25KB）、评论、DLNA 投屏、动态 | 整体移除，减编译体积 |

HTTP 与基础设施直接复用 wiliwili 已内嵌的：cpr（HTTP 客户端）、nlohmann/json、mongoose（**本地解密代理直接用它**，零新依赖）。

## 5. 里程碑

1. **M1 环境与骨架**（进行中）：Mac 上编译运行 wiliwili 桌面版
2. **M2 接口层**：C++ 实现 hongguo API（免签名部分先行跑通：首页/详情/网页搜索）
3. **M3 签名与播放**：移植 Signer + MediaCrypto，本地代理线程跑通真实播放
4. **M4 UI 整合**：页面改造、竖屏适配、收藏/历史
5. **M5 Switch 构建**：devkitPro 交叉编译 `.nro`，GitHub Actions CI，真机实测

## 6. 风险

- 红果非官方接口随时可能变更失效（hongguoTV 同样面临）
- 签名移植细节多（字节序、无符号运算），需逐函数对照单测
- 任天堂对破解系统打击趋严，用户群限于大气层用户
- GPL-3.0 传染性：公开发布必须开源

## 7. 本地资产

- `hongguoTV/`：接口参考实现（Signer.kt / MediaCrypto.kt / ContentRepository.kt / server/provider.mjs）
- `probe-hongguo.mjs`：免签名接口 JS 原型（首页/详情链路），C++ 实现的直接参照
- `wiliwili-yoga.zip`：wiliwili 源码快照（默认分支为 `yoga`，非 master）

## 8. 验证记录（2026-10-11）

| 验证项 | 结果 |
| --- | --- |
| 红果网页版首页接口 | ✅ 真实可用，拉到 24 部剧（含 series_id/标题/集数/标签/封面） |
| 红果网页版详情接口 | ✅ 真实可用，拿到《首辅娇娘》69 集分集 ID |
| wiliwili 默认分支 | `yoga`（`main`/`master` 均不存在） |
| 本机编译环境 | ❌ 沙箱限制：brew 无系统目录写权限、Xcode CLT 未装全、无 docker。Mac 桌面版编译与 Switch 交叉编译需在环境放开后执行 |
| 网络状况 | GitHub 直连 ~10KB/s（codeload 可达），gh-proxy.com 镜像可用但 zip 路径有缓存污染风险；npmmirror 偶发超时 |

里程碑调整：M1 的"Mac 编译桌面版"受环境限制后置，M2（C++ 接口层代码）提前以纯代码产出推进，编译验证并入构建指南。
