// 对拍台自检：Node 神谕能否复现 hongguoTV 的 signing.json？
//
// 这份 signing.json 是上游从 JS 生成、并已验证 Kotlin 移植与之逐字节一致的锚点。
// 若本脚本能复现它，说明神谕的打桩（时钟 + 随机数）与调用顺序都正确，
// 之后就可以放心用它去校验 C++ 实现。

import fs from "node:fs";
import path from "node:path";
import { createOracle } from "./oracle.mjs";

const VECTORS = path.resolve(
  import.meta.dirname,
  "../../../hongguoTV/kotlin-tv/core/src/test/resources/signing.json"
);

const rows = JSON.parse(fs.readFileSync(VECTORS, "utf8"));
const oracle = createOracle();

const FIELDS = ["hash", "gorgon", "helios", "medusa"];
let pass = 0;
let fail = 0;

for (let i = 0; i < rows.length; i += 1) {
  const row = rows[i];
  const got = oracle.vector(row);
  const problems = [];

  if (got.branch !== 0) problems.push(`branch=${got.branch}（期望 0）`);
  for (const field of FIELDS) {
    if (got[field] !== row[field]) {
      problems.push(
        `${field}\n        期望 ${row[field]}\n        实得 ${got[field]}`
      );
    }
  }

  if (problems.length === 0) {
    pass += 1;
    console.log(`  ✓ [${i}] ts=${row.ts} body=${row.body ? "有" : "无"}`);
  } else {
    fail += 1;
    console.log(`  ✗ [${i}] ts=${row.ts} body=${row.body ? "有" : "无"}`);
    for (const p of problems) console.log(`      ${p}`);
  }
}

console.log();
console.log(`结果：${pass} 通过 / ${fail} 失败 / 共 ${rows.length}`);
process.exit(fail === 0 ? 0 : 1);
