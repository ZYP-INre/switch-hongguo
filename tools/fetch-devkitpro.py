#!/usr/bin/env python3
"""从 devkitPro 官方源取回构建 Switch homebrew 所需的包，装到本地目录。

为什么不用 dkp-pacman：
  1. 官方源 pkg.devkitpro.org 被 Cloudflare 拦（403）
  2. pacman 拒绝非 root 运行，而其自带的 fakeroot 在 macOS 上找不到预载库

绕过的办法是 curl-impersonate（伪装 Chrome 的 TLS 指纹）+ 本地代理——
两者缺一不可：只换 IP 仍会被指纹规则拦，只换指纹仍会被 IP 规则拦。

本脚本自己解析包数据库、算依赖闭包、下载、解包。逻辑直白，出错也好查。
"""

import argparse
import os
import shutil
import subprocess
import sys
import tarfile
from pathlib import Path

# ⚠ 必须用 curl-impersonate，不能用系统 curl。
#   官方源的 Cloudflare 会按 TLS 指纹拦截普通客户端（返回 403 HTML）。
#   同时还需要一个未被封禁的出口 IP——在本机是本地代理。
CURL = "curl_chrome136"

BASE = "https://pkg.devkitpro.org/packages"
REPOS = {
    # 仓库名 -> (db 路径, 包路径前缀)
    "dkp-libs": (f"{BASE}/dkp-libs.db", f"{BASE}"),
    "dkp-osx": (f"{BASE}/osx/x86_64/dkp-osx.db", f"{BASE}/osx/x86_64"),
}

# 构建一个 Switch .nro 真正需要的东西
ROOTS = [
    # ---- 内容层 .nro 需要的最小集合 ----
    "devkita64-gcc",       # aarch64-none-elf 编译器（含 libstdc++）
    "libnx",               # Switch 系统库
    "switch-tools",        # elf2nro、nxlink 等
    "switch-curl",         # libcurl（内容层的唯一外部依赖）
    "switch-pkg-config",   # 供 Makefile 解析 libcurl 的依赖链
    "dkp-toolchain-vars",  # 工具链路径变量

    # ---- wiliwili（UI + 播放）额外需要 ----
    "switch-cmake",            # 提供 cmake/Switch.cmake 工具链文件
    "devkita64-cmake",
    "dkp-cmake-common-utils",
    "switch-libwebp",          # 封面图
    "switch-freetype",         # 字体渲染
    "switch-harfbuzz",
    "switch-libpng",
    "switch-libjpeg-turbo",
    "switch-bzip2",
    "switch-liblzma",
    "switch-libass",        # mpv 依赖
    "switch-libfribidi",
    "switch-liblua51",
    "switch-libplacebo",
    "switch-glm",
    "switch-glad",
    "switch-glfw",        # borealis 的 Switch 平台用 GLFW
    "switch-mbedtls",     # cpr 在 Switch 上的 TLS 后端
    "switch-dav1d",       # ffmpeg 的 AV1 解码器
]


def run(cmd, **kwargs):
    return subprocess.run(cmd, check=True, **kwargs)


def parse_desc(path: Path) -> dict:
    """解析 pacman 的 desc 文件（%KEY%\\n值... 的重复块）。"""
    fields, key = {}, None
    for line in path.read_text(errors="replace").splitlines():
        if line.startswith("%") and line.endswith("%"):
            key = line.strip("%")
            fields[key] = []
        elif key:
            fields[key].append(line)
    out = {}
    for k, v in fields.items():
        # 只有 DEPENDS 之类是列表；其余取首个非空值
        vals = [x for x in v if x.strip()]
        out[k] = vals
    return out


