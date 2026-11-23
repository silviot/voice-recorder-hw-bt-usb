/*
 * Cassini OpenFW Browser Emulator — Main Thread UI, Canvas & Web Audio Controller
 *
 * Uses zero-allocation SharedArrayBuffer ring buffers for both browser microphone
 * capture (DMAC CH1) and stereo speaker playback (DMAC CH0) to avoid Web Audio
 * node/buffer churn and GC pressure during long sessions.
 *
 * SPDX-License-Identifier: MIT
 */
"use strict";

const CTRL_WAKE_SEQ    = 0;
const CTRL_KEY_MASK    = 1;
const CTRL_REC_SW      = 2;
const CTRL_HP_PLUG     = 3;
const CTRL_VBUS        = 4;
const CTRL_VBAT_MV     = 5;
const CTRL_PWR_DOWN    = 6;
const CTRL_PWR_SEQ     = 7;
const CTRL_AUDIO_SRC   = 8;
const CTRL_CMD         = 9;
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
const MIC_RING_SAMPLES = 32768;
const MIC_RING_MASK    = MIC_RING_SAMPLES - 1;
const DAC_OFFSET_BYTES = MIC_OFFSET_BYTES + MIC_RING_SAMPLES * 2;
const DAC_RING_FRAMES  = 32768;
const DAC_RING_MASK    = DAC_RING_FRAMES - 1;
const SAB_TOTAL_BYTES  = DAC_OFFSET_BYTES + DAC_RING_FRAMES * 4;

const $ = (id) => document.getElementById(id);

const canvas    = $("lcd");
const ctx       = canvas.getContext("2d", { alpha: false });
const imgData   = ctx.createImageData(160, 80);
const rgba32    = new Uint32Array(imgData.data.buffer);
const logEl     = $("uart-log");
const filesEl   = $("sd-files");
const statusEl  = $("emu-status");

let worker      = null;
let ctrl        = null;
let sabFb       = null;
let sabMic      = null;
let sabDac      = null;
let lastFbVer   = -1;
let lastBl      = -1;
let keyMask     = 0;

// Persistent zero-allocation Web Audio playback node
let audioCtx    = null;
let dacNode     = null;
let audioMuted  = false;
let dacFrac     = 0;
let dacPrevL    = 0;
let dacPrevR    = 0;
let dacCurL     = 0;
let dacCurR     = 0;
let dacHaveCur  = false;

// Live browser microphone state (default input source)
let micStream   = null;
let micNode     = null;
let micSource   = null;
let micZeroGain = null;
let micStarting = false;
let micWanted   = true;

function initDacNode() {
  if (!audioCtx || dacNode) return;
  dacNode = audioCtx.createScriptProcessor(2048, 0, 2);
  const inv = 1 / 32768;

  dacNode.onaudioprocess = (e) => {
    const outL = e.outputBuffer.getChannelData(0);
    const outR = e.outputBuffer.getChannelData(1);
    if (!ctrl || !sabDac) {
      outL.fill(0);
      outR.fill(0);
      return;
    }

    let rd = Atomics.load(ctrl, CTRL_DAC_RD) >>> 0;
    const wr = Atomics.load(ctrl, CTRL_DAC_WR) >>> 0;
    let avail = (wr - rd) >>> 0;
    if (avail > DAC_RING_FRAMES) {
      rd = wr;
      avail = 0;
    }

    const rate = Atomics.load(ctrl, CTRL_DAC_RATE);
    if (audioMuted || rate <= 0 || avail === 0) {
      if (audioMuted && avail > 0) {
        Atomics.store(ctrl, CTRL_DAC_RD, wr | 0);
      }
      outL.fill(0);
      outR.fill(0);
      dacFrac = 0;
      dacPrevL = 0;
      dacPrevR = 0;
      dacCurL = 0;
      dacCurR = 0;
      dacHaveCur = false;
      return;
    }

    // Keep playback latency tight if the browser paused callbacks (e.g. background tab)
    const maxLag = rate > 8000 ? rate : 16000;
    if (avail > maxLag) {
      const keep = rate >> 2;
      rd = (wr - keep) >>> 0;
      avail = keep;
      dacHaveCur = false;
      dacFrac = 0;
    }

    if (!dacHaveCur && avail > 0) {
      const idx0 = (rd & DAC_RING_MASK) << 1;
      dacCurL = sabDac[idx0] * inv;
      dacCurR = sabDac[idx0 + 1] * inv;
      dacPrevL = dacCurL;
      dacPrevR = dacCurR;
      rd = (rd + 1) >>> 0;
      avail--;
      dacHaveCur = true;
    }

    const step = rate / audioCtx.sampleRate;
    const n = outL.length;
    for (let i = 0; i < n; i++) {
      dacFrac += step;
      while (dacFrac >= 1) {
        dacFrac -= 1;
        if (avail > 0) {
          dacPrevL = dacCurL;
          dacPrevR = dacCurR;
          const idx = (rd & DAC_RING_MASK) << 1;
          dacCurL = sabDac[idx] * inv;
          dacCurR = sabDac[idx + 1] * inv;
          rd = (rd + 1) >>> 0;
          avail--;
        } else {
          dacPrevL = 0;
          dacPrevR = 0;
          dacCurL = 0;
          dacCurR = 0;
          dacHaveCur = false;
          dacFrac = 0;
          break;
        }
      }
      outL[i] = dacPrevL + (dacCurL - dacPrevL) * dacFrac;
      outR[i] = dacPrevR + (dacCurR - dacPrevR) * dacFrac;
    }

    Atomics.store(ctrl, CTRL_DAC_RD, rd | 0);
  };

  dacNode.connect(audioCtx.destination);
}

