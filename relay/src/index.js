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
const ERASE_PAUSE_MS = 15000;             // the firmware route erases a whole flash slot first
const ANSWER_TIMEOUT_MS = 30000;
const PASS_DOWN = ["authorization", "content-type", "x-api-key", "if-none-match"];

export default {
  async fetch(request, env) {
    const relay = env.RELAY.get(env.RELAY.idFromName("kp3s"));
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
  }

  device() {
    const all = this.state.getWebSockets("device");
    return all.length ? all[all.length - 1] : null;
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
        const text = m.text || (m.document ? "/__document" : "");
        if (text)
          ws.send(JSON.stringify({ t: "tg", c: String(m.chat.id), x: text }));
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
    if (hasBody && !length) {
      buffered = new Uint8Array(await request.arrayBuffer());
      length = buffered.length;
    }
    if (length > UPLOAD_LIMIT)
      return new Response("Too large to send through the relay.\n", { status: 413 });

    const headers = {};
    for (const name of PASS_DOWN) {
      const v = request.headers.get(name);
      if (v) headers[name] = v;
    }

    const id = this.next++;
    if (this.next > 0x7fffffff) this.next = 1;

    let resolve;
    const answered = new Promise((r) => (resolve = r));
    const w = { resolve, timer: null, writer: null, credit: IN_FLIGHT, wake: null };
    this.waiting.set(id, w);
    // The module pours an upload into its card at its own pace, well after the last
    // byte has left here, so the wait grows with the size (a floor of 16 KB/s).
    const wait = ANSWER_TIMEOUT_MS + (hasBody ? Math.ceil(length / 16384) * 1000 : 0);
    const arm = () => {
      w.timer = setTimeout(() => {
        this.waiting.delete(id);
        if (w.writer) w.writer.abort("timeout").catch(() => {});
        else resolve(new Response("The printer module did not answer in time.\n", { status: 504 }));
      }, wait);
    };

    const send = async () => {
      ws.send(JSON.stringify({ i: id, m: request.method, p: url.pathname + url.search, h: headers,
                               l: hasBody ? length : 0 }));
      if (hasBody) {
        // The firmware route wipes its flash slot as soon as the part header arrives and
        // reads nothing for several seconds. Module builds before 04.11.2026 gave up on
        // a stalled loopback within two seconds, so the rest waits until the wipe is done.
        let sent = 0;
        const pushChunks = async (bytes) => {
          for (let o = 0; o < bytes.length; o += CHUNK) {
            if (url.pathname === "/update" && sent === 1)
              await new Promise((r) => setTimeout(r, ERASE_PAUSE_MS));
            sent++;
            while (w.credit <= 0) {
              const more = await new Promise((r) => {
                w.wake = r;
                setTimeout(() => r(false), ANSWER_TIMEOUT_MS);
              });
              if (more === false && w.credit <= 0) throw new Error("the module stopped taking the upload");
            }
            w.credit--;
            ws.send(framed(id, bytes.subarray(o, o + CHUNK)));
          }
        };
        if (buffered) await pushChunks(buffered);
        else {
          const reader = request.body.getReader();
          for (;;) {
            const { done, value } = await reader.read();
            if (done) break;
            await pushChunks(value);
          }
        }
      }
      arm();
    };

    const turn = hasBody ? (this.bodies = this.bodies.then(send, send)) : send();
    turn.catch(() => {
      clearTimeout(w.timer);
      this.waiting.delete(id);
      resolve(new Response("The printer module went away.\n", { status: 503 }));
    });
    return answered;
  }

  async webSocketMessage(ws, message) {
    if (typeof message === "string") {
      let m;
      try { m = JSON.parse(message); } catch { return; }
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
          w.credit += m.n || 1;
          if (w.wake) { const wake = w.wake; w.wake = null; wake(true); }
        }
        return;
      }
      const w = this.waiting.get(m.i);
      if (!w) return;
      if (m.e) {
        clearTimeout(w.timer);
        this.waiting.delete(m.i);
        if (w.writer) w.writer.close().catch(() => {});
        return;
      }
      if (m.s) {
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
    if (w && w.writer) w.writer.write(bytes.subarray(4)).catch(() => {});
  }

  webSocketClose() {
    for (const [, w] of this.waiting) {
      clearTimeout(w.timer);
      if (w.writer) w.writer.abort("closed").catch(() => {});
      else w.resolve(new Response("The printer module went away.\n", { status: 503 }));
    }
    this.waiting.clear();
  }

  webSocketError() {
    this.webSocketClose();
  }
}
