// Web Serial helpers shared by the configuration pages.
// Desktop Chrome / Edge only (navigator.serial).

export const hasSerial = typeof navigator !== "undefined" && "serial" in navigator;

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// Base: opens a port, runs a read loop, and serializes requests.
class SerialBase {
  constructor() {
    this.port = null;
    this.reader = null;
    this.queue = Promise.resolve();
    this.onDisconnect = null;
  }

  get connected() { return this.port !== null; }

  async connect(baudRate = 115200) {
    const port = await navigator.serial.requestPort();
    await port.open({ baudRate });
    this.port = port;
    navigator.serial.addEventListener("disconnect", this._onDisc = (e) => {
      if (e.target === this.port) this._closed();
    });
    this._readLoop();
    await sleep(300);   // let a board that reset on open start talking
  }

  async disconnect() {
    const port = this.port;
    if (!port) return;
    this.port = null;
    try { await this.reader?.cancel(); } catch {}
    try { await port.close(); } catch {}
    this._closed();
  }

  _closed() {
    this.port = null;
    if (this._onDisc) navigator.serial.removeEventListener("disconnect", this._onDisc);
    this._onDisc = null;
    this.onDisconnect?.();
  }

  async _readLoop() {
    while (this.port && this.port.readable) {
      this.reader = this.port.readable.getReader();
      try {
        for (;;) {
          const { value, done } = await this.reader.read();
          if (done) break;
          if (value) this._onBytes(value);
        }
      } catch {
        // device unplugged or port closed
      } finally {
        try { this.reader.releaseLock(); } catch {}
      }
      if (!this.port) break;
      await sleep(50);
    }
    if (this.port) this._closed();
  }

  async _write(bytes) {
    const w = this.port.writable.getWriter();
    try { await w.write(bytes); } finally { w.releaseLock(); }
  }

  // one request at a time; errors don't block the queue
  _enqueue(fn) {
    const p = this.queue.then(fn, fn);
    this.queue = p.catch(() => {});
    return p;
  }
}

// Text console (the relay's USB CLI): send "cmd\r", the device echoes the
// command and answers on a line starting with "  -> ".
export class LineConsole extends SerialBase {
  constructor() {
    super();
    this.buf = "";
    this.decoder = new TextDecoder();
    this.waiter = null;
    this.onLine = null;   // (line) => void, every line from the device
  }

  _onBytes(bytes) {
    this.buf += this.decoder.decode(bytes, { stream: true });
    let i;
    while ((i = this.buf.search(/\r?\n/)) >= 0) {
      const line = this.buf.slice(0, i);
      this.buf = this.buf.slice(i + (this.buf[i] === "\r" ? 2 : 1));
      if (line.trim() === "") continue;
      this.onLine?.(line);
      const m = line.match(/^\s*->\s?(.*)$/);
      if (m && this.waiter) {
        const w = this.waiter;
        this.waiter = null;
        w.resolve(m[1]);
      }
    }
  }

  // Sends one command, resolves with the reply text (after "-> ").
  command(cmd, timeoutMs = 4000) {
    return this._enqueue(async () => {
      if (!this.port) throw new Error("Not connected");
      const reply = new Promise((resolve, reject) => {
        this.waiter = { resolve };
        setTimeout(() => {
          if (this.waiter && this.waiter.resolve === resolve) {
            this.waiter = null;
            reject(new Error(`No reply to "${cmd}"`));
          }
        }, timeoutMs);
      });
      await this._write(new TextEncoder().encode(cmd + "\r"));
      return reply;
    });
  }
}

// Companion protocol over USB: to device '<' len16le payload,
// from device '>' len16le payload. Codes >= 0x80 are unsolicited pushes.
export class FrameLink extends SerialBase {
  constructor() {
    super();
    this.rx = [];
    this.waiter = null;
    this.onPush = null;   // (frame: Uint8Array) => void
  }

  _onBytes(bytes) {
    for (const b of bytes) this.rx.push(b);
    for (;;) {
      const start = this.rx.indexOf(0x3e);   // '>'
      if (start < 0) { this.rx.length = 0; return; }
      if (start > 0) this.rx.splice(0, start);   // drop stray text before the frame
      if (this.rx.length < 3) return;
      const len = this.rx[1] | (this.rx[2] << 8);
      if (len === 0 || len > 300) { this.rx.splice(0, 1); continue; }   // not a real header
      if (this.rx.length < 3 + len) return;
      const frame = Uint8Array.from(this.rx.slice(3, 3 + len));
      this.rx.splice(0, 3 + len);
      if (frame[0] >= 0x80) {
        this.onPush?.(frame);
      } else if (this.waiter) {
        const w = this.waiter;
        this.waiter = null;
        w.resolve(frame);
      }
    }
  }

  // Sends one command frame, resolves with the next non-push reply frame.
  request(payload, timeoutMs = 3000) {
    return this._enqueue(async () => {
      if (!this.port) throw new Error("Not connected");
      const reply = new Promise((resolve, reject) => {
        this.waiter = { resolve };
        setTimeout(() => {
          if (this.waiter && this.waiter.resolve === resolve) {
            this.waiter = null;
            reject(new Error("No reply from device"));
          }
        }, timeoutMs);
      });
      const out = new Uint8Array(3 + payload.length);
      out[0] = 0x3c;   // '<'
      out[1] = payload.length & 0xff;
      out[2] = payload.length >> 8;
      out.set(payload, 3);
      await this._write(out);
      return reply;
    });
  }
}

// ---- small shared helpers

export function toHex(bytes) {
  return Array.from(bytes, (b) => b.toString(16).padStart(2, "0")).join("");
}

export function fromHex(hex) {
  const clean = hex.trim();
  if (!/^([0-9a-fA-F]{2})*$/.test(clean)) return null;
  const out = new Uint8Array(clean.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(clean.substr(i * 2, 2), 16);
  return out;
}

export function fromBase64(s) {
  try {
    const bin = atob(s.trim());
    return Uint8Array.from(bin, (c) => c.charCodeAt(0));
  } catch { return null; }
}

export function toBase64(bytes) {
  return btoa(String.fromCharCode(...bytes));
}

export function toast(msg) {
  let t = document.querySelector(".toast");
  if (!t) {
    t = document.createElement("div");
    t.className = "toast";
    t.setAttribute("role", "status");
    document.body.appendChild(t);
  }
  t.textContent = msg;
  t.classList.add("show");
  clearTimeout(t._h);
  t._h = setTimeout(() => t.classList.remove("show"), 2600);
}

export function serialUnsupportedNote() {
  return `<div class="note"><strong>This browser can't talk to USB devices.</strong>
    Configuring needs desktop Chrome or Edge (Windows, Mac, Linux or ChromeOS).
    Phones and Firefox/Safari can't use Web Serial.</div>`;
}
