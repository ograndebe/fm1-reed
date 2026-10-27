/* Reed FM-1 AudioWorklet: loads the fixed-point engine as WebAssembly and
 * renders the mono model into a stereo output. No dependencies. */
class ReedProcessor extends AudioWorkletProcessor {
  constructor(options) {
    super();
    this.ready = false;
    this.wasm = null;
    this.queue = [];
    this.frames = 0;
    this.outPtr = 0;
    this.wasmBytes = options && options.processorOptions && options.processorOptions.wasmBytes;
    this.port.onmessage = (ev) => this.handle(ev.data);
    this.load();
  }

  async load() {
    try {
      if (!this.wasmBytes) throw new Error('engine bytes missing');
      const inst = await WebAssembly.instantiate(this.wasmBytes, {});
      this.wasm = inst.instance.exports;
      this.wasm.wr_init_web();
      this.outPtr = this.wasm.wr_out_ptr();
      this.i32 = new Int32Array(this.wasm.memory.buffer, this.outPtr, 2048);
      this.ready = true;
      this.port.postMessage({ type: 'ready' });
      for (const m of this.queue) this.handle(m);
      this.queue = [];
    } catch (e) {
      this.port.postMessage({ type: 'error', message: String(e) });
    }
  }

  handle(m) {
    if (!this.ready) {
      this.queue.push(m);
      return;
    }
    const w = this.wasm;
    switch (m.type) {
      case 'noteOn': w.wr_note_on_web(m.note, m.vel); break;
      case 'noteOff': w.wr_note_off_web(m.note); break;
      case 'sustain': w.wr_sustain_web(m.on ? 1 : 0); break;
      case 'param': w.wr_param_web(m.id, m.value); break;
      case 'allOff': w.wr_all_off_web(); break;
      case 'panic': w.wr_panic_web(); break;
    }
  }

  process(inputs, outputs) {
    const out = outputs[0];
    const left = out[0];
    const right = out.length > 1 ? out[1] : out[0];
    if (!this.ready) {
      left.fill(0);
      if (right !== left) right.fill(0);
      return true;
    }
    try {
      this.wasm.wr_render_web(left.length);
      for (let i = 0; i < left.length; i++) {
        const s = this.i32[i] * (1 / 1048576);
        left[i] = s;
        right[i] = s;
      }
      if ((++this.frames & 31) === 0) {
        this.port.postMessage({ type: 'voices', n: this.wasm.wr_voices_web() });
      }
    } catch (err) {
      if (!this.failed) {
        this.failed = true;
        this.port.postMessage({ type: 'error', message: String(err) });
      }
      left.fill(0);
      if (right !== left) right.fill(0);
    }
    return true;
  }
}

registerProcessor('reed', ReedProcessor);
