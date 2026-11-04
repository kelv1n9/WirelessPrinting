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

const UPLOAD_LIMIT = 64 * 1024 * 1024;
const CHUNK = 2040;                       // plus four bytes of number, one module buffer
const IN_FLIGHT = 5;                      // one fewer than the module has slots for pieces
const ACK_GRACE_MS = 4000;
const UPLOAD_ATTEMPTS = 3;
const RECONNECT_WAIT_MS = 90000;
const SILENT_GIVE_UP_MS = 10000;
const ANSWER_TIMEOUT_MS = 30000;
const PASS_DOWN = ["authorization", "content-type", "x-api-key", "if-none-match", "x-relay-once"];
const LOST = Symbol("lost");
const CACHED = new Set(["/api/status", "/printer/send/status"]);
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
  }

  note(text) {
    if (!this.env.TRACE) return;
    this.trail.push(`${new Date().toISOString().slice(11, 23)} ${text}`);
    if (this.trail.length > 200) this.trail.shift();
  }

  device() {
    const open = this.state.getWebSockets("device").filter((ws) => ws.readyState === 1);
    return open.length ? open[open.length - 1] : null;
  }

  async fetch(request) {
    const url = new URL(request.url);

    if (url.pathname === "/.device") {
      if (request.headers.get("Upgrade") !== "websocket")
        return new Response("expected a websocket\n", { status: 426 });
      const given = (request.headers.get("Authorization") || "").replace(/^Bearer /, "");
      if (!sameSecret(given, this.env.DEVICE_KEY))
        return new Response("no\n", { status: 403 });
      for (const old of this.state.getWebSockets("device"))
        old.close(1000, "replaced");
      const pair = new WebSocketPair();
      this.state.acceptWebSocket(pair[1], ["device"]);
      pair[1].send(JSON.stringify({ t: "hello", tg: !!this.env.TELEGRAM_TOKEN }));
      return new Response(null, { status: 101, webSocket: pair[0] });
    }

    if (url.pathname === "/.trace") {
      const given = (request.headers.get("Authorization") || "").replace(/^Bearer /, "");
      if (!sameSecret(given, this.env.DEVICE_KEY))
        return new Response("no\n", { status: 403 });
      return new Response(this.trail.join("\n") + "\n");
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

    // A large answer coming back up the socket hangs the module (seen 04.11.2026: a
    // 200 KB download stopped after 2 KB and the module needed a power cycle). Closed
    // here until the module side is fixed and can be flashed.
    if (url.pathname === "/download")
      return new Response("Downloading files through the relay is switched off for now.\n", { status: 501 });

    const ws = this.device();
    if (!ws)
      return new Response("The printer module is not connected right now.\n",
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
    if (!hasBody && this.uploading > 0) {
      const cached = this.cache.get(url.pathname);
      if (cached) {
        const h = new Headers(cached.headers || {});
        h.set("x-relay-cached", "uploading");
        return new Response(cached.body, { status: 200, headers: h });
      }
      return new Response("The module is busy taking a file. Try again when it is done.\n",
                          { status: 503, headers: { "retry-after": "5" } });
    }
    if (!hasBody && CACHED.has(url.pathname)) {
      const last = await this.sendBody(ws, request.method, url.pathname + url.search, headers, new Uint8Array(0));
      if (last.lost) return wentAway();
      if (last.status === 200) this.cache.set(url.pathname, last);
      return new Response(last.body, { status: last.status, headers: new Headers(last.headers || {}) });
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
    const last = await this.deliver(request.method, url.pathname + url.search, headers, buffered);
    if (!last)
      return new Response("The upload did not get through the relay. Try again.\n", { status: 503 });
    const h = new Headers(last.headers || {});
    h.set("x-robots-tag", "noindex");
    return new Response(last.body, { status: last.status, headers: h });
  }

  // A body, tried up to three times, one body at a time. Resolves with the module's
  // whole answer, or null when it never got through.
  deliver(method, path, headers, body) {
    const turn = this.bodies.then(async () => {
      this.uploading++;
      try {
      let last = null;
      for (let attempt = 1; attempt <= UPLOAD_ATTEMPTS; attempt++) {
        const device = await this.waitForDevice(RECONNECT_WAIT_MS);
        if (!device) break;
        last = await this.sendBody(device, method, path, headers, body);
        // A firmware image that went down whole and then lost the module is a flash that
        // worked: the module reboots before its answer is out. Sending it again only
        // met the new firmware with yesterday's credentials.
        if (last.lost && last.sentAll && path.startsWith("/update")) {
          this.note(`all of ${path} went down and the module dropped away, taken as flashed`);
          return { status: 200, headers: { "content-type": "text/plain" },
                   body: new TextEncoder().encode("Sent whole; the module dropped away to reboot, which is what a good flash does. Check /api/status in a minute.\n") };
        }
        if (!last.lost && last.status !== 502 && last.status !== 504) break;
        this.note(`attempt ${attempt} of ${path} did not get through (${last.lost ? "lost" : last.status}), ${attempt < UPLOAD_ATTEMPTS ? "trying again" : "giving up"}`);
      }
      return last && !last.lost ? last : null;
      } finally {
        this.uploading--;
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

    const answer = await this.deliver("POST", m.route, {
      "content-type": `multipart/form-data; boundary=${boundary}`,
      "x-relay-once": m.token,
    }, body);
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
  carry(ws, method, path, headers) {
    const id = this.newId();
    let resolve;
    const answered = new Promise((r) => (resolve = r));
    const w = { resolve, timer: null, writer: null, t0: Date.now() };
    this.waiting.set(id, w);
    w.timer = setTimeout(() => {
      this.waiting.delete(id);
      if (w.writer) w.writer.abort("timeout").catch(() => {});
      else resolve(new Response("The printer module did not answer in time.\n", { status: 504 }));
    }, ANSWER_TIMEOUT_MS);
    try {
      ws.send(JSON.stringify({ i: id, m: method, p: path, h: headers, l: 0 }));
    } catch (e) {
      clearTimeout(w.timer);
      this.waiting.delete(id);
      resolve(LOST);
    }
    return answered;
  }

  // One try at a body: resolves with the module's whole answer, or { lost: true }
  sendBody(ws, method, path, headers, body) {
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
        ws.send(JSON.stringify({ i: id, m: method, p: path, h: headers, l: body.length }));
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
              this.note(`req ${id} no acknowledgement for ${silentLimit} ms after ${w.sentPieces} pieces, waiting for the module to say so`);
              w.timer = setTimeout(() => w.finish({ lost: true }), 45000);
              return;              // the module answers once it gives up on its side
            }
            w.credit++;               // presumed lost while the module is still answering
          }
          if (w.finished) return;
          w.credit--;
          ws.send(framed(id, body.subarray(o, o + CHUNK)));
          w.sentPieces++;
        }
      } catch (e) {
        return w.finish({ lost: true });
      }
      // The module pours an upload into its card at its own pace, well after the last
      // byte has left here, so the wait grows with the size (a floor of 16 KB/s).
      w.timer = setTimeout(() => w.finish({ lost: true }),
                           ANSWER_TIMEOUT_MS + Math.ceil(body.length / 16384) * 1000);
    });
  }

  async webSocketMessage(ws, message) {
    if (typeof message === "string") {
      let m;
      try { m = JSON.parse(message); } catch { return; }
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
          if (w.acks <= 12 || w.acks % 20 === 0)
            this.note(`ack ${w.acks} for req ${m.k} n ${m.n || 1} at ${Date.now() - w.t0} ms, sent ${w.sentPieces}, credit ${w.credit}`);
          const most = w.inFlight || IN_FLIGHT;
          if (w.credit < most) w.credit = Math.min(most, w.credit + (m.n || 1));
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
          this.note(`req ${m.i} answered ${w.status} at ${Date.now() - w.t0} ms after ${w.sentPieces} pieces`);
          w.finish({ status: w.status || 502, headers: w.headers, body });
        }
        return;
      }
      if (m.e) {
        clearTimeout(w.timer);
        this.waiting.delete(m.i);
        if (w.writer) w.writer.close().catch(() => {});
        return;
      }
      if (m.s) {
        this.note(`req ${m.i} answered ${m.s} at ${Date.now() - w.t0} ms after ${w.sentPieces || 0} pieces`);
        const { readable, writable } = new TransformStream();
        w.writer = writable.getWriter();
        const h = new Headers(m.h || {});
        h.set("x-robots-tag", "noindex");
        w.resolve(new Response(readable, { status: m.s, headers: h }));
      }
      return;
    }
    const bytes = new Uint8Array(message);
    if (bytes.length < 4) return;
    const id = (bytes[0] | (bytes[1] << 8) | (bytes[2] << 16) | (bytes[3] << 24)) >>> 0;
    const w = this.waiting.get(id);
    if (w && w.collect) w.collect.push(bytes.slice(4));
    else if (w && w.writer) w.writer.write(bytes.subarray(4)).catch(() => {});
  }

  webSocketClose() {
    this.note("device socket closed");
    for (const [, w] of this.waiting) {
      if (w.finish) { w.finish({ lost: true }); continue; }
      clearTimeout(w.timer);
      if (w.writer) w.writer.abort("closed").catch(() => {});
      else w.resolve(LOST);
    }
    this.waiting.clear();
  }

  webSocketError() {
    this.webSocketClose();
  }
}
