#pragma once

#include <Arduino.h>

// The page the module had before the architecture moved under it, brought back and
// fitted to what this build can honestly tell it. What it cannot know yet, the gcode
// it has not read, it leaves as a dash rather than inventing.
static const char PAGE_HTML[] = R"PAGE(<!doctype html>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>KP3S</title>
<link rel="icon" href="data:image/svg+xml,%3Csvg xmlns='http://www.w3.org/2000/svg' viewBox='0 0 32 32'%3E%3Crect width='32' height='32' rx='7' fill='%235b9cff'/%3E%3Cpath d='M8 21l8-11 8 11z' fill='%230d0f12'/%3E%3C/svg%3E">
<style>
:root{
  --bg:#0d0f12;--panel:#15181d;--sunk:#1b1f26;--line:#262b33;
  --tx:#e8ebf0;--dim:#8a93a2;--acc:#5b9cff;--ok:#3ddc84;--warn:#ffb020;--bad:#ff5f56;
  --hot:#ff7a45;--bed:#ffc14d;
  --hov:#1a1f28;--sel:#161d2a;--rim:#3a424e;--onacc:#06101f;--acc2:#7aaeff;
  --okrim:#1e4433;--busyrim:#22406e;--badrim:#5a2724;--warnrim:#5a4520;
}
/* The day palette is the one the stripped down page wore while the architecture was
   being rebuilt. Night is the original. Auto follows the system and is the default. */
