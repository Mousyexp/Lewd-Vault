// Makes the decorations (art/decor/*.webp) from the pictures in art/decor-source/: the background
// is cut out and the picture recoloured with the rose palette, like the icon (make-icon.mjs).
//   npm run decor
// Backgrounds are found by filling inwards from the picture's edges over background-coloured pixels
// (so white inside the picture, like eyes and highlights, stays); the cut edge is then softened.
import sharp from 'sharp';
import fs from 'node:fs';
import { fileURLToPath } from 'node:url';

const here = new URL('..', import.meta.url);
const path = p => fileURLToPath(new URL(p, here));

// name -> source file and what its background looks like
const PICTURES = {
  corset: { file: 'corset.png' },                       // already transparent
  bikini: { file: 'bikini.webp', bg: 'plain' },         // flat light gray
  suit: { file: 'suit.png', bg: 'checker', keep: 0.03 }, // a pale checkerboard; her sleeve is in pieces
  tearju: { file: 'tearju.webp', bg: 'checker' },
  croptop: { file: 'croptop.png' },
  yoruichi: { file: 'yoruichi.png', bg: 'checker' },
  sonico: { file: 'sonico.png', bg: 'checker' }
  // left out: a cat girl, a miko, a knit sweater, an office Sonico, two maids and a red bikini (white
  // clothes or skin highlights matched the background and came out with holes or white patches,
  // or the edges were ragged)
};

// the palette (#F9CDD4 #ECA1AC #E27589 #B25B6E #7C3042) with a deeper wine and a near-white at the ends
const STOPS = [[0, '#2a0d17'], [0.16, '#7C3042'], [0.36, '#B25B6E'], [0.55, '#E27589'], [0.72, '#ECA1AC'], [0.86, '#F9CDD4'], [1, '#fff3f5']]
  .map(([at, hex]) => [at, [1, 3, 5].map(i => parseInt(hex.slice(i, i + 2), 16))]);
const lut = Array.from({ length: 256 }, (_, v) => {
  const t = v / 255;
  let k = 1;
  while (k < STOPS.length - 1 && STOPS[k][0] < t) k++;
  const [a, ca] = STOPS[k - 1], [b, cb] = STOPS[k], f = Math.min(1, Math.max(0, (t - a) / (b - a)));
  return ca.map((c, i) => Math.round(c + (cb[i] - c) * f));
});

