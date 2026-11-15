# Task reboot-0118 Result

- **Branch:** `reboot-0118` (branched from `enchilada` at `fbdd383`)
- **Status:** Host-verified only (not built in XNU CI, not pushed, not tested on device)

## Deliverables

- `xnu/patches/0118-op6-psci-reset.patch`
- `op6/xnu-patches/0118-op6-psci-reset.patch` (byte-identical mirror)
- `op6/GPU-NOTES.md` (documented PSCI reset/power-off patch 0118 and SSH reboot-into-fastboot procedure)
- `delegations/reboot-0118.result.md` (this report)

## Implementation Summary (`0118-op6-psci-reset.patch`)

1. **`iokit/Kernel/arm/SweetPlatformExpert.cpp`** (`#if defined(SWEET)` / `ARM64_BOARD_CONFIG_SWEET`):
   - In `SweetPlatformExpert::start(IOService *provider)`, parses optional boot-arg `sweetpanicreboot` (default `0`, off) via `PE_parse_boot_argn` and installs `PE_halt_restart = sweet_halt_restart`.
   - `sweet_halt_restart(unsigned int type)`:
     - `kPEPanicRestartCPU` / `kPEPanicRestartCPUNoCallouts`: if `sweet_panic_reboot == 0` (default), prints `SWEET: panic screen held (set sweetpanicreboot=1 to reset on panic)` via `kprintf` and spins in `wfe` so `kdp_machine_reboot_type()` does not fall through to `halt_all_cpus(TRUE)` -> `kPERestartCPU`. If `sweet_panic_reboot != 0`, proceeds to the `kPERestartCPU` reset path.
     - `kPERestartCPU`: calls `sweet_spmi_flush_next_boot()`, logs `SWEET: PSCI SYSTEM_RESET (fn=0x84000009, type=...)` via `kprintf`, issues `dsb sy`, and invokes `smc #0` with `x0 = 0x84000009ULL` (`PSCI_SYSTEM_RESET`).
     - `kPEHaltCPU`: logs `SWEET: PSCI SYSTEM_OFF (fn=0x84000008, type=...)` via `kprintf`, issues `dsb sy`, and invokes `smc #0` with `x0 = 0x84000008ULL` (`PSCI_SYSTEM_OFF`).
     - Logs and returns `-1` if `smc #0` returns or an unsupported `type` is passed.

2. **`bsd/dev/sweet_spmi.c` & `bsd/dev/sweet_spmi.h`** (`#if defined(ARM64_BOARD_CONFIG_SWEET)`):
   - Caches `sp_pon_chan` (`sp_pon_chan_apid`) and `sp_imem` in `sp_pon_set_reason()` while preserving 0090's exact PON `SOFT_RB_SPARE` and IMEM write/readback/log semantics.
   - Adds `sweet_spmi_flush_next_boot(void)`: if `sweet_next_boot != 0` and `ml_get_interrupts_enabled()`, re-runs `sp_pon_set_reason((unsigned)sweet_next_boot)` to ensure ordering right before reset (skipped in IRQ-off panic context where `sysctl` already wrote PON/IMEM and `IOLockLock`/`ml_io_map` would be unsafe), followed by `dsb sy`.

3. **`osfmk/kern/debug.c`** (`#if defined(ARM64_BOARD_CONFIG_SWEET)`):
   - In `panic_init()`, checks optional boot-arg `sweetpanicreboot`; if non-zero, clears `panicDebugging = FALSE` so panics proceed to `kdp_machine_reboot_type(kPEPanicRestartCPU, ...)` even when `debug=0xc` (`DB_NMI`) is set in boot-args.

## Host Verification

- **Reconstructed tree:** Fetched `xnu-8796.141.3` originals from `apple-oss-distributions/xnu` via `gh api` (`pexpert/arm/pe_init.c`, `pexpert/pexpert/pexpert.h`, `pexpert/pexpert/arm/protos.h`, `pexpert/pexpert/arm64/board_config.h`, `iokit/Kernel/IOPlatformExpert.cpp`, `iokit/IOKit/IOPlatformExpert.h`, `osfmk/kern/debug.c`, `osfmk/kern/debug.h`, `osfmk/arm64/cpu.c`, `osfmk/arm64/machine_routines.c`, `bsd/kern/kern_shutdown.c`) and applied all hunks from `xnu/patches/0000..0117` touching those files in order with `git apply`.
- **Patch check:** `git apply --check xnu/patches/0118-op6-psci-reset.patch` passed cleanly on the reconstructed `0000..0117` tree; `git diff --check` clean; `xnu/patches/0118-op6-psci-reset.patch` and `op6/xnu-patches/0118-op6-psci-reset.patch` are byte-identical (`cmp -s`).
- **Syntax checks (with XNU stub headers, `clang`/`clang++` 23.1.4 `--target=arm64-apple-darwin -fsyntax-only -Wall -Wextra -Werror`):**
  - `bsd/dev/sweet_spmi.c` with `-DARM64_BOARD_CONFIG_SWEET` (`-Wno-unused-variable` for stubbed `SYSCTL_PROC`): 0 errors, 0 warnings.
  - `bsd/dev/sweet_spmi.c` without `-DARM64_BOARD_CONFIG_SWEET`: 0 errors, 0 warnings.
  - `iokit/Kernel/arm/SweetPlatformExpert.cpp` with `-DARM64_BOARD_CONFIG_SWEET` (`-std=c++17`): 0 errors, 0 warnings.
  - `iokit/Kernel/arm/SweetPlatformExpert.cpp` without `-DARM64_BOARD_CONFIG_SWEET` (`-std=c++17`): 0 errors, 0 warnings.
  - `panic_init()` snippet from `osfmk/kern/debug.c` with and without `-DARM64_BOARD_CONFIG_SWEET`: 0 errors, 0 warnings.

## Next Steps (for orchestrator / user)

1. Merge/cherry-pick `reboot-0118` to `enchilada`, push, and run XNU CI (`gh workflow run xnu.yml -R BJT1698/ios-s9 --ref enchilada`), verifying `0118-op6-psci-reset.patch` applies without skipping.
2. Device verification (with user confirmation):
   - Normal `reboot` over SSH -> SoC resets cleanly without hanging.
   - `sysctl -w kern.sweet_next_boot=2; reboot` over SSH -> SoC resets and check whether OnePlus ABL enters fastboot.
   - `halt` -> SoC powers off cleanly via PSCI `SYSTEM_OFF`.
   - Panic default (without `sweetpanicreboot=1`) keeps the panic screen; optional `sweetpanicreboot=1` resets on panic.