function ensureAudioCtx() {
  if (!audioCtx) {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (AC) {
      audioCtx = new AC();
      initDacNode();
    }
  }
  if (audioCtx && audioCtx.state === "suspended") {
    audioCtx.resume().catch(() => {});
  }
  if (micWanted && !micStream && !micStarting && $("audio-src").value === "mic") {
    enableBrowserMic(true);
  }
}

function wakeWorker() {
  if (!ctrl) return;
  Atomics.add(ctrl, CTRL_WAKE_SEQ, 1);
  Atomics.notify(ctrl, CTRL_WAKE_SEQ);
}

function setKeyBit(bit, down) {
  ensureAudioCtx();
  if (down) keyMask |= 1 << bit;
  else keyMask &= ~(1 << bit);
  if (ctrl) {
    Atomics.store(ctrl, CTRL_KEY_MASK, keyMask);
    wakeWorker();
  }
  const btn = document.querySelector(`[data-key-bit="${bit}"]`);
  if (btn) btn.classList.toggle("active", down);
}

function releaseAllKeys() {
  if (keyMask !== 0) {
    keyMask = 0;
    if (ctrl) {
      Atomics.store(ctrl, CTRL_KEY_MASK, 0);
      wakeWorker();
    }
    document.querySelectorAll("[data-key-bit].active").forEach((b) => b.classList.remove("active"));
  }
  const pwrBtn = $("btn-power");
  if (pwrBtn && pwrBtn.classList.contains("active")) {
    setPowerButton(false);
  }
}

function setPowerButton(down) {
  ensureAudioCtx();
  if (ctrl) {
    Atomics.store(ctrl, CTRL_PWR_DOWN, down ? 1 : 0);
    Atomics.add(ctrl, CTRL_PWR_SEQ, 1);
    wakeWorker();
  }
  const btn = $("btn-power");
  if (btn) btn.classList.toggle("active", down);
}

function setRecSwitch(on) {
  ensureAudioCtx();
  if (ctrl) {
    Atomics.store(ctrl, CTRL_REC_SW, on ? 1 : 0);
    wakeWorker();
  }
  $("rec-switch").checked = on;
  $("rec-led").classList.toggle("on", on);
  $("rec-label").textContent = on ? "REC ON" : "STOP";
}

function appendLog(text) {
  logEl.textContent += text;
  if (logEl.textContent.length > 24000) {
    logEl.textContent = logEl.textContent.slice(-16000);
  }
  logEl.scrollTop = logEl.scrollHeight;
}

function renderFiles(files) {
  if (!files || files.length === 0) {
    filesEl.innerHTML = `<div class="muted">No files in /record yet.</div>`;
    return;
  }
  filesEl.innerHTML = "";
  for (const f of files) {
    const row = document.createElement("div");
    row.className = "file-row";
    const kb = (f.size / 1024).toFixed(1);
    row.innerHTML = `
      <span class="file-name">${f.name}</span>
      <span class="file-meta">${kb} KB</span>
      <button class="btn-mini" type="button">Download</button>
    `;
    row.querySelector("button").addEventListener("click", () => {
      if (worker) worker.postMessage({ type: "download", name: f.name });
    });
    filesEl.appendChild(row);
  }
}