def download(url: str, dest: Path, curl: str) -> bool:
    if dest.exists() and dest.stat().st_size > 0:
        return True
    dest.parent.mkdir(parents=True, exist_ok=True)
    try:
        run([curl, "-sS", "-L", "--max-time", "900", "--retry", "3",
             "-o", str(dest), url])
    except subprocess.CalledProcessError:
        return False
    if not dest.exists() or dest.stat().st_size == 0:
        return False
    # 被 Cloudflare 拦下时返回的是 HTML —— 必须挡在这里，
    # 否则会一路走到解包才报出难懂的错
    magic = dest.read_bytes()[:4]
    if magic[:1] == b"<" or magic[:4] == b"<!DO":
        print(f"\n  ✗ 上游返回了 HTML（疑似被 WAF 拦截）：{url}", file=sys.stderr)
        dest.unlink()
        return False
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dest", default=str(Path.home() / "devkitpro"),
                    help="安装根目录（默认 ~/devkitpro）")
    ap.add_argument("--dry-run", action="store_true", help="只解析依赖，不下载")
    ap.add_argument("--curl", default=None,
                    help=f"下载器（默认 {CURL}）。必须是 curl-impersonate，见脚本顶部说明")
    args = ap.parse_args()

    curl = args.curl or CURL
    if not shutil.which(curl):
        print(f"找不到下载器 {curl}。请先安装 curl-impersonate 并用 --curl 指定路径。",
              file=sys.stderr)
        return 1

    dest = Path(args.dest).expanduser()
    cache = dest / "cache"
    root = dest / "root"
    cache.mkdir(parents=True, exist_ok=True)
    root.mkdir(parents=True, exist_ok=True)

    # ---- 1. 取包数据库 ----
    index = {}   # 包名 -> {filename, repo, depends}
    for repo, (db_url, pkg_base) in REPOS.items():
        db_file = cache / f"{repo}.db"
        print(f"取数据库 {repo} …")
        if not download(db_url, db_file, curl):
            print(f"  ✗ 失败：{db_url}", file=sys.stderr)
            return 1
        with tarfile.open(db_file, "r:gz") as tar:
            for member in tar.getmembers():
                if not member.name.endswith("/desc"):
                    continue
                f = tar.extractfile(member)
                if not f:
                    continue
                tmp = cache / "_one.desc"
                tmp.write_bytes(f.read())
                fields = parse_desc(tmp)
                if "NAME" not in fields or "FILENAME" not in fields:
                    continue
                name = fields["NAME"][0].strip()
                index[name] = {
                    "filename": fields["FILENAME"][0].strip(),
                    "repo": repo,
                    "base": pkg_base,
                    "depends": [d.split(">")[0].split("=")[0].strip()
                                for d in fields.get("DEPENDS", [])],
                    "version": fields.get("VERSION", [""])[0].strip(),
                }
        print(f"  {len(index)} 个包")

    # ---- 2. 算依赖闭包 ----
    want, queue, missing = set(), list(ROOTS), []
    while queue:
        name = queue.pop()
        if name in want:
            continue
        if name not in index:
            missing.append(name)
            continue
        want.add(name)
        queue.extend(index[name]["depends"])

    print(f"\n依赖闭包：{len(want)} 个包")
    for name in sorted(want):
        info = index[name]
        print(f"  {name:28s} {info['version']:12s} [{info['repo']}]")
    if missing:
        print(f"\n⚠ 源里没有这些依赖（可能已在基础系统里）：{', '.join(sorted(set(missing)))}")

    if args.dry_run:
        return 0

    # ---- 3. 下载并解包 ----
    print()
    failed = []
    for i, name in enumerate(sorted(want), 1):
        info = index[name]
        url = f"{info['base']}/{info['filename']}"
        pkg = cache / info["filename"]
        print(f"[{i}/{len(want)}] {name} …", end=" ", flush=True)
        if not download(url, pkg, curl):
            print("下载失败")
            failed.append(name)
            continue
        try:
            # .pkg.tar.zst 里是相对路径（opt/devkitpro/...），直接解到 root 下
            run(["tar", "--zstd", "-xf", str(pkg), "-C", str(root)])
            print("ok")
        except subprocess.CalledProcessError as error:
            print(f"解包失败: {error}")
            failed.append(name)

    if failed:
        print(f"\n✗ 以下包未成功：{', '.join(failed)}", file=sys.stderr)
        return 1

    devkitpro = root / "opt" / "devkitpro"
    print(f"\n✅ 安装完成：{devkitpro}")
    print(f"\n使用方式：")
    print(f"  export DEVKITPRO={devkitpro}")
    print(f"  export PATH=$DEVKITPRO/tools/bin:$DEVKITPRO/devkitA64/bin:$PATH")
    return 0


if __name__ == "__main__":
    sys.exit(main())