@media(prefers-color-scheme:light){:root:not([data-t=night]){
  --bg:#f6f7f9;--panel:#fff;--sunk:#eef1f5;--line:#d9dee6;
  --tx:#14181d;--dim:#5d6672;--acc:#2f6fd0;--ok:#1c8a4e;--warn:#9a6300;--bad:#c62d24;
  --hot:#cf5420;--bed:#a9790c;
  --hov:#eef2f8;--sel:#e4edfb;--rim:#aab4c2;--onacc:#fff;--acc2:#2258a8;
  --okrim:#9fd9bb;--busyrim:#a8c6f3;--badrim:#edb3ae;--warnrim:#e3c48a;
}}
:root[data-t=day]{
  --bg:#f6f7f9;--panel:#fff;--sunk:#eef1f5;--line:#d9dee6;
  --tx:#14181d;--dim:#5d6672;--acc:#2f6fd0;--ok:#1c8a4e;--warn:#9a6300;--bad:#c62d24;
  --hot:#cf5420;--bed:#a9790c;
  --hov:#eef2f8;--sel:#e4edfb;--rim:#aab4c2;--onacc:#fff;--acc2:#2258a8;
  --okrim:#9fd9bb;--busyrim:#a8c6f3;--badrim:#edb3ae;--warnrim:#e3c48a;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);font:14px/1.5 -apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,sans-serif}
a{color:var(--acc);text-decoration:none}
header{display:flex;align-items:center;gap:14px;padding:14px 22px;border-bottom:1px solid var(--line);flex-wrap:wrap}
h1{font-size:16px;margin:0;font-weight:600;letter-spacing:.3px}
.badge{font-size:12px;padding:3px 10px;border-radius:99px;background:var(--sunk);border:1px solid var(--line);color:var(--dim)}
.badge.live{color:var(--ok);border-color:var(--okrim)}
.badge.busy{color:var(--acc);border-color:var(--busyrim)}
.badge.bad{color:var(--bad);border-color:var(--badrim)}
.badge.warn{color:var(--warn);border-color:var(--warnrim)}
tr.pick{cursor:pointer}
tr.pick:hover td{background:var(--hov)}
.spacer{flex:1}
.meta{font-size:12px;color:var(--dim);display:flex;gap:14px;flex-wrap:wrap}
#preheat{margin-top:14px}
#bars{flex:none;margin-right:-6px}
#bars rect{fill:var(--line)}
#bars rect.on{fill:var(--tx)}
#bars.weak rect.on{fill:var(--warn)}
#bars.gone rect.on{fill:var(--bad)}
main{padding:22px;display:grid;gap:18px;grid-template-columns:minmax(0,2fr) minmax(300px,1fr);max-width:1400px;margin:0 auto}
@media(max-width:900px){main{grid-template-columns:1fr;padding:14px}}
section{background:var(--panel);border:1px solid var(--line);border-radius:12px;padding:18px}
section h2{margin:0 0 14px;font-size:12px;text-transform:uppercase;letter-spacing:.9px;color:var(--dim);font-weight:600}
.wide{grid-column:1/-1}
.job{display:flex;gap:18px;align-items:flex-start}
.thumb{width:118px;height:118px;border-radius:10px;background:var(--sunk);border:1px solid var(--line);object-fit:contain;flex:none}
.noimg{visibility:hidden}
.jobmain{flex:1;min-width:0}
.fname{font-size:15px;font-weight:600;word-break:break-all;margin-bottom:12px}
.pct{font-size:40px;font-weight:650;line-height:1;letter-spacing:-1px}
.pct small{font-size:16px;color:var(--dim);font-weight:500;margin-left:4px}
.bar{height:7px;border-radius:99px;background:var(--sunk);overflow:hidden;margin:12px 0 16px}
.bar i{display:block;height:100%;background:var(--acc);width:0;transition:width .4s}
.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(112px,1fr));gap:14px 18px}
.kv .k{font-size:11px;color:var(--dim);text-transform:uppercase;letter-spacing:.6px}
.kv .v{font-size:17px;font-weight:600;margin-top:2px;font-variant-numeric:tabular-nums}
.kv .v.sm{font-size:14px;font-weight:500}
.temp{display:flex;align-items:baseline;gap:8px;margin-bottom:4px}
.temp .now{font-size:26px;font-weight:650;font-variant-numeric:tabular-nums}
.temp .tgt{font-size:13px;color:var(--dim);font-variant-numeric:tabular-nums}
.tlabel{font-size:11px;text-transform:uppercase;letter-spacing:.6px;color:var(--dim);margin-bottom:6px}
.spark{display:block;width:100%;height:34px;margin-top:6px}
.tbox{padding:10px;border-radius:10px;margin:0 -10px}
.tbox+.tbox{margin-top:8px}
.settable{cursor:pointer}
.settable:hover{background:var(--line)}
.hint{float:right;text-transform:none;letter-spacing:0;opacity:0;transition:opacity .15s}
.settable:hover .hint{opacity:.6}
button{font:inherit;color:var(--tx);background:var(--sunk);border:1px solid var(--line);border-radius:8px;padding:7px 13px;cursor:pointer}
button:hover:not(:disabled){border-color:var(--rim);background:var(--hov)}
button:disabled{opacity:.4;cursor:default}
button.p{background:var(--acc);border-color:var(--acc);color:var(--onacc);font-weight:600}
button.p:hover:not(:disabled){background:var(--acc2);border-color:var(--acc2)}
button.d{color:var(--bad)}
.row{display:flex;gap:9px;flex-wrap:wrap;align-items:center}
table{width:100%;border-collapse:collapse}
td,th{padding:9px 10px;border-bottom:1px solid var(--line);text-align:left;vertical-align:middle}
th{font-size:11px;text-transform:uppercase;letter-spacing:.6px;color:var(--dim);font-weight:600}
tr:last-child td{border-bottom:0}
tr.sel td{background:var(--sel)}
.tmini{width:40px;height:40px;border-radius:6px;background:var(--sunk);object-fit:contain;display:block}
.nm{font-weight:500;word-break:break-all;width:100%}
.thumbcell{width:1%}
.acts{width:1%;white-space:nowrap}
.acts .row{justify-content:flex-end;flex-wrap:nowrap}
.dim{color:var(--dim)}
.num{font-variant-numeric:tabular-nums;white-space:nowrap}
.foot{display:flex;justify-content:space-between;gap:12px;margin-top:14px;flex-wrap:wrap;align-items:center}
progress{width:190px;height:7px;accent-color:var(--acc)}
.side{display:grid;gap:18px;align-content:start;min-width:0}
#conlog{height:300px;overflow-y:auto;background:var(--sunk);border:1px solid var(--line);border-radius:10px;
  padding:10px 12px;font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:12px;line-height:1.55;
  white-space:pre-wrap;word-break:break-word}
#conlog .out{color:var(--acc)}
#conlog .in{color:var(--tx)}
#conlog .empty{color:var(--dim)}
#conform{display:flex;gap:8px;margin-top:12px}
#concmd{flex:1;min-width:0;background:var(--sunk);border:1px solid var(--line);border-radius:8px;
  padding:9px 11px;color:var(--tx);font-family:ui-monospace,SFMono-Regular,Menlo,monospace;font-size:13px}
