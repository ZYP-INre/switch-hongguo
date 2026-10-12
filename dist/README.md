# 交付物

## `hongguo.nro`（37.5 MB）— 成品

**hbmenu 里显示为「红果短剧」（作者 hongguo-switch），带独立图标。**

红果短剧的 Switch 客户端。基于 wiliwili 的界面与播放栈，内容层是自研的 `hongguo-core`。

```
启动 → 首页列表（红果的剧）
      → 选中一部 → 分集列表
                  → 选中一集 → 播放（mpv 播放本地代理解密后的流）
```

按 `B` 返回上一层。播放页的控制（暂停、进度、音量、全屏）沿用 wiliwili 的 OSD。

## `hongguo-probe.nro`（1.5 MB）— 诊断程序

不含 UI，只在屏幕上打印内容层各环节的执行结果。用于在真机上快速定位问题：
如果诊断版能跑通而成品不能，问题在界面层；反之在内容层。

---

## 安装

1. Switch 需已安装大气层（Atmosphère）自制系统
2. 把 `.nro` 拷到 SD 卡的 `switch/` 目录
3. 从 hbmenu 启动

---

## 从源码构建

### 成品（wiliwili + 红果内容层）

```sh
export DEVKITPRO=$HOME/devkitpro/root/opt/devkitpro
export PATH=$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$DEVKITPRO/portlibs/switch/bin:$PATH
export PKG_CONFIG_PATH=$DEVKITPRO/portlibs/switch/lib/pkgconfig
export PKG_CONFIG_LIBDIR=$DEVKITPRO/portlibs/switch/lib/pkgconfig

cd wiliwili
cmake -B cmake-build-switch -DCMAKE_BUILD_TYPE=Release -DPLATFORM_SWITCH=ON \
      -DBRLS_UNITY_BUILD=ON -DBUILTIN_NSP=OFF
make -C cmake-build-switch wiliwili.nro -j$(sysctl -n hw.ncpu)
```

产物在 `wiliwili/cmake-build-switch/hongguo.nro`。

加 `-DHONGGUO_UI=OFF` 可以编回原版 wiliwili 界面——红果的接入是**完全自包含**的，
不碰原有 B 站代码，出问题也不会影响 wiliwili 本身。

### 诊断版

```sh
cd switch && PROXY=http://127.0.0.1:7897 ./build.sh
```

### 工具链

`tools/fetch-devkitpro.py` 会准备 devkitPro 工具链（含 mpv/ffmpeg 的 Switch 版）。
为什么需要它见 `switch/README.md`——简言之：官方源被 Cloudflare 按 **TLS 指纹**拦截，
需要 `curl-impersonate` 伪装 Chrome 指纹、加一个未被封的出口 IP，两者缺一不可。

---

## 已验证 / 未验证

### 已验证（在本机）

- ✅ **内容层**：43 项单测全绿；对真实接口端到端跑通——首页/搜索/详情/播放解析/解密/
  本地代理，`ffprobe` 判定为 HEVC 1080×1920 + AAC 合法 MP4，`ffmpeg` 无错解出视频帧
- ✅ **交叉编译**：wiliwili + borealis + nanovg + mpv + ffmpeg + hongguo-core
  全部用 devkitA64 零错误编过
- ✅ **产物结构**：NRO0 合法；含 282 个 `hongguo::` 符号；
  红果的 XML 布局与中文文案都在 romfs 里

### 未验证（需要真机）

- ⬜ **运行时行为**——能不能真的联网、签名、解密、出画。编译问题已排除，
  剩下的是运行时问题
- ⬜ `switch-curl` / `mbedtls` 的 TLS 后端与桌面端是否一致（签名链路对字节敏感）
- ⬜ libnx 的 BSD socket 与 macOS 的差异（本地代理用到了）
- ⬜ Switch 的内存：wiliwili 本身 + 内容层 + mpv 解码缓冲是否够用
- ⬜ 竖屏视频（1080×1920）在 16:9 电视上的显示效果——目前是 wiliwili 默认的适配策略

**建议先跑 `hongguo-probe.nro`**：它把内容层每一段的结果打在屏幕上，
能快速区分"内容层的问题"还是"界面层的问题"。
