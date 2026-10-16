# hongguo-core

红果短剧/漫剧的**内容层**，C++17 静态库。零 UI 依赖，macOS 与 devkitPro 均可编译。

它是"红果短剧 Switch 版"三块拼图中的第一块（内容层），也是唯一无法靠换方案绕开的一块：
UI 与构建都是成熟路径，而内容层依赖对一整套私有签名协议的正确移植——错了就是黑屏，
且几乎没有外部资料可查。

---

## 它做什么

| 能力 | 说明 |
| --- | --- |
| 首页 / 分类 | 官网 SSR 数据，免签名 |
| 搜索 | App 接口，需完整签名；失败自动回落官网搜索 |
| 详情 / 分集 | 官网 SSR 数据，免签名 |
| 播放地址解析 | 两级跳 + 两层解密（`key_seed` 解地址、`spade_a` 派生内容密钥） |
| 分片解密 | CENC 加密 MP4 的样本索引解析与 AES-CTR 解密 |
| 本地流服务 | 进程内 HTTP 服务，把解密后的视频以可 Range 访问的流提供给 mpv |

短剧与漫剧两类内容都支持。

---

## 它不做什么

- 任何 UI、布局、焦点管理
- 收藏 / 历史 / 续播 / 备份
- 登录与账号（内容链路以 `user_is_login=0` 即可跑通）
- 交叉编译与 `.nro` 产物

---

## 编译

```sh
cd hongguo-core
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

依赖只有两个：

| 依赖 | 用途 | macOS | Switch |
| --- | --- | --- | --- |
| libcurl | HTTP | 系统自带 | `switch-curl` |
| nlohmann/json | 解析上游 JSON | 已内置于 `third_party/` | 同左 |

**密码学零外部依赖。** 这是刻意的：`AesV3` 是按时间戳选 sbox 分片的私有变体、
`hashF13` 的 112 轮与 `xmxor` 的位反转都必须手写，剩下标准的 MD5/SHA-512/AES/SM3
也自带实现，为的是保证 macOS 与 Switch 上**逐字节一致**——不必把结果押在
"目标平台的 OpenSSL 有没有编进 SM3"这类问题上。这对本项目的验收标准是决定性的。

## 跑测试

```sh
./build/hongguo_tests          # 全部
./build/hongguo_tests signer   # 按名字过滤
```

43 项测试，其中密码学部分全部与上游实现**逐字节对拍**。

## 用命令行验证真实接口

```sh
./build/hgctl home                          # 首页
./build/hgctl search 总裁                    # 搜索（走完整签名）
./build/hgctl detail <剧集ID>                # 详情与分集
./build/hgctl play <分集ID>                  # 解析播放地址
./build/hgctl fetch <分集ID> out.mp4 --bytes 8000000   # 下载并解密
./build/hgctl stream <分集ID>                # 起本地代理并自测
```

`stream` 打印出的地址可以直接喂给 mpv：

```sh
mpv "http://127.0.0.1:59062/stream"
```

---

## 目录

```
hongguo-core/
├── include/hongguo/          公开头文件（每个模块一个）
├── src/                      实现
├── tests/                    单测与向量文件
├── tools/
│   ├── hgctl.cpp             命令行入口
│   └── oracle/               对拍台（见下）
├── third_party/nlohmann/     内置的 JSON 解析器
└── CMakeLists.txt
```

### 模块分层

依赖方向严格向下，不存在回头依赖。

```
① 交付物    hgctl          local_proxy
② 编排      content_repo
③ 能力      signer   media_crypto   http(libcurl)   model/device/stream_reader
④ 基础      digest  aes  aesv3  protobuf  bytes  vendor
```

最容易出错的四处——`aesv3`、`hashF13`、`xmxor`、CENC 样本解析——被切成独立单元，
可以各自喂固定向量单独对拍，不必整条链一起调。业务规则全部集中在 `content_repo`，
上游改版时只动那一个文件。

---

## 怎么重新验证

密码学移植最大的敌人是"看起来对"。本项目用**两档验收**把它变成机器可判定的问题。

### 第一档：逐字节对拍

`tools/oracle/` 把 hongguoTV 的 Node 原实现放进受控沙箱，打桩 `Date.now()` 与
`Math.random()`（LCG 种子 17，与上游 Kotlin 测试同款），导出全部内部函数作为"神谕"。

```sh
node tools/oracle/gen-vendor.mjs > include/hongguo/vendor.hpp   # 重新导出常量
node tools/oracle/gen-vectors.mjs                                # 重新生成向量
node tools/oracle/check-signing.mjs                              # 自检对拍台
```

`vendor.hpp` 是**生成**的，不是手抄的。sbox 有 1024 字节、查表 64 项、
设备指纹 27 项且**顺序本身参与签名**——手抄错一个字节，签名就全错，且几乎无法靠读代码定位。

对拍台的自检会复现上游那份 `signing.json`（12 组向量，四个签名全覆盖）。
复现得了，神谕才可信；神谕可信，C++ 的每一处偏差才能被机器指出来。

### 第二档：真实接口

`hgctl` 直连上游。这一档证明"接口是活的、我们的用法是对的"。

两档的分工很重要：**第一档失败 = 移植写错了；第二档失败 = 上游改版了或请求姿势不对。**
分开才能快速判断问题归属。

---

## 播放通道：2026-10-15 的换代

这一节描述**当前**的播放链路。旧文档里那套 `fqnovel` 域名下的 `video_model`
单集接口已经不可用，别再照它去调。

### 旧通道是怎么死的

2026-10-15 起，`api5-normal-sinfonlineb.fqnovel.com` 下的 `video_model` 与
`multi_video_model` 对所有请求一律返回**空 200**（批量接口返回 `110001`）。
关键判据：**故意把 `x-gorgon` / `x-medusa` / `x-ss-req-ticket` 改成坏值，
返回与合法签名完全一致** —— 说明服务端在验签之前就短路了。

也就是说这不是"请求姿势不对"，换签名、换设备指纹、换域名、换 HTTP 方法
都没用。读接口（首页 / 搜索 / 详情）仍然正常，封的是播放。

### 新通道：网站自己的 play-info

网页版走的是另一条路，不需要 App 签名：

```
POST https://hongguoduanju.com/api/video/protected/play-info
     Origin/Referer 指向 hongguoduanju.com
     {"series_id":"<剧集ID>","item_id":"<分集ID>","definition":"720p"}