function requestFileList() {
  if (!ctrl) return;
  Atomics.store(ctrl, CTRL_CMD, 1);
  wakeWorker();
}

async function enableBrowserMic(enable) {
  micWanted = enable;
  if (!enable) {
    micStarting = false;
    if (micNode) { micNode.onaudioprocess = null; micNode.disconnect(); micNode = null; }
    if (micZeroGain) { micZeroGain.disconnect(); micZeroGain = null; }
    if (micSource) { micSource.disconnect(); micSource = null; }
    if (micStream) { micStream.getTracks().forEach((t) => t.stop()); micStream = null; }
    if (ctrl) { Atomics.store(ctrl, CTRL_AUDIO_SRC, 0); wakeWorker(); }
    $("mic-status").textContent = "Simulated voice + pauses (fallback mode)";
    return;
  }

  if (micStream || micStarting) return;
  if (!navigator.mediaDevices || !navigator.mediaDevices.getUserMedia) {
    micWanted = false;
    $("audio-src").value = "synth";
    $("mic-status").textContent = "Browser mic API unavailable; using simulated voice";
    return;
  }

  micStarting = true;
  if (!audioCtx) {
    const AC = window.AudioContext || window.webkitAudioContext;
    if (AC) {
      audioCtx = new AC();
      initDacNode();
    }
  }
  if (audioCtx && audioCtx.state === "suspended") {
    audioCtx.resume().catch(() => {});
  }
  $("mic-status").textContent = "Requesting browser microphone…";

  try {
    const stream = await navigator.mediaDevices.getUserMedia({
      audio: {
        echoCancellation: false,
        noiseSuppression: false,
        autoGainControl: false,
      },
    });
    if (!micWanted) {
      stream.getTracks().forEach((t) => t.stop());
      micStarting = false;
      return;
    }
    micStream = stream;
    micSource = audioCtx.createMediaStreamSource(micStream);
    micNode = audioCtx.createScriptProcessor(2048, 1, 1);
    const ratio = audioCtx.sampleRate / 16000;
    let fracPos = 0;

    micNode.onaudioprocess = (e) => {
      if (!ctrl || !sabMic) return;
      const input = e.inputBuffer.getChannelData(0);
      let wr = Atomics.load(ctrl, CTRL_MIC_WR) >>> 0;
      while (fracPos < input.length) {
        const idx = Math.floor(fracPos);
        const s = Math.max(-32767, Math.min(32767, Math.round(input[idx] * 32767)));
        sabMic[wr & MIC_RING_MASK] = s;
        wr = (wr + 1) >>> 0;
        fracPos += ratio;
      }
      fracPos -= input.length;
      Atomics.store(ctrl, CTRL_MIC_WR, wr | 0);
    };
    micSource.connect(micNode);
    // Keep ScriptProcessor alive without echoing mic to speakers
    micZeroGain = audioCtx.createGain();
    micZeroGain.gain.value = 0;
    micNode.connect(micZeroGain);
    micZeroGain.connect(audioCtx.destination);

    micStarting = false;
    if (ctrl) { Atomics.store(ctrl, CTRL_AUDIO_SRC, 1); wakeWorker(); }
    $("mic-status").textContent = `Live browser mic active (${audioCtx.sampleRate} Hz → 16 kHz)`;
  } catch (err) {
    micStarting = false;
    micWanted = false;
    $("audio-src").value = "synth";
    if (ctrl) { Atomics.store(ctrl, CTRL_AUDIO_SRC, 0); wakeWorker(); }
    $("mic-status").textContent = `Mic unavailable (${err.message}); using simulated voice`;
  }
}

function renderLoop() {
  if (ctrl && sabFb) {
    const fbVer = Atomics.load(ctrl, CTRL_FB_VER);
    const bl = Atomics.load(ctrl, CTRL_BACKLIGHT);
    const pwrOff = Atomics.load(ctrl, CTRL_POWERED_OFF);
    const spkPa = Atomics.load(ctrl, CTRL_SPEAKER_PA);

    $("ind-bl").textContent = pwrOff ? "OFF" : `${bl}%`;
    $("ind-pa").textContent = spkPa ? "ON (Speaker)" : "OFF";
    $("ind-pa").classList.toggle("on", !!spkPa);
    $("lcd-off-overlay").classList.toggle("visible", pwrOff !== 0);

    if (fbVer !== lastFbVer || bl !== lastBl) {
      lastFbVer = fbVer;
      lastBl = bl;
      const scale = bl <= 0 ? 0 : (0.2 + 0.8 * (bl / 100));
      for (let i = 0; i < FB_PIXELS; i++) {
        const p = sabFb[i];
        let r = ((p >> 11) & 0x1f) * 255 / 31;
        let g = ((p >> 5) & 0x3f) * 255 / 63;
        let b = (p & 0x1f) * 255 / 31;
        r = (r * scale) | 0;
        g = (g * scale) | 0;
        b = (b * scale) | 0;
        rgba32[i] = (0xff << 24) | (b << 16) | (g << 8) | r;
      }
      ctx.putImageData(imgData, 0, 0);
    }
  }
  requestAnimationFrame(renderLoop);
}

