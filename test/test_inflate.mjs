// The module's inflater, built for the host on the same miniz decoder the ROM holds,
// against the Worker's packer: every file in every chunk size must come back exactly.
import pako from "../relay/node_modules/pako/dist/pako.esm.mjs";
import { execFileSync } from "child_process";
import { readFileSync } from "fs";
import crypto from "crypto";
const bin = process.argv[2], extra = process.argv[3];
let bad = 0;
const cases = [["empty", Buffer.alloc(0)], ["run", Buffer.alloc(300000, 9)], ["random", crypto.randomBytes(50000)],
  ["gcode", readFileSync("cold_cube_head.gcode")], ["all values", Buffer.from(Array.from({ length: 256 }, (_, i) => i))]];
if (extra) cases.push(["big", readFileSync(extra)]);
for (const [name, data] of cases) for (const chunk of [1, 7, 2036]) {
  if (name === "big" && chunk < 2036) continue;
  const packed = Buffer.from(pako.deflateRaw(new Uint8Array(data), { level: 9, windowBits: 12 }));
  let back;
  try { back = execFileSync(bin, [String(chunk)], { input: packed, maxBuffer: 1 << 27 }); } catch (e) { back = Buffer.from("failed " + e.status); }
  if (Buffer.compare(back, data) !== 0) { bad = 1; console.log(`  FAIL ${name} chunk ${chunk}: ${data.length} -> ${back.length}`); }
}
const cut = Buffer.from(pako.deflateRaw(readFileSync("cold_cube_head.gcode"), { windowBits: 12 })).subarray(0, 5000);
try { execFileSync(bin, ["2036"], { input: cut, stdio: ["pipe", "ignore", "ignore"] }); bad = 1; console.log("  FAIL a cut stream was taken for whole"); } catch (e) {}
console.log(bad ? "\nsomething is wrong" : "module inflater: all checks passed");
process.exit(bad);
