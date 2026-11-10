# OnePlus 6 (enchilada) iOS port: handoff for the next agent

Last update: 2026-11-10. This file is meant to let another agent (Antigravity or similar) carry on the work without redoing anything.

Read these first, in this order: this file, `op6/STATUS.md` (what has been verified on the device), `op6/GPU-NOTES.md` (GPU device results), `op6/GPU-XNU-PLAN.md` (GPU driver architecture and milestones), `op6/XNU-PLAN.md`, `op6/USB-NOTES.md`, `op6/TOUCH-NOTES.md`.

## 1. What this is

- Apple's open-source XNU (xnu-8796.141.3), plus the **unmodified iPhone X (iPhone10,3) iOS 16.7.16 20H392** userland, booted bare-metal on a OnePlus 6 (SDM845, 8 GB, slot b, unlocked bootloader).
- Based on the Galaxy S9 port `syncmodz/ios-s9`, itself based on `dipa-studio/ios-sweet`.
- The workbench came from `Chromitsune/Portsmith-iOS`, cloned at `~/Progetti/Portsmith-iOS`.
- Repository: `~/Progetti/enchilada-work/ios-op6` = fork `github.com/BJT1698/ios-s9`, **branch `enchilada`**. All OP6 work lives in `op6/` and in `xnu/patches/0000`, `0102+`.

### Working on the device

- Boot: ABL → `fastboot boot <img>`, which is temporary and flashes nothing → our shim (`op6/shim`, stage m4) → XNU → iOS root on `userdata` (HFS+).
- Display at 1080x2280 (iOS draws 828x1792 @2x, scaled by the CPU).
- Multi-touch (Synaptics RMI4) and the volume/power keys.
- USB Ethernet: phone 192.168.7.2, PC 192.168.7.1 + NAT.
- `ssh -i ~/.ssh/ios-op6 root@192.168.7.2`; apt via Procursus.

### GPU (Adreno 630)

- **Bare-metal shim:** M10 GMU boot, M11 GPU SMMU + HFI, M12 GX on, M13 zap via TZ + CP `CP_MEM_WRITE` 0xC0FFEE01.
- **Inside XNU:** G1 (patch 0107, bring-up + power-off) and **G2** (patch 0108, `SweetGPUUserClient` msm DRM uAPI subset, BO carve-out allocator, `CP_INDIRECT_BUFFER` + fence completion, verified from iOS userland with `sweetgpu-test`).

### Not working

Wi-Fi, BT, cellular, audio, camera, sensors, real battery readings, RTC, GPU rendering of the UI.

## 2. Files outside git (do NOT commit, Apple/Qualcomm proprietary or personal)

| Path | What it is |
|---|---|
| `~/Progetti/enchilada-work/ipsw/` | IPSW + extracted `dmg/` (OS 087-86698-044, SystemOS 087-86593-047, AppOS 087-86715-047) |
| `ios-op6/s9/research/ipsw-x/d22ap.adt` | D22AP device tree (lzfse-decompressed), used by mkadt |
| `~/Progetti/enchilada-work/gpu-fw/` | `a630_gmu.bin` (32768), `a630_sqe.fw` (34188), `a630_zap.mbn` (14256, OnePlus-signed, from sdm845-mainline firmware-oneplus-sdm845) |
| `~/Progetti/enchilada-work/op6-backup/` | **calibration backup** (persist, modemst1/2, fsg, fsc, boot/dtbo/vbmeta a+b), read-only, SHA256SUMS. Never lose it. |
| `~/Progetti/enchilada-work/root/` | `ios16-root.img`/`.simg` (fresh root) and `userdata-16G-before-ssh.img` (migrated root before the SSH changes) |
| `~/Progetti/enchilada-work/op6-live.dts`, `op6-hardware.md`, `s9-inventory.md` | DT of the phone running pmOS + hardware notes |
| `~/.ssh/ios-op6` | SSH key for root on iOS |
| `ios-op6/op6/out/` | every build output (git-ignored), including the ADT with GPU firmware |
| `ios-op6/op6/shim/fw_a630_*.h` | headers generated from the firmware (git-ignored) |

