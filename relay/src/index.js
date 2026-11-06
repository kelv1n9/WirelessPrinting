// The module sits behind the house router and cannot be reached. It dials out to
// here instead and keeps one WebSocket open; a browser request is carried down that
// socket, answered by the module's own web server, and carried back. The module's
// password still guards everything: this only moves bytes.
//
// Down the socket, a text frame opens a request, binary frames carry its body, and a
// text frame says the body is over:
//   {"i":7,"m":"POST","p":"/api/files/local","h":{...},"l":123456}
//   <7><bytes> <7><bytes> ...                 (four bytes of request number first)
// Back up, the same shape: a text frame with status and headers, binary frames of
// body, then {"i":7,"e":1}.
//
// Telegram goes through here too, because the module cannot hold a second encrypted
// connection: {"t":"tg","c":chat,"x":text} in either direction.

import { deflate } from "pako";

const UPLOAD_LIMIT = 64 * 1024 * 1024;
const CHUNK = 2040;                       // plus four bytes of number, one module buffer
const IN_FLIGHT = 3;                      // one fewer than the module has slots for pieces
const ACK_GRACE_MS = 4000;
const UPLOAD_ATTEMPTS = 3;
const RECONNECT_WAIT_MS = 90000;
const SILENT_GIVE_UP_MS = 10000;
const ANSWER_TIMEOUT_MS = 30000;
const SILENT_MS = 35000;              // three of the module's beats missed
const OFF_SILENT_MS = 13000;
const OFF_WATCH_MS = 180000;
const PASS_DOWN = ["authorization", "content-type", "x-api-key", "if-none-match", "x-relay-once"];
const LOST = Symbol("lost");
// What the page asks for is kept, so the page still works while the module is busy
// taking a file. The console is not: handed out again it would repeat its lines.
const CACHE_ITEMS = 64;
const SESSION_DAYS = 30;
const CACHE_ITEM_LIMIT = 64 * 1024;
const cacheKey = (url) => url.pathname + url.search;
const asSent = (body, status, headers) =>
  new Response(body, { status, headers, encodeBody: headers.has("content-encoding") ? "manual" : "automatic" });
const wentAway = () => new Response("The printer module went away.\n", { status: 503 });

export default {
  async fetch(request, env) {
    const relay = env.RELAY.get(env.RELAY.idFromName("kp3s"));
    // curl and libcurl, Orca among them, ask "Expect: 100-continue" before a body over
    // 1 MB, and passed on to the Durable Object the body then never arrived: Cloudflare
    // gave up at 55 s with a 502 of its own. Without it the body flows.
    if (request.headers.has("expect")) {
      const headers = new Headers(request.headers);
      headers.delete("expect");
      request = new Request(request, { headers });
    }
    return relay.fetch(request);
  },
};

// Digest uses MD5 and WebCrypto has none, so it is here: the module checks its password
// this way, and the Worker has to answer the same while the module is busy.
function md5(text) {
  const bytes = new TextEncoder().encode(text);
  const words = new Uint32Array(((bytes.length + 8 >> 6) + 1) * 16);
  for (let i = 0; i < bytes.length; i++) words[i >> 2] |= bytes[i] << (i % 4 * 8);
  words[bytes.length >> 2] |= 0x80 << (bytes.length % 4 * 8);
  words[words.length - 2] = bytes.length * 8;
  const K = new Uint32Array(64), S = [7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21];
  for (let i = 0; i < 64; i++) K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296);
  let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
  for (let off = 0; off < words.length; off += 16) {
    let a = a0, b = b0, c = c0, d = d0;
    for (let i = 0; i < 64; i++) {
      let f, g;
      if (i < 16) { f = (b & c) | (~b & d); g = i; }
      else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
      else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
      else { f = c ^ (b | ~d); g = (7 * i) % 16; }
      const t = d; d = c; c = b;
      const x = (a + f + K[i] + words[off + g]) | 0;
      const sh = S[(i >> 4) * 4 + i % 4];
      b = (b + ((x << sh) | (x >>> (32 - sh)))) | 0;
      a = t;
    }
    a0 = (a0 + a) | 0; b0 = (b0 + b) | 0; c0 = (c0 + c) | 0; d0 = (d0 + d) | 0;
  }
  return [a0, b0, c0, d0].map((v) => [0, 8, 16, 24].map((s) => ((v >>> s) & 255).toString(16).padStart(2, "0")).join("")).join("");
}

