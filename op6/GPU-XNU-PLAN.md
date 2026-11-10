# OnePlus 6 GPU in XNU (Adreno 630): architecture and milestones

Status 2026-11-10: **G1 verified on device** (`xnu/patches/0107-op6-gpu-bringup.patch`, `SweetGPUStage=7`,
`SweetGPUMagic=0xC0FFEE01`, `SweetGPUState="off"`). **G2 patch and userland test tool written and locally
verified** (`xnu/patches/0108-op6-gpu-userclient.patch`, copy in `op6/xnu-patches/`, and
`s9/ios-tools/sweetgpu-test.c`). Patch `0108` passes `git apply --check` on xnu-8796.141.3 + 0000-0107 and
`clang -fsyntax-only` under XNU's `-Weverything -Werror` flags; `sweetgpu-test.c` passes `clang -Wall
-Wextra -Werror`. Ready for CI build (`xnu.yml` + `s9-ios-tools.yml` on `macos-26`) and device test with
`sweetgpu=2`.

Labels: **[V]** read in source/DT or verified on the device, **[I]** inferred, **[?]** open.

## 1. What the other ports did (survey 2026-11-10)

| port | kernel driver | firmware to the kernel | user interface | renderer | licence |
|---|---|---|---|---|---|
| **HackiOS** (oosexgang/hackios-xnu-kernel @63fc2b4, POCO X5 Pro, SC7280/A642L) | `drivers/gpu/hakios.adreno.c` (power-up in 22 steps, with own 4K-in-16K table allocator `space_new_table`/`space_map`, VA free list `hakios_gpu_address`) + `hakios.gpu.cpp` (`HakiosGPU` IOService created by hand, boot-arg `gpu=1`) | files from the RAM-disk image (`hakios_image_file("fw/...")`) | **msm DRM uAPI over IOUserClient**, selector = ioctl number low byte (0x40 GET_PARAM ... 0x47 WAIT_FENCE, 0x4a-c SUBMITQUEUE, + 0x7d userptr, 0x7e IOSurface, 0x7f show); fake libdrm `tools/mesa/hakios-drm/hakios.drm.c` (MIT) | freedreno GLES (EAGL replacement in backboardd, `userland/src/eagl.c`) and Turnip | `/LICENSE`: **APSL 2.0 for `drivers/`, `xnu/`** (the GPU driver; no per-file headers, no Linux code copied), MIT for userland/tools. `hackios-xnu-userland` @2573c4a: README only (moved into the kernel repo) [V] |
| **guacamole** (Portsmith, OnePlus 7 Pro, A640) | `bsd/dev/sweet_gpu.c` (stages, `sweetgpu=N`, 60 s after boot) + `iokit/Kernel/arm/RedwoodGPU.cpp` (IOService published by hand after stage 10) | **ADT node `/sweet-gpu`** (`tools/mkadt.py --guacamole`), never committed | msm uAPI over IOUserClient (from topaz/HackiOS), fake libdrm `redwood_drm.c` (`drmOpenWithType("msm")` -> IOServiceOpen, `/dev/null` fd) | Mesa 25.0.7 freedreno + OSMesa; `eagl.c` replaces 7 EAGLContext methods; **iOS 16 UI on the GPU at 60 Hz** | **no top-level LICENSE**: facts only; contains APSL/MIT pieces from HackiOS/topaz [V] |
| **flame** (Portsmith, Pixel 4, A640) | `bsd/dev/sweet_a6xx.c` + `SweetAccelerator : IOAcceleratorES` (IOResources personality, `sweetmtl`) | ADT `/chosen` props (`mkpayload.py --gpufw`) | own NativeAdreno ABI (neither msm nor KGSL), Turnip `tu_knl_darwin.cc` | Metal via Turnip + metal2vulkan (GPL/LGPL) | MIT (dipa-studio) + APSL patches; CREDITS.md exceptions [V] |
| **Chlorophyll** (ry-diffusion @787338f, Poco F1 A630, iOS 18) | none (Bergamota's kernel, no licence) | - | libdrm stand-in over Bergamota's msm-style kernel interface | Metal API over Gallium freedreno (`src/bgmtl.m`, AIR->NIR `air/`) | MIT (`LICENSE.hackios-userland`, (c) OOSEX GANG) [V] |

Common to all three working ports: **polled completion, no GPU IRQ**; one shared GPU address space; their own
SMMU tables with 4K PTEs; an IOService plus an IOUserClient. Our tree today has no accelerator class at
all: CoreAnimation renders in software into `AppleCLCD`'s IOSurface (0046/0068/0096).

How iOS 16 picks the renderer (`s9/ESTUDO-2026-10-25-gpu.md` sec. 2, QuartzCore 20H392): it tries
`CAMetalContextCreate`, and with no Metal device it falls back to `[EAGLContext initWithAPI:3]` (then 2)
plus `CA::OGL::GLESContext`. `CA_ENABLE_OGL` defaults to 1. The system GLES is layered on Metal, so it is
useless without Metal. **backboardd only needs an EAGLContext of ours, backed by Mesa.** No vtable hook is
needed on iOS 16.

## 2. Architecture

### 2.1 Firmware delivery: ADT node `/sweet-gpu` (chosen)

`op6/tools/mkadt-op6.py --gpu-fw ../gpu-fw [--gpu-dma PA,SIZE]` adds the node `/sweet-gpu` with these
properties:

- `gmu-firmware`, `sqe-firmware`, `zap-firmware`: whole files, each sha256-checked against sec. 4 of
  `GPU-NOTES.md`.
- `dma-region` = `<u64 pa, u64 size>`.

The ADT goes into the payload (`op6/out/`, git-ignored), so git never sees the firmware. Why this mechanism:

- It is the leanest fit with our flow. mkadt and mkpayload already run locally, the CI only builds the
  kernel, and the loader copies the ADT behind the kernel at any size.
- The kernel gets the firmware without a filesystem (SecureDT), so bring-up works before or without the
  root.
- guacamole proved the same approach on device.
- The DT memory stays mapped: mkpayload adds no `/chosen/memory-map DeviceTree`, so `IODeviceTreeAlloc`
  does not free it, and the props are `OSData::withBytesNoCopy` [V `IODeviceTreeSupport.cpp`].
- The cost is about 80 KB in the ADT, also visible in `ioreg -l` (harmless, and not in git).

Rejected alternatives:

- Root-FS file plus userspace helper: needs a user client before the first test, a late start, and a
  launchd job.
- A vnode read in the kernel: possible after mount, but more code and failure modes.
- Embedding in the kernel image: firmware in a CI artifact, which is not allowed.

### 2.2 Memory: a GPU carve-out outside the XNU span (chosen for G1/G2)

The default `dma-region` is `0xa0000000 + 16 MiB`. It lies in the OP6 RAM scan's free
`[0xa0000000,0xac000000)`, below dram-base `0xae000000`, and has no DT reserved-memory entry [V
op6-live.dts]. The kernel maps it with `ml_io_map_unmappable(pa, size, VM_WIMG_WCOMB)`, which gives Normal non-cacheable memory from `kernel_map`. Not `ml_io_map_wcomb`: that allocates from the 8 MiB `io_submap` with `KMA_NOFAIL` (`osfmk/arm/io_map.c:65,120`), so 16 MiB or more would panic. HFI reply reads use `dmb ld` between the index and the payload, because Normal-NC loads may be reordered.

- This is exactly the "no cache in the way" view the shim had with the MMU off.
- XNU has no cached alias of this RAM: it is outside the physmap.
- No cache maintenance is needed.
- It avoids `pmap_sync_page_attributes_phys`, which truncates PAs above 4 GiB (`pp << PAGE_SHIFT` on a
  32-bit `ppnum_t`, `pmap.c:6967`; guacamole hit this, patch 0120). Our span goes to 0x180000000.

mkadt refuses a region that overlaps the span or leaves the free window.

- **GPU pages vs XNU pages:** the GPU SMMU uses its own io-pgtable (stage 1, 4K granule, 39-bit,
  TTBR0 only). One PTE is written per 4K GPU page, and table pages are 4K carve-out pieces. XNU's 16K pages
  never enter the GPU tables in G1. [V shim M11/M13]
- **G2+:** a 4K-granular allocator over the carve-out, for GEM objects (sized up as needed, 64-192 MiB;
  the window allows up to 192 MiB). Userspace maps it write-combined, through `clientMemoryForType` with
  an `IOMemoryDescriptor::withAddressRange(pa, ...)` of the carve-out. Cached GEM (kmem pages with
  `flush_dcache64` by PA, as `sweet_ufs` does) only if write-combine is too slow for CPU reads (G3
  measurement).
- Per-process GPU address spaces: not before G4. One shared space, as all three ports do.

### 2.3 Power-up: the shim M10-M13 port, in a late kernel thread

`SweetGPU` is an IOService with an IOResources personality and IOMatchCategory, like SweetBattery. It
starts only if `/chosen/sweet-board == "oneplus,enchilada"` and boot-arg `sweetgpu=1|2`. Its thread is
bound to cpu 0, so the TZ SCM INTERRUPTED/resume loop stays on one core. The thread waits `sweetgpudelay`
seconds (default 30), so that the persistent log and USB networking are already up if anything hangs. It
then runs `bsd/dev/sweet_gpu.c`:

1. cmd-db `cx/mx/gfx.lvl` give the perf votes. CX GDSC on, GMU clocks at 200 MHz.
2. GPU SMMU: the GPU gets the first free context bank, the GMU the next one. GMU tables (dummy, debug,
   log, HFI at 0x60000000), SID 5 and 4, `CLIENTPD` cleared, ATS1PR check.
3. RSC/PDC micro-code (third TCS command 0x30080, as M12/M13 ran), `a630_gmu.bin` into ITCM,
   `GMU_RPMH_CTRL=0xF11`. The GMU must answer `0xBABEFACE`.
4. GX at 710 MHz via OOB BOOT_SLUMBER, SPTPRAC; HFI v1 (INIT, FW_VERSION, PERF, BW, TEST); DCVS; OOB
   GPU_SET.
5. Zap via PAS 13 (init_image, mem_setup, copy to 0x8c415000, auth_and_reset). `is_supported` is only
   logged.
6. SQE, GPU context bank (SID 0, HUPCF, USFCFG), hw_init a630 subset, 32K ring, ME_INIT,
   SET_SECURE_MODE 0, NOP + CP_MEM_WRITE `0xC0FFEE01`, then read back.
7. The rest depends on the boot-arg:
   - `sweetgpu=1`: the shim's verified shutdown (VBIF halt, slumber 0xF, GX off, CM3 reset).
   - `sweetgpu=2`: the GPU is left on (GX held by GPU_SET), for G2.

What changes from the shim:

- MMIO goes through `ml_io_map`, with every access a single `ldr`/`str` (inline asm, so the
  hypervisor-trap rule holds even though the GPU SMMU proved directly programmable).
- `delay()` and `mach_absolute_time` replace the shim's timers. Messages go to `printf`; there is no
  screen or watchdog.

### 2.4 Ring, fences, interrupts

- **G1-G3: polled.** A fence is a `CP_EVENT_WRITE` / `CP_MEM_WRITE` of a seqno into the scratch page.
  `WAIT_FENCE` polls it with `IODelay`, or `IOSleep(1)` after 100 us. This is the guacamole/HackiOS
  model.
- **Interrupt numbers (OP6 DT [V]):**

  | source | DT SPI | GIC INTID |
  |---|---|---|
  | `gpu@5000000` | 0x12c | 332 |
  | `gmu@506a000` HFI | 0x130 | 336 |
  | `gmu@506a000` GMU | 0x131 | 337 |
  | GPU SMMU global | 0xe5, 0xe7 | 261, 263 |
  | GPU SMMU contexts | 0x16c-0x173 | 396-403 |

- The SWEET kernel's GICv3 code (`pe_fiq.c`/`sleh.c`) only delivers the timer and SGIs. GPU IRQs need a
  small SPI-dispatch patch (enable the SPI in GICD, route it to cpu 0, call a registered handler from
  `sleh_irq`). That is milestone G5, an optimisation only.
- **Single ring for now:** 32K, `RB_CNTL` NO_UPDATE plus the CP_WHERE_AM_I shadow.
  - User submits are `CP_INDIRECT_BUFFER` packets that point at the user command BOs.
  - The kernel adds the fence write.
  - A ring that does not drain within 2 s is reported as a hang; recovery is not planned before G4.

### 2.5 User interface: the msm DRM uAPI subset over an IOUserClient (chosen)

Mesa's gallium freedreno has only two kernel backends, `msm` and `virtio` (`src/freedreno/drm/`, Mesa main
[V 2026-11-10]). KGSL exists only in Turnip (`tu_knl_kgsl.cc`), next to `tu_knl_drm_msm.cc`. So KGSL would
not give us GLES, and **the msm uAPI covers both freedreno (GLES, G3/G4) and Turnip (later Vulkan/Metal)**.
It is also what HackiOS and guacamole run on device.

`SweetGPUUserClient` (G2):

- `externalMethod(selector = msm ioctl nr low byte, struct in/out)` with:
  - 0x40 GET_PARAM (GPU_ID 630, CHIP_ID 0x06030002, GMEM_SIZE 1 MiB, GMEM_BASE 0x100000, VA_START /
    VA_SIZE, TIMESTAMP via CP_ALWAYS_ON, NR_RINGS 1, PRIORITIES 1, FAULTS)
  - 0x42 GEM_NEW
  - 0x43 GEM_INFO (IOVA, OFFSET = mmap cookie, SET_NAME)
  - 0x44 / 0x45 CPU_PREP / CPU_FINI (wait on the BO's last fence)
  - 0x46 GEM_SUBMIT (cmds -> IBs, bos list, fence out; softpin IOVAs)
  - 0x47 WAIT_FENCE
  - 0x48 MADVISE (stub)
  - 0x4a-0x4c SUBMITQUEUE_NEW / CLOSE / QUERY (one queue)
  - 0x09 GEM_CLOSE
  - 0x7e (ours): IOSurface ID -> BO, for G4 zero-copy
- `clientMemoryForType(type = handle)` maps a BO into the process.
- Userland: a fake `libdrm` (`drmOpenWithType("msm")` -> `IOServiceOpen("SweetGPU")`, `drmIoctl` ->
  `IOConnectCallStructMethod`, `drmGetVersion` = msm 1.7+ as guacamole reports, softpin only; exact minimum to check in G3 against freedreno_device.c). It is written
  by us; the HackiOS one (MIT) is a permitted reference with attribution.

### 2.6 EAGL bridge (G4)

A dylib loaded into backboardd (env/insert as `S9CAEnv` does) that implements `EAGLContext` over Mesa
freedreno:

- `initWithAPI:`, `renderbufferStorage:fromDrawable:`, `presentRenderbuffer:`, `texImageIOSurface`, and
  the function table CA reads (`getMacroContextPrivate`; slot offsets checked against our 20H392 dyld
  cache).
- Rendering targets IOSurfaces imported with selector 0x7e. Present stays `AppleCLCD`'s swap path
  (0096).
- Our own implementation. HackiOS/guacamole `eagl.c` are references only (guacamole has no licence).

## 3. Milestones (each tested over SSH, one `fastboot boot` each)

| | goal | test / pass criterion |
|---|---|---|
| **G1** | the kernel brings the GPU up and its CP writes `0xC0FFEE01` (patch 0107) | log `SWEET GPU: G1 OK: THE CP WROTE 0xc0ffee01 ...`; `ioreg -l -c SweetGPU` shows `SweetGPUStage=7`, `SweetGPUMagic=0xc0ffee01`, `SweetGPUState="off"`; iOS keeps running (SSH, touch) |
| G1b | same with `sweetgpu=2` (GPU left on), idle for 10 min | no hang, no SMMU fault; then reboot |
| G2 | `SweetGPUUserClient` (msm subset) + `op6-gputest` CLI: GET_PARAM, GEM_NEW, mmap, SUBMIT of an IB with `CP_MEM_WRITE`, WAIT_FENCE | the tool prints the value the GPU wrote into its BO, and fence 1, 2, ... in sequence |
| G3 | Mesa freedreno (a630) cross-built for iOS arm64 + fake libdrm; `glClear` + `glReadPixels` (EGL-less/OSMesa-like harness or `fd` pipe test), then a triangle | pixels read back equal to the clear color / reference PNG over SSH |
| G4 | EAGL bridge in backboardd; CoreAnimation on the GPU into IOSurfaces | SpringBoard draws, CA's renderer log says GLES, fps > CPU path |
| G5 | GPU/GMU IRQs (SPI dispatch), DCVS/GMU idle (slumber when idle), hang recovery | lower CPU use while waiting; GPU idles to slumber |
| G6 | Metal (Chlorophyll MIT front end over freedreno, or Turnip) | later |

## 4. G1 test plan

1. Kernel: CI build with patches through 0107 (`gh workflow run xnu.yml`, or a local compile through
   `tools/xnu-cc.py`; the user triggers CI).
2. ADT and payload (local; the firmware stays in `op6/out/`):

   ```
   python3 op6/tools/mkadt-op6.py op6/out/op6-gpu.adt --scale2 --gpu-fw ../gpu-fw
   python3 s9/tools/mkpayload.py $KERNEL op6/out/op6-gpu.adt op6/out/payload-gpu.bin --cmdline "<usual op6 args> sweetgpu=1 sweetgpudelay=30"
   PAYLOAD=op6/out/payload-gpu.bin bash op6/build.sh m4   # -> fastboot boot
   ```

3. After the boot, over SSH: read the log with `op6/tools/op6-log-pull.py` or `dmesg | grep 'SWEET GPU'`,
   then run `ioreg -l -c SweetGPU | grep SweetGPU`.
4. Expected lines, in order:
   - `SWEET GPU: thread on cpu 0, starting in 30 s`
   - `firmware from ADT: gmu 32768, sqe 34188, zap 14256 bytes; carve-out a0000000+1000000`
   - `cmd-db cx.lvl @30000 ..., gfx.lvl @30050 n=10`
   - `votes gx ...`, `CX gdscr=...`
   - `2 SMMU idr0=48016605 ... smr=5 cb=5 pg=4K n=8`
   - `-> gpu cb0 gmu cb1`
   - `sid 5 smrN=80000005 ...`, `ATS 0x60009000 -> par=...a0...`
   - `GMU init=babeface ... rpmh_ctrl=00000f11`
   - `OOB BOOT ack ...`, `SPTPRAC st=28 on`
   - `HFI INIT/FW_VERSION/PERF_TABLE/BW_TABLE/TEST: ACK ok`, `GMU fw version 10060000`
   - `OOB GPU_SET ack`, `RBBM st=00000001`
   - `ZAP n=3 hash=1 load=1 rel size=1000/2000`
   - `SCM init_image ... ok`, `mem_setup ok`, `ZAP copied 7b0 bytes ... wrong=0`, `auth_and_reset ok`,
     `ZAP accepted by TrustZone (PAS 13)`
   - `SQE ... wrong=0`, `ATS ring ... sqe+8000 ...`, `hw_init writes=41 differ=2`
   - `ME_INIT rptr=12 shadow=12 wptr=12 ... ok`, `SECURE0 rptr=17 ...`, `MEMWRITE rptr=26 ...`
   - `MEM value=c0ffee01`, **`G1 OK: THE CP WROTE 0xc0ffee01 THROUGH THE GPU SMMU`**
   - `7 shutdown ...`, `slumber ok (0xf)`, `G1 done: GMU in reset, GX off`
   - `service registered (stage 7, magic 0xc0ffee01, GPU off)`
5. Failure readings are those of `GPU-NOTES.md` sec. 5/8. The stage number in `G1 FAILED at stage N`
   and the last line printed say where it stopped; the shutdown always runs once the GMU booted.

## 5. Risks and open points

- **Carve-out RAM [I]:** "free" comes from the shim scan and the DT. If ABL or a firmware ever touches
  0xa0000000+, GPU memory gets corrupted. That would show as a fault or a bad readback, not a crash.
  `--gpu-dma` moves it. The shim used 0x800xxxxx.
- **Zap once per boot:** TZ keeps the zap until reboot. A second G1 run in the same boot would refuse
  `init_image`; G1 runs once. G2+ will keep a "zap loaded" flag.
- **SCM from XNU:** same EL1 as the shim. The thread is bound to cpu 0, and preemption stays on during
  TZ calls (INTERRUPTED/resume loop) [I].
- **GX at 710 MHz** with `sweetgpu=2` stays on until reboot: thermal and battery cost. G5 brings slumber.
- **Not done:** the CPU `gfx-mem` interconnect vote (DDR busy anyway) and LLCC. Hang recovery and IRQs
  are G5.
- **`ioreg -l` dump size:** the firmware props are in the IODeviceTree plane (~80 KB).
- **Build:** the stub syntax check is not the CI compiler. Possible CI-only errors: missing prototypes
  from real headers, `delay`/`absolutetime_to_nanoseconds` visibility in BSD (both in `kern/clock.h`,
  which `sweet_touch.c` already includes).

- **Review (Opus, 2026-11-10, against the shim and the xnu tree):** fixed the io_submap panic (above), the
  HFI read ordering, the GMU fw version property (it was overwritten by later acks), a `meta_pa`
  maybe-uninitialized warning (an error under xnu's -Werror), the SMC `cc` clobber, zap ELF bounds checks and
  the `sweetgpudelay` cap. It also restored the shim's ~100 ms always-on counter step between GPU_SET and the
  zap. Otherwise the reviewer found the sequence identical to m13. Left out on purpose: the read-only RSC
  probes and `smmu_after_zap`.

## 6. Files (G1 + G2)

- `xnu/patches/0107-op6-gpu-bringup.patch` (+ copy `op6/xnu-patches/`):
  - new `bsd/dev/sweet_gpu.c` (hardware sequence) and `iokit/Kernel/arm/SweetGPU.cpp` (service + thread
    + result properties)
  - `bsd/conf/files`, `iokit/conf/files.arm64`, `iokit/KernelConfigTables.cpp` (SweetGPU personality)
- `xnu/patches/0108-op6-gpu-userclient.patch` (+ copy `op6/xnu-patches/`):
  - `bsd/dev/sweet_gpu.c`: 4K-page bitmap allocator over the remaining carve-out (16K-aligned for XNU's 16K
    VM device pager, mapped with 4K PTEs into the GPU SMMU at `0x10000000 + p*4K`), 64-page GPU page-table
    pool, `pt_unmap` + `CB_TLBIALL`/`CB_TLBSYNC`, circular ring wrap-around (`wp = (wp + 1) % RING_DW`),
    `CP_INDIRECT_BUFFER` + `CP_WAIT_FOR_IDLE` + `CP_EVENT_WRITE(CACHE_FLUSH_TS)` fence submission, and `msm`
    parameter reporting.
  - `iokit/Kernel/arm/SweetGPU.cpp`: fixes `OSNumber` bit-width (64 instead of 32) on `SweetGPUMagic` and
    sibling properties, sets `kIOUserClientClassKey = "SweetGPUUserClient"`, and implements
    `SweetGPUUserClient` (selectors `0x09` `GEM_CLOSE`, `0x40` `GET_PARAM`, `0x41` `SET_PARAM`, `0x42`
    `GEM_NEW`, `0x43` `GEM_INFO`, `0x44` `GEM_CPU_PREP`, `0x45` `GEM_CPU_FINI`, `0x46` `GEM_SUBMIT`,
    `0x47` `WAIT_FENCE`, `0x48` `GEM_MADVISE`, `0x4a`..`0x4c` `SUBMITQUEUE_*`, plus `clientMemoryForType`
    with `kIOMapWriteCombineCache`).
- `s9/ios-tools/sweetgpu-test.c` + `.github/workflows/s9-ios-tools.yml` (moved to `macos-26`):
  - opens `SweetGPU`, prints registry properties and `GET_PARAM` values, allocates command + destination BOs,
    maps them via `IOConnectMapMemory64`, submits an indirect buffer with `CP_NOP` + `CP_MEM_WRITE 0xC0FFEE02`,
    waits via `WAIT_FENCE`, verifies readback, and runs a 100-submit stress loop (`-n COUNT`).
- `op6/tools/mkadt-op6.py`: `--gpu-fw DIR`, `--gpu-dma PA,SIZE` (`/sweet-gpu` node; sha256 and overlap
  checks).
- Origin of the code: our shim (`op6/shim/gpu.c`, `m10.c`-`m13.c`, `rpmh.c`; ios-op6, MIT); G2 uAPI
  dispatch and ring submission reference `oosexgang/hackios-xnu-kernel` @ `63fc2b4` (`drivers/gpu/hakios.adreno.c`
  and `drivers/gpu/hakios.gpu.cpp`, APSL 2.0) and Mesa's `drm-uapi/msm_drm.h` (MIT). No code is copied from
  guacamole (no licence), Linux (GPL) or Bergamota (no licence).