fs.mkdirSync(path('art/decor/'), { recursive: true });
const only = process.argv.slice(2);
for (const [name, { file, bg, keep: keepShare = 0.1 }] of Object.entries(PICTURES).filter(([n]) => !only.length || only.includes(n))) {
  const { data, info } = await sharp(path(`art/decor-source/${file}`)).ensureAlpha().raw().toBuffer({ resolveWithObject: true });
  const { width: W, height: H } = info, N = W * H;
  const alpha = new Float32Array(N);
  for (let p = 0; p < N; p++) alpha[p] = data[p * 4 + 3] / 255;

  if (bg) {
    const at = p => data.subarray(p * 4, p * 4 + 3);
    const corner = at(0);
    const isBg = bg === 'checker'
      ? p => { const [r, g, b] = at(p); return Math.max(r, g, b) - Math.min(r, g, b) < 14 && r + g + b > 3 * 205; }
      : p => { const [r, g, b] = at(p); return Math.abs(r - corner[0]) + Math.abs(g - corner[1]) + Math.abs(b - corner[2]) < 30; };
    // the fill only travels through pixels whose 4 neighbours are background too, so it can't slip
    // through a 1 px gap in an outline into a white shirt; afterwards it grows back over the edge
    const core = p => { const x = p % W; return isBg(p) && (x === 0 || isBg(p - 1)) && (x === W - 1 || isBg(p + 1)) && (p < W || isBg(p - W)) && (p >= N - W || isBg(p + W)); };
    const seen = new Uint8Array(N), stack = [];
    for (let x = 0; x < W; x++) stack.push(x, (H - 1) * W + x);
    for (let y = 0; y < H; y++) stack.push(y * W, y * W + W - 1);
    while (stack.length) {
      const p = stack.pop();
      if (seen[p] || !(bg === 'checker' ? core(p) : isBg(p))) continue;
      seen[p] = 1; alpha[p] = 0;
      const x = p % W;
      if (x > 0) stack.push(p - 1);
      if (x < W - 1) stack.push(p + 1);
      if (p >= W) stack.push(p - W);
      if (p < N - W) stack.push(p + W);
    }
    for (let grow = 0; grow < 2; grow++) {
      const add = [];
      for (let p = 0; p < N; p++) {
        if (seen[p] || !isBg(p)) continue;
        const x = p % W;
        if ((x > 0 && seen[p - 1]) || (x < W - 1 && seen[p + 1]) || (p >= W && seen[p - W]) || (p < N - W && seen[p + W])) add.push(p);
      }
      for (const p of add) { seen[p] = 1; alpha[p] = 0; }
    }
    // a checkerboard also shows through gaps the fill can't reach (between strands of hair): any
    // enclosed patch made almost only of the checkerboard's shades (learned from the outer
    // background) goes too; smooth whites like a shirt aren't two exact shades, so they stay
    if (bg === 'checker') {
      const hist = new Map();
      for (let p = 0; p < N; p++) if (seen[p]) { const v = data[p * 4]; hist.set(v, (hist.get(v) || 0) + 1); }
      const shades = [...hist].sort((a, b) => b[1] - a[1]).slice(0, 2).map(([v]) => v);
      const isShade = p => isBg(p) && shades.some(v => Math.abs(data[p * 4] - v) <= 5);
      const done = new Uint8Array(N);
      for (let start = 0; start < N; start++) {
        if (seen[start] || done[start] || !isBg(start)) continue;
        const patch = [], todo = [start];
        done[start] = 1;
        while (todo.length) {
          const p = todo.pop(); patch.push(p);
          const x = p % W;
          for (const q of [x > 0 ? p - 1 : -1, x < W - 1 ? p + 1 : -1, p - W, p + W]) {
            if (q >= 0 && q < N && !done[q] && !seen[q] && isBg(q)) { done[q] = 1; todo.push(q); }
          }
        }
        // (a pocket may hold squares of one shade only, so either shade counts)
        if (patch.length >= 40 && patch.filter(isShade).length >= patch.length * 0.85) for (const p of patch) alpha[p] = 0;
      }
    }
    // soften the cut: each edge pixel takes the average coverage of its 3 x 3 neighbourhood
    const soft = Float32Array.from(alpha);
    for (let y = 1; y < H - 1; y++) for (let x = 1; x < W - 1; x++) {
      const p = y * W + x;
      if (!alpha[p]) continue;
      let s = 0;
      for (let dy = -1; dy <= 1; dy++) for (let dx = -1; dx <= 1; dx++) s += alpha[p + dy * W + dx];
      soft[p] = Math.min(alpha[p], s / 9 * 1.15);
    }
    alpha.set(soft);
  }

  // keep the figure: pieces not joined to it (a stamp or logo beside her, leftover specks) go,
  // unless they're a sizeable part of the picture (at least 10% of the biggest piece)
  {
    const label = new Int32Array(N).fill(-1), sizes = [];
    for (let start = 0; start < N; start++) {
      if (label[start] >= 0 || alpha[start] < 0.5) continue;
      const id = sizes.length, todo = [start];
      label[start] = id; let n = 0;
      while (todo.length) {
        const p = todo.pop(); n++;
        const x = p % W;
        for (const q of [x > 0 ? p - 1 : -1, x < W - 1 ? p + 1 : -1, p - W, p + W]) {
          if (q >= 0 && q < N && label[q] < 0 && alpha[q] >= 0.5) { label[q] = id; todo.push(q); }
        }
      }
      sizes.push(n);
    }
    const keep = Math.max(...sizes) * keepShare;
    for (let p = 0; p < N; p++) {
      if (label[p] >= 0 ? sizes[label[p]] < keep : alpha[p] > 0 && !hasKeptNeighbour(p)) alpha[p] = 0;
    }
    function hasKeptNeighbour(p) {
      const x = p % W;
      return [x > 0 ? p - 1 : -1, x < W - 1 ? p + 1 : -1, p - W, p + W].some(q => q >= 0 && q < N && label[q] >= 0 && sizes[label[q]] >= keep);
    }
  }

  const out = Buffer.alloc(N * 4);
  for (let p = 0; p < N; p++) {
    const [r, g, b] = data.subarray(p * 4, p * 4 + 3);
    out.set(lut[Math.round(0.2126 * r + 0.7152 * g + 0.0722 * b)], p * 4);
    out[p * 4 + 3] = Math.round(alpha[p] * 255);
  }
  // trimmed to the figure, at most 900 px tall (they're shown well below that)
  await sharp(out, { raw: { width: W, height: H, channels: 4 } })
    .trim({ background: { r: 0, g: 0, b: 0, alpha: 0 }, threshold: 1 })
    .resize({ height: Math.min(900, H), withoutEnlargement: true })
    .webp({ quality: 82, alphaQuality: 90 })
    .toFile(path(`art/decor/${name}.webp`));
  console.log(`make-decor: art/decor/${name}.webp`);
}
