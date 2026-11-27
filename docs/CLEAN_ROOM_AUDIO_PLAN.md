# Clean-Room Audio Processing Plan (`open_fw`)

**Target Hardware:** Smartlink SL6801 (`HW_76`, Cortex-M4F @ 96–192 MHz, 512 KiB SRAM, dual differential analog mics `AMIC1 + AMIC2` at `18 mm` spacing, 16-bit / 24-bit ADC up to 48 kHz).  
**Primary Downstream Consumers:**
1. **NVIDIA Parakeet-TDT-0.6B (`v2` / `v3`)** — 16 kHz mono FastConformer-TDT, high native noise robustness, degraded by aggressive spectral subtraction or steep formant-distorting EQ.
2. **Cactus Whistle (55M / 16.9 MB)** — 16 kHz mono edge ASR (≤30 s chunks, 80-bin log-mel, built-in Aho-Corasick keyword biasing), benefits from clean host-side VAD + `150–7000 Hz` bandpass.
3. **Human Playback ("Voice Clarity" Mode)** — Default recording profile and clean-room approximation of the stock firmware's "human voice focus" sound signature (without reverse-engineered binary blobs).

---

## 1. Design Principles

1. **Preserve natural speech dynamics in the Raw profile.**
   Raw processing should prevent hardware/acoustic failure modes (DC offset, handling rumble, ADC clipping) while leaving spectral detail and dual-mic phase intact for Parakeet, Whistle, and host-side beamforming.
2. **Zero proprietary blobs, zero heap allocations.**  
   Pure C99 (`float32_t` using Cortex-M4F hardware FPU, or Q31 fallback), static per-channel state structs, deterministic execution inside `recorder_poll()` (never inside `DMAC_CH1_IRQHandler`).
3. **Zero-overhead drop-in to `open_fw`.**  
   Plugs directly between `audio_pop_frame()` in `open_fw/src/audio_dma.c` and `exfat_write()` in `open_fw/src/recorder.c`, reusing the existing 4-slot (`4 × 128 ms`) lock-free DMAC ring buffer.
4. **Host-testable first.**  
   Every DSP block compiles and passes automated unit tests on Linux (`open_fw/tests/test_audio_dsp.c`) before touching hardware.

---

## 2. Architecture & File Layout

```text
open_fw/
├── include/
│   └── audio_dsp.h          # Public API, profile enum, config & state structs + VOR
├── src/
│   ├── audio_dsp.c          # Clean-room biquads, soft limiter, compressor, VOR, stats
│   ├── audio_dma.c          # Dual-mic AMIC1+AMIC2 stereo / mono capture via DMAC CH1
│   ├── recorder.c           # Wires audio_dsp_process_i16() + VOR between audio_pop_frame() & exfat_write()
│   └── xrec_proto.c         # Exposes DSP profile & PGA gain over +XREC
└── tests/
    └── test_audio_dsp.c     # Host unit tests (frequency response, DC, limiter, stereo link, VOR)
```

---

## 3. On-Device DSP Profiles (`audio_dsp_profile_t`)

### Profile A: `AUDIO_DSP_ASR_RAW` *(For Parakeet & Whistle)*
Minimal, phase-safe, non-destructive capture conditioning:

1. **Stage 1 — 75 Hz 2nd-Order Butterworth High-Pass Filter (per channel)**
   - **Topology:** Direct Form II Transposed biquad (`float32_t`, per-channel state `z1, z2`).
   - **Purpose:** Strips ADC DC offset, pocket/chassis handling thumps, and wind rumble without attenuating male voice fundamentals (`85–180 Hz`) or distorting vowel formants.
   - **Standard RBJ coefficients (`fs = 16000 Hz, fc = 75 Hz, Q = 0.70710678`):**
     - `b0 =  0.979388f`, `b1 = -1.958776f`, `b2 =  0.979388f`
     - `a1 = -1.958351f`, `a2 =  0.959202f`