#concmd:focus{outline:none;border-color:var(--acc)}
.diag{display:grid;grid-template-columns:repeat(auto-fit,minmax(140px,1fr));gap:12px 18px;font-size:13px}
.diag b{display:block;font-size:11px;color:var(--dim);text-transform:uppercase;letter-spacing:.6px;font-weight:600}
/* The printer's card lives in the narrow column, where a name and a button cannot
   sit side by side without pushing one of them off the edge. */
.hide{display:none!important}
#toast{position:fixed;left:50%;bottom:26px;transform:translateX(-50%);background:var(--panel);border:1px solid var(--line);padding:10px 18px;border-radius:9px;opacity:0;transition:opacity .25s;pointer-events:none;z-index:9}
#toast.on{opacity:1}

/* A phone is 375 points wide. The files table cannot hold a thumbnail, a long name
   and three buttons across that, and when it tries the name ends up one letter per
   line, so on a narrow screen each row becomes a little card instead. */
@media(max-width:560px){
  header{padding:11px 14px;gap:9px}
  header .meta{font-size:11px;gap:10px;width:100%;order:5}
  header .row{width:100%;order:6}
  header .row button{flex:1}
  .spacer{display:none}

  main{padding:12px;gap:12px}
  section{padding:14px;border-radius:11px}

  .job{gap:12px}
  .thumb{width:84px;height:84px}
  .pct{font-size:34px}
  .grid{grid-template-columns:repeat(2,minmax(0,1fr));gap:12px 14px}
  .kv .v{font-size:15px}

  #filesec table,#filesec tbody,#filesec tr,#filesec td{display:block;width:auto}
  #filesec tr{position:relative;padding:11px 0 11px 52px;min-height:52px;border-bottom:1px solid var(--line)}
  #filesec tr:last-child{border-bottom:0}
  #filesec td{border:0;padding:0}
  #filesec .thumbcell{position:absolute;left:0;top:11px;width:40px}
  #filesec .nm{word-break:break-word;margin-bottom:9px;line-height:1.35}
  #filesec .acts{width:auto;white-space:normal}
  #filesec .acts .row{justify-content:flex-start;flex-wrap:wrap}
  #filesec tr.sel td{background:transparent}
  #filesec tr.sel{background:var(--sel);border-radius:9px;padding-left:52px}

  #bprint,#bcancel{flex:1;white-space:nowrap;padding:10px 6px}
  #jobsec label{width:100%;text-align:left}
  #conlog{height:190px}
  button{padding:9px 14px}
  progress{width:120px}
}

/* Narrower still, and the preview earns its place less than the name does. */
@media(max-width:360px){
  .grid{grid-template-columns:1fr 1fr;gap:10px}
  #filesec tr{padding-left:0}
  #filesec tr.sel{padding:11px 10px}
  #filesec .thumbcell{display:none}
}
.open{margin:0;padding:.6rem 1rem;background:var(--warn);color:var(--onacc);font-weight:600}
.open a{color:inherit}
</style>

<header>
  <svg id="bars" viewBox="0 0 18 14" width="18" height="14" aria-hidden="true"><title></title>
    <rect x="0"  y="9" width="4" height="5"  rx="1"/>
    <rect x="7"  y="5" width="4" height="9"  rx="1"/>
    <rect x="14" y="0" width="4" height="14" rx="1"/>
  </svg>
  <h1 id="machine">printer</h1>
  <span class="badge" id="state">connecting</span>
  <span class="spacer"></span>
  <span class="badge busy hide" id="xfer"></span>
  <span class="badge warn hide" id="soon"></span>
  <div class="meta">
    <span id="build"></span>
    <span><a href="/yandex">socket</a></span>
    <span><a href="/auth">password</a></span>
  </div>
  <div class="row">
    <button id="btheme" title="day, night, or whatever the system says">auto</button>
    <button id="brestart">Restart module</button>
    <button class="d" id="bpower">Turn printer off</button>
  </div>
</header>
<p class="open hide" id="unlocked">No password is set. Anyone on this network can print,
upload and reflash. <a href="/auth">Set one</a>.</p>

