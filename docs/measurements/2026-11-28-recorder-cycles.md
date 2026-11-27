# CPU0 cost of recording with phone sync

On one SL6801, recording with the radio off left CPU0's idle task running for 69.6% of the screen-off interval. The same recording path with a paired phone reading files at 29.6 KiB/s left 11.6% idle. Voice Clarity processing occupied 4.5% and 5.6% of elapsed time respectively, including preemption. Background transfer is the higher-priority target for cycle profiling.

## On-device comparison

These USB-powered runs used the home screen's REC-switch path, 16 kHz mono, Voice Clarity with digital AGC, a fixed analog gain of 20, VOR off, and a 30 s screen timeout. Each run recorded for approximately 66 s. The phone stayed paired with automatic sync enabled. The radio-off control inhibited background sync for that run. With sync allowed, the recorder served real GETS payloads.

| AGC code placement | Phone transfer | DSP cycles/frame | CPU0 idle | DSP elapsed | WAV writes and checkpoints | GETS KiB/s |
|---|---|---:|---:|---:|---:|---:|
| Flash (production placement) | Radio off | 271.23 | 69.61% | 4.52% | 3.35% | 0 |
| Flash | Active | 334.86 | 11.62% | 5.59% | 3.91% | 29.61 |
| SRAM | Radio off | 269.65 | 69.93% | 4.50% | 3.24% | 0 |
| SRAM | Active | 354.29 | 11.98% | 5.91% | 4.20% | 29.63 |

Each row pools four complete 8 s windows (5 through 8), after the screen timeout. Ratios use sums of cycles, frames, bytes and time. Recording startup, radio connection and the final partial window are excluded. All four recordings had zero ring overruns, lost or unaccounted samples, and SD errors. Each contained the exact number of frames reported by DMA. The four rows are from individual runs with live audio and a live phone. Windows within a run share those conditions.

At 96 MHz the flash rows correspond to 29.2 versus 84.8 million cycles per second of non-idle CPU0 time. The difference is about 1.88 million additional non-idle cycles per KiB served under this workload. This includes the cost of enabling the radio, the Bluetooth stack, file service, and scheduling. Recording a connected phone with zero payload is the missing control for isolating incremental transfer cost.

Moving the 852-byte gain-control routine to SRAM saved 1.58 DSP cycles/frame with the radio off and took 19.43 more elapsed cycles/frame during sync. Neither result justifies reserving the SRAM for this routine. Production keeps the flash placement.

## Where the sync cycles went

A second radio-off and transfer pair used the fixed firmware, flash AGC placement and per-task snapshots. In the 32–64 s interval, the radio-off app task used 30.2% of CPU0 and idle used 69.7%. With phone transfer, the app used 59.4%, the controller task 11.2%, BTU 9.1%, HCI 4.4%, BTC 2.6% and Bluetooth delivery 1.6%. Idle fell to 10.8%. The independent recording windows counted 31.09 KiB/s of GETS payload. Each task snapshot took about 1 ms with the radio off and 2.2 ms with it on.

The app task accounted for the largest increase, **29.2 percentage points**. The longest recorder poll in this transfer run was 88.1 ms, including a WAV write that lasted 81.3 ms. All eight short recordings, including the first two timer-probe runs, finished with zero lost samples, ring overruns and SD errors.

1. Profile the home app task inside `bt_bg_poll`, splitting XREC file reads and transmit assembly from connection polling and UI updates. Count polls that advance zero bytes. Measure a bonded idle link and a steady transfer against the same radio-off control.
2. Target the largest measured component. Compare alternating builds at matched payload rates. Track CPU cycles per KiB, CPU0 idle share and worst recorder-poll gap alongside payload throughput. The existing recording acceptance target calls for at least 20% idle. Both transfer pairs here fell below it.
3. Once idle headroom improves, repeat an unplugged run with cumulative transfer bytes and battery readings timestamped on the device. Measuring battery current during recording, idle connection and active transfer would give an energy cost per recorded minute or transferred MiB.

The session also exposed a separate shutdown bug. An open SPP link refreshed the timestamp used for the four-second grace after the phone reported zero pending work. The fix gives completed sync its own timer and resets it when recording or transfer resumes. Host regression coverage exercises repeated idle polls, resumed activity and tick wrap. The live transfer runs exercised a phone with work pending. A live phone that signals completion and holds the connection open is still needed to measure the energy saved after sync.

[Window counters from the six SysTick runs](2026-11-28-recorder-cycles.csv) · [Recording acceptance targets](../BT_SYNC_SPEED_AND_RECORDING.md)

## Reproducing the measurements

The profiler is enabled only in a dedicated bench build.

```sh
make -C open_fw BT=1 BENCH=1 PROFILE_REC=1 BUILD_DIR=/tmp/openfw-rec-profile \
  OUT_BIN=/tmp/openfw-rec-profile.bin OUT_UP=/tmp/openfw-rec-profile.up update-up
```

With the desired screen timeout and a bonded phone, use one request per boot in the root of the card as `BTBENCH.RUN` and save `BTBENCH.LOG` after each run.

```text
mode=rec secs=66 delay_s=2 rec_ch=1 rec_gain=20 rec_dsp=1 rec_vor=0
```

Append `sync_s=12` to allow normal background sync during recording and up to 12 s for it to finish after stop. Keep a prior recording available to sync. Confirm nonzero `gets_b` in the actual comparison windows. A Bluetooth connection alone cannot establish this.

```sh
python3 tools/profile_recording.py radio-off.log phone-sync.log > windows.csv
```

The profiler takes CPU-clocked SysTick timestamps around each DSP call and accumulates unsigned cycle differences. The hardware DWT cycle register returned zero on this unit. An independent microsecond timer measured 95.90–95.95 elapsed cycles/µs across the four pooled rows. These measurements include interrupts, task switches and memory stalls. FreeRTOS charges an interrupt to the task it interrupted. SD figures include waits and successful WAV writes and checkpoints. CPU1, radio current, SD current and display current need separate measurements.

## Off-USB recording

The normal firmware produced 214,208,512 frames of mono audio, or **3 h 43 min 8.032 s**. The lowest unplugged voltage in the retained log was 3,763 mV. The log shows a phone connection and pending sync on battery. The two transcript commits visible in the retained log occurred after USB power returned. The file establishes the recording duration. The run ended by choice and the release log does not contain a cumulative on-battery byte count or a battery discharge curve.
