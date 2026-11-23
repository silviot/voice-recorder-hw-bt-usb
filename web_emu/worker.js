/*
 * Cassini OpenFW Browser Emulator — WebAssembly SL6801 Worker
 *
 * Runs the real compiled openfw.wasm firmware inside a dedicated Worker using
 * SharedArrayBuffer + Atomics.wait so openfw_main() runs unmodified at 60 fps
 * with <1 ms button response, zero-allocation live audio I/O (ring buffers in
 * SharedArrayBuffer), and a persistent virtual exFAT microSD card.
 *
 * SPDX-License-Identifier: MIT
 */
"use strict";

// SharedArrayBuffer Int32 control indices (must match app.js)
const CTRL_WAKE_SEQ    = 0;
const CTRL_KEY_MASK    = 1;
const CTRL_REC_SW      = 2;
const CTRL_HP_PLUG     = 3;
const CTRL_VBUS        = 4;
const CTRL_VBAT_MV     = 5;
const CTRL_PWR_DOWN    = 6;
const CTRL_PWR_SEQ     = 7;
const CTRL_AUDIO_SRC   = 8;  // 0 = synthetic voice/melody, 1 = live browser mic
const CTRL_CMD         = 9;  // 0 = none, 1 = list SD, 2 = read file, 3 = power-on/reboot, 4 = hard reset
const CTRL_POWERED_OFF = 10;
const CTRL_BACKLIGHT   = 11;
const CTRL_SPEAKER_PA  = 12;
const CTRL_FB_VER      = 13;
const CTRL_MIC_WR      = 14;
const CTRL_MIC_RD      = 15;
const CTRL_DAC_WR      = 16;
const CTRL_DAC_RD      = 17;
const CTRL_DAC_RATE    = 18;

const FB_OFFSET_BYTES  = 256;
const FB_PIXELS        = 160 * 80;
const MIC_OFFSET_BYTES = FB_OFFSET_BYTES + FB_PIXELS * 2;
const MIC_RING_SAMPLES = 32768; // ~2 s @ 16 kHz mono (Int16)
const MIC_RING_MASK    = MIC_RING_SAMPLES - 1;
const DAC_OFFSET_BYTES = MIC_OFFSET_BYTES + MIC_RING_SAMPLES * 2;
const DAC_RING_FRAMES  = 32768; // ~2 s @ 16 kHz stereo (Int16 pairs)
const DAC_RING_MASK    = DAC_RING_FRAMES - 1;

const SAMPLE_FILENAMES = [
  "cassini-final-moments.wav",
  "cassini-final-moments.cwx",
];

let wasmModule = null;
let sampleFiles = []; // Array of { name: string, bytes: Uint8Array }
let ctrl = null;
let sabFb = null;
let sabMic = null;
let sabDac = null;

let savedSdImage = null;
let pendingDownloadName = null;

const textEncoder = new TextEncoder();
const textDecoder = new TextDecoder();

// Synthetic voice/formant phase state (produces realistic Mel spectrogram bands + pauses)
let synthSampleIdx = 0;
let synthPh1 = 0;
let synthPh2 = 0;
let synthPh3 = 0;

