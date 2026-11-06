import assert from "node:assert";
import { tgHtml } from "../relay/src/index.js";
assert.equal(tgHtml("KP3S · idle\n\nFile: a<b>&.gcode"), "<b>KP3S · idle</b>\n\nFile: a&lt;b&gt;&amp;.gcode");
assert.equal(tgHtml("Printing x.gcode."), "Printing x.gcode.");
assert.equal(tgHtml("one\ntwo\n\nthree"), "one\ntwo\n\nthree");
console.log("telegram format ok");
