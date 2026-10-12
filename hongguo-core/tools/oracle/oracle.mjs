// 神谕（oracle）：把 hongguoTV 的 Node 原实现放进受控的 vm 里跑。
//
// 唯一目的：为 C++ 移植提供"逐字节对拍"的参照物。
// 原实现里的不确定性只有两处 —— Date.now() 与 Math.random() —— 全部在此打桩，
// 因此同一个输入必然得到同一个输出。
//
// 注意：hongguo.cjs 是 userscript 形态、没有导出。这里用 provider.mjs 同款手法，
// 把需要的内部函数挂到 this.api 上取出来。

import fs from "node:fs";
import vm from "node:vm";
import crypto from "node:crypto";
import path from "node:path";

const DEFAULT_CJS = path.resolve(
  import.meta.dirname,
  "../../../hongguoTV/server/vendor/hongguo.cjs"
);

// 需要在 vm 外部可见的内部函数（与 hongguo-core 的模块划分一一对应）
const EXPOSED = [
  // 基础层
  "md5", "sm3", "u32", "rol32", "ror32", "ror64", "le32", "be32",
  "zigzag", "varint", "protoField", "proto", "str", "cleanUrl", "pythonUrlEncode",
  // 签名链
  "getIv", "sumMd5", "md5V3Step", "md5SumV3", "branch2F13", "bxor", "hashF13",
  "reverseBits", "rc4Gorgon", "xGorgon", "deviceProto", "xorBytes", "matrix",
  "flat", "aesMix", "xmxor", "keyHash", "buildMedusa", "helios", "branchOf",
  "AesV3", "SBOX",
  // 媒体
  "deriveContentKey", "decryptSpadeUrl", "readBox", "findBox", "boxBody",
  "readU64", "parseTrack", "buildSamples", "replaceFourcc", "replaceSinf",
  "prepareCencHeader", "addCounter", "decryptPartial", "parseRangeHeader",
  "qualityNumber", "qualityLabel", "qualityRows", "parseFallback",
  // 内容
  "extractAssignedJson", "itemToCard", "categoryData", "rankData", "rankItemToCard",
  // 常量（用于程序化导出 vendor.hpp，避免手抄出错）
  "DEVICE", "VIDEO_UA", "APP_UA", "VIDEO_API", "APP_SEARCH_API",
];

/**
 * 创建一个可复现的神谕实例。
 * @param {{ cjsPath?: string }} [opts]
 */
export function createOracle(opts = {}) {
  const cjsPath = opts.cjsPath ?? DEFAULT_CJS;
  const source = fs.readFileSync(cjsPath, "utf8");

  let nowMs = 0;
  let rngState = 0;
  let rngEnabled = false;

  const mathStub = Object.create(Math);
  mathStub.random = function random() {
    if (!rngEnabled) return Math.random();
    // 与 Kotlin 侧 SignerTest 完全一致的 LCG：
    //   seed = (seed * 1664525 + 1013904223) & 0xffffffff;  return seed / 2^32
    rngState = (rngState * 1664525 + 1013904223) >>> 0;
    return rngState / 4294967296;
  };

  const dateStub = Object.create(Date);
  dateStub.now = () => nowMs;

  const context = vm.createContext({
    require: (name) => {
      if (name === "node:crypto") return crypto;
      throw new Error(`oracle: unsupported module ${name}`);
    },
    Buffer,
    URL,
    URLSearchParams,
    axios: { create: () => () => { throw new Error("oracle: network disabled"); } },
    Math: mathStub,
    Date: dateStub,
  });

  vm.runInContext(
    source + `\nthis.api = { ${EXPOSED.join(", ")} };`,
    context,
    { timeout: 5000 }
  );

  const api = context.api;

  return {
    api,

    /** 固定时钟（毫秒）。buildMedusa 内部会取 now/1000。 */
    setNow(ms) {
      nowMs = ms >>> 0 === ms ? ms : ms;
      return this;
    },

    /** 用固定种子开确定性随机；传 null 恢复真随机。 */
    setRng(seed) {
      if (seed === null || seed === undefined) {
        rngEnabled = false;
      } else {
        rngState = seed >>> 0;
        rngEnabled = true;
      }
      return this;
    },

    /** 当前 RNG 内部状态，便于断言"两边消耗了同样多的随机数"。 */
    rngState: () => rngState,

    /** 便捷：按 Kotlin SignerTest 的口径复现一行向量。 */
    vector(row) {
      const { ts, url, body } = row;
      const bodyBuf = body == null ? null : Buffer.from(body);
      const query = url.slice(url.indexOf("?") + 1);
      const querySm3 = api.sm3(query);
      const bodyMd5 = bodyBuf
        ? crypto.createHash("md5").update(bodyBuf).digest()
        : Buffer.alloc(16);

      this.setNow(ts * 1000);
      this.setRng(17); // 与 SignerTest 一致：每行重置

      return {
        branch: api.branchOf(url, bodyBuf, ts),
        hash: api.hashF13(querySm3, bodyMd5, api.le32(ts), ts).toString("hex"),
        gorgon: api.xGorgon(query, bodyBuf, ts, 42131),
        helios: api.helios(ts),
        medusa: api.buildMedusa(url, bodyBuf, ts),
      };
    },
  };
}