function fillSyntheticVoice(dst, base, frames, channels) {
  const rate = 16000;
  for (let i = 0; i < frames; i++) {
    const t = synthSampleIdx / rate;
    synthSampleIdx++;

    // Spoken cadence: 2.2s active speech phrase, 0.9s pause (exercises VOR too!)
    const phraseT = t % 3.1;
    let env = 0;
    if (phraseT < 2.2) {
      const syl = 0.5 + 0.5 * Math.sin(2 * Math.PI * 4.2 * phraseT);
      const gate = Math.min(1, phraseT * 8, (2.2 - phraseT) * 6);
      env = (0.25 + 0.75 * syl) * gate;
    }

    // Sweeping fundamental + 2 formants for rich spectrogram curves
    const f0 = 145 + 45 * Math.sin(2 * Math.PI * 0.65 * t) + 18 * Math.sin(2 * Math.PI * 2.1 * t);
    const f1 = 520 + 240 * Math.sin(2 * Math.PI * 1.15 * t + 0.7);
    const f2 = 1650 + 550 * Math.sin(2 * Math.PI * 0.85 * t + 1.9);

    synthPh1 += (2 * Math.PI * f0) / rate;
    synthPh2 += (2 * Math.PI * f1) / rate;
    synthPh3 += (2 * Math.PI * f2) / rate;

    const sig =
      (0.45 * Math.sin(synthPh1) +
        0.35 * Math.sin(synthPh2) +
        0.20 * Math.sin(synthPh3)) *
      env;
    const noise = (((synthSampleIdx * 1103515245 + 12345) >>> 16) & 0x7fff) / 32768 - 0.5;
    const sampleL = Math.max(-32767, Math.min(32767, Math.round(sig * 16000 + noise * 90)));
    if (channels === 2) {
      const sampleR = Math.max(-32767, Math.min(32767, Math.round(sig * 14800 - noise * 90)));
      dst[base + i * 2] = sampleL;
      dst[base + i * 2 + 1] = sampleR;
    } else {
      dst[base + i] = sampleL;
    }
  }
}

function pullBrowserMic(dst, base, frames, channels) {
  let rd = Atomics.load(ctrl, CTRL_MIC_RD) >>> 0;
  const wr = Atomics.load(ctrl, CTRL_MIC_WR) >>> 0;
  let avail = (wr - rd) >>> 0;
  if (avail > MIC_RING_SAMPLES) {
    rd = (wr - frames) >>> 0;
    avail = frames;
  }
  for (let i = 0; i < frames; i++) {
    let s = 0;
    if (avail > 0) {
      s = sabMic[rd & MIC_RING_MASK];
      rd = (rd + 1) >>> 0;
      avail--;
    }
    if (channels === 2) {
      dst[base + i * 2] = s;
      dst[base + i * 2 + 1] = s;
    } else {
      dst[base + i] = s;
    }
  }
  Atomics.store(ctrl, CTRL_MIC_RD, rd | 0);
}

class RebootSignal extends Error {
  constructor(keepSd) {
    super("REBOOT");
    this.keepSd = keepSd;
  }
}