2. **Stage 2 — Transparent Safety Peak Limiter (No Upward AGC)**
   - **Purpose:** Prevents harsh digital clipping on sudden loud speech/plosives while keeping background noise floor completely flat (no AGC "breathing/pumping" that confuses VAD and ASR).
   - **Parameters:**
     - **Automatic Mic Gain (Auto-PGA):** Starts each recording at PGA `20` (manual override only via `+XREC:GAIN=<0..31>`); if raw ADC peaks hit `>= 31200` on active DSP profiles (`ASR_RAW` / `VOICE_CLARITY`), PGA steps down by `2` down to a floor of `8` with a 4-block (`~512 ms`) cooldown, and recovers slowly `+1` per 64 consecutive quiet blocks (`< 2000`, VOR open) back toward `20`.
     - Soft-knee ceiling starting at `-3.0 dBFS` (`0.707`), hard asymptote at `-0.5 dBFS` (`0.944`) via smooth `tanh`-like rational saturator or envelope-follower gain reduction (`attack = 1 ms`, `release = 80 ms`).
   - **Stereo linking (Phase 2):** Compute peak envelope as `max(|L|, |R|)` and apply identical gain to both channels to preserve inter-channel level/phase cues for host beamforming.

### Profile B: `AUDIO_DSP_VOICE_CLARITY` *(Default for voice recording and listening)*
Replicates the intelligible "broadcast voice" signature of stock `codec.admomini` using standard textbook DSP:

1. **Stage 1 — 350 Hz 2nd-Order Butterworth HPF (`fs = 16000 Hz, Q = 0.7071`)**
   - Cuts room boom, HVAC hum, and low-frequency proximity mud.
   - RBJ coefficients: `b0 = 0.907363f`, `b1 = -1.814726f`, `b2 = 0.907363f`, `a1 = -1.806200f`, `a2 = 0.823251f`.
2. **Stage 2 — 2200 Hz High-Shelf `+9 dB` (`fs = 16000 Hz, Q = 0.7071`)**
   - Boosts consonant clarity and sibilant presence (`2.2–6 kHz`).
3. **Stage 3 — Gentle 2:1 Voice Compressor + Makeup + Limiter**
   - Threshold: `-24 dBFS`, Ratio: `2:1`, Attack: `5 ms`, Release: `120 ms`, Makeup: `+6 dB`, followed by the `-0.5 dBFS` safety limiter.
4. **Stage 4 *(Optional Phase 4)* — Light Dual-Mic / STFT Noise Suppressor**
   - Either simple `18 mm` broadside delay-and-sum (`(L + R) * 0.5`) or a lightweight 256-pt (16 ms) real FFT Wiener/MMSE-LSA floor capped at `-12 dB` attenuation (avoids musical noise). Strictly disabled in `ASR_RAW`.

### Profile C: `AUDIO_DSP_BYPASS` *(Calibration & Diagnostics)*
- Passes raw ADC samples unmodified to WAV for measuring analog noise floor, DC offset, and verifying dual-mic channel match.

---

## 4. Public C API (`open_fw/include/audio_dsp.h`)

```c
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef enum {
    AUDIO_DSP_ASR_RAW       = 0, /* 75 Hz HPF + safety limiter (for speech recognition) */
    AUDIO_DSP_VOICE_CLARITY = 1, /* 350 Hz HPF + 2.2 kHz +9 dB shelf + 2:1 comp + limiter     */
    AUDIO_DSP_BYPASS        = 2, /* Raw ADC samples, no processing                            */
} audio_dsp_profile_t;

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2;
} audio_biquad_t;

typedef struct {
    audio_dsp_profile_t profile;
    uint32_t sample_rate;
    uint8_t  channels;          /* 1 (mono) or 2 (interleaved stereo) */
    audio_biquad_t hpf[2];
    audio_biquad_t shelf[2];
    float comp_env;             /* Linear RMS/peak envelope for compressor */
    float lim_env;              /* Linear peak envelope for safety limiter */
    uint32_t clip_events;       /* Counter of samples engaging safety limiter */
    float    rms_dbfs;          /* Last-frame RMS level (for VU / telemetry) */
} audio_dsp_t;

void audio_dsp_init(audio_dsp_t *dsp, audio_dsp_profile_t profile,
                    uint32_t sample_rate, uint8_t channels);
void audio_dsp_reset(audio_dsp_t *dsp);

/* In-place processing of interleaved int16 PCM (num_frames = samples per channel). */
void audio_dsp_process_i16(audio_dsp_t *dsp, int16_t *pcm, size_t num_frames);
```

---

## 5. Hardware Constraints, CPU & Memory Budget on SL6801