→ { "play_info": { main_url, backup_url, player_access_key, kid,
                   init_range, index_range, first_moof_range, size, duration, ...,
                   "audio": { 同结构 } } }
```

几个必须照做的点（都实测过）：

- **剧集 ID 与分集 ID 必须配套**，只给分集 ID 直接 `403 protected_playback_unavailable`。
  所以 `ContentRepo::stream()` 的签名是 `(seriesId, episodeId, maxQuality)`。
- 档位只认 `480p` / `720p`，给 `1080p` 返回 `400`。不传 `definition` 默认 720p。
- 媒体是 **DASH 分轨**（视频、音频各一条），各自 **CENC 加密**。
  返回的 `init_range` / `index_range` / `first_moof_range` 都是**闭区间**的绝对偏移。
- 该接口**按 IP 限流**，密集调试会被 403 一段时间。`HG_PLAY_INFO_FILE`
  可以喂一份保存好的响应，避免反复打上游。

### 密钥：`deriveAccessKey`

新通道的 `player_access_key` 与旧的 `spade_a` 不是同一个值，但**同族**：
都是"长度由头三字节异或决定 + 两个交替种子逐字节混淆"。

```
buf[i] = (buf[i] ^ (i奇数 ? a : b)) + popcount(i) + 21     // a=85, b=250
key    = hex(buf[1..33])
```

与旧 `deriveKey` 的两处差异（照站点内嵌的 `aw.wasm` 逐字节对拍确认）：

1. 种子 `b` 是 **250**（旧版 246）
2. 末步是 `+popcount(i) + 21`（旧版是 `-21 - popcount(i)`）

这一步的算法是从混淆过的 WASM（`aw.wasm`，控制流平坦化）里逆出来的。
如果哪天 `work[1..33]` 出来的不是 32 个合法十六进制字符，说明上游又改版了 ——
`deriveAccessKey` 会直接抛错，不会静默返回半个密钥。

### 为什么要自己把两条轨合成一条 fMP4

`DashMuxer` 把视频轨与音频轨拼成一条普通 fMP4，播放器看到的就是一个寻常的本地流。

**不是因为喜欢造轮子**：目标机的 ffmpeg（devkitPro 的 `libavformat`）**没有编译
dash 解复用器** —— `nm` 里只有 `ff_mov_demuxer` / `ff_mpegts_demuxer`，
没有 `ff_dash_demuxer`。MPD 那条路在 Switch 上根本走不通。而 mov 解复用器是有的。

拼法只做三件事：搬 box、改音频轨的 `track_ID`（两条轨各自生成时都是 1，合到一个
moov 里会撞车）、把音频分片 `tfhd` 里的 `track_ID` 一起改掉。**不重编码、不重排时间**。

`DashTrackReader` 的输出与源轨**偏移完全一致**（CENC 是就地等长的），
所以合成器只需要维护一张"输出偏移 → 源轨 + 偏移"的分片表。

已验证：整集（111 秒）经本地代理解码无错，逐帧可播，seek 正常。

---

## 上游行为备忘

移植过程中确认的、不明显但必须照做的行为。改代码前请先读这节。

### 签名

- **`gorgon` 用的是非标准 RC4**：密钥调度只做 `s[i] = s[j]`，**不交换**。
  照教科书补上 swap 会导致签名全错。上游 Node 与 Kotlin 两版都是如此。
- **`hashF13` 有三个分支**，上游 Node 版三支全实现，Kotlin 版只实现了 branch 0。
  本实现跟随 Kotlin：强制 branch 0，通过抖动 `_rticket`（上限 127 次）命中。
  branch 0 是唯一被 `signing.json` 逐字节验证过的路径。
- **时钟与随机数必须可注入**。`ts` 取当前秒、`_rticket` 取当前毫秒，
  `gorgon`/`helios`/`medusa` 各含随机数。若内部直接读系统时间，两个实现永远算不出
  同一输入，对拍就无从谈起。
- **RNG 的消耗顺序是规格的一部分**：`branch`→`hashF13`→`gorgon`→`helios`→`medusa`，
  其中 `helios` 消耗 1 次、`medusa` 消耗 5 次。顺序不同结果就不同。
- **MD5 的填充长度字段是**小端**，SHA-512 与 SM3 是**大端**。
  写错的典型症状是"空输入正确、非空输入全错"（长度为 0 时两种写法都是全零）。
- **URL 编码的不转义集合**为 `A-Za-z0-9 - _ . ~ *`，十六进制一律大写。
  Java 的 `URLEncoder` 与 JS 的 `encodeURIComponent` 形式不同但等价于此集合。

### protobuf

- **空字符串与空字节数组待遇不同**：空字符串字段被**整体省略**，空字节数组仍会编码出
  `tag + 长度0`。上游用 `=== ''` 判空，该判断对 Buffer 恒为假。

### 媒体

- **`media_crypto` 以上游 Kotlin 版为准，不是 Node 版。** Node 版有两处缺陷：
  1. 把样本条目的 fourcc **硬编码**改为 `hvc1`/`mp4a`。AVC 与 HEVC 混用时前者播不出来。
     Kotlin 版从 `frma` 还原原始编码，本实现照做。
  2. 不支持子样本加密。Kotlin 版支持并保留明文段。
- **`headerEnd()` 要求 `moov` 在文件开头且 ≤ 8MiB。** 不满足时**直接报错**而非静默出错。
  若将来遇到尾部 `moov` 的资源，需补一次尾部 Range 探测。
- **排除 `bytevc2`/`bvc2` 清晰度。** 这类资源在标准 MP4 解复用器下只有音频没有视频轨。
  全被排除时退到最低一档。
- **`deviceProto` 的 `version` 参数在上游就是死参数**（Node 收下但从不使用，
  Kotlin 干脆没这个参数）。保留形参以对齐上游签名。

### 网络

- **第二跳 CDN（`vas-lf-x.snssdk.com`）握手常在 10 秒上下且偶发失败。**
  HTTP 层必须带重试，否则基本不可用。`http.cpp` 里对连接/握手阶段的瞬时故障重试 3 次。
- 视频源站必须支持 Range（返回 206），否则无法边下边解。

---

## Switch 交叉编译（尚未完成）

内容层本身是平台无关的，但要编成 `.nro` 还需要：

1. **devkitPro 工具链**（本机未安装）
2. **`switch-curl`** 包，替代系统 libcurl
3. **socket 初始化**：libnx 需要在用 socket 前调用 `socketInitializeDefault()`
4. **入口与生命周期**：内容层是纯库，没有 `main`，接入方（wiliwili）负责

CMake 预留了平台判断的位置但尚未实装。这一步必须在装有 devkitPro 的机器上做
（官方推荐用 `devkitpro/devkita64` Docker 镜像）。

---

## 许可

GPL-3.0-only。移植自 [hongguoTV](https://github.com/N3urda/hongguoTV)（GPL-3.0），
衍生作品公开发布时须同协议开源。
