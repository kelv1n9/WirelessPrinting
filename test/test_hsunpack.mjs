// The module's unpacker, built for the host, against the Worker's packer: every file
// in every chunk size must come back byte for byte.
import { heatshrinkEncode } from "../relay/src/heatshrink.js";
import { execFileSync } from "child_process";
import { readFileSync } from "fs";
import crypto from "crypto";
const bin = process.argv[2];
let bad = 0;
const g = readFileSync("cold_cube_head.gcode");
const cases = [["empty", Buffer.alloc(0)], ["run", Buffer.alloc(3000, 9)], ["random", crypto.randomBytes(50000)], ["gcode", g],
  ["all values", Buffer.from(Array.from({ length: 256 }, (_, i) => i))]];
for (const [name, data] of cases) for (const chunk of [1, 7, 2040]) {
  const packed = Buffer.from(heatshrinkEncode(new Uint8Array(data)));
  const back = execFileSync(bin, [String(chunk)], { input: packed, maxBuffer: 1 << 26 });
  const same = Buffer.compare(back, data) === 0;
  if (!same) { bad = 1; console.log(`  FAIL ${name} chunk ${chunk}: ${data.length} -> ${back.length}`); }
}
console.log(bad ? "\nsomething is wrong" : "module unpacker: all checks passed");
process.exit(bad);
