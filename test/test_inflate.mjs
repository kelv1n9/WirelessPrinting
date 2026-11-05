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
// The window the module has now, and the one an older Worker packed for: a module's
// window may be larger than the file's, never smaller.
for (const [bits, window] of [[14, 16384], [12, 16384], [12, 4096]])
  for (const [name, data] of cases) for (const chunk of [1, 7, 2036]) {
    if (name === "big" && chunk < 2036) continue;
    const packed = Buffer.from(pako.deflate(new Uint8Array(data), { level: 6, windowBits: bits }));
    let back;
    try { back = execFileSync(bin, [String(chunk), String(window)], { input: packed, maxBuffer: 1 << 27 }); } catch (e) { back = Buffer.from("failed " + e.status); }
    if (Buffer.compare(back, data) !== 0) { bad = 1; console.log(`  FAIL ${name} bits ${bits} window ${window} chunk ${chunk}: ${data.length} -> ${back.length}`); }
  }
// And the other way round has to be refused, not written out wrong
const refused = (what, input, window) => {
  try { execFileSync(bin, ["2036", String(window)], { input, stdio: ["pipe", "ignore", "ignore"], maxBuffer: 1 << 27 }); bad = 1; console.log("  FAIL " + what); } catch (e) {}
};
const whole = Buffer.from(pako.deflate(readFileSync("cold_cube_head.gcode"), { windowBits: 14 }));
refused("a file packed wider than the window was taken", Buffer.from(pako.deflate(readFileSync("cold_cube_head.gcode"), { windowBits: 15 })), 4096);
// One byte changed on the way, anywhere: the checksum at the end has to catch it
for (const at of [2, 100, whole.length >> 1, whole.length - 10, whole.length - 1]) {
  const hurt = Buffer.from(whole);
  hurt[at] ^= 0x10;
  refused(`a stream with byte ${at} changed was taken`, hurt, 16384);
}
const cut = Buffer.from(pako.deflate(readFileSync("cold_cube_head.gcode"), { windowBits: 12 })).subarray(0, 5000);
try { execFileSync(bin, ["2036"], { input: cut, stdio: ["pipe", "ignore", "ignore"] }); bad = 1; console.log("  FAIL a cut stream was taken for whole"); } catch (e) {}
console.log(bad ? "\nsomething is wrong" : "module inflater: all checks passed");
process.exit(bad);