function bindControls() {
  document.addEventListener("pointerdown", ensureAudioCtx, { passive: true });

  // Tactile buttons (blur on press so Space/Enter never double-trigger a focused HTML button)
  document.querySelectorAll("[data-key-bit]").forEach((btn) => {
    const bit = parseInt(btn.dataset.keyBit, 10);
    const down = (e) => { e.preventDefault(); btn.blur(); btn.setPointerCapture?.(e.pointerId); setKeyBit(bit, true); };
    const up   = (e) => { e.preventDefault(); setKeyBit(bit, false); };
    btn.addEventListener("pointerdown", down);
    btn.addEventListener("pointerup", up);
    btn.addEventListener("pointercancel", up);
  });

  const pwrBtn = $("btn-power");
  pwrBtn.addEventListener("pointerdown", (e) => { e.preventDefault(); pwrBtn.blur(); pwrBtn.setPointerCapture?.(e.pointerId); setPowerButton(true); });
  pwrBtn.addEventListener("pointerup", (e) => { e.preventDefault(); setPowerButton(false); });
  pwrBtn.addEventListener("pointercancel", () => setPowerButton(false));

  $("rec-switch").addEventListener("change", (e) => { e.target.blur(); setRecSwitch(e.target.checked); });

  $("hp-toggle").addEventListener("change", (e) => {
    e.target.blur();
    ensureAudioCtx();
    if (ctrl) { Atomics.store(ctrl, CTRL_HP_PLUG, e.target.checked ? 1 : 0); wakeWorker(); }
  });

  $("usb-toggle").addEventListener("change", (e) => {
    e.target.blur();
    ensureAudioCtx();
    if (ctrl) { Atomics.store(ctrl, CTRL_VBUS, e.target.checked ? 1 : 0); wakeWorker(); }
  });

  $("vbat-slider").addEventListener("input", (e) => {
    const mv = parseInt(e.target.value, 10);
    $("vbat-val").textContent = `${(mv / 1000).toFixed(2)} V`;
    if (ctrl) { Atomics.store(ctrl, CTRL_VBAT_MV, mv); wakeWorker(); }
  });

  $("audio-src").addEventListener("change", (e) => { e.target.blur(); enableBrowserMic(e.target.value === "mic"); });

  $("mute-toggle").addEventListener("change", (e) => {
    e.target.blur();
    audioMuted = e.target.checked;
    if (!audioMuted) ensureAudioCtx();
  });

  $("btn-refresh-sd").addEventListener("click", (e) => { e.target.blur(); requestFileList(); });
  $("btn-reboot").addEventListener("click", (e) => {
    e.target.blur();
    if (!ctrl) return;
    setRecSwitch(false);
    Atomics.store(ctrl, CTRL_CMD, 3);
    wakeWorker();
  });
  $("btn-wipe-sd").addEventListener("click", (e) => {
    e.target.blur();
    if (!ctrl) return;
    setRecSwitch(false);
    Atomics.store(ctrl, CTRL_CMD, 4);
    wakeWorker();
    setTimeout(requestFileList, 800);
  });
  $("btn-clear-log").addEventListener("click", (e) => { e.target.blur(); logEl.textContent = ""; });

  // Keyboard shortcuts:
  //   ArrowLeft / ArrowRight     -> Prev / Next
  //   ArrowUp / ArrowDown (+ / -) -> VOL+ / VOL-
  //   B / Delete / Backspace / Esc -> Back
  //   Enter / O                  -> OK / Play
  //   P / Space                  -> Power
  //   R                          -> Record slide switch
  const keyMap = {
    ArrowLeft: 0,
    ArrowRight: 1,
    ArrowUp: 2,
    "+": 2,
    "=": 2,
    ArrowDown: 3,
    "-": 3,
    b: 4,
    B: 4,
    Delete: 4,
    Backspace: 4,
    Escape: 4,
    Enter: 5,
    o: 5,
    O: 5,
  };

  const isPowerKey = (key) => key === "p" || key === "P" || key === " ";

  window.addEventListener("keydown", (e) => {
    if (e.ctrlKey || e.metaKey || e.altKey) return;
    if (e.target.tagName === "SELECT" || (e.target.tagName === "INPUT" && e.target.type === "range")) return;

    if (e.key === "r" || e.key === "R") {
      if (!e.repeat) setRecSwitch(!$("rec-switch").checked);
      e.preventDefault();
      return;
    }
    if (isPowerKey(e.key)) {
      if (!e.repeat) setPowerButton(true);
      e.preventDefault();
      return;
    }
    if (e.key in keyMap) {
      e.preventDefault();
      if (!e.repeat) setKeyBit(keyMap[e.key], true);
    }
  });

  window.addEventListener("keyup", (e) => {
    if (isPowerKey(e.key)) {
      setPowerButton(false);
      e.preventDefault();
      return;
    }
    if (e.key in keyMap) {
      setKeyBit(keyMap[e.key], false);
      e.preventDefault();
    }
  });

  window.addEventListener("blur", releaseAllKeys);

  window.addEventListener("beforeunload", () => {
    if (worker) { worker.terminate(); worker = null; }
    if (dacNode) { dacNode.onaudioprocess = null; dacNode.disconnect(); dacNode = null; }
    if (micNode) { micNode.onaudioprocess = null; micNode.disconnect(); micNode = null; }
    if (micZeroGain) { micZeroGain.disconnect(); micZeroGain = null; }
    if (micSource) { micSource.disconnect(); micSource = null; }
    if (micStream) { micStream.getTracks().forEach((t) => t.stop()); micStream = null; }
    if (audioCtx) { audioCtx.close().catch(() => {}); audioCtx = null; }
  });
}