function runSession(keepSd) {
  let inst = null;
  let uartLine = "";
  let bootTime0 = performance.now();
  let lastKeyMask = -1;
  let lastRecSw = -1;
  let lastHp = -1;
  let lastVbus = -1;
  let lastVbat = -1;
  let lastPwrSeq = Atomics.load(ctrl, CTRL_PWR_SEQ);
  let poweredOff = false;

  // Cached typed-array views over WASM linear memory (zero per-frame allocations)
  let cachedBuf = null;
  let wasmU8 = null;
  let wasmI16 = null;
  let wasmU32 = null;
  let wasmFb = null;
  let wasmSd = null;
  let wasmXfer = null;
  let fbPtr = 0;
  let sdPtr = 0;
  let sdSize = 0;
  let xferPtr = 0;

  const ensureViews = () => {
    const buf = inst.exports.memory.buffer;
    if (buf === cachedBuf) return;
    cachedBuf = buf;
    wasmU8 = new Uint8Array(buf);
    wasmI16 = new Int16Array(buf);
    wasmU32 = new Uint32Array(buf);
    wasmFb = new Uint16Array(buf, fbPtr, FB_PIXELS);
    wasmSd = new Uint8Array(buf, sdPtr, sdSize);
    wasmXfer = new Uint8Array(buf, xferPtr, 65536);
  };

  const flushDacRing = () => {
    Atomics.store(ctrl, CTRL_DAC_RD, Atomics.load(ctrl, CTRL_DAC_WR));
    Atomics.store(ctrl, CTRL_DAC_RATE, 0);
  };

  const flushUart = () => {
    if (uartLine.length > 0) {
      self.postMessage({ type: "uart", text: uartLine });
      uartLine = "";
    }
  };

  const syncInputs = () => {
    const km = Atomics.load(ctrl, CTRL_KEY_MASK);
    if (km !== lastKeyMask) { lastKeyMask = km; inst.exports.wasm_set_key_mask(km >>> 0); }

    const rec = Atomics.load(ctrl, CTRL_REC_SW);
    if (rec !== lastRecSw) { lastRecSw = rec; inst.exports.wasm_set_rec_switch(rec ? 1 : 0); }

    const hp = Atomics.load(ctrl, CTRL_HP_PLUG);
    if (hp !== lastHp) { lastHp = hp; inst.exports.wasm_set_hp_plugged(hp ? 1 : 0); }

    const vbus = Atomics.load(ctrl, CTRL_VBUS);
    if (vbus !== lastVbus) { lastVbus = vbus; inst.exports.wasm_set_vbus(vbus ? 1 : 0); }

    const vbat = Atomics.load(ctrl, CTRL_VBAT_MV);
    if (vbat !== lastVbat) { lastVbat = vbat; inst.exports.wasm_set_vbat_mv(vbat >>> 0); }

    const pseq = Atomics.load(ctrl, CTRL_PWR_SEQ);
    if (pseq !== lastPwrSeq) {
      lastPwrSeq = pseq;
      const pdown = Atomics.load(ctrl, CTRL_PWR_DOWN) ? 1 : 0;
      inst.exports.wasm_power_button(pdown);
    }
  };

  const syncOutputs = () => {
    ensureViews();
    if (inst.exports.wasm_fb_take_dirty()) {
      sabFb.set(wasmFb);
      Atomics.add(ctrl, CTRL_FB_VER, 1);
    }
    Atomics.store(ctrl, CTRL_BACKLIGHT, inst.exports.wasm_get_backlight());
    Atomics.store(ctrl, CTRL_SPEAKER_PA, inst.exports.wasm_get_speaker_pa());
    flushUart();
  };

  const serviceHostCommands = () => {
    const cmd = Atomics.exchange(ctrl, CTRL_CMD, 0);
    if (cmd === 0) return;
    ensureViews();

    if (cmd === 1) {
      const n = inst.exports.wasm_list_records();
      const text = textDecoder.decode(wasmU8.subarray(xferPtr, xferPtr + n));
      const files = text
        .split("\n")
        .filter((l) => l.includes("\t"))
        .map((l) => {
          const [name, sz] = l.split("\t");
          return { name, size: parseInt(sz, 10) || 0 };
        });
      self.postMessage({ type: "files", files });
    } else if (cmd === 2 && pendingDownloadName) {
      const name = pendingDownloadName;
      pendingDownloadName = null;
      const enc = textEncoder.encode(name);
      const chunks = [];
      let offset = 0;
      while (true) {
        wasmXfer.set(enc, 0);
        const got = inst.exports.wasm_read_record_chunk(enc.length, offset, 65536);
        if (got === 0) break;
        chunks.push(wasmU8.slice(xferPtr, xferPtr + got));
        offset += got;
        if (got < 65536) break;
      }
      const total = new Uint8Array(offset);
      let pos = 0;
      for (const c of chunks) { total.set(c, pos); pos += c.length; }
      self.postMessage({ type: "fileData", name, data: total.buffer }, [total.buffer]);
    } else if (cmd === 3) {
      flushDacRing();
      savedSdImage = new Uint8Array(wasmSd);
      throw new RebootSignal(true);
    } else if (cmd === 4) {
      flushDacRing();
      savedSdImage = null;
      throw new RebootSignal(false);
    }
  };

  const env = {
    host_yield(waitMs) {
      syncInputs();
      syncOutputs();
      serviceHostCommands();

      if (poweredOff) {
        Atomics.store(ctrl, CTRL_POWERED_OFF, 1);
        Atomics.store(ctrl, CTRL_BACKLIGHT, 0);
        flushDacRing();
        const prevPwr = Atomics.load(ctrl, CTRL_PWR_SEQ);
        const prevRec = Atomics.load(ctrl, CTRL_REC_SW);
        while (true) {
          const seq = Atomics.load(ctrl, CTRL_WAKE_SEQ);
          Atomics.wait(ctrl, CTRL_WAKE_SEQ, seq, 100);
          serviceHostCommands();
          const newPwr = Atomics.load(ctrl, CTRL_PWR_SEQ);
          const pdown = Atomics.load(ctrl, CTRL_PWR_DOWN);
          const newRec = Atomics.load(ctrl, CTRL_REC_SW);
          if ((newPwr !== prevPwr && pdown) || (newRec && !prevRec)) {
            ensureViews();
            savedSdImage = new Uint8Array(wasmSd);
            throw new RebootSignal(true);
          }
        }
      }

      const seq = Atomics.load(ctrl, CTRL_WAKE_SEQ);
      const sleepMs = Math.min(Math.max(1, waitMs | 0), 16);
      Atomics.wait(ctrl, CTRL_WAKE_SEQ, seq, sleepMs);

      syncInputs();
      serviceHostCommands();
      return Math.floor(performance.now() - bootTime0) >>> 0;
    },

    host_uart_putc(ch) {
      const c = String.fromCharCode(ch & 0xff);
      if (c === "\r") return;
      uartLine += c;
      if (c === "\n" || uartLine.length >= 120) {
        flushUart();
      }
    },

    host_dac_push(ptr, numFrames, sampleRate) {
      if (numFrames <= 0) {
        flushDacRing();
        return;
      }
      ensureViews();
      let wr = Atomics.load(ctrl, CTRL_DAC_WR) >>> 0;
      const rd = Atomics.load(ctrl, CTRL_DAC_RD) >>> 0;
      const avail = (wr - rd) >>> 0;
      // Cap backlog if browser audio consumer is suspended so latency never grows unbounded
      if (avail + numFrames >= DAC_RING_FRAMES) {
        const dropTo = (wr + numFrames - (DAC_RING_FRAMES - 1)) >>> 0;
        Atomics.store(ctrl, CTRL_DAC_RD, dropTo | 0);
      }
      const srcBase = ptr >> 1;
      for (let i = 0; i < numFrames; i++) {
        const dstIdx = (wr & DAC_RING_MASK) << 1;
        sabDac[dstIdx] = wasmI16[srcBase + (i << 1)];
        sabDac[dstIdx + 1] = wasmI16[srcBase + (i << 1) + 1];
        wr = (wr + 1) >>> 0;
      }
      Atomics.store(ctrl, CTRL_DAC_RATE, sampleRate | 0);
      Atomics.store(ctrl, CTRL_DAC_WR, wr | 0);
    },

    host_adc_pull(ptr, numFrames, channels) {
      ensureViews();
      const base = ptr >> 1;
      const useMic = Atomics.load(ctrl, CTRL_AUDIO_SRC) === 1;
      if (useMic) {
        pullBrowserMic(wasmI16, base, numFrames, channels);
      } else {
        fillSyntheticVoice(wasmI16, base, numFrames, channels);
      }
      return numFrames;
    },

    host_on_poweroff(mode) {
      flushUart();
      flushDacRing();
      if (mode === 0) {
        poweredOff = true;
        Atomics.store(ctrl, CTRL_POWERED_OFF, 1);
        Atomics.store(ctrl, CTRL_BACKLIGHT, 0);
        self.postMessage({ type: "uart", text: "[EMU] Device powered off. Press Power (P / Space) or slide REC to boot.\n" });
      } else {
        self.postMessage({ type: "uart", text: "[EMU] Firmware requested reboot...\n" });
        ensureViews();
        savedSdImage = new Uint8Array(wasmSd);
        throw new RebootSignal(true);
      }
    },

    host_get_wall_clock(ptr) {
      ensureViews();
      const d = new Date();
      const base = ptr >> 2;
      wasmU32[base] = d.getFullYear();
      wasmU32[base + 1] = d.getMonth() + 1;
      wasmU32[base + 2] = d.getDate();
      wasmU32[base + 3] = d.getHours();
      wasmU32[base + 4] = d.getMinutes();
      wasmU32[base + 5] = d.getSeconds();
    },
  };

  inst = new WebAssembly.Instance(wasmModule, { env });
  fbPtr = inst.exports.wasm_fb_ptr();
  sdPtr = inst.exports.wasm_sd_ptr();
  sdSize = inst.exports.wasm_sd_size();
  xferPtr = inst.exports.wasm_xfer_ptr();
  ensureViews();

  Atomics.store(ctrl, CTRL_POWERED_OFF, 0);
  flushDacRing();

  let sdReady = 0;
  if (keepSd && savedSdImage) {
    wasmSd.set(savedSdImage);
    savedSdImage = null; // Free temporary 32 MiB snapshot immediately
    sdReady = 1;
  } else if (sampleFiles.length > 0 && inst.exports.wasm_sd_format_empty) {
    inst.exports.wasm_sd_format_empty();
    let seededOk = true;
    for (const sf of sampleFiles) {
      const nameBytes = textEncoder.encode(sf.name);
      wasmXfer.set(nameBytes, 0);
      if (inst.exports.wasm_seed_file_begin(nameBytes.length) !== 0) {
        seededOk = false;
        break;
      }
      let off = 0;
      while (off < sf.bytes.length) {
        const chunkLen = Math.min(65536, sf.bytes.length - off);
        wasmXfer.set(sf.bytes.subarray(off, off + chunkLen), 0);
        if (inst.exports.wasm_seed_file_write(chunkLen) !== 0) {
          seededOk = false;
          break;
        }
        off += chunkLen;
      }
      if (inst.exports.wasm_seed_file_end() !== 0) {
        seededOk = false;
      }
      if (!seededOk) break;
    }
    if (seededOk) {
      sdReady = 1;
    }
  }

  bootTime0 = performance.now();
  inst.exports.wasm_boot(sdReady);
}