<main>
  <section class="wide" id="jobsec">
    <h2 id="jobtitle">Job</h2>
    <div class="job">
      <img class="thumb" id="jthumb" alt="" onerror="this.classList.add('noimg')">
      <div class="jobmain">
        <div class="fname" id="jname">—</div>
        <div class="pct"><span id="jpct">0</span><small>%</small></div>
        <div class="bar"><i id="jbar"></i></div>
        <div class="grid">
          <div class="kv"><div class="k">Layer</div><div class="v" id="jlayer">—</div></div>
          <div class="kv"><div class="k">Height</div><div class="v" id="jz">—</div></div>
          <div class="kv"><div class="k">Elapsed</div><div class="v" id="jel">—</div></div>
          <div class="kv"><div class="k">Remaining</div><div class="v" id="jrem">—</div></div>
          <div class="kv"><div class="k">Finishes</div><div class="v" id="jend">—</div></div>
          <div class="kv"><div class="k">Fan</div><div class="v" id="jfan">—</div></div>
          <div class="kv hide" id="kwatt"><div class="k">Drawing</div><div class="v" id="jwatt">—</div></div>
          <div class="kv hide" id="kgram"><div class="k">Filament</div><div class="v" id="jgram">—</div></div>
        </div>
        <div class="row" style="margin-top:16px">
          <button class="p" id="bprint">Print selected</button>
          <button class="d" id="bcancel">Cancel print</button>
          <span class="spacer"></span>
          <span class="dim num" id="xline"></span>
          <label class="dim" style="font-size:13px">switch the printer off when done
            <input type="checkbox" id="cauto" style="margin-left:6px;vertical-align:middle">
          </label>
        </div>
      </div>
    </div>
  </section>

  <section id="filesec">
    <h2>On the printer</h2>
    <table><tbody id="ptab"><tr><td class="dim">asking the printer</td></tr></tbody></table>
    <div class="foot">
      <div class="row">
        <button class="p" id="bpickp">Upload and print</button>
        <button id="bpick">Upload</button>
        <input type="file" id="file" accept=".gcode,.gco,.g" hidden>
        <button id="bpcard">Refresh</button>
      </div>
      <div class="row">
        <progress id="uprog" class="hide" max="100" value="0"></progress>
      </div>
    </div>
    <details id="stage">
      <summary class="dim" style="cursor:pointer;margin-top:14px;font-size:13px">Module card, on the way through</summary>
      <table><tbody id="ftab"></tbody></table>
      <div class="foot"><span class="dim num" id="space"></span></div>
    </details>
  </section>

  <div class="side">
  <section id="tempsec">
    <h2>Temperatures</h2>
    <div class="tbox settable" id="boxn">
      <div class="tlabel">Nozzle<span class="hint">click to set</span></div>
      <div class="temp"><span class="now" id="tn">—</span><span class="tgt" id="tnt"></span></div>
      <svg class="spark" id="sn" viewBox="0 0 200 34" preserveAspectRatio="none"><polyline fill="none" stroke="var(--hot)" stroke-width="1.6" points=""/></svg>
    </div>
    <div class="tbox settable" id="boxb">
      <div class="tlabel">Bed<span class="hint">click to set</span></div>
      <div class="temp"><span class="now" id="tb">—</span><span class="tgt" id="tbt"></span></div>
      <svg class="spark" id="sb" viewBox="0 0 200 34" preserveAspectRatio="none"><polyline fill="none" stroke="var(--bed)" stroke-width="1.6" points=""/></svg>
    </div>
    <div class="row" id="preheat">
      <button data-hot="245" data-bed="85">PETG 245 / 85</button>
      <button data-hot="200" data-bed="60">PLA 200 / 60</button>
      <button class="d" data-hot="0" data-bed="0">Cool down</button>
    </div>
  </section>

  <section id="consec">
    <h2>Console</h2>
    <div id="conlog"><div class="empty">nothing yet, send a command</div></div>
    <form id="conform" autocomplete="off">
      <input id="concmd" placeholder="M105, G28, M117 hello" spellcheck="false">
      <button type="submit" class="p">Send</button>
    </form>
  </section>

  <section id="fwsec">
    <h2>Module firmware</h2>
    <div class="row">
      <button id="bfw">Flash a new image</button>
      <input type="file" id="fwfile" accept=".bin" hidden>
      <progress id="fwprog" class="hide" max="100" value="0"></progress>
    </div>
    <p class="dim" style="margin:10px 0 0;font-size:12px">The module keeps the image it
    is running and goes back to it if the new one does not settle, so a bad flash is not
    the end. The printer keeps printing throughout.</p>
  </section>
  </div>

</main>

<div id="toast"></div>

<script>
const $ = id => document.getElementById(id);
const hist = {n:[], b:[]};
let files = [], seen = null;

const hhmm = s => { s = Math.max(0, Math.round(s));
  const h = Math.floor(s/3600), m = Math.floor(s%3600/60);
  return (h ? h+'h ' : '') + (h||m ? m+'m ' : '') + (h ? '' : (s%60)+'s'); };