async function sha256hex(text) {
  const h = new Uint8Array(await crypto.subtle.digest("SHA-256", new TextEncoder().encode(text)));
  return [...h].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function field(header, name) {
  const m = new RegExp(`(?:^|[\\s,])${name}=(?:"([^"]*)"|([^,\\s]*))`).exec(header);
  return m ? (m[1] !== undefined ? m[1] : m[2]) : "";
}

// What a person sees when the module cannot answer: the page's own colours, and it
// tries again by itself, instead of a bare line of text.
function personPage(title, line, retrySeconds, more = "") {
  const html = `<!doctype html><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<meta http-equiv=refresh content="${retrySeconds}"><title>KP3S</title>
<style>:root{--bg:#0d1117;--card:#161b22;--tx:#e8ebf0;--dim:#8a93a2;--acc:#5b9cff}
@media(prefers-color-scheme:light){:root{--bg:#f4f6f9;--card:#fff;--tx:#14181d;--dim:#5d6672;--acc:#2f6fd0}}
body{margin:0;min-height:100vh;display:grid;place-items:center;background:var(--bg);color:var(--tx);font:16px/1.5 system-ui,sans-serif}
.c{background:var(--card);padding:28px 32px;border-radius:14px;max-width:26rem;margin:16px;box-shadow:0 4px 24px #0003}
h1{font-size:20px;margin:0 0 8px}p{margin:0;color:var(--dim)}form{margin:18px 0 0}
button{font:inherit;padding:10px 18px;border:0;border-radius:8px;background:var(--acc);color:#fff;cursor:pointer}.s{display:inline-block;width:10px;height:10px;border-radius:50%;background:var(--acc);margin-right:8px;animation:p 1.2s infinite}
@keyframes p{50%{opacity:.3}}</style>
<div class=c><h1><span class=s></span>${title}</h1><p>${line}</p>${more}</div>`;
  return new Response(html, { status: 503, headers: { "content-type": "text/html; charset=utf-8", "retry-after": String(retrySeconds), "cache-control": "no-store" } });
}

const wantsPage = (request) => (request.headers.get("accept") || "").includes("text/html");

function indexOfBytes(hay, needle, from) {
  outer: for (let i = from; i <= hay.length - needle.length; i++) {
    for (let j = 0; j < needle.length; j++) if (hay[i + j] !== needle[j]) continue outer;
    return i;
  }
  return -1;
}

// Takes a multipart form apart: the one file in it and its other fields. Null when
// the form is anything other than that, and then it goes down as it came.
function takeForm(contentType, body) {
  const m = /boundary=(?:"([^"]+)"|([^;\s]+))/i.exec(contentType || "");
  if (!m) return null;
  const enc = new TextEncoder(), dec = new TextDecoder();
  const sep = enc.encode("--" + (m[1] || m[2]));
  const crlf2 = enc.encode("\r\n\r\n");
  const fields = {};
  let file = null;
  let at = indexOfBytes(body, sep, 0);
  while (at >= 0) {
    at += sep.length;
    if (body[at] === 45 && body[at + 1] === 45) break;          // "--" ends the form
    const headEnd = indexOfBytes(body, crlf2, at);
    if (headEnd < 0) return null;
    const head = dec.decode(body.subarray(at, headEnd));
    const next = indexOfBytes(body, enc.encode("\r\n--" + (m[1] || m[2])), headEnd + 4);
    if (next < 0) return null;
    const value = body.subarray(headEnd + 4, next);
    const name = /name="([^"]*)"/i.exec(head);
    const filename = /filename="([^"]*)"/i.exec(head);
    if (filename) {
      if (file) return null;                                   // two files is not ours
      file = { name: filename[1], bytes: value };
    }
    else if (name) fields[name[1]] = dec.decode(value);
    at = next + 2;
  }
  return file ? { file, fields } : null;
}

function sameSecret(a, b) {
  if (typeof a !== "string" || typeof b !== "string" || a.length !== b.length || !a.length)
    return false;
  let diff = 0;
  for (let i = 0; i < a.length; i++) diff |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return diff === 0;
}

async function hookSecret(env) {
  const bytes = new TextEncoder().encode("telegram:" + env.DEVICE_KEY);
  const hash = new Uint8Array(await crypto.subtle.digest("SHA-256", bytes));
  return [...hash.slice(0, 16)].map((b) => b.toString(16).padStart(2, "0")).join("");
}

function framed(id, bytes) {
  const out = new Uint8Array(4 + bytes.length);
  out[0] = id & 0xff; out[1] = (id >> 8) & 0xff; out[2] = (id >> 16) & 0xff; out[3] = (id >>> 24) & 0xff;
  out.set(bytes, 4);
  return out;
}

export class Relay {
  constructor(state, env) {
    this.state = state;
    this.env = env;
    this.next = 1;
    this.waiting = new Map();
    this.bodies = Promise.resolve();      // one body at a time, or their chunks would interleave
    this.trail = [];
    this.cache = new Map();
    this.uploading = 0;
    // The module's beat, answered without waking this object
    this.state.setWebSocketAutoResponse(new WebSocketRequestResponsePair("p", "q"));
  }

  note(text) {
    if (!this.env.TRACE) return;
    this.trail.push(`${new Date().toISOString().slice(11, 23)} ${text}`);
    if (this.trail.length > 200) this.trail.shift();
  }

  // Something that went wrong, kept in storage: the trail lives in memory and is gone
  // the moment the object sleeps, which is usually before anybody looks. Rare by nature,
  // so the writes cost nothing that counts.
  incident(text) {
    this.note(text);
    const line = `${new Date().toISOString().slice(0, 19)} ${text}`;
    this.state.storage.get("incidents").then((list) => {
      list = (list || []).concat(line).slice(-100);
      return this.state.storage.put("incidents", list);
    }).catch(() => {});
  }

  // A module whose power was cut says no goodbye, and its socket stays open here for
  // minutes. So a socket counts only while the module's beat is heard on it; one that
  // has never sent a beat is an older firmware and is taken on trust, as before.
  device() {
    const now = Date.now();
    const open = this.state.getWebSockets("device").filter((ws) => {
      if (ws.readyState !== 1) return false;
      const beat = this.state.getWebSocketAutoResponseTimestamp(ws);
      const limit = now - (this.offAsked || 0) < OFF_WATCH_MS ? OFF_SILENT_MS : SILENT_MS;
      return !beat || now - Math.max(beat.getTime(), this.heard || 0) < limit;
    });
    return open.length ? open[open.length - 1] : null;
  }