async function fetchSampleFiles() {
  const loaded = [];
  for (const name of SAMPLE_FILENAMES) {
    try {
      const resp = await fetch(`./samples/${name}`);
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      const buf = await resp.arrayBuffer();
      loaded.push({ name, bytes: new Uint8Array(buf) });
    } catch (err) {
      self.postMessage({
        type: "uart",
        text: `[EMU] Sample ${name} unavailable (${err.message}); using built-in demo.\n`,
      });
      return [];
    }
  }
  return loaded;
}

self.onmessage = async (ev) => {
  const msg = ev.data;
  if (msg.type === "download") {
    pendingDownloadName = msg.name;
    Atomics.store(ctrl, CTRL_CMD, 2);
    Atomics.add(ctrl, CTRL_WAKE_SEQ, 1);
    Atomics.notify(ctrl, CTRL_WAKE_SEQ);
    return;
  }
  if (msg.type !== "init") return;

  ctrl = new Int32Array(msg.sab, 0, 32);
  sabFb = new Uint16Array(msg.sab, FB_OFFSET_BYTES, FB_PIXELS);
  sabMic = new Int16Array(msg.sab, MIC_OFFSET_BYTES, MIC_RING_SAMPLES);
  sabDac = new Int16Array(msg.sab, DAC_OFFSET_BYTES, DAC_RING_FRAMES * 2);

  try {
    const [wasmResp, samples] = await Promise.all([
      fetch(msg.wasmUrl),
      fetchSampleFiles(),
    ]);
    if (!wasmResp.ok) throw new Error(`HTTP ${wasmResp.status} loading ${msg.wasmUrl}`);
    const bytes = await wasmResp.arrayBuffer();
    wasmModule = await WebAssembly.compile(bytes);
    sampleFiles = samples;
    self.postMessage({ type: "ready" });

    let keepSd = false;
    while (true) {
      try {
        runSession(keepSd);
        break;
      } catch (e) {
        if (e instanceof RebootSignal) {
          keepSd = e.keepSd;
          continue;
        }
        throw e;
      }
    }
  } catch (err) {
    self.postMessage({ type: "error", message: String(err && err.stack ? err.stack : err) });
  }
};