- **Hardware FPU confirmed in silicon (`fpv4-sp-d16`):** Both BootROM and stock firmware execute single-precision VFPv4-SP instructions natively. Firmware built with `-mfpu=fpv4-sp-d16 -mfloat-abi=hard` **must** enable `SCB->CPACR (0xE000ED88) |= (0xFu << 20); __DSB(); __ISB();` in `Reset_Handler` before running any hard-float C code.
- **DMAC Transfer-Complete IRQ bit (`0x00080002u`):** On SL6801 DMAC (`0x40001000`), `DMAC_CH_IRQ_EN`/`IRQ_STAT` **bit 0 (`0x01`) is an early remaining-bytes watermark IRQ** (`[31:14] = 0x20` → 32 bytes remaining), while **bit 1 (`0x02`) is Transfer Complete (`TC`)**. Both `audio_dma.c` and `audio_dac.c` **must** enable **`0x00080002u`** and rearm on **bit 1 (`DMAC_IRQ_TC = 1u << 1`)** — arming bit 0 aborts every 128 ms DMA buffer 16 bytes (4 stereo frames = 1 burst) early, leaving a periodic 0.25 ms zero gap / filter ramp.
- **ADC channel FIFO enable (`ADC_CHx_CTRL = 0x09`):** Channel control registers have stride `0x30` (`CH1 = 0x40009250` for `AMIC1`, `CH2 = 0x40009280` for `AMIC2`). Each active channel must set **bit 3 (`ADC_CHx_FIFO_EN`) | bit 0 (`ADC_CHx_EN`) = `0x09`**.
- **Frame size:** `2048` frames @ 16 kHz = `128 ms` per DMAC interrupt (`8192` bytes stereo).
- **Compute cost (`ASR_RAW`, Cortex-M4F w/ FPU @ 96 MHz):**
  - 1 biquad DF-II-T = ~12 cycles/sample; limiter = ~15 cycles/sample.
  - Mono: `2048 × ~30 cycles ≈ 61k cycles` = **0.64 ms per 128 ms frame (~0.5% CPU)**.
  - Stereo: **~1.0% CPU**.
  - `VOICE_CLARITY`: **~1.5–2.0% CPU** (even with 2 biquads + compressor + limiter).
- **RAM footprint:** `sizeof(audio_dsp_t) ≈ 128 bytes` (static BSS, zero malloc).
- **Profiling:** Benchmark on silicon using the 1 MHz 64-bit hardware timer (`0x40082104` / `0x40082108`), **not** `DWT->CYCCNT` (unused in stock BootROM/firmware and frozen on sibling SL6806 silicon).

---

## 6. Playback Volume & Built-In Speaker PA (`open_fw/src/audio_dac.c`)

- **Logarithmic dB Volume Curve:** Hardware `CODEC_DAC01_ANA_VOL` (`0x40009030`, `0..31`) has limited audible range on its own; stock firmware keeps `CODEC_DAC0/1_VOL` (`0x40009138`/`0x40009158`) at `0x1FF` (0 dB) and scales PCM in software using a logarithmic dB curve (`10^(dB/20)`). Use a **32-entry logarithmic dB Q15 lookup table** (`0` = mute, `-48 dB` at level 1 up to `0 dB` at level 31, `~1.6 dB/step`) instead of linear or quadratic scaling so every volume-button step produces a uniform perceptual change.
- **Pop-Free Speaker PA (`PB30`):** The built-in front speaker shares `DAC0 + DAC1` (`DAC2` is unused) through an external PA enabled on **`PB30`** (`GPIOB_BASE = 0x40081040`, `mux = 0` Hi-Z: internal pull-up ON `0x40081068[29:28] = 2` enables PA; no-pull disables PA via external pull-down). Keep `PB30` OFF whenever headphones are detected (`PB11 == 0`) and during DAC analog power-up/down depop ramps.

---

## 7. Stock Firmware Baseline (For Phase 4 VOR & A/B Benchmarks)

- **No hardware AGC:** Analog mic PGAs (`AMIC1`/`AMIC2`) and ADC digital gain (`0 dB`) are static during recording.
- **Stock software AGC (`codec.admomini`, active by default at `512 kbps`):** `2:1` compressor, `-24 dBFS` upper / `-60 dBFS` lower threshold, `1 ms` attack / `20–50 ms` release, `+8 dB` makeup gain (plus 350 Hz HPF, 2.2 kHz `+9 dB` high-shelf, and `-15 dB` 512-pt dual-mic STFT noise suppression).
- **Stock VOR (`enc.vad`, opt-in):** `20–1000 Hz` spectral VAD on 128-sample mono-downmixed frames (`-40 / -20 dBFS` thresholds), 30-frame (`240 ms` @ 16 kHz) hangover bitmask, and 25-slot (`~200 ms`) circular pre-roll ring.

