# 红果短剧 Switch 版 · 内容层设计

> 日期：2026-10-11 ｜ 阶段：内容层（第一阶段） ｜ 状态：**已完成，端到端验证通过**

## 1. 背景

目标是在 Nintendo Switch（大气层）上跑一个红果短剧客户端，产物是 `.nro`。骨架用
[wiliwili](https://github.com/xfangfang/wiliwili)（C++17 / borealis / nanovg / mpv），
内容能力从 [hongguoTV](https://github.com/N3urda/hongguoTV)（GPL-3.0）的逆向实现移植。

整个项目分三块：**内容层移植**、**UI 改造**、**交叉编译**。本设计只覆盖第一块。

### 为什么内容层优先

这是全项目唯一"做不出来就全盘皆输"的部分，也是唯一无法通过换方案绕开的部分。
UI 和构建都是成熟路径，内容层则依赖对一整套私有签名协议的正确移植——错了就是黑屏，
且几乎没有外部资料可查。先把它做到"可验证的正确"，后面两块才有意义。

## 2. 范围

### 做

- 红果**短剧**与**漫剧**两类内容的完整链路：首页/分类、搜索、详情+分集、播放地址解析
- 抖音系请求签名（`x-gorgon` / `x-medusa` / `x-helios` / `x-ladon` / `x-argus` / `x-khronos`）
- 播放地址两层解密 + CENC 加密 MP4 的分片解密
- 本地 HTTP 代理：把解密后的视频以可 Range 访问的流提供出去（供 mpv 播放）
- 命令行工具 `hgctl`：本机验证与将来接入 wiliwili 的样例

### 不做（本阶段明确排除）

- 任何 UI、界面布局、焦点管理
- 收藏 / 历史 / 续播 / 备份（Kotlin 侧有 `LibraryIndex` / `Watching` / `LibraryBackup`，属客户端状态，归 UI 阶段）
- 登录与账号（红果内容链路以 `user_is_login=0` 可跑通）
- Switch 交叉编译与 `.nro` 产物（归第三阶段）
- 不实现"复用 hongguoTV 的 Node 服务"这条退路。它只作为移植卡死时的对照组存在。

## 3. 架构

`hongguo-core/`：C++17 静态库，**零 UI 依赖**，macOS 与 devkitPro 均可编译。
依赖方向严格向下，不存在回头依赖。

```
① 交付物    hgctl (CLI)          local_proxy (本地流服务)
② 编排      content_repo
③ 能力      signer  media_crypto  http(libcurl)  model
④ 基础      digest  aes  aesv3  protobuf  bytes  vendor
```

| 模块 | 职责 | 移植来源 |
| --- | --- | --- |
| `bytes` | hex / base64 / le32 / be32 / rol / ror / xor | `Bytes.kt` |
| `digest` | MD5 / SHA-512 / SM3 一次性摘要 | `Bytes.kt` |
| `aes` | AES-CBC 解密 / AES-CTR 加解密 | `Bytes.kt` |
| `aesv3` | 非标准 AES 变体（按时间戳选 sbox 分片） | `AesV3.kt` |
| `protobuf` | 手工 protobuf 编码器（varint / sint / float / bytes） | `Signer.kt` 内联 |
| `vendor` | 设备指纹 27 项 / sbox 1024 字节 / hashTable 64 项 | `VendorConstants.kt` |
| `signer` | 完整签名链与请求头装配 | `Signer.kt` |
| `media_crypto` | 播放 URL 解密 / CENC box 解析 / 分片解密 | `MediaCrypto.kt` |
| `http` | libcurl 薄封装（GET/POST、自定义头、Range、超时） | — |
| `model` | 纯数据结构 | `ContentRepository.kt` 顶部 |
| `content_repo` | 拼 URL、挑清晰度、抠 `_ROUTER_DATA`、字段映射、搜索游标 | `ContentRepository.kt` |
| `stream_reader` | 拉取 → 解密 → 提供区间读取 | `RemoteVideo.kt` |
| `local_proxy` | HTTP 服务，供 mpv 消费 | `server/http.mjs` |

### 边界划分的理由

最容易出错的是 `aesv3`、`hashF13`、`xmxor`、CENC 样本解析这四处。它们被切成独立单元，
可以各自喂固定向量单独对拍，无需整条链一起调。基础层零依赖意味着每个都能写几毫秒跑完的单测。
业务规则全部集中在 `content_repo`，上游改版时只动一个文件。

## 4. 关键接口

```cpp
// signer.hpp —— 时钟与随机数必须可注入，这是字节对拍的前提
class Signer {
public:
  using Clock = std::function<int64_t()>;   // 毫秒
  using Rng   = std::function<double()>;

  Signer(Clock clock, Rng rng);

  struct Request { std::string url; HeaderMap headers; Bytes body; };
  Request sign(const std::string& base, const StrMap& values, const Bytes& body = {});

  // 以下仅为单测暴露，用于逐段对拍定位
  int   branch (const std::string& query, const Bytes& body, int ts) const;
  Bytes hashF13(const Bytes& query, const Bytes& body, int ts) const;
  std::string gorgon(const std::string& query, const Bytes& body, int ts, int random) const;
  std::string medusa(const std::string& url, const Bytes& body, int ts) const;
  std::string helios(int ts) const;
};

// content_repo.hpp
class ContentRepo {
public:
  explicit ContentRepo(Http& http);
  CatalogPage home  (int page = 1, ContentType type = ContentType::Short);
  CatalogPage search(const std::string& keyword, int page = 1, ContentType type = ContentType::Short);
  Detail      detail(const std::string& seriesId);
  StreamInfo  stream(const std::string& episodeId, int maxQuality = 1080);
};

// stream_reader.hpp —— RemoteVideo.kt 的等价物
class StreamReader {
public:
  StreamReader(Http& http, StreamInfo info);
  void    prepare();                        // 拉头部、解析 moov、建样本索引
  void    warm();                           // 预热 1MB 前缀
  Bytes   read(int64_t start, int length);  // 拉取 + 解密
  int64_t total() const;
  bool    hasKey() const;
};

// local_proxy.hpp
class LocalProxy {
public:
  std::string start(StreamReader& reader);  // 返回可喂给 mpv 的 URL
  void        stop();
};
```

### 注入时钟与随机数的必要性

签名里 `ts` 取当前秒、`_rticket` 取当前毫秒，`medusa` / `gorgon` / `helios` 各含一处随机数。
若 `Signer` 内部直接读系统时间，两个实现永远算不出同一输入，字节对拍无从谈起。
生产环境传系统实现，测试环境传固定值。此设计照搬 Kotlin 侧的 `Signer(clock, random)`。

### 为什么内部函数要单独暴露

密码学移植的调试效率取决于"能否定位到函数"。若只暴露 `sign()`，出错时只能看到"请求失败"；
暴露 `hashF13` 后可以直接判断是那 112 轮的问题，还是 `xmxor` 位反转的问题。

## 5. 数据流

### A · 签名请求

1. 组装 query：设备指纹 27 项 + 业务参数 + `ts`（秒）+ `_rticket`（毫秒）
2. `for offset in 0..127`：以 `ts + offset` 为 ticket 计算 `branch()`，直到 `branch == 0`
3. 生成六个签名头。`medusa` 内部串行经过 `hashF13` → `xmxor` → `AesV3` → 自定义编码
4. 发请求；POST 额外携带 `x-ss-stub`（body MD5 的大写十六进制）

### B · 播放地址解析

1. 签名 POST `multi_video_model` → `video_model` → 内部的 `fallback_api` URL
2. 免签名 GET 该 URL → `video_info.data.video_list`（各清晰度）
3. 挑清晰度：数值降序取第一个 ≤ `maxQuality`；**排除 `bytevc2` / `bvc2`**
4. `key_seed` 非空 → `decryptUrl()`（SHA-512 双重哈希 + AES-CBC）解出真实地址
5. `spade_a` 非空 → `deriveKey()` 出 16 字节 AES key
6. 产出 `StreamInfo{ url, key?, quality }`

### C · 本地代理

mpv 播 `http://127.0.0.1:PORT/stream`，代理收到 Range 请求后：

1. 首次先拉 256KB 头部，解析 `moov`，建立逐样本索引（明文区间、IV、加密偏移）
2. 请求区间若不在前缀缓存中，向 CDN 发起 Range 拉取（必须返回 206）
3. 查样本表定位涉及的加密分片
4. AES-CTR 解密，IV 按样本内偏移递增
5. 以 206 + `Content-Range` 返回明文

**为什么必须有本地代理**：红果视频是 CENC 加密的 MP4，mpv 无法在解复用前插入解密逻辑。
hongguoTV 在 Android 上走 `VideoDataSource`、在 Node 服务里走 `/api/episodes/:id/stream`，
是同一思路。本设计把它挪进 C++ 进程内。

## 5.1 上游两版实现的差异（移植前必须知道）

hongguoTV 有两份签名实现：`server/vendor/hongguo.cjs`（Node，服务端在跑）与
`kotlin-tv/core/.../Signer.kt`（Kotlin，Android TV 在跑）。二者**不完全等价**：

| | Node (`hongguo.cjs`) | Kotlin (`Signer.kt`) |
| --- | --- | --- |
| `hashF13` 分支 | 三支全实现（branch 2 走 `branch2F13`，branch 0 走 112 轮主线，其余走简路） | **只实现 branch 0**，其余抛异常 |
| 票据抖动 | `offset < 32`，条件为 `branch !== 1`（即接受 0 或 2） | `offset in 0..127`，条件为 `branch == 0` |
| 依据 | `signedVideoRequest` 注释：branch 1 的变体服务端不认 | `signing.json` 12 组向量全部断言 branch == 0 |

**本设计选择与 Kotlin 一致：强制 branch 0，抖动上限 127。**

理由：branch 0 是唯一被 `signing.json` 逐字节验证过的路径，也是上线 Android TV 客户端在用的路径。
抖动空间足够大——抖动改变 `_rticket` 即改变 query，branch 近似均匀分布，连续 128 次落空的概率
约为 `(2/3)^128`，可忽略。

代价是 `hashF13` 的 branch 1/2 路径不实现。若将来遇到 128 次抖动仍无解的情况，
需回补 `branch2F13`（Node 侧已有完整实现，可直接移植）。

**对拍台位置**：`hongguo-core/tools/oracle/oracle.mjs`。它把 Node 原实现放进 `vm`，
打桩 `Date.now()` 与 `Math.random()`（LCG 种子 17，与 `SignerTest.kt` 同款），
导出全部内部函数。自检脚本 `check-signing.mjs` 已复现 `signing.json` 全部 12 组向量。

## 6. 验证策略

验收标准为**字节对拍 + 真实接口**两档，不使用 mpv。

### 第一档：逐字节对拍

以 hongguoTV 的 `server/vendor/hongguo.cjs`（Node 侧原始实现）为神谕：

- 固定时钟与随机数，同一组输入分别喂给 Node 与 C++
- 比对 `Signer` 各内部函数的输出、完整请求头、解密结果，要求完全一致
- Kotlin 侧既有测试（`SignerTest.kt` / `MediaCryptoTest.kt`）的向量作为第二组金标准

这一档保证"算法是对的"。

### 第二档：真实接口

`hgctl` 直连上游：

- `home` / `search` / `detail` 能返回结构化数据且字段正确
- `fetch` 把某集解密后的分片写盘，用 MP4 解析器验证容器与样本索引完整
- `stream` 起本地代理，连续 Range 请求能稳定返回正确的 `Content-Range` 与数据

这一档保证"接口是活的、我们的用法是对的"。

### 两档的分工

第一档失败 = 移植写错了；第二档失败 = 上游改版了或请求姿势不对。
分开才能快速判断问题归属。

## 7. 里程碑（均为实际结果）

| 阶段 | 内容 | 结果 |
| --- | --- | --- |
| M0 | 对拍台（Node 神谕打桩导出） | ✅ 复现上游 `signing.json` 全部 12 组向量 |
| M1 | 基础层 `bytes` / `digest` / `aes` / `aesv3` / `protobuf` / `vendor` | ✅ 29 项单测；`vendor.hpp` 由神谕生成而非手抄 |
| M2 | `signer` 全链 | ✅ 与神谕逐字节一致；**真实服务端接受签名** |
| M3 | `media_crypto` | ✅ 移植 Kotlin 的 8 项用例（含 2000+ 组非对齐区间 CTR 校验） |
| M4 | `http` + `content_repo` | ✅ `hgctl home/search/detail/play` 全部返回正确数据 |
| M5 | `stream_reader` + `local_proxy` + `hgctl` | ✅ 解密产出可解析 MP4；代理 Range 自测四项全过 |
| M6 | 交付整理 | ✅ `hongguo-core/README.md` |

**端到端证据**：`hgctl fetch` 拉取真实剧集并解密后，`ffprobe` 判定为
HEVC 1080×1920 + AAC 的合法 MP4；`ffmpeg` 无错解出视频帧。H.265 解码对数据错误
极度敏感——密钥流错一个字节就会立刻报错，能干净解出即证明分片解密逐字节正确。

合计 43 项单测全绿。

## 8. 风险

| 风险 | 影响 | 应对 |
| --- | --- | --- |
| 红果接口随时变更 | 功能失效 | 内容层集中在 `content_repo`，改版只动一处；`hgctl` 保留原始响应便于定位 |
| 签名移植细节错（字节序、无符号运算、位反转） | 静默失败 | 第一档逐函数对拍 |
| 清晰度资源被 `bytevc2` 占满 | 无可用流 | 已保留回退到最低档的逻辑；若仍不可播需另找策略 |
| GPL-3.0 传染性 | 公开发布须开源 | 自用无影响；发布时整体以 GPL-3.0 开源 |

## 9. 已知限制

- `headerEnd()` 要求 MP4 的 `moov` 在文件开头且 ≤ 8MB。红果资源满足此前提；
  若遇到尾部 `moov` 的资源会**直接报错**（而非静默出错），届时需补一次尾部 Range 探测。
- 设备指纹为硬编码的 Redmi 机型 + 固定 `device_id`，与 hongguoTV 相同。
- 上游 URL 有时效性，`hgctl play` 打印的地址仅用于调试，不作持久标识。