const kib = b => b >= 1073741824 ? (b/1073741824).toFixed(1)+' GB'
              : b >= 1048576 ? (b/1048576).toFixed(1)+' MB'
              : Math.round(b/1024)+' KB';

function toast(t){ const e = $('toast'); e.textContent = t; e.classList.add('on'); setTimeout(()=>e.classList.remove('on'), 2600); }

async function call(url, opt){
  const r = await fetch(url, opt);
  if (!r.ok) { toast(await r.text() || ('HTTP ' + r.status)); throw 0; }
  return r;
}

function spark(svg, arr){
  if (arr.length < 2) return;
  const lo = Math.min(...arr), hi = Math.max(...arr), span = hi - lo || 1;
  svg.firstElementChild.setAttribute('points', arr.map((v,i) =>
    (i * 200 / (arr.length - 1)).toFixed(1) + ',' + (32 - (v - lo) / span * 30).toFixed(1)).join(' '));
}

function render(d){
  $('machine').textContent = d.system.machine;
  $('unlocked').classList.toggle('hide', d.locked !== false);
  conPrinting = d.printing;

  const b = $('state');
  b.textContent = d.state;
  b.className = 'badge ' + (d.printing ? 'busy' : d.connected ? 'live' : 'bad');
  const rssi = d.system.rssi, bars = $('bars');
  const strength = rssi >= -60 ? 3 : rssi >= -70 ? 2 : rssi >= -80 ? 1 : 0;
  bars.setAttribute('class', strength >= 2 ? '' : strength === 1 ? 'weak' : 'gone');   // an svg className is not a string
  bars.querySelectorAll('rect').forEach((r, i) => r.classList.toggle('on', i < Math.max(strength, 1)));
  bars.querySelector('title').textContent = rssi + ' dBm';
  $('build').textContent = d.system.version;

  const j = d.job;
  $('jobtitle').textContent = d.printing ? 'Printing' : 'Selected';
  $('jname').textContent = j.file;
  $('jpct').textContent = j.completion.toFixed(1);
  $('jbar').style.width = j.completion + '%';
  $('jlayer').textContent = j.layers ? j.layer + ' / ' + j.layers : (j.layer || '—');
  $('jz').textContent = j.z ? j.z.toFixed(2) + ' mm' : '—';
  $('jel').textContent = d.printing ? hhmm(j.elapsed) : '—';
  $('jrem').textContent = j.remaining >= 0 ? hhmm(j.remaining) : '—';
  $('jend').textContent = j.remaining >= 0 ? new Date(Date.now() + j.remaining*1000).toLocaleTimeString([], {hour:'2-digit', minute:'2-digit'}) : '—';
  $('jfan').textContent = j.fan < 0 ? '—' : Math.round(j.fan/2.55) + '%';

  const e = d.energy, metered = e.watt >= 0;
  $('kwatt').classList.toggle('hide', !metered);
  $('jwatt').textContent = metered ? e.watt.toFixed(0) + ' W' : '—';

  const sock = d.socket, usable = sock.token && sock.device;
  const auto_ = $('cauto');
  if (document.activeElement !== auto_) auto_.checked = sock.auto;
  auto_.disabled = !usable;
  auto_.parentElement.title = usable ? sock.name + ' · ' + sock.state
                                     : 'no socket set up, see the socket page';
  const soon = $('soon'), going = sock.state.indexOf('switching off') === 0;
  soon.classList.toggle('hide', !going);
  if (going) soon.textContent = sock.state;
  $('bpower').disabled = d.printing || !usable;

  $('bprint').disabled = d.printing || !d.connected || busy;
  $('bcancel').disabled = !d.printing;

  const t = d.temperature;
  targets.n = t.tool.target; targets.b = t.bed.target;
  $('tn').textContent = t.tool.actual.toFixed(1) + '°';
  $('tnt').textContent = t.tool.target > 0 ? '→ ' + t.tool.target.toFixed(0) + '°' : 'off';
  $('tb').textContent = t.bed.actual.toFixed(1) + '°';
  $('tbt').textContent = t.bed.target > 0 ? '→ ' + t.bed.target.toFixed(0) + '°' : 'off';
  hist.n.push(t.tool.actual); hist.b.push(t.bed.actual);
  if (hist.n.length > 120) { hist.n.shift(); hist.b.shift(); }
  spark($('sn'), hist.n); spark($('sb'), hist.b);

  $('space').textContent = kib(+d.storage.free * 1024) + ' free';
  const mark = d.storage.selected + '|' + d.printing;
  if (seen !== null && seen !== mark) loadFiles().catch(()=>{});
  seen = mark;
  const th = $('jthumb'), want = j.file && j.file !== '—' ? '/thumbnail?name=' + encodeURIComponent(j.file) : '';
  if (th.dataset.src !== want) { th.dataset.src = want; th.className = 'thumb'; th.src = want; }
  $('kgram').classList.toggle('hide', !(d.energy.grams > 0));
  $('jgram').textContent = d.energy.grams.toFixed(1) + ' g';
  $('brestart').disabled = d.printing;
}

