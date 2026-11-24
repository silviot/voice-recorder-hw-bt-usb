# FreeRTOS on open_fw (CPU0)

`make RTOS=1` (implied by `BT=1`) runs the legacy super-loop as the FreeRTOS task `app` on CPU0. CPU1 (LVGL worker)
stays bare-metal. Kernel: FreeRTOS-Kernel V11.1.0, vendored unmodified in `third_party/freertos` (GCC ARM_CM4F port,
heap_5). Design and rationale: `docs/BLUETOOTH_BLUEDROID_PLAN.md` §3. Silicon results: `docs/BT_HW_RESULTS.md`.

## How it is wired

| Piece | Where | Notes |
|---|---|---|
| Config | `FreeRTOSConfig.h` | 1 kHz tick, 10 priorities, timer daemon 9, `configMAX_SYSCALL_INTERRUPT_PRIORITY` = 0x80 (3 priority bits) |
| Priorities | `include/openfw_rtos.h` | mirror Bluedroid's `configMAX_PRIORITIES - 3/-5/-6` (hciT 7, BTU 5, BTC 4); static-asserted in `rtos_port.c` |
| Tick | `startup.c` SysTick handler → `openfw_rtos_tick()` | SysTick stays owned by startup.c (`g_systick_ms`, keys, experiment watchdog) |
| Tick start | `vPortSetupTimerInterrupt()` → `systick_init()` | inside `xPortStartScheduler()`, after SHPR3 is set |
| Heap | `sl6801_firm.ld` `RTOS_HEAP_LO` (0x805C00, 81.9 KiB) + `RTOS_HEAP` (0x86B000, 84 KiB minus static) | both validated on silicon (Exp 0); ascending order required by heap_5 |
| Kernel static objects | `.rtos_static` section | idle/timer TCBs + stacks, bt_alarm task; main `.bss` is nearly full |
| Task waits | `OPENFW_TASK_WAIT()` (`sl6801_regs.h`) | WFI in bare-metal builds, `ulTaskNotifyTake(1 tick)` under the RTOS |
| Self-test | `RTOS_SELFTEST=1`, `rtos_selftest.c` | 14 tests; `make test-qemu-rtos`, `make test-qemu-bt`; on silicon read `g_rtos_selftest` with `exp/exp_peek.c` |

## Lessons (each one cost a debugging round)

- **Never `WFI` in task context.** The app super-loop used `WFI` to sleep; as a task that keeps it *Running*, so the
  idle task never runs and **self-deleted tasks are never freed** (Bluedroid deletes its own tasks on disable). Every
  task-context wait goes through `OPENFW_TASK_WAIT()`; only the idle hook executes `WFI`. Terminal reboot/power-off
  loops keep `WFI`.
- **Deleting another task frees it immediately; self-deletion is reaped by idle.** A test that only deletes other
  tasks passes even when idle is starved — `T_REAP` covers self-deletion. Once idle started running, an old test bug
  surfaced: deleting a task handle after the task had already deleted itself is a double free (heap_5 asserts in
  `vPortFree`).
- **Start SysTick inside `vPortSetupTimerInterrupt()`, never before `vTaskStartScheduler()`.** Interrupts are
  already enabled in `reset_c_entry`; an early SysTick at default priority can enter the kernel before the port sets
  SysTick/PendSV priorities. `main.c`'s later `systick_init()` is a no-op when the configuration is unchanged.
- **`vApplicationMallocFailedHook` must not reboot in BT builds.** Otherwise `malloc()` can never return NULL on
  target and every fail-soft path in Bluedroid is dead code (and an active recording would be lost). The BT build
  counts failures; non-BT builds keep the fatal `0xF1` record.
- **When a QEMU run "hangs", look for `system_reboot()`.** The QEMU model does not complete a reboot, so a fatal
  error looks like a stall. `tools/run_qemu.py` prints the PC, stack and `g_rtos_fatal` (a RAM copy of the
  `AON_BACKUP` fault record, which QEMU does not keep) when the self-test does not finish.
- **`.data` must never go into a `NOLOAD` section.** Initialised data needs a load address in XIP, a copy loop, and
  `-j <section>` in the `objcopy` that builds `xip.bin`.
- **`.ram_boot` has ~170 B of headroom.** Put new boot-time code in XIP (`bt_static_init()` runs after XIP is mapped)
  and new hot code in `.fast_ramtext`, not `.ramtext`.
- Keep the default image byte-identical: guard every RTOS/BT change with `OPENFW_RTOS` / `OPENFW_BT_BLUEDROID` and
  compare `FIRM CRC-16` of `make all` before committing (currently 0xC786).
