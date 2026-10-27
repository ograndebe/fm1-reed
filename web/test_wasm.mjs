// Node smoke test for the browser engine. No dependencies.
//   node web/test_wasm.mjs [path/to/wurli.wasm]
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const here = path.dirname(fileURLToPath(import.meta.url));
const file = process.argv[2] || path.join(here, 'try', 'wurli.wasm');
const bytes = fs.readFileSync(file);
const { instance } = await WebAssembly.instantiate(bytes, {});
const e = instance.exports;
const required = ['wr_init_web', 'wr_note_on_web', 'wr_note_off_web', 'wr_render_web', 'wr_out_ptr'];
for (const name of required) {
  if (typeof e[name] !== 'function') throw new Error(`missing export ${name}`);
}
e.wr_init_web();
e.wr_note_on_web(60, 100);
const ptr = e.wr_out_ptr();
const out = new Int32Array(e.memory.buffer, ptr, 128);
let peak = 0;
for (let block = 0; block < 44100 / 128; block++) {
  e.wr_render_web(128);
  for (let i = 0; i < 128; i++) peak = Math.max(peak, Math.abs(out[i]));
}
if (!(peak > 0.005)) throw new Error(`no audio (peak ${peak})`);
if (e.wr_voices_web() < 1) throw new Error('no active voice');
e.wr_note_off_web(60);
e.wr_render_web(128);
console.log(`wasm ok: ${(bytes.length / 1024).toFixed(0)} KB, peak ${(peak / 1048576).toFixed(3)}`);
