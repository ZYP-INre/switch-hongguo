# Switch 端

把红果内容层编成 Nintendo Switch 的 homebrew。

**产物：`hongguo-probe.nro`（1.5 MB）** —— 已在本机构建并核验通过。

---

## 构建

```sh
cd switch
PROXY=http://127.0.0.1:7897 ./build.sh      # 首次会自动准备工具链
```

之后可以直接 `make`。需要环境变量：

```sh
export DEVKITPRO=$HOME/devkitpro/root/opt/devkitpro
export PATH=$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$DEVKITPRO/portlibs/switch/bin:$PATH
```

### 为什么需要 `build.sh` 这么绕

devkitPro 官方源 `pkg.devkitpro.org` 被 Cloudflare 拦截，所有路径都返回 403——
包括包数据库。试过且**不通**的路径：

| 路径 | 结果 |
| --- | --- |
| 官方源直连 | 403，Cloudflare "Attention Required!" |
| Docker 镜像 `devkitpro/devkita64` | Docker Hub 不可达 |
| 清华 / 中科大 / 北外 / 上交 镜像 | 均不收录 devkitPro |
| `downloads.devkitpro.org` | 302 跳回被挡的域名 |
| 社区镜像站 `devkitpro.marisa.ml` | 站点活着，但它的上游代理也 403 |
| brew 的裸机 `aarch64-elf-gcc` | 无 libstdc++，编不了 C++ |
| 走本地代理 | 只换 IP 仍被拦 |

**关键在于拦截依据是 TLS 指纹，不是 IP。** 用 `curl-impersonate` 伪装 Chrome 的
TLS 指纹（JA3/JA4 + HTTP/2 SETTINGS）**再加**一个未被封的出口 IP，两个条件缺一不可：

- 只换 IP（挂代理）→ 仍被指纹规则拦
- 只换指纹 → 仍被 IP 规则拦

`build.sh` 做的就是这件事：拉一份 curl-impersonate，用 `tools/fetch-devkitpro.py`
解析包数据库、算依赖闭包（12 个包）、下载、解包到 `~/devkitpro`。
**不需要 sudo，不写系统目录。**

### 备选：GitHub Actions

`.github/workflows/build-switch.yml` 也能构建（那台机器没有网络限制）。
推送到 GitHub 后产物在 Actions 的 Artifacts 里；打 `v*` 标签会自动建 Release。
不想在本机装工具链时用这条。

---

## 这个 .nro 做什么

它是**诊断程序**，不是最终产品。因为没有 UI，它把内容层的每一段在真机上跑一遍
并打在屏幕上：

```
① 首页        取到多少部剧，列出前几部
② 详情        取到多少集
③ 播放解析    清晰度、是否加密、密钥是否派生成功
④ 解密验证    资源总大小，容器头部是否为 ftyp
⑤ 本地代理    起服务并自测 Range，打印可播放地址
```

按 `+` 退出。

**它的意义**：在没有 UI 的情况下，证明内容层能在真机上完成
联网 → 签名 → 解密 → 起本地流服务这一整条链路。这是 UI 阶段的地基。

**它不是**：能刷剧的成品。那需要把 wiliwili 的界面接上来。

---

## 安装到 Switch

1. 确保 Switch 已装大气层（Atmosphère）自制系统
2. 把 `hongguo-probe.nro` 拷到 SD 卡的 `switch/` 目录
3. 从 hbmenu 启动

---

## 已验证 / 未验证

### 已验证

- ✅ **交叉编译通过**：整个内容层（含 900KB 的 nlohmann/json）用
  `aarch64-none-elf-gcc 16.1.0` 零警告编过，链接成 `static-pie` aarch64 ELF
- ✅ **产物合法**：NRO 魔数在偏移 0x10（NRO 头部设计如此）；`.nacp` 标题为
  「红果内容层诊断」；`.nro` 内含 185 个 `hongguo::` 符号
  （Signer / ContentRepo / LocalProxy / StreamReader / Http / AesV3 …）与中文界面文案
- ✅ **内容层本身**：43 项单测全绿，对真实接口端到端跑通
  （首页/搜索/详情/播放解析/解密/本地代理，ffmpeg 无错解出视频帧）

### 未验证（需要真机）

- ⬜ **运行时行为**：能不能真的联网、签名、解密——只在真机上才知道
- ⬜ **`switch-curl` 的 TLS 后端**：`switch-curl` 依赖 `switch-zlib` + `libnx`，
  但它的 TLS 实现与桌面版 libcurl 可能不同
- ⬜ **libnx 的 BSD socket**：`SO_NOSIGPIPE` 在 libnx 上不存在（已用 `#ifdef` 兜住），
  但真机上是否还需要别的处理不确定
- ⬜ **Switch 的内存**：可用内存远小于桌面，大缓冲是否够用未知
- ⬜ **devkitA64 的 libstdc++ 异常**在真机上的行为

**编译问题已经排除，剩下的是运行时问题。** 这是比之前好得多的处境——
原来连"能不能编"都是未知数。

---

## 构建过程中踩到的坑

改 Makefile 前请先读这节。

1. **`-ffast-math` 会毁掉签名。** 签名链路里多处浮点参与运算
   （`nextRandom32` 的 `rng()*2^32`、protobuf 的 float 编码、`envLaunch` 的取整），
   该选项允许编译器改写浮点语义，会让 Switch 上算出的签名与桌面端不一致——
   症状是"mac 上好好的、Switch 上签名就是不对"，极难定位。已用 `-fno-fast-math`。

2. **必须保留异常。** devkitPro 的模板默认加 `-fno-exceptions`，
   但 hongguo-core 全程用 `std::runtime_error` 做错误处理，关掉就编不过。
   已显式 `-fexceptions`。

3. **`LIBDIRS` 要填根目录**，不是 `lib` 子目录——`-I$(dir)/include` 与
   `-L$(dir)/lib` 是 devkitPro 的规则自己拼的。

4. **`$(INCLUDE)` 由子 make 从环境继承。** devkitPro 的模板靠
   `export INCLUDE := ...` 传给子 make；父 make 里它是空的。
   别因为"父 make 里看到是空的"就把它从 CFLAGS 里删掉。

5. **`$(BUILD)` 必须标记为 `.PHONY`。** 它是个目录，不作 PHONY 的话
   make 会认为"已存在即已达成"，不进入子 make。

6. **链接要用编译器驱动。** devkitPro 的规则写 `$(LD)`，但它并未定义 `LD`，
   于是落到系统 `ld`——而 `-specs=` / `-march=` 只有 gcc 驱动认识。
   已设 `LD = $(CXX)`（用 `=` 而非 `:=`，因为 `CXX` 要等 `switch_rules` 引入
   `base_tools` 后才定义）。

7. **`LIBDIRS` 里不要引用 `$(PORTLIBS)`/`$(LIBNX)`。** 那两个变量由文件末尾才
   include 的 `switch_rules` 定义，而 `:=` 是立即展开，会在上面变成空。已写死路径。

---

## 文件

```
switch/
├── build.sh          一条龙：准备工具链 + 构建 + 核验
├── Makefile          devkitPro 构建（改动前请读上面的坑）
├── source/main.cpp   诊断程序入口
└── README.md         本文件

../tools/fetch-devkitpro.py   解析包数据库、算依赖闭包、下载、解包
```

内容层的源码通过 `SOURCES := source ../hongguo-core/src` 直接编进去，
不做预编译库——避免两边工具链 ABI 不一致的麻烦。