// What the wire is doing, kept apart from the job because a transfer is not a print.
let busy = false;

async function pollTransfer(){
  const t = await (await fetch('/printer/send/status')).text();
  const of = s => (t.match(new RegExp('^' + s + ' +(.*)$', 'm')) || ['',''])[1];
  const phase = of('phase'), w = of('written').split(' ');
  const was = busy;
  busy = phase && 'idle done failed'.indexOf(phase) < 0;
  if (was && !busy) setTimeout(() => printerCard().catch(()=>{}), 1500);
  const pct = +w[2] ? Math.round(+w[0] * 100 / +w[2]) : 0;
  const chore = (of('as').split(', chore ')[1] || '');
  $('xfer').classList.toggle('hide', !busy);
  if (busy) $('xfer').textContent = phase + ' ' + pct + '%';
  $('xline').textContent = busy ? 'to the printer card, ' + pct + '% of ' + w[2] + ' bytes'
    : (chore.indexOf('failed') === 0 ? chore : '');
}

function renderFiles(sel){
  const tb = $('ftab');
  tb.textContent = '';
  if (!files.length) { tb.insertRow().insertCell().innerHTML = '<span class="dim">card is empty</span>'; return; }
  for (const f of files) {
    const tr = tb.insertRow();
    tr.className = 'pick' + (f.name === sel ? ' sel' : '');
    tr.onclick = () => act('/select?name=' + encodeURIComponent(f.name));
    const n = tr.insertCell(); n.className = 'nm';
    n.textContent = f.name;
    const z = document.createElement('span');
    z.className = 'dim num'; z.style.marginLeft = '8px'; z.textContent = kib(f.size);
    n.append(z);
    const a = tr.insertCell(); a.className = 'acts';
    a.onclick = e => e.stopPropagation();
    const box = document.createElement('div'); box.className = 'row';
    box.append(btn('Send and print', 'p', () => act('/select?name=' + encodeURIComponent(f.name)).then(() => job('start'))),
               btn('Send', '', () => act('/select?name=' + encodeURIComponent(f.name)).then(() => sendOnly(f.name))),
               btn('Rename', '', () => rename(f.name)),
               btn('Delete', 'd', () => confirm('Delete ' + f.name + '?') && act('/delete?name=' + encodeURIComponent(f.name))));
    a.append(box);
  }
}

function btn(text, cls, fn){ const b = document.createElement('button'); b.textContent = text; if (cls) b.className = cls; b.onclick = fn; return b; }

// The extension is put back by the firmware, so dropping it here is harmless.
async function rename(from){
  const to = prompt('New name for ' + from, from);
  if (to === null || to.trim() === '' || to === from) return;
  await call('/api/rename', {method: 'POST', body: new URLSearchParams({from, to})});
  await loadFiles();
  toast('renamed');
}

async function act(url){ await call(url, {method:'POST'}); await loadFiles(); }
async function job(cmd){
  await call('/api/job', {method:'POST', headers:{'Content-Type':'application/json'}, body: JSON.stringify({command: cmd})});
  toast(cmd === 'start' ? 'print started' : 'cancelling');
}

async function loadFiles(){
  const d = await (await fetch('/api/files')).json();
  files = d.files;
  const s = await (await fetch('/api/status')).json();
  seen = s.storage.selected + '|' + s.printing;
  renderFiles(s.storage.selected);
  $('space').textContent = kib(+d.free * 1024) + ' free';
}

async function sendOnly(name){
  await call('/printer/send?name=' + encodeURIComponent(name), {method:'POST'});
  toast('on its way to the printer card');
  pollTransfer().catch(()=>{});
}

