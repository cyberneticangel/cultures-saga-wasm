// Runs the recompiled game (cultures-<variant>.wasm) in a Web Worker.
// The game's main loop never yields, so everything it needs is synchronous:
// files via synchronous XHR to serve.py, input/frames/audio via a SharedArrayBuffer.
'use strict';

let mem, u8, shared, ctl, frameBytes, audio16;
let variant = 'gamesaga';   // which game version: selects the wasm and the save folder (serve.py ?v=)
const fsq = () => 'v=' + encodeURIComponent(variant);
const L = self.LAYOUT = null;

// ---------------------------------------------------------------- strings (cp1252)
const dec1252 = new TextDecoder('windows-1252');
const enc1252 = (() => {
  const m = new Map();
  for (let i = 0; i < 256; i++) m.set(dec1252.decode(new Uint8Array([i])), i);
  return s => Uint8Array.from(s, ch => m.get(ch) ?? 63);
})();
function cstr(p) {
  let e = p;
  while (u8[e]) e++;
  return dec1252.decode(u8.subarray(p, e));
}
function refresh() { if (u8.buffer !== mem.buffer) u8 = new Uint8Array(mem.buffer); }

// ---------------------------------------------------------------- virtual file system
// index: lowercase path -> {path, dir, size}
const index = new Map();
const children = new Map();   // lowercase dir -> Set of names
function addEntry(path, dir, size) {
  const k = path.toLowerCase();
  index.set(k, { path, dir, size });
  const slash = path.lastIndexOf('/');
  const parent = slash < 0 ? '' : path.slice(0, slash).toLowerCase();
  if (!children.has(parent)) children.set(parent, new Map());
  children.get(parent).set(path.slice(slash + 1).toLowerCase(), path.slice(slash + 1));
  if (dir && !children.has(k)) children.set(k, new Map());
}
function removeEntry(path) {
  const k = path.toLowerCase();
  index.delete(k);
  const slash = path.lastIndexOf('/');
  const parent = slash < 0 ? '' : path.slice(0, slash).toLowerCase();
  children.get(parent)?.delete(path.slice(slash + 1).toLowerCase());
}
function norm(p) { return p === '.' ? '' : p.replace(/^\.\//, ''); }

function xhr(method, url, body, headers) {
  const x = new XMLHttpRequest();
  x.open(method, url, false);
  x.responseType = 'arraybuffer';
  for (const k in headers || {}) x.setRequestHeader(k, headers[k]);
  x.send(body ?? null);
  return x;
}
function loadIndex() {
  const x = new XMLHttpRequest();
  x.open('GET', '/fs/index?' + fsq(), false);
  x.send();
  const idx = JSON.parse(x.responseText);
  for (const p in idx) addEntry(p, !!idx[p][0], idx[p][1]);
  children.set('', children.get('') || new Map());
}

// block cache for read-only file data
const BLOCK = 1 << 20;
const cache = new Map();   // key -> Uint8Array
let cacheBytes = 0;
const CACHE_MAX = 192 << 20;
function getBlock(path, size, n) {
  const key = path + '#' + n;
  let b = cache.get(key);
  if (b) { cache.delete(key); cache.set(key, b); return b; }
  const a = n * BLOCK, e = Math.min(size, a + BLOCK) - 1;
  const x = xhr('GET', '/fs/file?' + fsq() + '&path=' + encodeURIComponent(path), null, { Range: `bytes=${a}-${e}` });
  if (x.status !== 206 && x.status !== 200) throw new Error('read failed ' + path + ' ' + x.status);
  b = new Uint8Array(x.response);
  if (x.status === 200 && a > 0) b = b.subarray(a, e + 1);
  cache.set(key, b);
  cacheBytes += b.length;
  while (cacheBytes > CACHE_MAX) {
    const [k, v] = cache.entries().next().value;
    cache.delete(k); cacheBytes -= v.length;
  }
  return b;
}
function invalidate(path) {
  for (const k of [...cache.keys()]) if (k.startsWith(path + '#')) { cacheBytes -= cache.get(k).length; cache.delete(k); }
}
function readWhole(path, size) {
  if (!size) return new Uint8Array(0);
  const x = xhr('GET', '/fs/file?' + fsq() + '&path=' + encodeURIComponent(path));
  return new Uint8Array(x.response);
}

const fds = [];   // {path, pos, size, data (if writable), dirty}
function allocFd(o) {
  let i = fds.indexOf(null);
  if (i < 0) { i = fds.length; fds.push(null); }
  fds[i] = o;
  return i;
}
function flush(f) {
  if (!f.data || !f.dirty) return;
  const body = f.data.slice(0, f.size);
  const x = xhr('PUT', '/fs/file?' + fsq() + '&path=' + encodeURIComponent(f.path), body);
  if (x.status !== 200) console.error('write failed', f.path, x.status);
  f.dirty = false;
  invalidate(f.path);
  const e = index.get(f.path.toLowerCase());
  if (e) e.size = f.size;
}
function fsop(body) {
  const x = new XMLHttpRequest();
  x.open('POST', '/fs/op?' + fsq(), false);
  x.send(JSON.stringify(body));
  return x.status === 200;
}

// ---------------------------------------------------------------- shared state with the page
// ctl (Int32) slots:
const C = { EVW: 0, EVR: 1, FSEQ: 2, FW: 3, FH: 4, AW: 5, AR: 6, ARATE: 7, WAKE: 8, EXIT: 9, MW: 10, MH: 11, FFMT: 12, SPEED: 13, QOL: 20 };
const EV_BASE = 64, EV_N = 256, FRAME_OFF = 8192, FRAME_MAX = 3440 * 1440 * 4;   // largest display mode (ddraw.c)
const AUDIO_OFF = FRAME_OFF + FRAME_MAX, AUDIO_FRAMES = 1 << 15;
// multiplayer: the page owns the WebSocket; incoming messages arrive in a byte ring ([u32 len][data]...)
const NET_OFF = AUDIO_OFF + AUDIO_FRAMES * 4, NET_SIZE = 4 << 20, NET_W = 16, NET_R = 17, NET_ST = 18;
let netRing, netUrl = '';

let logBuf = '';
function log(s) {
  logBuf += s;
  let i;
  while ((i = logBuf.indexOf('\n')) >= 0) {
    postMessage({ type: 'log', text: logBuf.slice(0, i) });
    logBuf = logBuf.slice(i + 1);
  }
}

class Exit extends Error { constructor(code) { super('exit ' + code); this.code = code; } }
const t0 = performance.now();

const env = {
  js_ticks_ms: () => performance.now() - t0,
  js_game_speed: () => Atomics.load(ctl, C.SPEED),   // percent, set by the page (0 = paused)
  js_qol_get: slot => Atomics.load(ctl, C.QOL + slot),   // qol.c settings and requests (QOL_* in runtime.h)
  js_qol_set: (slot, v) => Atomics.store(ctl, C.QOL + slot, v),
  js_sleep: ms => { if (ms > 0) Atomics.wait(ctl, C.WAKE, Atomics.load(ctl, C.WAKE), ms); },
  js_log: (p, n) => { refresh(); log(dec1252.decode(u8.subarray(p, p + n))); },
  js_exit: code => { throw new Exit(code); },
  js_set_mode: (w, h) => { Atomics.store(ctl, C.MW, w); Atomics.store(ctl, C.MH, h); postMessage({ type: 'mode', w, h }); },
  js_present: (p, w, h, fmt) => {   // fmt 0 = RGBA, 1 = RGB565 (converted by the page's WebGL shader)
    refresh();
    const n = w * h * (fmt === 1 ? 2 : 4);
    if (n > FRAME_MAX) return;
    frameBytes.set(u8.subarray(p, p + n));
    ctl[C.FW] = w; ctl[C.FH] = h; ctl[C.FFMT] = fmt;
    Atomics.add(ctl, C.FSEQ, 1);
  },
  js_poll_event: p => {
    const r = Atomics.load(ctl, C.EVR), w = Atomics.load(ctl, C.EVW);
    if (r === w) return 0;
    const i32 = new Int32Array(mem.buffer, p, 5);
    const o = EV_BASE + (r % EV_N) * 5;
    for (let k = 0; k < 5; k++) i32[k] = ctl[o + k];
    Atomics.store(ctl, C.EVR, r + 1);
    return 1;
  },
  js_open: (pp, write, create, trunc) => {
    refresh();
    const path = norm(cstr(pp));
    let e = index.get(path.toLowerCase());
    if (e && e.dir) return -1;
    if (!e && !create) return -1;
    if (!e) {
      // parent must exist
      const slash = path.lastIndexOf('/');
      if (slash >= 0 && !index.get(path.slice(0, slash).toLowerCase())) return -1;
      addEntry(path, false, 0);
      e = index.get(path.toLowerCase());
      const f = { path: e.path, pos: 0, size: 0, data: new Uint8Array(4096), dirty: true };
      flush(f);
      return allocFd(f);
    }
    const f = { path: e.path, pos: 0, size: trunc ? 0 : e.size, data: null, dirty: !!trunc };
    if (write) {
      f.data = trunc ? new Uint8Array(4096) : readWhole(e.path, e.size);
      if (f.data.length === 0) f.data = new Uint8Array(4096);
      if (trunc) flush(f);
    }
    return allocFd(f);
  },
  js_read: (fd, p, n) => {
    const f = fds[fd];
    if (!f) return -1;
    refresh();
    n = Math.max(0, Math.min(n, f.size - f.pos));
    if (f.data) {
      u8.set(f.data.subarray(f.pos, f.pos + n), p);
    } else {
      let done = 0;
      while (done < n) {
        const pos = f.pos + done, bn = Math.floor(pos / BLOCK), off = pos - bn * BLOCK;
        const b = getBlock(f.path, f.size, bn);
        const take = Math.min(n - done, b.length - off);
        u8.set(b.subarray(off, off + take), p + done);
        done += take;
      }
    }
    f.pos += n;
    return n;
  },
  js_write: (fd, p, n) => {
    const f = fds[fd];
    if (!f) return -1;
    refresh();
    if (!f.data) { f.data = readWhole(f.path, f.size); if (!f.data.length) f.data = new Uint8Array(4096); }
    const end = f.pos + n;
    if (end > f.data.length) {
      const nd = new Uint8Array(Math.max(end, f.data.length * 2));
      nd.set(f.data); f.data = nd;
    }
    f.data.set(u8.subarray(p, p + n), f.pos);
    f.pos = end;
    if (end > f.size) f.size = end;
    f.dirty = true;
    return n;
  },
  js_seek: (fd, off, whence) => {
    const f = fds[fd];
    if (!f) return -1;
    const np = whence === 0 ? off : whence === 1 ? f.pos + off : f.size + off;
    if (np < 0) return -1;
    f.pos = np;
    return np;
  },
  js_size: fd => fds[fd] ? fds[fd].size : -1,
  js_close: fd => { const f = fds[fd]; if (f) { flush(f); fds[fd] = null; } },
  js_unlink: pp => {
    refresh();
    const path = norm(cstr(pp)), e = index.get(path.toLowerCase());
    if (!e) return -1;
    fsop({ op: 'unlink', path: e.path });
    removeEntry(e.path); invalidate(e.path);
    return 0;
  },
  js_mkdir: pp => {
    refresh();
    const path = norm(cstr(pp));
    if (index.has(path.toLowerCase())) return -1;
    fsop({ op: 'mkdir', path });
    addEntry(path, true, 0);
    return 0;
  },
  js_rename: (ap, bp) => {
    refresh();
    const a = norm(cstr(ap)), b = norm(cstr(bp)), e = index.get(a.toLowerCase());
    if (!e) return -1;
    for (const f of fds) if (f && f.path === e.path) flush(f);
    if (!fsop({ op: 'rename', path: e.path, to: b })) return -1;
    removeEntry(e.path); invalidate(e.path);
    addEntry(b, false, e.size);
    return 0;
  },
  js_listdir: (pp, namesP, cap, isDirP, maxn) => {
    refresh();
    const path = norm(cstr(pp));
    const ch = children.get(path.toLowerCase());
    if (!ch) return -1;
    let o = 0, n = 0;
    for (const name of ch.values()) {
      if (n >= maxn) break;
      const bytes = enc1252(name);
      if (o + bytes.length + 1 > cap) break;
      u8.set(bytes, namesP + o); u8[namesP + o + bytes.length] = 0;
      o += bytes.length + 1;
      const full = path ? path + '/' + name : name;
      u8[isDirP + n] = index.get(full.toLowerCase())?.dir ? 1 : 0;
      n++;
    }
    return n;
  },
  js_stat: (pp, isDirP, sizeP) => {
    refresh();
    const e = index.get(norm(cstr(pp)).toLowerCase());
    if (!e) return -1;
    new Int32Array(mem.buffer, isDirP, 1)[0] = e.dir ? 1 : 0;
    new Uint32Array(mem.buffer, sizeP, 1)[0] = e.size;
    return 0;
  },
  js_set_cursor: (id, p, w, h, hx, hy) => {
    let rgba = null;
    if (p) { refresh(); rgba = u8.slice(p, p + w * h * 4); }
    postMessage({ type: 'cursor', id, rgba, w, h, hx, hy });
  },
  js_audio_open: rate => { Atomics.store(ctl, C.ARATE, rate); postMessage({ type: 'audio', rate }); },
  js_audio_queued: () => {
    const w = Atomics.load(ctl, C.AW), r = Atomics.load(ctl, C.AR);
    return ctl[C.ARATE] && Atomics.load(ctl, 12) ? w - r : 1 << 30;   // slot 12: audio running
  },
  js_net_open: up => {
    refresh();
    const url = cstr(up) || netUrl;
    Atomics.store(ctl, NET_R, Atomics.load(ctl, NET_W));   // drop anything from an earlier connection
    Atomics.store(ctl, NET_ST, 1);
    postMessage({ type: 'net-open', url });
  },
  js_net_state: () => Atomics.load(ctl, NET_ST),
  js_net_send: (p, n) => {
    if (Atomics.load(ctl, NET_ST) !== 2) return -1;
    refresh();
    const data = u8.slice(p, p + n);
    postMessage({ type: 'net-send', data }, [data.buffer]);
    return 0;
  },
  js_net_recv: (p, cap) => {
    let r = Atomics.load(ctl, NET_R) >>> 0;
    const w = Atomics.load(ctl, NET_W) >>> 0;
    if (r === w) return 0;
    const byte = k => netRing[(r + k) % NET_SIZE];
    const len = (byte(0) | byte(1) << 8 | byte(2) << 16 | byte(3) << 24) >>> 0;
    refresh();
    const n = Math.min(len, cap), start = (r + 4) % NET_SIZE;
    const first = Math.min(n, NET_SIZE - start);
    u8.set(netRing.subarray(start, start + first), p);
    if (n > first) u8.set(netRing.subarray(0, n - first), p + first);
    Atomics.store(ctl, NET_R, (r + 4 + len) | 0);
    return n;
  },
  js_net_close: () => { Atomics.store(ctl, NET_ST, 0); postMessage({ type: 'net-close' }); },
  js_audio_push: (p, n) => {
    refresh();
    let w = Atomics.load(ctl, C.AW);
    const src = new Int16Array(mem.buffer, p, n * 2);
    for (let i = 0; i < n; i++) {
      const o = (w % AUDIO_FRAMES) * 2;
      audio16[o] = src[2 * i]; audio16[o + 1] = src[2 * i + 1];
      w++;
    }
    Atomics.store(ctl, C.AW, w);
  },
};

const wasi = {
  environ_sizes_get: (countP, sizeP) => { new Uint32Array(mem.buffer, countP, 1)[0] = 0; new Uint32Array(mem.buffer, sizeP, 1)[0] = 0; return 0; },
  environ_get: () => 0,
  proc_exit: code => { throw new Exit(code); },
  clock_time_get: (id, prec, out) => {
    const ns = BigInt(Math.round((id === 0 ? Date.now() : performance.now()) * 1e6));
    new BigUint64Array(mem.buffer, out, 1)[0] = ns;
    return 0;
  },
};

onmessage = async ev => {
  const msg = ev.data;
  if (msg.type !== 'start') return;
  shared = msg.shared;
  ctl = new Int32Array(shared, 0, 2048);
  frameBytes = new Uint8Array(shared, FRAME_OFF, FRAME_MAX);
  audio16 = new Int16Array(shared, AUDIO_OFF, AUDIO_FRAMES * 2);
  netRing = new Uint8Array(shared, NET_OFF, NET_SIZE);
  netUrl = msg.netUrl || '';
  variant = msg.variant;
  const exe = msg.exe;
  try {
    postMessage({ type: 'status', text: 'Indexing game files…' });
    loadIndex();
    if (!index.has(exe.toLowerCase())) throw new Error(exe + ' not found in the served game directory');
    postMessage({ type: 'status', text: `Loading cultures-${variant}.wasm…` });
    const { instance } = await WebAssembly.instantiateStreaming(fetch(`cultures-${variant}.wasm`), { env, wasi_snapshot_preview1: wasi });
    mem = instance.exports.memory;
    u8 = new Uint8Array(mem.buffer);
    instance.exports._initialize();
    postMessage({ type: 'status', text: 'Running' });
    instance.exports.game_main(msg.logLevel ?? 1, msg.skipIntro ? 1 : 0);
    postMessage({ type: 'exit', code: 0 });
  } catch (e) {
    for (const f of fds) if (f) flush(f);
    if (e instanceof Exit) postMessage({ type: 'exit', code: e.code });
    else { log('\n'); postMessage({ type: 'error', text: String(e && e.stack || e) }); }
  }
};