## 3. Procedures

**XNU kernel** (CI only, needs macOS):

```
gh workflow run xnu.yml -R BJT1698/ios-s9 --ref enchilada
```

- `macos-14` is rejected for this account. The workflow uses `macos-26` (Xcode 26) plus patch `0000-xcode26-iig-iorpc-kernelcontent.patch`.
- Then `gh run download <ID> -R BJT1698/ios-s9 -n xnu-kernel -D xnu/out-XXX` and check that the log has no `Skipping patch .../01xx`. The 4 skipped dxb patches are normal.
- macOS arm64 capacity is sometimes exhausted and the run gets cancelled after 15 minutes: relaunch it.

**ADT, payload and boot image:**

```
python3 op6/tools/mkadt-op6.py op6/out/X.adt --scale2 [--gpu-fw ../gpu-fw]
python3 s9/tools/mkpayload.py xnu/out-XXX/kernel.development.sweet op6/out/X.adt op6/out/payload-X.bin --cmdline "$ARGS"
PAYLOAD=$PWD/op6/out/payload-X.bin bash op6/build.sh m4 && cp op6/out/boot.img op6/out/boot-X.img
```

- **Gotcha:** `build.sh` copies every build to `out/boot-<stage>.img`. Rename variants only after the last build. This once caused three tests to run on the wrong image.
- The shim is built with clang/lld (`-fno-jump-tables`). No ABS64 relocations are allowed in C objects (no static arrays of pointers).

**Current boot-args** (milestone D, plus GPU):

```
debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=ufsroot0 ufsroot=userdata ufsrw=1 rootrw=1 ufswait=1 ufslogsecs=60 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 cs_enforcement_disable=1 sweetfb_noconsole=1 sweetfb_hz=60 sweetcpu=0 sweetbattery=0 sweetusb=2 sweettouch=1 sweettouchdelay=20 sweetgpu=1 sweetgpudelay=30
```

`sweetcpu=0` and `sweetbattery=0` are **required**: the S9 OSM clocks and the MAX77705 do not exist on the OP6.

**Ready images** (in `op6/out/`):

| Image | What it boots |
|---|---|
| `boot-D.img` | iOS + USB + SSH, no GPU |
| `boot-G1.img` | as boot-D, plus the GPU test at boot (`sweetgpu=1`, shuts GPU down after test) |
| `boot-G2.img` | as boot-D, plus `SweetGPUUserClient` (`sweetgpu=2`, leaves GPU on for userland msm uAPI) |
| `pmos-debugshell.img` | pmOS kernel with a debug shell |
| `boot-m5/m7-smmu/m10-gmu/m11-hfi/m12-gx/m13-zap/m13-cp.img` | bare-metal shim stages |

**Phone into fastboot:**

