// 从神谕导出 vendor 常量，生成 C++ 头文件。
//
// 为什么生成而不是手抄：sbox 有 1536 字节、hashTable 有 64 个 32 位字、
// 设备指纹有 40 项且**顺序参与签名**。其中任何一个字节抄错，
// 签名结果就全错，而且几乎无法靠读代码定位。让机器导，是唯一稳妥的做法。
//
// 用法：node tools/oracle/gen-vendor.mjs > include/hongguo/vendor.hpp

import fs from "node:fs";
import path from "node:path";
import { createOracle } from "./oracle.mjs";

const CJS = path.resolve(
  import.meta.dirname,
  "../../../hongguoTV/server/vendor/hongguo.cjs"
);

const oracle = createOracle();
const api = oracle.api;
const src = fs.readFileSync(CJS, "utf8");

// tt02 是 hashF13 内部的局部常量，不在 api 上；从源码文本里取。
function extractIntArray(name) {
  const match = src.match(new RegExp(`const ${name} = \\[([^\\]]+)\\]`));
  if (!match) throw new Error(`未能从源码提取 ${name}`);
  return match[1]
    .split(",")
    .map((piece) => piece.trim())
    .filter(Boolean)
    .map((piece) => Number(BigInt(piece)) >>> 0);
}

const tt02 = extractIntArray("tt02");

function bytesLiteral(name, data, perLine = 16) {
  const lines = [];
  for (let i = 0; i < data.length; i += perLine) {
    const chunk = Array.from(data.subarray(i, i + perLine))
      .map((b) => `0x${b.toString(16).padStart(2, "0")}`)
      .join(", ");
    lines.push(`    ${chunk},`);
  }
  return `inline constexpr uint8_t ${name}[] = {\n${lines.join("\n")}\n};`;
}

function u32Literal(name, values, perLine = 6) {
  const lines = [];
  for (let i = 0; i < values.length; i += perLine) {
    const chunk = values
      .slice(i, i + perLine)
      .map((v) => `0x${v.toString(16).padStart(8, "0")}u`)
      .join(", ");
    lines.push(`    ${chunk},`);
  }
  return `inline constexpr uint32_t ${name}[] = {\n${lines.join("\n")}\n};`;
}

const sbox = Buffer.from(api.SBOX);
const deviceEntries = Object.entries(api.DEVICE);

const out = `// SPDX-License-Identifier: GPL-3.0-only
//
// 本文件由 tools/oracle/gen-vendor.mjs 自动生成，请勿手改。
// 重新生成：node tools/oracle/gen-vendor.mjs > include/hongguo/vendor.hpp
//
// 常量来源：hongguoTV/server/vendor/hongguo.cjs
// 生成时间戳未写入，以保持输出可复现。

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hongguo::vendor {

// ---- 请求头 User-Agent ----

inline constexpr std::string_view kAppUserAgent = ${JSON.stringify(api.APP_UA)};
inline constexpr std::string_view kVideoUserAgent = ${JSON.stringify(api.VIDEO_UA)};

// ---- 上游端点 ----

inline constexpr std::string_view kVideoApi = ${JSON.stringify(api.VIDEO_API)};
// 单集接口。批量接口 multi_video_model 自 2026-10-13 起一律返回 110001，
// 这个仍然可用。
inline constexpr std::string_view kSingleVideoApi =
    "https://api5-normal-sinfonlineb.fqnovel.com/novel/player/video_model/v1/";
inline constexpr std::string_view kAppSearchApi = ${JSON.stringify(api.APP_SEARCH_API)};
inline constexpr std::string_view kSiteBase = "https://hongguoduanju.com";

// ---- 设备指纹 ----
//
// ⚠ 顺序即语义：这些键值会按此顺序拼进 query string，直接进入签名。
// 不要排序、不要去重、不要改用 map。

struct DeviceField {
  std::string_view key;
  std::string_view value;
};

inline constexpr DeviceField kDevice[] = {
${deviceEntries.map(([k, v]) => `    {"${k}", ${JSON.stringify(v)}},`).join("\n")}
};
inline constexpr size_t kDeviceCount = ${deviceEntries.length};

// ---- AesV3 的 sbox ----
//
// 共 1024 字节，按 khronos & 3 切成四片，每片 256 字节。
// 这不是标准 AES 的 sbox。

${bytesLiteral("kSbox", sbox)}

// ---- hashF13 branch 0 的查表 ----

${u32Literal("kHashTable", tt02)}

}  // namespace hongguo::vendor
`;

process.stdout.write(out);
