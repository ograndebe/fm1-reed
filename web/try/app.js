/* Reed FM-1 emulator UI. No framework, no build step. */
(() => {
  'use strict';

  const P = { VOLUME: 0, TREMOLO: 1, DRIVE: 2, SPEAKER: 3, TUNE: 4 };
  const MIDI_LO = 53, MIDI_HI = 79; // F3..G5: the FM-1's 27 keys
  const WHITE = new Set([0, 2, 4, 5, 7, 9, 11]);
  const NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B'];

  const $ = (s) => document.querySelector(s);
  const kb = $('#keyboard');
  const overlay = $('#overlay');
  const powerBtn = $('#power');
  const scope = $('#scope');
  const ctx2d = scope.getContext('2d');

  let audio = null, node = null, analyser = null, gain = null;
  let powered = false, starting = false;
  let octave = 0, sustain = false;
  let activePointers = new Map();
  let keys = new Map(); // midi -> element
  let held = new Set();
  let midiAccess = null;
  let voiceCount = 0;

  /* ------------------------------------------------------------ keyboard --- */
  function noteName(n) {
    return NAMES[n % 12] + (Math.floor(n / 12) - 1);
  }

  function buildKeyboard() {
    const whites = [];
    for (let n = MIDI_LO; n <= MIDI_HI; n++) if (WHITE.has(n % 12)) whites.push(n);
    const w = 100 / whites.length;
    let wi = 0;
    for (let n = MIDI_LO; n <= MIDI_HI; n++) {
      const el = document.createElement('div');
      el.className = WHITE.has(n % 12) ? 'white' : 'black';
      if (WHITE.has(n % 12)) {
        el.style.left = (wi * w) + '%';
        el.style.width = w + '%';
        wi++;
        if (n % 12 === 0) {
          const lab = document.createElement('span');
          lab.className = 'key-label';
          lab.textContent = noteName(n);
          el.appendChild(lab);
        }
      } else {
        el.style.left = (wi * w - w * 0.32) + '%';
        el.style.width = (w * 0.64) + '%';
      }
      el.dataset.note = n;
      el.addEventListener('pointerdown', (ev) => {
        ev.preventDefault();
        el.setPointerCapture(ev.pointerId);
        const r = el.getBoundingClientRect();
        const vel = Math.max(20, Math.min(127, Math.round(30 + 97 * (1 - (ev.clientY - r.top) / r.height))));
        activePointers.set(ev.pointerId, n);
        play(n + octave * 12, vel);
      });
      const release = (ev) => {
        const note = activePointers.get(ev.pointerId);
        if (note !== undefined) {
          activePointers.delete(ev.pointerId);
          stop(note + octave * 12);
        }
      };
      el.addEventListener('pointerup', release);
      el.addEventListener('pointercancel', release);
      el.addEventListener('lostpointercapture', release);
      kb.appendChild(el);
      keys.set(n, el);
    }
  }

  function paint(note, on) {
    const el = keys.get(note);
    if (el) el.classList.toggle('down', on);
  }

  function play(note, vel) {
    if (!powered || note < 33 || note > 96) return;
    held.add(note);
    paint(note - octave * 12, true);
    node.port.postMessage({ type: 'noteOn', note, vel });
    flash(note, vel);
  }

  function stop(note) {
    held.delete(note);
    paint(note - octave * 12, false);
    if (powered) node.port.postMessage({ type: 'noteOff', note });
  }

  function panic() {
    held.clear();
    activePointers.clear();
    downKeys.clear();
    document.querySelectorAll('.keyboard .down').forEach((e) => e.classList.remove('down'));
    if (powered) node.port.postMessage({ type: 'panic' });
  }

  let flashTimer = 0;
  function flash(note, vel) {
    const now = performance.now();
    if (now - flashTimer < 40) return;
    flashTimer = now;
    $('#note-readout').textContent = noteName(note) + '  vel ' + vel;
  }

  /* --------------------------------------------------------------- params --- */
  const params = [
    ['volume', P.VOLUME, 102, 0, 127, v => Math.round(v / 1.27) + '%'],
    ['tremolo', P.TREMOLO, 64, 0, 127, v => Math.round(v / 1.27) + '%'],
    ['drive', P.DRIVE, 64, 0, 127, v => Math.round(v / 1.27) + '%'],
    ['speaker', P.SPEAKER, 90, 0, 127, v => Math.round(v / 1.27) + '%'],
    ['tune', P.TUNE, 0, -50, 50, v => (v > 0 ? '+' : '') + v + ' ct'],
  ];

  function sendParam(id, value) {
    if (powered && node) node.port.postMessage({ type: 'param', id, value });
  }

  function wireParams() {
    for (const [id, pid, def, min, max, fmt] of params) {
      const el = document.getElementById(id);
      const out = document.querySelector(`output[for="${id}"]`);
      el.min = min; el.max = max; el.value = def;
      out.textContent = fmt(def);
      el.addEventListener('input', () => {
        const v = Number(el.value);
        out.textContent = fmt(v);
        sendParam(pid, v);
      });
    }
  }

  function resetParams() {
    for (const [id, pid, def, , , fmt] of params) {
      const el = document.getElementById(id);
      el.value = def;
      document.querySelector(`output[for="${id}"]`).textContent = fmt(def);
      sendParam(pid, def);
    }
  }

  /* ---------------------------------------------------------------- power --- */
  async function powerOn() {
    if (powered || starting) return;
    starting = true;
    powerBtn.textContent = 'STARTING…';
    try {
      audio = new AudioContext({ sampleRate: 44100, latencyHint: 'interactive' });
      await audio.audioWorklet.addModule('worklet.js');
      const wasmUrl = new URL('wurli.wasm', location.href).href;
      const wasmBytes = await (await fetch(wasmUrl)).arrayBuffer();
      node = new AudioWorkletNode(audio, 'reed', {
        numberOfInputs: 0,
        numberOfOutputs: 1,
        outputChannelCount: [2],
        processorOptions: { wasmBytes },
      });
      node.port.onmessage = (ev) => {
        const m = ev.data;
        if (m.type === 'ready') {
          powered = true;
          starting = false;
          overlay.classList.add('hidden');
          powerBtn.textContent = 'POWER';
          powerBtn.classList.remove('off');
          $('#audio-status').textContent = 'RUNNING';
          $('#audio-status').style.color = 'var(--green)';
          $('#rate').textContent = (audio.sampleRate / 1000).toFixed(1) + ' kHz';
          resetParams();
          connectMidi();
        } else if (m.type === 'voices') {
          voiceCount = m.n;
          $('#voices').textContent = m.n;
        } else if (m.type === 'error') {
          $('#overlay-msg').textContent = 'Audio engine error: ' + m.message;
          overlay.classList.remove('hidden');
          $('#audio-status').textContent = 'ERROR';
          $('#audio-status').style.color = 'var(--red)';
        }
      };
      gain = audio.createGain();
      gain.gain.value = 0.85;
      analyser = audio.createAnalyser();
      analyser.fftSize = 2048;
      analyser.smoothingTimeConstant = 0.65;
      node.connect(gain).connect(analyser).connect(audio.destination);
      if (audio.state === 'suspended') await audio.resume();
    } catch (err) {
      console.error(err);
      powerBtn.textContent = 'RETRY';
      starting = false;
      $('#overlay-msg').textContent = 'Could not start audio: ' + err.message;
    }
  }

  async function powerOff() {
    if (!audio) return;
    panic();
    await audio.close();
    audio = null; node = null; powered = false;
    powerBtn.classList.add('off');
    powerBtn.textContent = 'POWER';
    overlay.classList.remove('hidden');
    $('#audio-status').textContent = 'STOPPED';
    $('#audio-status').style.color = 'var(--amber)';
    $('#rate').textContent = '—';
    $('#voices').textContent = '0';
  }

  /* ----------------------------------------------------------------- MIDI --- */
  function midiMessage(data) {
    const cmd = data[0] & 0xf0;
    if (cmd === 0x90 && data[2] > 0) play(data[1], data[2]);
    else if (cmd === 0x80 || (cmd === 0x90 && data[2] === 0)) stop(data[1]);
    else if (cmd === 0xb0) {
      if (data[1] === 64) setSustain(data[2] >= 64);
      else if (data[1] === 7) setSlider('volume', data[2]);
      else if (data[1] === 1) setSlider('tremolo', data[2]);
      else if (data[1] === 11) setSlider('drive', data[2]);
    }
  }

  function setSlider(id, value) {
    const el = document.getElementById(id);
    if (!el) return;
    el.value = value;
    el.dispatchEvent(new Event('input'));
  }

  async function connectMidi() {
    if (!navigator.requestMIDIAccess) {
      $('#midi-status').textContent = 'WEB MIDI N/A';
      return;
    }
    try {
      midiAccess = await navigator.requestMIDIAccess();
      const attach = () => {
        const names = [];
        for (const input of midiAccess.inputs.values()) {
          input.onmidimessage = (ev) => midiMessage(ev.data);
          names.push(input.name);
        }
        $('#midi-status').textContent = names.length ? 'MIDI: ' + names[0] : 'MIDI: none';
      };
      attach();
      midiAccess.onstatechange = attach;
    } catch {
      $('#midi-status').textContent = 'MIDI: blocked';
    }
  }

  function setSustain(on) {
    sustain = on;
    $('#sustain').classList.toggle('on', on);
    if (powered) node.port.postMessage({ type: 'sustain', on });
  }

  /* -------------------------------------------------------------- computer --- */
  const QWERTY = { a: 0, w: 1, s: 2, e: 3, d: 4, f: 5, t: 6, g: 7, y: 8, h: 9, u: 10, j: 11, k: 12, o: 13, l: 14, p: 15 };
  const downKeys = new Set();

  function keyToNote(code) {
    if (!(code in QWERTY)) return null;
    return 60 + QWERTY[code] + octave * 12;
  }

  window.addEventListener('keydown', (e) => {
    if (e.metaKey || e.ctrlKey || e.altKey) return;
    const k = e.key.toLowerCase();
    if (k === 'z') { setOctave(octave - 1); return; }
    if (k === 'x') { setOctave(octave + 1); return; }
    if (k === ' ') { e.preventDefault(); setSustain(!sustain); return; }
    const note = keyToNote(k);
    if (note === null) return;
    e.preventDefault();
    if (downKeys.has(k)) return;
    downKeys.add(k);
    play(note, 100);
  });

  window.addEventListener('keyup', (e) => {
    const k = e.key.toLowerCase();
    if (!(k in QWERTY)) return;
    const note = keyToNote(k);
    downKeys.delete(k);
    if (note !== null) stop(note);
  });

  /* --------------------------------------------------------------- octave --- */
  function setOctave(v) {
    const next = Math.max(-2, Math.min(2, v));
    if (next === octave) return;
    panic();
    octave = next;
    $('#octave').textContent = (octave > 0 ? '+' : '') + octave;
  }

  /* ----------------------------------------------------------------- scope --- */
  const wave = new Float32Array(2048);
  function drawScope() {
    requestAnimationFrame(drawScope);
    const w = scope.width, h = scope.height;
    ctx2d.clearRect(0, 0, w, h);
    ctx2d.strokeStyle = 'rgba(224,169,74,.18)';
    ctx2d.lineWidth = 1;
    ctx2d.beginPath();
    ctx2d.moveTo(0, h / 2); ctx2d.lineTo(w, h / 2); ctx2d.stroke();
    if (!powered || !analyser) return;
    analyser.getFloatTimeDomainData(wave);
    let peak = 0;
    for (let i = 0; i < wave.length; i++) {
      const a = Math.abs(wave[i]);
      if (a > peak) peak = a;
    }
    const g = peak > 0.002 ? Math.min(0.9 / peak, 12) : 1;
    ctx2d.beginPath();
    for (let i = 0; i < wave.length; i++) {
      const x = (i / (wave.length - 1)) * w;
      const y = h / 2 - wave[i] * (h / 2) * g;
      i ? ctx2d.lineTo(x, y) : ctx2d.moveTo(x, y);
    }
    ctx2d.strokeStyle = '#e0a94a';
    ctx2d.lineWidth = 1.4;
    ctx2d.stroke();
  }

  /* ------------------------------------------------------------------ init --- */
  buildKeyboard();
  wireParams();
  drawScope();

  powerBtn.addEventListener('click', () => (powered ? powerOff() : powerOn()));
  $('#overlay-power').addEventListener('click', powerOn);
  $('#sustain').addEventListener('click', () => setSustain(!sustain));
  $('#panic').addEventListener('click', panic);
  $('#oct-down').addEventListener('click', () => setOctave(octave - 1));
  $('#oct-up').addEventListener('click', () => setOctave(octave + 1));
  $('#reset').addEventListener('click', resetParams);

  if (location.search.indexOf('autostart') >= 0) setTimeout(powerOn, 300);

  window.addEventListener('blur', () => {
    for (const n of held) stop(n);
    for (const k of downKeys) {
      const note = keyToNote(k);
      if (note !== null) stop(note);
    }
    downKeys.clear();
  });
})();