  async fetch(request) {
    const url = new URL(request.url);
    if (this.offAsked === undefined) this.offAsked = (await this.state.storage.get("offAsked")) || 0;

    if (url.pathname === "/.device") {
      if (request.headers.get("Upgrade") !== "websocket")
        return new Response("expected a websocket\n", { status: 426 });
      const given = (request.headers.get("Authorization") || "").replace(/^Bearer /, "");
      if (!sameSecret(given, this.env.DEVICE_KEY))
        return new Response("no\n", { status: 403 });
      for (const old of this.state.getWebSockets("device"))
        old.close(1000, "replaced");
      if (this.offAsked) {
        this.offAsked = 0;
        await this.state.storage.delete("offAsked");
      }
      const pair = new WebSocketPair();
      this.state.acceptWebSocket(pair[1], ["device"]);
      pair[1].send(JSON.stringify({ t: "hello", tg: !!this.env.TELEGRAM_TOKEN }));
      this.note("device connected");
      return new Response(null, { status: 101, webSocket: pair[0] });
    }

    // The module's socket closed from this end, the way Cloudflare closes it now and
    // then, so that how fast the module comes back can be measured and not waited for
    if (url.pathname === "/.kick") {
      const given = (request.headers.get("Authorization") || "").replace(/^Bearer /, "");
      if (!sameSecret(given, this.env.DEVICE_KEY))
        return new Response("no\n", { status: 403 });
      for (const old of this.state.getWebSockets("device")) old.close(1012, "kicked");
      this.note("device socket kicked");
      return new Response("kicked\n");
    }

    if (url.pathname === "/.trace") {
      const given = (request.headers.get("Authorization") || "").replace(/^Bearer /, "");
      if (!sameSecret(given, this.env.DEVICE_KEY))
        return new Response("no\n", { status: 403 });
      const incidents = (await this.state.storage.get("incidents")) || [];
      let where = "";
      try { where = (await (await fetch("https://cloudflare.com/cdn-cgi/trace")).text()).match(/colo=\w+/)[0]; } catch (e) {}
      return new Response("object runs at " + where + "\nincidents:\n" + incidents.join("\n") + "\n\ntrail:\n" + this.trail.join("\n") + "\n");
    }

    // How long a message takes to the module and back, with nothing else in the way
    if (url.pathname === "/.rtt") {
      const given = (request.headers.get("Authorization") || "").replace(/^Bearer /, "");
      if (!sameSecret(given, this.env.DEVICE_KEY))
        return new Response("no\n", { status: 403 });
      const ws = this.device();
      if (!ws) return new Response("no module\n", { status: 503 });
      const out = [];
      for (let i = 0; i < 12; i++) {
        const t = Date.now();
        await new Promise((r) => {
          this.rttWake = r;
          ws.send(JSON.stringify({ t: "hello", tg: !!this.env.TELEGRAM_TOKEN }));
          setTimeout(r, 3000);
        });
        out.push(Date.now() - t);
      }
      return new Response(out.join(" ") + "\n");
    }

    if (url.pathname === "/.tg-setup") {
      const given = (request.headers.get("Authorization") || "").replace(/^Bearer /, "");
      if (!sameSecret(given, this.env.DEVICE_KEY))
        return new Response("no\n", { status: 403 });
      if (!this.env.TELEGRAM_TOKEN)
        return new Response("no TELEGRAM_TOKEN secret on the Worker\n", { status: 409 });
      const secret = await hookSecret(this.env);
      const api = `https://api.telegram.org/bot${this.env.TELEGRAM_TOKEN}/`;
      const call = request.method === "DELETE"
        ? fetch(api + "deleteWebhook", { method: "POST" })
        : fetch(api + "setWebhook", {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ url: `${url.origin}/.tg/${secret}`, secret_token: secret,
                                   allowed_updates: ["message"], drop_pending_updates: true }),
          });
      const r = await call;
      const said = await r.json().catch(() => ({}));
      return new Response(said.ok ? "ok\n" : "telegram said no\n", { status: said.ok ? 200 : 502 });
    }

    if (url.pathname.startsWith("/.tg/")) {
      const secret = await hookSecret(this.env);
      if (url.pathname !== `/.tg/${secret}` ||
          !sameSecret(request.headers.get("X-Telegram-Bot-Api-Secret-Token") || "", secret))
        return new Response("no\n", { status: 403 });
      const update = await request.json().catch(() => null);
      const m = update && update.message;
      const ws = this.device();
      if (m && ws) {
        if (m.document)
          ws.send(JSON.stringify({ t: "tgdoc", c: String(m.chat.id), id: m.document.file_id,
                                   name: m.document.file_name || "file", size: m.document.file_size || 0 }));
        else if (m.text)
          ws.send(JSON.stringify({ t: "tg", c: String(m.chat.id), x: m.text }));
      }
      return new Response("ok");
    }

    const ws = this.device();
    if (url.pathname === "/.power-on" && request.method === "POST")
      return this.powerOn(request, url, !!ws);
    if (!ws)
      return wantsPage(request)
        ? this.awayPage(request, url)
        : new Response("The printer module is not connected right now.\n",
                       { status: 503, headers: { "content-type": "text/plain; charset=utf-8" } });

    const hasBody = request.method !== "GET" && request.method !== "HEAD" && request.body;
    let length = Number(request.headers.get("content-length") || 0);
    let buffered = null;
    // The whole body is taken in first. Reading it from the client at the module's
    // pace, minutes for a firmware image, had Cloudflare's edge give up on the request
    // at about 55 seconds with a 502 of its own.
    if (hasBody) {
      if (length > UPLOAD_LIMIT)
        return new Response("Too large to send through the relay.\n", { status: 413 });
      this.note(`taking in a body of ${length} for ${url.pathname}`);
      buffered = new Uint8Array(await request.arrayBuffer());
      length = buffered.length;
      this.note(`took it in, ${length} bytes`);
    }
    if (length > UPLOAD_LIMIT)
      return new Response("Too large to send through the relay.\n", { status: 413 });

    const headers = {};
    for (const name of PASS_DOWN) {
      const v = request.headers.get(name);
      if (v) headers[name] = v;
    }

    // While a file is going down the module's web server is busy with it and answers
    // nothing else, so the page would see every poll time out and the module look dead.
    // The status it last gave is handed out instead, and the rest is told to wait.
    // The cache answers without asking the module, so it answers only a browser that
    // has already got past the module's password and been given a session for it.
    let giveSession = false;
    if (!hasBody && this.uploading > 0 && !(await this.knownSession(request))) {
      const verdict = await this.checkHere(request, url);
      this.note(`busy, ${url.pathname} without a session: ${verdict}`);
      if (verdict === "challenge") {
        const r = new Response(`<!doctype html><meta charset=utf-8><title>KP3S</title><p style='font:15px system-ui;padding:1rem'>The user name is <b>${this.auth.user}</b>. Reload the page to try again.</p>`,
                               { status: 401, headers: { "content-type": "text/html", "www-authenticate": `Digest realm="${this.auth.realm}", qop="auth", nonce="${this.auth.nonce}", opaque="0"` } });
        return r;
      }
      if (verdict !== "ok")
        return wantsPage(request)
          ? personPage("Printer module is taking a file", "It answers again in a moment. This page checks by itself.", 3)
          : new Response("The module is busy taking a file. Try again when it is done.\n", { status: 503, headers: { "retry-after": "5" } });
      giveSession = true;
    }
    if (!hasBody && this.uploading > 0) {
      const key = cacheKey(url);
      let cached = this.cache.get(key);
      if (!cached && url.pathname === "/") {
        const kept = await this.state.storage.get("page");
        if (kept) cached = kept;
      }
      if (cached) {
        const h = new Headers(cached.headers || {});
        h.set("x-relay-cached", "uploading");
        let body = cached.body;
        // The status carries how far the file has got, the only progress there is now
        if (url.pathname === "/api/status" && this.progress) {
          try {
            const d = JSON.parse(new TextDecoder().decode(body));
            const p = this.progress, secs = (Date.now() - p.t0) / 1000;
            d.relayUpload = { sent: p.sent, total: p.total, rate: secs > 1 ? Math.round(p.sent / secs) : 0 };
            body = new TextEncoder().encode(JSON.stringify(d));
          } catch (e) {}
        }
        if (giveSession) await this.giveSession(request, h);
        return asSent(body, 200, h);
      }
      return wantsPage(request)
        ? personPage("Printer module is taking a file", "It answers again in a moment. This page checks by itself.", 3)
        : new Response("The module is busy taking a file. Try again when it is done.\n", { status: 503, headers: { "retry-after": "5" } });
    }
    if (!hasBody && request.method === "GET" && url.pathname !== "/download") {
      const last = await this.sendBody(ws, request.method, url.pathname + url.search, headers, new Uint8Array(0));
      if (last.lost) return wentAway();
      if (last.status === 200 && last.body.length <= CACHE_ITEM_LIMIT && url.pathname !== "/api/console") {
        const key = cacheKey(url);
        this.cache.delete(key);
        this.cache.set(key, last);
        while (this.cache.size > CACHE_ITEMS) this.cache.delete(this.cache.keys().next().value);
        if (url.pathname === "/") this.state.storage.put("page", last).catch(() => {});
      }
      const answer = new Headers(last.headers || {});
      if (last.status === 200 && (headers.authorization || headers["x-api-key"]))
        await this.giveSession(request, answer);
      return asSent(last.body, last.status, answer);
    }

    // A file off the module's card. Carried through the module's web server a large
    // answer hung it (04.11.2026), so the module reads the file and sends it up itself.
    // It answers nothing else meanwhile, so for the page this counts as an upload: the
    // password is checked here and the status comes from what was last heard.
    if (!hasBody && url.pathname === "/download") {
      const verdict = await this.checkHere(request, url);
      if (verdict === "challenge")
        return new Response(`The user name is ${this.auth.user}.\n`, { status: 401, headers: { "www-authenticate": `Digest realm="${this.auth.realm}", qop="auth", nonce="${this.auth.nonce}", opaque="0"` } });
      // In the same line as the uploads: the module does one of them at a time, and an
      // upload sent into a running download waited for acknowledgements that could not come
      let free;
      const before = this.bodies;
      this.bodies = before.then(() => new Promise((r) => (free = r)));
      await before;
      const device = this.device();
      if (!device) { free(); return wentAway(); }
      const answer = await this.carry(device, "GET", url.pathname + url.search, headers,
                                      { t: "get", name: url.searchParams.get("name") || "" }, free);
      return answer === LOST ? wentAway() : answer;
    }

    if (!hasBody) {
      // Asked again once if the socket went away before any answer came: the module
      // reconnects in seconds, and a browser would otherwise show the error page.
      for (let attempt = 1; ; attempt++) {
        const device = attempt === 1 ? ws : await this.waitForDevice(20000);
        if (!device) break;
        const answer = await this.carry(device, request.method, url.pathname + url.search, headers);
        if (answer !== LOST || attempt >= 2) return answer === LOST ? wentAway() : answer;
      }
      return wentAway();
    }

    // A body is sent again if it did not get through: an upload overwrites the same
    // file and a firmware image starts over, so trying twice is harmless, and a print
    // asked for alongside starts only once the whole file has landed.
    let extra = null, file = null;
    if (request.method === "POST" && (url.pathname === "/api/files/local" || url.pathname === "/upload")) {
      const form = takeForm(request.headers.get("content-type"), buffered);
      const safe = form && /^[^\/\\\x00-\x1f"]{1,120}$/.test(form.file.name);
      if (safe) {
        const asked = url.searchParams.get("print") || form.fields.print || "";
        file = form.file.bytes;
        extra = { t: "put", name: form.file.name, size: form.file.bytes.length,
                  print: asked === "true" || asked === "1" };
      }
    }
    const last = extra
      ? await this.putFile(request.method, url.pathname + url.search, headers, file, extra, { headers, body: buffered })
      : await this.deliver(request.method, url.pathname + url.search, headers, buffered, null,
                           /^\/(update|upload|api\/files\/local)/.test(url.pathname) ? UPLOAD_ATTEMPTS : 1);
    if (!last)
      return new Response("The upload did not get through the relay. Try again.\n", { status: 503 });
    const h = new Headers(last.headers || {});
    h.set("x-robots-tag", "noindex");
    return asSent(last.body, last.status, h);
  }

  // A file on its own, packed as zlib with the window the module said it unpacks in,
  // and written by the module straight onto its card. The stream ends with a checksum
  // of the whole file, which the module's decoder checks. A module that has not said so, or
  // has no memory for the decoder just then (507), gets the form as it came instead.
  async putFile(method, path, headers, bytes, extra, form) {
    const bits = this.auth && this.auth.inflate;
    if (Number.isInteger(bits) && bits) {
      const body = deflate(bytes, { level: 6, windowBits: bits });
      this.note(`put ${extra.name}: ${bytes.length} packed to ${body.length}, window ${bits} bits`);
      const last = await this.deliver(method, path, headers, body, { ...extra, d: true });
      if (!last || last.status !== 507) return last;
    }
    return this.deliver(method, path, form.headers, form.body, null, extra.print ? 1 : UPLOAD_ATTEMPTS);
  }

  // A body, tried up to three times, one body at a time. Resolves with the module's
  // whole answer, or null when it never got through.
  deliver(method, path, headers, body, extra = null, tries = UPLOAD_ATTEMPTS) {
    const turn = this.bodies.then(async () => {
      this.uploading++;
      try {
      let last = null;
      for (let attempt = 1; attempt <= tries; attempt++) {
        const device = await this.waitForDevice(RECONNECT_WAIT_MS);
        if (!device) break;
        if (attempt > 1 && extra && extra.print) {
          extra = { ...extra, print: false };
          this.incident(`${extra.name} is sent again without the print: the module dropped away on the first try`);
        }
        last = await this.sendBody(device, method, path, headers, body, extra);
        // A firmware image that went down whole and then lost the module is a flash that
        // worked: the module reboots before its answer is out. Sending it again only
        // met the new firmware with yesterday's credentials.
        if (last.lost && last.sentAll && (path.startsWith("/update") || path.startsWith("/restart"))) {
          const flashed = path.startsWith("/update");
          this.incident(`all of ${path} went down and the module dropped away, taken as ${flashed ? "flashed" : "restarted"}`);
          return { status: 200, headers: { "content-type": "text/plain" },
                   body: new TextEncoder().encode(flashed
                     ? "Sent whole; the module dropped away to reboot, which is what a good flash does. Check /api/status in a minute.\n"
                     : "restarting\n") };
        }
        if (!last.lost && last.status !== 502 && last.status !== 504) break;
        this.incident(`attempt ${attempt} of ${path} did not get through (${last.lost ? "lost" : last.status}), ${attempt < tries ? "trying again" : "giving up"}`);
      }
      return last && !last.lost ? last : null;
      } finally {
        this.uploading--;
        if (!this.uploading) this.progress = null;
      }
    });
    this.bodies = turn.catch(() => {});
    return turn;
  }

  async telegram(method, payload) {
    const r = await fetch(`https://api.telegram.org/bot${this.env.TELEGRAM_TOKEN}/${method}`, {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify(payload),
    }).catch(() => null);
    return r ? r.json().catch(() => ({})) : {};
  }

  // The module has looked at a file sent to the bot and said where it goes. Telegram
  // hands files only to whoever holds the bot token, and the module cannot afford the
  // second encrypted connection, so the file is fetched here and carried down.
  async fetchForModule(m) {
    const say = (text) => this.telegram("sendMessage", { chat_id: m.c, text });
    const started = Date.now();
    const info = await this.telegram("getFile", { file_id: m.id });
    const filePath = info && info.result && info.result.file_path;
    if (!filePath) return say("Telegram would not say where that file is. Send it again.");
    const got = await fetch(`https://api.telegram.org/file/bot${this.env.TELEGRAM_TOKEN}/${filePath}`).catch(() => null);
    if (!got || !got.ok) return say("Telegram would not hand the file over. Send it again.");
    const file = new Uint8Array(await got.arrayBuffer());

    const boundary = "kp3s" + crypto.randomUUID().replace(/-/g, "");
    const safeName = String(m.name).replace(/["\r\n]/g, "_");
    const head = new TextEncoder().encode(
      `--${boundary}\r\nContent-Disposition: form-data; name="${m.field}"; filename="${safeName}"\r\n` +
      `Content-Type: application/octet-stream\r\n\r\n`);
    const tail = new TextEncoder().encode(`\r\n--${boundary}--\r\n`);
    const body = new Uint8Array(head.length + file.length + tail.length);
    body.set(head, 0); body.set(file, head.length); body.set(tail, head.length + file.length);

    // A file for the printer goes straight onto the card, packed; firmware still goes
    // through the module's flash route, which needs the form.
    const form = { headers: { "content-type": `multipart/form-data; boundary=${boundary}`, "x-relay-once": m.token }, body };
    const answer = m.route === "/update"
      ? await this.deliver("POST", m.route, form.headers, form.body)
      : await this.putFile("POST", m.route, { "x-relay-once": m.token }, file, { t: "put", name: safeName, size: file.length, print: false }, form);
    const seconds = Math.round((Date.now() - started) / 1000);
    const kb = Math.round(file.length / 1024);
    const said = answer ? new TextDecoder().decode(answer.body).trim() : "";

    if (m.route === "/update") {
      if (answer && answer.status === 200)
        return say(`Firmware flashed \u2014 ${m.name}\n\nSize       ${kb} KB\nTransfer   ${seconds} s\n\nRebooting. Send /status in a minute.`);
      return say(`Firmware not flashed \u2014 ${said || "it did not get through"}\n\nThe module is still running what it ran before.`);
    }
    if (answer && answer.status < 300)
      return say(`Saved \u2014 ${m.name}\n\nSize       ${kb} KB\nTransfer   ${seconds} s\n\nGoing on to the printer's card now. I will say when it is there.`);
    return say(`Could not save ${m.name} \u2014 ${said || "it did not get through"}`);
  }

  // "ok", "challenge" (ask for the password), or "unknown" (the module never said how)
  async checkHere(request, url) {
    if (!this.auth) this.auth = (await this.state.storage.get("auth")) || null;
    if (!this.auth) {
      const ws = this.device();
      if (ws) {
        try { ws.send(JSON.stringify({ t: "hello", tg: !!this.env.TELEGRAM_TOKEN })); } catch (e) {}
        for (let i = 0; i < 20 && !this.auth; i++) await new Promise((r) => setTimeout(r, 100));
      }
    }
    const a = this.auth;
    if (!a) return "unknown";
    if (a.open) return "ok";
    const key = request.headers.get("x-api-key");
    if (key && a.key && (await sha256hex(key)) === a.key) return "ok";
    const h = request.headers.get("authorization") || "";
    if (h.startsWith("Basic ")) {
      let pair = "";
      try { pair = atob(h.slice(6).trim()); } catch (e) {}
      const at = pair.indexOf(":");
      if (at > 0 && pair.slice(0, at) === a.user && md5(`${a.user}:${a.realm}:${pair.slice(at + 1)}`) === a.ha1) return "ok";
      return "challenge";
    }
    if (h.startsWith("Digest ")) {
      const d = h.slice(7);
      if (field(d, "username") !== a.user || field(d, "nonce") !== a.nonce) return "challenge";
      const ha2 = md5(`${request.method}:${field(d, "uri")}`);
      const qop = field(d, "qop");
      const want = qop === "auth"
        ? md5(`${a.ha1}:${a.nonce}:${field(d, "nc")}:${field(d, "cnonce")}:auth:${ha2}`)
        : md5(`${a.ha1}:${a.nonce}:${ha2}`);
      return want === field(d, "response").toLowerCase() ? "ok" : "challenge";
    }
    return "challenge";
  }

  // The printer is off or starting, and the module with it. Someone who has signed in
  // is told which, and can switch it on: the module cannot be asked, so the password is
  // checked here, against what the module last said about it. Nobody else gets more
  // than the plain page. Only "on" is ever sent, and only to the printer's own socket:
  // switching off stays with the module, which knows whether a print is running.
  canPower() {
    return !!(this.env.YANDEX_TOKEN && this.auth && !this.auth.open && this.auth.socket);
  }

  async signedIn(request, url) {
    if (await this.knownSession(request)) return "ok";
    return this.checkHere(request, url);
  }

  challenge() {
    return new Response(`<!doctype html><meta charset=utf-8><title>KP3S</title><p style='font:15px system-ui;padding:1rem'>The user name is <b>${this.auth.user}</b>. Reload the page to try again.</p>`,
                        { status: 401, headers: { "content-type": "text/html", "cache-control": "no-store",
                                                  "www-authenticate": `Digest realm="${this.auth.realm}", qop="auth", nonce="${this.auth.nonce}", opaque="0"` } });
  }

  async yandex(path, body = null) {
    const r = await fetch("https://api.iot.yandex.net" + path, {
      method: body ? "POST" : "GET",
      headers: body ? { authorization: "Bearer " + this.env.YANDEX_TOKEN, "content-type": "application/json" }
                    : { authorization: "Bearer " + this.env.YANDEX_TOKEN },
      body: body || undefined,
      signal: AbortSignal.timeout(8000),
    }).catch(() => null);
    if (!r || !r.ok) return null;
    return r.json().catch(() => null);
  }

  async awayPage(request, url) {
    const plain = () => personPage("Printer module is starting up", "It is not connected to Cloudflare at the moment, usually because it is restarting or the printer is switched off. This page checks again every few seconds.", 5);
    if (!this.auth) this.auth = (await this.state.storage.get("auth")) || null;
    if (!this.canPower()) return plain();
    const verdict = await this.signedIn(request, url);
    if (verdict === "challenge") return this.challenge();
    if (verdict !== "ok") return plain();
    const d = await this.yandex("/v1.0/devices/" + this.auth.socket);
    const onOff = d && (d.capabilities || []).find((c) => c.type === "devices.capabilities.on_off");
    let page;
    if (!d || d.state !== "online" || !onOff || !onOff.state)
      page = personPage("Printer module is not connected", "Its socket did not answer either, so it cannot be switched on from here right now. This page checks again every few seconds.", 10);
    else if (onOff.state.value)
      page = personPage("Printer is on, the module is starting", "The socket is on. The module connects about a quarter of a minute after power comes. This page checks again every few seconds.", 4);
    else
      page = personPage("Printer is switched off", "Its socket is off, so the module has no power.", 20,
                        "<form method=post action=/.power-on><button>Switch the printer on</button></form>");
    const h = new Headers(page.headers);
    await this.giveSession(request, h);
    return new Response(page.body, { status: page.status, headers: h });
  }

  async powerOn(request, url, connected) {
    const back = () => new Response(null, { status: 303, headers: { location: "/", "cache-control": "no-store" } });
    if (connected) return back();
    if (!this.auth) this.auth = (await this.state.storage.get("auth")) || null;
    if (!this.canPower()) return new Response("Not set up.\n", { status: 404 });
    // A form on another site must not be able to press this for a signed-in browser
    const origin = request.headers.get("origin");
    if (origin && origin !== url.origin) return new Response("no\n", { status: 403 });
    const verdict = await this.signedIn(request, url);
    if (verdict === "challenge") return this.challenge();
    if (verdict !== "ok") return new Response("no\n", { status: 403 });
    const last = (await this.state.storage.get("poweredAt")) || 0;
    if (Date.now() - last < 20000) return back();
    await this.state.storage.put("poweredAt", Date.now());
    const press = JSON.stringify({ devices: [{ id: this.auth.socket,
      actions: [{ type: "devices.capabilities.on_off", state: { instance: "on", value: true } }] }] });
    const said = (await this.yandex("/v1.0/devices/actions", press)) || (await this.yandex("/v1.0/devices/actions", press));
    this.incident(`power on asked from the web page: ${said ? said.status : "yandex did not answer"}`);
    return back();
  }

  async loadSessions() {
    if (!this.sessions) this.sessions = (await this.state.storage.get("sessions")) || {};
    return this.sessions;
  }

  async knownSession(request) {
    const m = /(?:^|;\s*)kp3s=([0-9a-f-]{36})/.exec(request.headers.get("cookie") || "");
    if (!m) return false;
    const sessions = await this.loadSessions();
    return !!sessions[m[1]] && sessions[m[1]] > Date.now();
  }

  async giveSession(request, headers) {
    if (await this.knownSession(request)) return;
    const sessions = await this.loadSessions();
    const now = Date.now();
    for (const [k, until] of Object.entries(sessions)) if (until < now) delete sessions[k];
    const id = crypto.randomUUID();
    sessions[id] = now + SESSION_DAYS * 86400000;
    await this.state.storage.put("sessions", sessions);
    headers.append("set-cookie", `kp3s=${id}; Path=/; Max-Age=${SESSION_DAYS * 86400}; Secure; HttpOnly; SameSite=Strict`);
  }

  async waitForDevice(limitMs) {
    const until = Date.now() + limitMs;
    for (;;) {
      const ws = this.device();
      if (ws) return ws;
      if (Date.now() > until) return null;
      await new Promise((r) => setTimeout(r, 1000));
    }
  }

  newId() {
    const id = this.next++;
    if (this.next > 0x7fffffff) this.next = 1;
    return id;
  }

  // A request with no body, answered as a stream straight back to the browser
  // With extra it is a download: the module is busy with it from start to end.
  carry(ws, method, path, headers, extra = null, whenOver = null) {
    const id = this.newId();
    let resolve;
    const answered = new Promise((r) => (resolve = r));
    const w = { resolve, timer: null, writer: null, t0: Date.now(), busy: !!extra };
    if (w.busy) this.uploading++;
    // Called once, however the answer ends
    w.over = () => {
      clearTimeout(w.timer);
      this.waiting.delete(id);
      if (w.busy) { w.busy = false; this.uploading--; }
      if (whenOver) { whenOver(); whenOver = null; }
    };
    // Silence is what is timed, so an answer of any length gets through
    w.quiet = () => {
      clearTimeout(w.timer);
      w.timer = setTimeout(() => {
        w.over();
        if (w.writer) w.writer.abort("timeout").catch(() => {});
        else resolve(new Response("The printer module did not answer in time.\n", { status: 504 }));
      }, ANSWER_TIMEOUT_MS);
    };
    this.waiting.set(id, w);
    w.quiet();
    try {
      ws.send(JSON.stringify(Object.assign({ i: id, m: method, p: path, h: headers, l: 0 }, extra || {})));
    } catch (e) {
      w.over();
      resolve(LOST);
    }
    return answered;
  }

  // One try at a body: resolves with the module's whole answer, or { lost: true }
  sendBody(ws, method, path, headers, body, extra = null) {
    const id = this.newId();
    return new Promise(async (done) => {
      // A firmware image goes one piece at a time: the module is short of memory while it
      // flashes, and the only flash through the relay so far went through like that.
      const inFlight = path.startsWith("/update") ? 2 : IN_FLIGHT;
      const w = { collect: [], credit: inFlight, inFlight, wake: null, t0: Date.now(), sentPieces: 0, timer: null, finished: false };
      w.finish = (result) => {
        if (w.finished) return;
        if (result.lost) result.sentAll = w.sentPieces * CHUNK >= body.length;
        w.finished = true;
        clearTimeout(w.timer);
        this.waiting.delete(id);
        done(result);
      };
      this.waiting.set(id, w);
      this.note(`req ${id} ${method} ${path} length ${body.length}`);
      try {
        ws.send(JSON.stringify(Object.assign({ i: id, m: method, p: path, h: headers, l: body.length }, extra || {})));
        for (let o = 0; o < body.length; o += CHUNK) {
          while (w.credit <= 0) {
            const more = await new Promise((r) => {
              w.wake = r;
              setTimeout(() => r(false), ACK_GRACE_MS);
            });
            if (w.finished) return;
            if (more !== false) continue;
            // The firmware route can sit for half a minute before it reads (it waits for
            // the TLS gate, then opens its flash). Starting another attempt while the
            // module is still busy with this one only has the two collide.
            const silentLimit = path.startsWith("/update") ? 60000 : SILENT_GIVE_UP_MS;
            if (Date.now() - (w.lastAck || w.t0) > silentLimit) {
              this.incident(`req ${id} no acknowledgement for ${silentLimit} ms after ${w.sentPieces} pieces, waiting for the module to say so`);
              w.timer = setTimeout(() => w.finish({ lost: true }), 45000);
              return;              // the module answers once it gives up on its side
            }
            w.credit++;               // presumed lost while the module is still answering
          }
          if (w.finished) return;
          w.credit--;
          ws.send(framed(id, body.subarray(o, o + CHUNK)));
          w.sentPieces++;
          if (body.length > CHUNK) {
            const whole = (extra && extra.size) || body.length;
            if (!this.progress || this.progress.id !== id) this.progress = { id, t0: Date.now(), sent: 0, total: whole };
            // What the module has acknowledged, not what has left here: with a wide
            // window the two differ by tens of kilobytes
            const landed = Math.max(0, w.sentPieces - (w.inFlight - w.credit));
            this.progress.sent = Math.round(Math.min(1, (landed * CHUNK) / body.length) * whole);
          }
        }
      } catch (e) {
        return w.finish({ lost: true });
      }
      // The module pours an upload into its card at its own pace, well after the last
      // byte has left here, so the wait grows with the size (a floor of 16 KB/s).
      // Counted from the module's last word and not from the last byte: a module that is
      // waiting for the printer's wire says so every two seconds, for minutes if it must
      w.finalMs = ANSWER_TIMEOUT_MS + Math.ceil(body.length / 16384) * 1000;
      w.timer = setTimeout(() => w.finish({ lost: true }), w.finalMs);
    });
  }

  async webSocketMessage(ws, message) {
    this.heard = Date.now();
    // Asked once each time the object wakes: what is kept in storage may be from an
    // older firmware that could not unpack deflate, or before a password change.
    if (!this.askedAuth) {
      this.askedAuth = true;
      if (!this.auth) this.auth = (await this.state.storage.get("auth")) || null;
      try { ws.send(JSON.stringify({ t: "hello", tg: !!this.env.TELEGRAM_TOKEN })); } catch (e) {}
    }
    if (typeof message === "string") {
      let m;
      try { m = JSON.parse(message); } catch { return; }
      if (m.t === "auth") {
        this.auth = m.open ? { open: true } : { user: m.user, realm: m.realm, nonce: m.nonce, ha1: m.ha1, key: m.key };
        // Which Yandex socket the printer is on, for the button that switches it on
        this.auth.socket = typeof m.socket === "string" && /^[\w-]{8,64}$/.test(m.socket) ? m.socket : "";
        // The window the module unpacks zlib in, in bits. A firmware that only said yes
        // took raw deflate, which is no longer sent: it gets the form.
        this.auth.inflate = Number.isInteger(m.inflate) && m.inflate >= 9 && m.inflate <= 15 ? m.inflate : 0;
        this.state.storage.put("auth", this.auth).catch(() => {});
        this.note(`auth from module, ${m.open ? "open" : "password set"}`);
        if (this.rttWake) { const wake = this.rttWake; this.rttWake = null; wake(); }
        return;
      }
      if (m.t === "st" && typeof m.b === "string") {
        // The module's own status, sent just before it starts taking a file
        this.cache.set("/api/status", { status: 200, headers: { "content-type": "application/json" },
                                        body: new TextEncoder().encode(m.b) });
        return;
      }
      if (m.t === "yx") {
        // A request the module would make to Yandex itself if it had the memory for a
        // second encrypted connection. Its own token comes with it and is not kept or
        // written anywhere here.
        let status = 0, text = "";
        if (typeof m.p === "string" && m.p.startsWith("/v1.0/") && typeof m.a === "string") {
          const r = await fetch("https://api.iot.yandex.net" + m.p, {
            method: m.b ? "POST" : "GET",
            headers: m.b ? { authorization: m.a, "content-type": "application/json" } : { authorization: m.a },
            body: m.b || undefined,
          }).catch(() => null);
          if (r) { status = r.status; text = await r.text().catch(() => ""); }
          if (status === 200 && m.p === "/v1.0/devices/actions" && /"value"\s*:\s*false/.test(m.b || "")) {
            this.offAsked = Date.now();
            await this.state.storage.put("offAsked", this.offAsked);
            this.incident("the module asked for its socket to be switched off");
          }
        }
        try { ws.send(JSON.stringify({ t: "yxr", q: m.q, s: status, b: text })); } catch (e) {}
        return;
      }
      if (m.t === "tgfetch") {
        if (this.env.TELEGRAM_TOKEN && m.c && m.id && m.route && m.token)
          await this.fetchForModule(m);
        return;
      }
      if (m.t === "tg") {
        if (this.env.TELEGRAM_TOKEN && m.c && m.x)
          await fetch(`https://api.telegram.org/bot${this.env.TELEGRAM_TOKEN}/sendMessage`, {
            method: "POST",
            headers: { "content-type": "application/json" },
            body: JSON.stringify({ chat_id: m.c, text: m.x }),
          }).catch(() => {});
        return;
      }
      if (m.k) {
        const w = this.waiting.get(m.k);
        if (w) {
          w.lastAck = Date.now();
          w.acks = (w.acks || 0) + 1;
          if (w.finalMs) {
            clearTimeout(w.timer);
            w.timer = setTimeout(() => w.finish({ lost: true }), w.finalMs);
          }
          if (w.acks <= 12 || w.acks % 20 === 0)
            this.note(`ack ${w.acks} for req ${m.k} n ${m.n || 1} at ${Date.now() - w.t0} ms, sent ${w.sentPieces}, credit ${w.credit}`);
          // The module can widen the window for one file when it has the memory for it
          if (m.c > 0 && m.c <= 32 && m.c !== w.inFlight) {
            w.credit += m.c - (w.inFlight || IN_FLIGHT);
            w.inFlight = m.c;
          }
          const most = w.inFlight || IN_FLIGHT;
          if (w.credit < most) w.credit = Math.min(most, w.credit + (m.n ?? 1));
          if (w.wake) { const wake = w.wake; w.wake = null; wake(true); }
        }
        return;
      }
      const w = this.waiting.get(m.i);
      if (!w) return;
      if (w.collect) {
        if (m.s) { w.status = m.s; w.headers = m.h; }
        if (m.e) {
          const total = w.collect.reduce((n, c) => n + c.length, 0);
          const body = new Uint8Array(total);
          let at = 0;
          for (const c of w.collect) { body.set(c, at); at += c.length; }
          const line = `req ${m.i} answered ${w.status} at ${Date.now() - w.t0} ms after ${w.sentPieces} pieces`;
          if ((w.status || 502) >= 500) this.incident(line); else this.note(line);
          w.finish({ status: w.status || 502, headers: w.headers, body });
        }
        return;
      }
      if (m.e) {
        w.over();
        if (w.writer) (m.x ? w.writer.abort("cut short") : w.writer.close()).catch(() => {});
        return;
      }
      if (m.s) {
        this.note(`req ${m.i} answered ${m.s} at ${Date.now() - w.t0} ms after ${w.sentPieces || 0} pieces`);
        const h = new Headers(m.h || {});
        // A download says its length, and a stream of a fixed length is what keeps the
        // header on the way out and fails the download if it ends short
        const length = w.busy && m.s === 200 ? Number(h.get("content-length")) : 0;
        const { readable, writable } = length > 0 ? new FixedLengthStream(length) : new TransformStream();
        w.writer = writable.getWriter();
        h.set("x-robots-tag", "noindex");
        w.resolve(asSent(readable, m.s, h));
      }
      return;
    }
    const bytes = new Uint8Array(message);
    if (bytes.length < 4) return;
    const id = (bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24)) >>> 0;
    const w = this.waiting.get(id);
    if (w && w.collect) w.collect.push(bytes.slice(4));
    else if (w && w.writer) {
      w.quiet();
      // Whoever was downloading has gone: the module is told, so that it stops reading
      // its card for nobody and the card is free for the next request
      w.writer.write(bytes.subarray(4)).catch(() => {
        if (w.gone) return;
        w.gone = true;
        w.over();
        this.note(`req ${id} was dropped by its reader, the module is told to stop`);
        try { ws.send(JSON.stringify({ t: "stop", i: id })); } catch (e) {}
      });
    }
  }

  webSocketClose(ws) {
    // The close of a socket the module has already replaced says nothing about the
    // requests in flight on the new one
    if (ws && this.state.getWebSockets("device").some((s) => s !== ws && s.readyState === 1)) return;
    this.incident("device socket closed");
    for (const [, w] of this.waiting) {
      if (w.finish) { w.finish({ lost: true }); continue; }
      w.over();
      if (w.writer) w.writer.abort("closed").catch(() => {});
      else w.resolve(LOST);
    }
    this.waiting.clear();
  }

  webSocketError(ws) {
    this.webSocketClose(ws);
  }
}
