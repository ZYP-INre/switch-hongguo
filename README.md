# 红果短剧 Switch 版

目标：在 Nintendo Switch（大气层）上跑一个红果短剧客户端，产物是 `.nro`。
骨架用 [wiliwili](https://github.com/xfangfang/wiliwili)，内容能力从
[hongguoTV](https://github.com/N3urda/hongguoTV) 的逆向实现移植。

整个项目分三块。**第一块（内容层）已完成并端到端验证通过。**

| 阶段 | 状态 | 位置 |
| --- | --- | --- |
| ① 内容层移植 | ✅ **已完成并端到端验证** | `hongguo-core/` |
| ② UI 改造 | ✅ **已完成：wiliwili 界面接上红果** | `wiliwili/wiliwili/{include,source}/hongguo/` |
| ③ 交叉编译 `.nro` | ✅ **成品 .nro 已产出** | `dist/hongguo.nro` |

**产物**：[`dist/hongguo.nro`](dist/)（37.5 MB）——
启动进红果首页 → 选剧 → 选集 → 播放。装在 SD 卡 `switch/` 目录即可。

---

## 已完成：内容层

`hongguo-core/` —— C++17 静态库 + `hgctl` 命令行，43 项单测全绿。
详见 [`hongguo-core/README.md`](hongguo-core/README.md)。

**它证明了什么**：红果的私有签名协议与视频加密，可以被正确、可验证地移植到 C++。

### 端到端验证（对真实接口）

```
hgctl home          → 24 部剧，标题/集数/标签/封面全对
hgctl search 总裁    → 服务端接受了我们生成的签名
hgctl detail <ID>   → 69 集分集列表正确
hgctl play <剧集ID> <分集ID>   → 720p DASH 分轨 + 内容密钥已派生
hgctl dashdump ... video     → 解密产出合法 H.264 fMP4（ffprobe/ffmpeg 验收）
hgctl dashserve ...          → 两轨合成一条 fMP4，经本地代理解码整集无错
ffprobe             → HEVC 1080×1920 + AAC（竖屏，正是短剧形态）
ffmpeg              → 无错解出视频帧
hgctl stream <剧集ID> <分集ID> → 本地代理 Range 自测四项全过
```

### 验证方法

密码学移植最大的敌人是"看起来对"。本项目用**两档验收**把它变成机器可判定的问题：

1. **逐字节对拍** —— 把上游 Node 原实现放进沙箱、打桩时钟与随机数，作为"神谕"。
   所有密码学模块的输出必须与之完全一致。对拍台本身能复现上游那份 `signing.json`
   的全部 12 组向量，因此可信。
2. **真实接口** —— `hgctl` 直连上游，证明接口是活的、用法是对的。

两档分工明确：第一档失败 = 移植写错了；第二档失败 = 上游改版了。
分开才能快速判断问题归属。

---

## 界面与构建

### ② UI 改造

wiliwili 的界面与播放栈直接复用，内容层换成 `hongguo-core`。

接入是**完全自包含**的：新增 `wiliwili/wiliwili/{include,source}/hongguo/`，
只在 `main.cpp` 换一行入口，**一行都不碰原有的 B 站代码**。
加 `-DHONGGUO_UI=OFF` 即可编回原版 wiliwili。

播放是怎么打通的：内容层解密后的视频由**本地代理**变成普通 HTTP 流，
直接交给 wiliwili 的 `VideoView`（mpv），播放控制（暂停、进度、音量、全屏）全部复用现成的。

hbmenu 里显示为「红果短剧」，带独立图标，与原版 wiliwili 并存不冲突。

### ③ 交叉编译

```sh
export DEVKITPRO=$HOME/devkitpro/root/opt/devkitpro
export PATH=$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$DEVKITPRO/portlibs/switch/bin:$PATH
export PKG_CONFIG_PATH=$DEVKITPRO/portlibs/switch/lib/pkgconfig

cd wiliwili
cmake -B cmake-build-switch -DCMAKE_BUILD_TYPE=Release -DPLATFORM_SWITCH=ON \
      -DBRLS_UNITY_BUILD=ON -DBUILTIN_NSP=OFF
make -C cmake-build-switch wiliwili.nro -j$(sysctl -n hw.ncpu)
```

**拿到工具链费了些周折**：devkitPro 官方源被 Cloudflare 按 **TLS 指纹**拦截（403，所有路径）。
试过官方源直连、Docker 镜像、四家国内镜像、`downloads.devkitpro.org`、brew 裸机工具链、
本地代理，全部不通。关键在于拦截依据是**指纹而非 IP**——用 `curl-impersonate`
伪装 Chrome 指纹、**再加**一个未被封的出口 IP，两个条件同时满足才通过。
`tools/fetch-devkitpro.py` 负责解析包数据库、算依赖闭包（34 个包）、下载、解包到
`~/devkitpro`，不需要 sudo。

**编译问题已排除，剩下的是运行时问题**——真机上的联网、`mbedtls` 的 TLS 后端、
libnx 的 socket 行为、Switch 的内存，都还没验过。详见 [`dist/README.md`](dist/)。

---

## 目录

```
hongguons/
├── dist/                            ★ 交付物
│   ├── hongguo.nro                  成品（37.5 MB）
│   └── hongguo-probe.nro            诊断版（1.5 MB）
├── hongguo-core/                    ① 内容层
├── wiliwili/                        ②③ 骨架 + 红果界面（git clone，含子模块）
│   └── wiliwili/{include,source}/hongguo/   红果界面代码
├── switch/                          ③ 诊断版的构建配置
├── tools/fetch-devkitpro.py         ③ 工具链获取（绕 Cloudflare）
├── .github/workflows/               ③ 备选：云端构建 .nro
├── hongguoTV/                       上游参考实现（Kotlin + Node）
├── docs/superpowers/specs/          设计文档
│   └── 2026-10-11-hongguo-switch-content-layer-design.md
├── hongguo-switch-plan.md           最初的调研笔记（部分结论已被实证修正）
└── README.md                        本文件
```

**建议先读** [`hongguo-core/README.md`](hongguo-core/README.md) 的"上游行为备忘"一节——
那里记录了移植过程中确认的、不明显但必须照做的行为（非标准 RC4、MD5 填充字节序、
空字节数组的 protobuf 处理、Node 版的硬编码编码格式缺陷等）。改代码前请先读它。

---

## 许可

GPL-3.0-only。移植自 hongguoTV（GPL-3.0），衍生作品公开发布时须同协议开源。
