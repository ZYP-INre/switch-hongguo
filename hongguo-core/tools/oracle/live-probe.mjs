// 用上游原实现（hongguo.cjs）的签名函数，发一次**真实**网络请求。
//
// 目的：判断"播放接口返回空响应"是我们的 C++ 移植写错了，还是上游接口本身挂了。
// 神谕台把网络禁掉了，所以这里只借它的签名，网络用 node 自带的 fetch。
//
// 用法：node live-probe.mjs [分集ID] [接口URL]

import crypto from "node:crypto";
import { createOracle } from "./oracle.mjs";

const episodeId = process.argv[2] ?? "7686713195246930968";
const api = process.argv[3] ?? null;

const { api: hg } = createOracle();
const base = api ?? hg.VIDEO_API;

const payload = JSON.stringify({
  video_id: episodeId,
  biz_param: {
    detail_page_version: 0,
    device_level: 3,
    need_all_video_definition: true,
    video_platform: 1024,
  },
});

const body = Buffer.from(payload);
const khronos = Math.floor(Date.now() / 1000);
const ticketBase = Date.now();
let requestTicket = ticketBase;
let query = "";
let url = "";

for (let offset = 0; offset < 32; offset += 1) {
  requestTicket = ticketBase + offset;
  const values = { ...hg.DEVICE, ts: String(khronos), _rticket: String(requestTicket) };
  query = hg.pythonUrlEncode(values);
  url = `${base}?${query}`;
  if (hg.branchOf(url, body, khronos) !== 1) break;
}

const random = Math.floor(Math.random() * 65536);
const headers = {
  "User-Agent": hg.VIDEO_UA,
  Accept: "application/json; charset=utf-8,application/x-protobuf",
  "Content-Type": "application/json; charset=UTF-8",
  "x-xs-from-web": "0",
  "x-ss-req-ticket": String(requestTicket),
  "x-tt-request-tag": "t=0;n=0",
  "sdk-version": "2",
  "passport-sdk-version": "50561",
  "x-vc-bdturing-sdk-version": "3.7.2.cn",
  "x-ss-stub": crypto.createHash("md5").update(body).digest("hex").toUpperCase(),
  "x-gorgon": hg.xGorgon(query, body, khronos, random),
  "x-khronos": String(khronos),
  "x-ladon": hg.be32(khronos).toString("base64"),
  "x-argus": hg.le32(khronos).toString("base64"),
  "x-helios": hg.helios(khronos),
  "x-medusa": hg.buildMedusa(url, body, khronos),
  "x-tt-dt": "",
};

// HGSABOTAGE=medusa|gorgon|ticket ：故意发一个坏签名，用来对照"空 200"是不是通用拒绝。
const sabotage = process.env.HGSABOTAGE;
if (sabotage === "medusa") headers["x-medusa"] = "AAAA";
if (sabotage === "gorgon") headers["x-gorgon"] = "00000000000000000000";
if (sabotage === "ticket") headers["x-ss-req-ticket"] = "1";

const resp = await fetch(url, { method: "POST", headers, body });
const text = await resp.text();
console.log(`POST ${base}`);
console.log(`HTTP ${resp.status}, ${Buffer.byteLength(text)} bytes`);
console.log(text.slice(0, 500) || "(空响应)");