async function start() {
  bindControls();

  // Register COI service worker automatically if served on a plain static HTTPS host
  if (!window.crossOriginIsolated) {
    if ("serviceWorker" in navigator && location.protocol !== "file:") {
      statusEl.textContent = "Enabling SharedArrayBuffer (reloading once)...";
      try {
        await navigator.serviceWorker.register("./coi-serviceworker.js");
        if (!sessionStorage.getItem("coi_reloaded")) {
          sessionStorage.setItem("coi_reloaded", "1");
          location.reload();
          return;
        }
      } catch (err) {
        console.warn("COI SW failed:", err);
      }
    }
    statusEl.textContent = "Requires COOP/COEP headers — run: python3 web_emu/serve.py";
    statusEl.className = "badge err";
    return;
  }

  const sab = new SharedArrayBuffer(SAB_TOTAL_BYTES);
  ctrl   = new Int32Array(sab, 0, 32);
  sabFb  = new Uint16Array(sab, FB_OFFSET_BYTES, FB_PIXELS);
  sabMic = new Int16Array(sab, MIC_OFFSET_BYTES, MIC_RING_SAMPLES);
  sabDac = new Int16Array(sab, DAC_OFFSET_BYTES, DAC_RING_FRAMES * 2);
  Atomics.store(ctrl, CTRL_VBAT_MV, 4020);

  worker = new Worker("./worker.js");
  worker.onmessage = (ev) => {
    const msg = ev.data;
    if (msg.type === "ready") {
      statusEl.textContent = "Running openfw.wasm (SL6801)";
      statusEl.className = "badge ok";
      setTimeout(requestFileList, 1200);
    } else if (msg.type === "uart") {
      appendLog(msg.text);
      if (msg.text.includes("Saved") || msg.text.includes("SD: OK")) {
        setTimeout(requestFileList, 250);
      }
    } else if (msg.type === "files") {
      renderFiles(msg.files);
    } else if (msg.type === "fileData") {
      const blob = new Blob([msg.data], { type: "application/octet-stream" });
      const url = URL.createObjectURL(blob);
      const a = document.createElement("a");
      a.href = url;
      a.download = msg.name;
      a.click();
      setTimeout(() => URL.revokeObjectURL(url), 5000);
    } else if (msg.type === "error") {
      statusEl.textContent = "Emulator Error";
      statusEl.className = "badge err";
      appendLog(`\n[ERROR] ${msg.message}\n`);
    }
  };

  worker.postMessage({ type: "init", sab, wasmUrl: "./openfw.wasm" });
  requestAnimationFrame(renderLoop);
}

start();