---

## 8. Implementation Status & Roadmap

### Phase 1 — Core Clean-Room DSP Module + Host Unit Tests *(Complete)*
- [x] Add `open_fw/include/audio_dsp.h` and `open_fw/src/audio_dsp.c` implementing `ASR_RAW`, `VOICE_CLARITY`, and `BYPASS`.
- [x] Fix `DMAC_IRQ_TC` to bit 1 (`0x00080002u`) in `sl6801_regs.h`, `audio_dma.c`, and `audio_dac.c` to eliminate the 16-byte/4-frame tail gap on every 128 ms buffer.
- [x] Add `open_fw/tests/test_audio_dsp.c` and hook into `open_fw/Makefile` host test target:
  - Test 1: DC step input → settles to `0` under `ASR_RAW` and `VOICE_CLARITY`.
  - Test 2: Sine frequency sweep (`30 Hz`, `75 Hz`, `350 Hz`, `1 kHz`, `2.2 kHz`, `5 kHz`) → verify gain within `±0.5 dB` of analytical Butterworth / RBJ shelf curves.
  - Test 3: Overdriven `+6 dB` burst → verify limiter holds peak `<= 32767` with zero wrap-around and smooth release.
  - Test 4: Stereo phase-link test → verify identical gain reduction on L and R channels.
- [x] Wire `audio_dsp_init()` in `recorder_start()` and `audio_dsp_process_i16()` in `recorder_poll()` (`open_fw/src/recorder.c`), defaulting to `AUDIO_DSP_VOICE_CLARITY`.

### Phase 2 — True Dual-Mic Stereo Capture (`AMIC1 + AMIC2`) in `audio_dma.c` *(Complete)*
- [x] Verify `AMIC2` analog GPIO mux pins (`PB14/PB15`), `CODEC_ADC_CH_MUX = 0x000D0D00`, and `ADC_CH1/2_CTRL = 0x09` (`0x40009250`/`0x40009280`) — **live-verified on silicon**.
- [x] Keep stereo mode in `audio_dma.c` (`4096` samples = `8192` bytes per 128 ms slot, `32 KiB` total ring buffer) with `DMAC_CH_IRQ_EN = 0x00080002u` verified gap-free in `BYPASS` mode.
- [x] Update `audio_dac.c` volume control to the 32-entry logarithmic dB Q15 curve.
- [ ] Optional: add 24-bit-in-32-bit capture mode if extra dynamic range is needed (avoids PGA tuning altogether).

### Phase 3 — Runtime Control & Host ASR Pipeline *(Complete)*
- [x] Expose `+XREC:DSP=<0|1|2>` and optional manual `+XREC:GAIN=<0..31>` override (on-device recording defaults to automatic mic gain starting at PGA `20`). Changes persist to `/cassini/settings.bin` when mounted; DSP profile applies to the next recording.
- [x] Add host helper script (`tools/asr_prep.py` with `--self-test`) implementing the two downstream recipes:
  - **Parakeet (`parakeet-tdt-0.6b-v2`/`v3`):** stereo-to-mono sum → clean-room / Silero VAD → 16 kHz mono WAV + JSON manifest.
  - **Whistle (Cactus 55M):** stereo-to-mono sum → `150–7000 Hz` bandpass → pause-aligned chunks `≤ 30 s` + JSON manifest + keyword list.

### Phase 4 — Optional Advanced Features
- [x] **Clean-room VOR (Voice-Operated Recording):** 200 ms pre-roll ring + stereo RMS/ZCR hysteresis gate with `500 ms` hangover and 5 ms fade-in/out (opt-in via Settings).
- [ ] **Optional `VOICE_CLARITY` light noise suppression:** 256-pt real FFT Wiener suppressor (`-12 dB` floor) or simple `18 mm` broadside beamformer.
- [ ] **Hardware A/B WER Benchmark:** Record the same noisy & quiet speech passages on SL6801 under `BYPASS`, `ASR_RAW`, `VOICE_CLARITY`, and stock firmware, and compare WER directly on Parakeet and Whistle.