// Three settings rather than two: what the system says is a choice worth keeping.
const themes = ['auto', 'day', 'night'];
function theme(t){
  if (t === 'auto') document.documentElement.removeAttribute('data-t');
  else document.documentElement.setAttribute('data-t', t);
  $('btheme').textContent = t;
  try { localStorage.kp3sTheme = t; } catch (e) {}
}
$('btheme').onclick = () => {
  let t = 'auto';
  try { t = localStorage.kp3sTheme || 'auto'; } catch (e) {}
  theme(themes[(themes.indexOf(t) + 1) % 3]);
};
(function(){ let t = 'auto'; try { t = localStorage.kp3sTheme || 'auto'; } catch (e) {} theme(t); })();

// The printer's card is asked for on purpose: it costs the wire two commands, and
// during a print those would wait behind every move already queued.
let asking = false;

async function printerCard(){
  if (asking) return;
  asking = true;
  try {
    const r = await fetch('/printer/files');
    const text = await r.text();
    const tb = $('ptab');
    tb.textContent = '';
    if (!r.ok) {
      const cell = tb.insertRow().insertCell();
      cell.className = 'dim';
      cell.textContent = text.trim();
      return;
    }
    for (const line of text.split('\n')) {
      if (/^waiting |^next /.test(line)) continue;
      const m = line.match(/^(\S+)\s+(\d+)\s+(.*)$/);
      if (!m) continue;
      const tr = tb.insertRow();
      const img = document.createElement('img');
      img.className = 'tmini'; img.loading = 'lazy'; img.alt = '';
      img.onerror = () => img.classList.add('noimg');
      img.src = '/thumbnail?name=' + encodeURIComponent(m[3]);
      const t = tr.insertCell(); t.className = 'thumbcell'; t.append(img);
      const n = tr.insertCell(); n.className = 'nm';
      n.textContent = m[3];
      const z = document.createElement('span');
      z.className = 'dim num'; z.style.marginLeft = '8px'; z.textContent = kib(+m[2]);
      n.append(z);
      const a = tr.insertCell(); a.className = 'acts';
      const box = document.createElement('div'); box.className = 'row';
      box.append(btn('Print', 'p', () => confirm('Print ' + m[3] + '?') &&
        call('/printer/print?name=' + encodeURIComponent(m[3]), {method:'POST'})
          .then(() => { toast('printing'); })),
        btn('Delete', 'd', () => confirm('Delete ' + m[3] + ' from the printer card?') &&
          call('/printer/delete?name=' + encodeURIComponent(m[3]), {method:'POST'})
            .then(() => { toast('deleted'); printerCard().catch(()=>{}); })));
      a.append(box);
    }
    if (!tb.rows.length) {
      const cell = tb.insertRow().insertCell();
      cell.className = 'dim';
      cell.textContent = 'nothing on the printer card';
    }
  } finally { asking = false; }
}
$('bpcard').onclick = () => printerCard().catch(()=>{});

$('cauto').onchange = e => call('/yandex/auto?on=' + (e.target.checked ? 1 : 0), {method:'POST'})
  .then(() => toast(e.target.checked ? 'will switch off after a print' : 'will stay on'));
$('bpower').onclick = () => confirm('Cut mains to the printer? This module loses power too and cannot switch it back on.')
  && call('/yandex/off', {method:'POST'}).then(() => toast('switching off'));
$('bprint').onclick = () => job('start');
$('bcancel').onclick = () => confirm('Cancel the running print?') && job('cancel');
$('brestart').onclick = () => confirm('Restart the module? The printer keeps running.') && call('/restart', {method:'POST'}).then(() => toast('restarting'));
let uploadThenPrint = false;
function pick(andPrint){
  if (busy) { toast('a transfer is running, upload when it finishes'); return; }
  if (conPrinting) { toast('a print is running, the wire is not free'); return; }
  uploadThenPrint = andPrint;
  $('file').click();
}
$('bpick').onclick = () => pick(false);
$('bpickp').onclick = () => pick(true);

$('bfw').onclick = () => $('fwfile').click();
$('fwfile').onchange = e => { if (e.target.files[0]) flash(e.target.files[0]); e.target.value = ''; };

function flash(f){
  if (!confirm('Flash ' + f.name + ' onto the module?')) return;
  const form = new FormData();
  form.append('firmware', f);
  const x = new XMLHttpRequest(), p = $('fwprog');
  p.classList.remove('hide'); p.value = 0;
  x.upload.onprogress = e => { if (e.lengthComputable) p.value = e.loaded * 100 / e.total; };
  x.onloadend = () => {
    p.classList.add('hide');
    toast(x.status === 200 ? 'flashed, the module is rebooting' : ('flash failed: ' + (x.responseText || x.status)));
  };
  x.open('POST', '/update');
  x.send(form);
}
$('file').onchange = e => { if (e.target.files[0]) upload(e.target.files[0]); e.target.value = ''; };

