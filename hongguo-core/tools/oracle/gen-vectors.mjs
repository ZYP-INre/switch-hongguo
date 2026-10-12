// 生成 C++ 测试用的向量文件（tests/vectors/*.txt）。
//
// 格式极简：每行 `<op>\t<参数...>`，全十六进制。C++ 侧自己解析，
// 不引 JSON 库 —— 测试要能在 devkitPro 上一起编。
//
// 参照来源：
//   digest / aes  → Node 标准 crypto（这两个是标准算法，标准库即权威）
//   aesv3 / proto → 神谕（红果的私有变体，只有原实现算数）

import fs from "node:fs";
import path from "node:path";
import crypto from "node:crypto";
import { createOracle } from "./oracle.mjs";

const OUT_DIR = path.resolve(import.meta.dirname, "../../tests/vectors");
fs.mkdirSync(OUT_DIR, { recursive: true });

const oracle = createOracle();
const api = oracle.api;

const hex = (buf) => Buffer.from(buf).toString("hex");

// 确定性伪随机数据（不用 Math.random —— 输出必须可复现）
function pseudoBytes(seed, length) {
  const out = Buffer.alloc(length);
  let x = seed >>> 0;
  for (let i = 0; i < length; i += 1) {
    x = (x * 1664525 + 1013904223) >>> 0;
    out[i] = (x >>> 24) & 0xff;
  }
  return out;
}

const lines = { digest: [], aes: [], aesv3: [], proto: [], signer: [] };

// ---------------------------------------------------------------- digest
//
// 长度覆盖各算法的填充边界：MD5/SM3 块长 64（边界 55/56/64），
// SHA-512 块长 128（边界 111/112/128）。

const LENGTHS = [0, 1, 2, 3, 55, 56, 57, 63, 64, 65, 111, 112, 113, 127, 128, 129, 1000];
for (const length of LENGTHS) {
  const data = pseudoBytes(0x5eed + length, length);
  lines.digest.push(`md5\t${hex(data)}\t${hex(crypto.createHash("md5").update(data).digest())}`);
  lines.digest.push(`sha512\t${hex(data)}\t${hex(crypto.createHash("sha512").update(data).digest())}`);
  lines.digest.push(`sm3\t${hex(data)}\t${hex(crypto.createHash("sm3").update(data).digest())}`);
}

// ------------------------------------------------------------------- aes

for (const length of [16, 32, 48, 64, 256]) {
  const key = pseudoBytes(0xa1 + length, 16);
  const iv = pseudoBytes(0xb2 + length, 16);
  const data = pseudoBytes(0xc3 + length, length);

  // CTR 是对合运算：解密即加密
  const cipher = crypto.createCipheriv("aes-128-ctr", key, iv);
  const stream = Buffer.concat([cipher.update(data), cipher.final()]);
  lines.aes.push(`ctr\t${hex(key)}\t${hex(iv)}\t${hex(data)}\t${hex(stream)}`);

  // CBC 用 NoPadding：取整块长度，密文即直接喂给解密的输入
  const enc = crypto.createCipheriv("aes-128-cbc", key, iv);
  enc.setAutoPadding(false);
  const cbcCiphertext = Buffer.concat([enc.update(data), enc.final()]);
  lines.aes.push(`cbc\t${hex(key)}\t${hex(iv)}\t${hex(cbcCiphertext)}\t${hex(data)}`);
}

// ----------------------------------------------------------------- aesv3
//
// 私有变体，只有原实现算数。输入必须 ≥ 248 字节（原实现有 require）。

for (const khronos of [1800500000, 1800500001, 1800500002, 1800500003]) {
  const key = Buffer.from("f1593376766ea98d34f31b057a9d5be4", "hex");
  const iv = Buffer.from("1fe109a4125283f418de9e051a969e12", "hex");
  const data = pseudoBytes(0xd4 + khronos, 288);
  const out = new api.AesV3(key, khronos).encrypt(data, iv);
  lines.aesv3.push(`aesv3\t${hex(key)}\t${hex(iv)}\t${khronos}\t${hex(data)}\t${hex(out)}`);
}

// ----------------------------------------------------------------- proto
//
// deviceProto 是 40 项、含 sint/float/bytes 混合的复合报文，
// 能同时覆盖 varint / zigzag / 小端 float / 嵌套消息四条路径。

for (const [deviceId, version] of [
  ["1905892595378490", "7.1.3.32"],
  ["1", "0"],
  ["9999999999999999", "9.9.9.99"],
]) {
  const out = api.deviceProto(deviceId, version);
  lines.proto.push(`deviceproto\t${deviceId}\t${version}\t${hex(out)}`);
}

// ---------------------------------------------------------------- signer
//
// 复刻 hongguoTV 的 signing.json（上游从 JS 生成、并已验证 Kotlin 移植一致），
// 再补一批神谕生成的新用例。两者共用同一套确定性前提：
//   时钟 = ts * 1000 毫秒；RNG = LCG(种子 17)，每行重置；
//   gorgon 的随机数固定为 42131。
// 调用顺序也必须是 branch → hashF13 → gorgon → helios → medusa，
// 因为 helios 与 medusa 会消耗 RNG，顺序不同结果就不同。

const SIGNING_JSON = path.resolve(
  import.meta.dirname,
  "../../../hongguoTV/kotlin-tv/core/src/test/resources/signing.json"
);

const signerCases = JSON.parse(fs.readFileSync(SIGNING_JSON, "utf8"));

// 追加用例：覆盖不同 url 形态、有无 body、以及时间戳跨一秒的情况
signerCases.push(
  { ts: 1700000000, url: "https://api5-normal-sinfonlinea.fqnovel.com/reading/bookapi/search/tab/v?query=%E7%9F%AD%E5%89%A7&count=20&offset=0", body: null },
  { ts: 1700000001, url: "https://api5-normal-sinfonlineb.fqnovel.com/novel/player/multi_video_model/v1/?iid=1&device_id=2", body: '{"biz_param":{"device_level":3},"mixed_video_id_map":{"1004":["7391"]}}' },
  { ts: 1999999999, url: "https://example.com/x?a=1&b=%20%2B~*", body: "" },
  { ts: 1234567890, url: "https://example.com/y?empty=", body: null },
  { ts: 1800500000, url: "https://example.com/api?q=%E7%9F%AD%E5%89%A7&ticket=6", body: '{"video":"1234567890"}' }
);

let emitted = 0;
let skipped = 0;
for (const row of signerCases) {
  const got = oracle.vector(row);
  // C++ 侧强制 branch 0（与 Kotlin 一致），非 0 的用例不纳入
  if (got.branch !== 0) {
    skipped += 1;
    continue;
  }
  // body 字段用 "-" 表示"无 body"；空字符串表示"body 为空字节序列"。
  // 两者在签名里含义不同（md5(空字节) vs 16 个零字节），必须区分。
  const bodyHex = row.body == null ? "-" : Buffer.from(row.body).toString("hex");
  lines.signer.push(
    [row.ts, row.url, bodyHex, got.hash, got.gorgon, got.helios, got.medusa].join("\t")
  );
  emitted += 1;
}

// ------------------------------------------------------------------ 落盘

for (const [name, rows] of Object.entries(lines)) {
  const file = path.join(OUT_DIR, `${name}.txt`);
  const header = `# 由 tools/oracle/gen-vectors.mjs 生成，请勿手改\n`;
  fs.writeFileSync(file, header + rows.join("\n") + "\n");
  console.log(`${name}.txt  ${rows.length} 条`);
}
if (skipped > 0) console.log(`（signer 跳过 ${skipped} 条非 branch 0 用例）`);
