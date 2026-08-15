// Generates resources/images/logo_25.png — the app menu icon (25x25).
// Minimal PNG encoder (RGBA, no external deps). Motif: three heading bars
// (RSS/feed-reader) in white on transparent, with an accent-blue dot.
'use strict';
const fs = require('fs');
const path = require('path');
const zlib = require('zlib');

const W = 25, H = 25;
const ACCENT = [0, 85, 170, 255];     // cobalt blue (matches DEFAULT_ACCENT)
const WHITE = [255, 255, 255, 255];

const px = new Array(W * H).fill(null).map(() => [0, 0, 0, 0]);

function set(x, y, c) {
  if (x < 0 || y < 0 || x >= W || y >= H) return;
  px[y * W + x] = c;
}
function rect(x0, y0, w, h, c) {
  for (let y = y0; y < y0 + h; y++) for (let x = x0; x < x0 + w; x++) set(x, y, c);
}
function dot(cx, cy, r, c) {
  for (let y = cy - r; y <= cy + r; y++)
    for (let x = cx - r; x <= cx + r; x++) {
      const dx = x - cx, dy = y - cy;
      if (dx * dx + dy * dy <= r * r + 0.5) set(x, y, c);
    }
}

// Three heading bars (feed-reader motif), left-aligned.
rect(3, 5, 15, 3, WHITE);
rect(3, 11, 19, 3, WHITE);
rect(3, 17, 11, 3, WHITE);
// Accent dot on the right.
dot(20, 19, 3, ACCENT);

// --- PNG encode ---
function crc32(buf) {
  let c, table = crc32.table;
  if (!table) {
    table = crc32.table = new Int32Array(256);
    for (let n = 0; n < 256; n++) {
      c = n;
      for (let k = 0; k < 8; k++) c = c & 1 ? 0xedb88320 ^ (c >>> 1) : c >>> 1;
      table[n] = c;
    }
  }
  c = 0xffffffff;
  for (let i = 0; i < buf.length; i++) c = table[(c ^ buf[i]) & 0xff] ^ (c >>> 8);
  return (c ^ 0xffffffff) >>> 0;
}
function chunk(type, data) {
  const len = Buffer.alloc(4);
  len.writeUInt32BE(data.length);
  const td = Buffer.concat([Buffer.from(type, 'ascii'), data]);
  const crc = Buffer.alloc(4);
  crc.writeUInt32BE(crc32(td));
  return Buffer.concat([len, td, crc]);
}

const ihdr = Buffer.alloc(13);
ihdr.writeUInt32BE(W, 0);
ihdr.writeUInt32BE(H, 4);
ihdr[8] = 8;  // bit depth
ihdr[9] = 6;  // color type RGBA
const raw = Buffer.alloc(H * (1 + W * 4));
for (let y = 0; y < H; y++) {
  raw[y * (1 + W * 4)] = 0; // filter: none
  for (let x = 0; x < W; x++) {
    const p = px[y * W + x];
    const o = y * (1 + W * 4) + 1 + x * 4;
    raw[o] = p[0]; raw[o + 1] = p[1]; raw[o + 2] = p[2]; raw[o + 3] = p[3];
  }
}
const png = Buffer.concat([
  Buffer.from([0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a]),
  chunk('IHDR', ihdr),
  chunk('IDAT', zlib.deflateSync(raw)),
  chunk('IEND', Buffer.alloc(0)),
]);
const out = path.join(__dirname, '..', 'resources', 'images', 'logo_25.png');
fs.mkdirSync(path.dirname(out), { recursive: true });
fs.writeFileSync(out, png);
console.log('wrote', out, png.length, 'bytes');