function upload(f){
  const form = new FormData();
  form.append('file', f);
  const x = new XMLHttpRequest(), p = $('uprog');
  p.classList.remove('hide'); p.value = 0;
  x.upload.onprogress = e => { if (e.lengthComputable) p.value = e.loaded * 100 / e.total; };
  x.onloadend = () => {
    p.classList.add('hide');
    toast(x.status === 201 ? 'uploaded, going to the printer card' : ('upload failed: ' + (x.responseText || x.status)));
    loadFiles();
    pollTransfer().catch(()=>{});
  };
  if (uploadThenPrint)
    form.append('print', 'true');
  x.open('POST', '/api/files/local');
  x.send(form);
}

let conSeq = 0, conPrinting = false;

function conLine(text){
  const div = document.createElement('div');
  div.className = text.startsWith('>') ? 'out' : 'in';
  div.textContent = text;
  const box = $('conlog');
  const atBottom = box.scrollHeight - box.scrollTop - box.clientHeight < 40;
  box.appendChild(div);
  while (box.childElementCount > 300) box.removeChild(box.firstChild);
  if (atBottom) box.scrollTop = box.scrollHeight;
}

async function pollConsole(){
  const d = await (await fetch('/api/console?since=' + conSeq)).json();
  if (d.seq === conSeq) return;
  conSeq = d.seq;
  const first = $('conlog').querySelector('.empty');
  if (first) first.remove();
  for (const l of d.lines) conLine(l);
}

async function send(cmd){
  const r = await fetch('/api/command', {method:'POST', body: new URLSearchParams({cmd})});
  if (!r.ok) toast('rejected: ' + r.status);
  return r.ok;
}

// Clicking a temperature box asks for a new target. The ceilings match the firmware:
// HEATER_0_MAXTEMP minus HOTEND_OVERSHOOT, and the same for the bed.
const targets = {n: 0, b: 0};

async function askTemp(which){
  const nozzle = which === 'n';
  const max = nozzle ? 275 : 110;
  const name = nozzle ? 'nozzle' : 'bed';
  const answer = prompt('Target for the ' + name + ' in °C, 0 turns it off (up to ' + max + ')',
                        String(Math.round(targets[which])));
  if (answer === null) return;
  const wanted = Math.round(Number(answer.replace(',', '.')));
  if (!Number.isFinite(wanted) || wanted < 0 || wanted > max) { toast('0 to ' + max + ' only'); return; }
  if (await send((nozzle ? 'M104 S' : 'M140 S') + wanted))
    toast(name + (wanted ? ' heating to ' + wanted + '°' : ' off'));
}

$('boxn').onclick = () => askTemp('n');
$('boxb').onclick = () => askTemp('b');

// The same numbers the firmware's own presets use, so the buttons and the printer's
// own menu mean the same thing.
$('preheat').onclick = async e => {
  const b = e.target.closest('button');
  if (!b) return;
  const hot = b.dataset.hot, bed = b.dataset.bed;
  if (conPrinting && !confirm('A print is running. Change the temperatures anyway?')) return;
  if (await send('M104 S' + hot)) await send('M140 S' + bed);
  toast(hot === '0' ? 'cooling down' : 'heating to ' + hot + ' / ' + bed);
  pollConsole().catch(()=>{});
};

$('concmd').onkeydown = e => { if (e.key === 'Enter') { e.preventDefault(); $('conform').requestSubmit(); } };

$('conform').onsubmit = async e => {
  e.preventDefault();
  const cmd = $('concmd').value.trim();
  if (!cmd) return;
  if (conPrinting && !confirm('A print is running. Send "' + cmd + '" anyway?')) return;
  $('concmd').value = '';
  await send(cmd);
  pollConsole().catch(()=>{});
};

(async function loop(){
  for (;;) {
    try { render(await (await fetch('/api/status')).json()); }
    catch (e) { $('state').textContent = 'offline'; $('state').className = 'badge bad'; }
    pollConsole().catch(()=>{});
    pollTransfer().catch(()=>{});
    await new Promise(r => setTimeout(r, busy ? 900 : 1500));
  }
})();
loadFiles().catch(()=>{});
printerCard().catch(()=>{});
</script>
)PAGE";

static String pageHtml() { return String(PAGE_HTML); }
