// The Worker's heatshrink encoder against Marlin's decoder: every file must come back
// byte for byte, and gcode must shrink by about what the module's own encoder manages.
import { heatshrinkEncode } from "../relay/src/heatshrink.js";
import { execFileSync } from "child_process";
import { readFileSync, existsSync } from "fs";
import crypto from "crypto";

const unpack = process.argv[2];
let bad = 0;
const cases = [
  ["empty", new Uint8Array(0)],
  ["one byte", new Uint8Array([65])],
  ["all values", Uint8Array.from({ length: 256 }, (_, i) => i)],
  ["one long run", new Uint8Array(5000).fill(7)],
  ["random", new Uint8Array(crypto.randomBytes(70000))],
];
for (const f of ["cold_cube_head.gcode"]) if (existsSync(f)) cases.push([f, new Uint8Array(readFileSync(f))]);
const g = readFileSync("cold_cube_head.gcode");
const big = new Uint8Array(g.length * 20); for (let i = 0; i < 20; i++) big.set(g, i * g.length);
cases.push(["gcode x20", big]);

for (const [name, data] of cases) {
  const packed = heatshrinkEncode(data);
  const back = execFileSync(unpack, { input: Buffer.from(packed), maxBuffer: 1 << 28 });
  const same = back.length === data.length && Buffer.compare(back, Buffer.from(data)) === 0;
  if (!same) bad = 1;
  console.log(`  ${same ? "ok  " : "FAIL"} ${name}: ${data.length} -> ${packed.length}` + (data.length ? ` (${(data.length / Math.max(1, packed.length)).toFixed(2)}x)` : ""));
}
console.log(bad ? "\nsomething is wrong" : "\nheatshrink JS: all checks passed");
process.exit(bad);