- The user does it: hold power for 10 s, then volume up + power.
- `sudo` on the phone or in pmOS needs a TTY, so the user runs it in their own terminal (the agent's `!` has no TTY).
- **Qualcomm CrashDump** (PC sees `05c6:900e`) means a secure fault, usually a DMA with no SMMU rule. It is harmless: hold power for 15+ s.
- If a `fastboot boot` transfer is cut short, ABL stops answering and replugging USB does not help: power-cycle.

**Kernel log without SSH:**

1. `fastboot boot op6/out/pmos-debugshell.img`
2. `python3 op6/tools/op6-log-pull.py ../logs/X.bin` (telnet 172.16.42.1:23; sometimes `busybox-extras httpd -p 8080 -h /tmp` has to be restarted by hand)
3. `python3 s9/tools/sweet-log.py ../logs/X.bin ../logs/X.txt`

The same debug shell can export `userdata` to the PC as a USB disk: `. /init_functions.sh; setup_usb_storage_configfs /dev/sda17`. That is how `op6/tools/op6-prep-usb-ssh.sh dev /dev/sdX` was run (sudo, by the user) to edit the iOS root.

**With SSH:** `dmesg` (the buffer rotates quickly, flooded by backboardd's IOK lines) and `ioreg -l -c SweetGPU`.

## 4. Verified facts that are not obvious (details in the notes)

- **USB:**
  - ABL leaves USB running after `fastboot boot`, but with the QUSB2 PHY off.
  - Apps SMMU: on this hypervisor `S2CR BYPASS` becomes FAULT. The rule for SID 0x740 must be a free CB with S1-translate/S2-bypass, `SCTLR.M=0`, and `S2CR` TRANSLATE (the U-Boot qcom-hyp-smmu recipe).
  - Without a rule, the first DMA sends the phone into CrashDump.
- **Touch:**
  - ABL leaves it unpowered. Bring-up order: GPIO88 high, RPMh vote `ldoa28` 3008 mV, reset on GPIO99.
  - Bus: bit-banged I2C on GPIO49/50, address 0x20, IRQ GPIO125.
  - The shim m4 does this before XNU.
- **RAM:**
  - Free from 0xa0000000 to 0x27c000000 (M8 scan).
  - XNU span 0xae000000–0x180000000 for now.
  - Holes: rmtfs 0xf5b00000; ramoops 0xac300000 (the shim's flight recorder).
- **GPU:**
  - OP6 cmd-db: `gfx.lvl` = 0x30050. 0x30080 is `xo.lvl`, which is still correct in the GPU PDC sequence (that is the XO vote).
  - TZ answers `pas_is_supported(13)` with 0, but init/mem_setup/auth then succeed: do not gate on it.
  - GPU SMMU: 5 SMR / 5 CB, 4K pages, ATOS. GMU SIDs 5 and 4, GPU SID 0.
  - GMU firmware v1.6 (0x10060000), SQE 0x207.
  - The CP always-on counter does count on the OP6.
- **First iOS boot** on a new root takes about 30 minutes on the Apple logo (data migration). Later boots take a few minutes.

## 5. Status of GPU G2 (verified on device 2026-11-10)

- **G2 verified on device (`boot-G2.img`, CI run `42847871854`, `sweetgpu=2 sweetgpudelay=30`)**:
  - **Patch `xnu/patches/0108-op6-gpu-userclient.patch`** (and copy in `op6/xnu-patches/0108-op6-gpu-userclient.patch`):
    - `bsd/dev/sweet_gpu.c`: 4K-page bitmap allocator over the remaining GPU carve-out (16K-aligned for XNU's 16K VM device pager, mapped with 4K PTEs into the GPU SMMU at `0x10000000 + p*4K`), 64-page GPU page-table pool, `pt_unmap` + `CB_TLBIALL`/`CB_TLBSYNC`, circular ring wrap-around, `CP_INDIRECT_BUFFER` + `CP_WAIT_FOR_IDLE` + `CP_EVENT_WRITE(CACHE_FLUSH_TS)` fence submission, and `msm` uAPI parameter reporting when `sweetgpu=2`.
    - `iokit/Kernel/arm/SweetGPU.cpp`: fixes `SweetGPUMagic` (and sibling numeric properties) `OSNumber` bit-width from 32 to 64 so `ioreg` displays `3237998081` (`0xC0FFEE01`) without sign-extension; sets `IOUserClientClass = "SweetGPUUserClient"`; implements `SweetGPUUserClient` (selectors `0x09` `GEM_CLOSE`, `0x40` `GET_PARAM`, `0x41` `SET_PARAM`, `0x42` `GEM_NEW`, `0x43` `GEM_INFO`, `0x44` `GEM_CPU_PREP`, `0x45` `GEM_CPU_FINI`, `0x46` `GEM_SUBMIT`, `0x47` `WAIT_FENCE`, `0x48` `GEM_MADVISE`, `0x4a`..`0x4c` `SUBMITQUEUE_*`, plus `clientMemoryForType` with write-combine physical `IOMemoryDescriptor`).
  - **iOS test tool `s9/ios-tools/sweetgpu-test.c`** (installed on device at `/var/root/sweetgpu-test`):
    - Verified `GET_PARAM` (`GPU_ID=630`, `GMEM_SIZE=0x100000`, `CHIP_ID=0x6030002`, `MAX_FREQ=710000000`, `TIMESTAMP` advancing at 19.2 MHz, `FAULTS=0`, `HIGHEST_BANK_BIT=15`).
    - Verified `GEM_NEW` + `GEM_INFO(GET_IOVA)` + `IOConnectMapMemory64` (`cmd BO` at `iova=0x10000000`, `dst BO` at `iova=0x10004000`, `map_size=0x4000`).
    - Verified single IB submit (`CP_NOP` + `CP_MEM_WRITE 0xC0FFEE02`) + `WAIT_FENCE`: `fence=1`, `69 us`, `dst[0]=0xc0ffee02`.
    - Verified 100-submit loop (`fences=2..101`, `53 us/submit`) and 1000-submit ring-wrap loop (`fences=103..1102`, `51 us/submit`, `FAULTS=0`), plus BO/PTE/TLB cleanup on `clientClose`.

## 6. What comes next (GPU-XNU-PLAN.md)

- **G3:** Mesa gallium freedreno (it supports only the msm uAPI, not KGSL) cross-built for arm64-iOS on top of a fake libdrm that talks to the SweetGPU user client. HackiOS has an MIT `hakios.drm.c` you can start from. First test: GLES clear + readback over SSH.
- **G4:** EAGL bridge inside backboardd, so that CoreAnimation renders through Mesa. backboardd looks up `IOAcceleratorES`, `IOAcceleratorESLegacy` and `AGXAccelerator`. Read s9/ESTUDO-2026-10-25-gpu.md section 2.
- **G5:** GPU IRQs (SPI 300 / INTID 332, GMU 336/337, SMMU 396-403; the SWEET GIC code only handles the timer and SGIs), slumber, hang recovery.
- **G6:** Metal (Chlorophyll is MIT; Turnip + metal2vulkan as in flame).
- **Smaller jobs:**
  - trim the daemons that loop (bluetoothd, fairplayd: s9/tools/trim-daemons.py);
  - wallpaper (S9Wallpaper is already in jb/);
  - CPU clocks (sweetcpu, OSM) after checking the OP6 values;
  - all 8 GB (`--mem-top 0x27c000000` in mkadt-op6);
  - bq27411 fuel gauge.

## 7. Rules

- **Licences:**
  - Bergamota (`ry-diffusion/Bergamota`), Guacamole and Linux (GPL): **facts only, never code**.
  - Reusable with attribution: HackiOS kernel `drivers/` (APSL 2.0) and the rest of it (MIT), Chlorophyll (MIT), Mesa uAPI headers (MIT).
  - The fork is public.
  - A licence-request e-mail to Bergamota's author is drafted at `~/Progetti/enchilada-work/bergamota-license-request.md`; the user sends it.
- **Never commit** firmware, the IPSW, the ADT with firmware, backups, keys or device dumps.
- **Device:** every flash, reboot or boot is confirmed by the user. `fastboot boot` writes nothing. A `fastboot flash userdata` erases the migrated iOS root, so always restore from the backups in `root/`.
- **Shim stages:** each step goes on screen and to the ramlog *before* it runs; check writes by reading them back; refuse on mismatch.
- **Reporting:** label every result as verified-on-device / inferred, and add it with the date to STATUS.md and GPU-NOTES.md.
- **Commits** end with `Co-Authored-By:` for the agent that wrote them.
