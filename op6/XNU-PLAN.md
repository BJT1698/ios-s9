# OnePlus 6 (enchilada): XNU bring-up plan

Scope: get the ios-s9 kernel (patches 0001-0101) plus iPhone X iOS 16.7.16 userland to boot on
the OnePlus 6 with one kernel shared with the Galaxy S9. The board is selected by data the
shim hands over (boot_args, ADT), not by `#ifdef`s or forked patches.

Tags: **[V]** verified in this work (on a reconstructed tree or by running a tool), **[HW]** an
OP6 hardware fact you gave me (shim runs), **[I]** inferred from code or DT, **[G]** a guess
that needs a test.

## 0. How this was checked

- Reconstructed the CI tree locally: `apple-oss-distributions/xnu` tag `xnu-8796.141.3` (what
  dxb's `macos-135/release.json` names for `MACOS_VERSION=13.5`), plus the three `xnu/patches/*.sh`
  staging scripts, then every `*.patch` in glob order with `git apply --check`/`git apply`, the
  same loop as dxb `build.sh` `patches()` at pinned `DXB_SHA`. All 0001-0101 applied. Of dxb's own
  patches, 4 are skipped, the same as CI. **[V]**
- `op6/xnu-patches/0102-*.patch` and `0103-*.patch` were made with `git diff` on that tree. They
  pass `git apply --check` (what CI runs) and `patch -p1 --dry-run` on the 0101 state, applied
  in order. No dxb patch touches the same files, so it does not matter that dxb's patches
  sort after them. **[V]**
- Compile checks: the new `sweet_early.c` is syntax-checked (clang `-Wall -Wextra`). It used a
  stub header that holds the real `boot.h` `Boot_Video`/`boot_args`, so the three
  `_Static_assert` offset checks were evaluated. The new `pmap.c` code (holes parser +
  `pmap_is_bad_ram`) is syntax-checked against stubs. The new `start.s` sequence assembles
  with clang for `arm64-apple-ios16.0`, and its disassembly shows the offsets
  #0x28/#0x38/#0x48. **[V]** I did NOT run a full xnu build. That needs CI.
- **CI caveat:** dxb *skips* a patch that fails `git apply --check` and only prints a warning,
  so the build still succeeds. After the first CI run with 0102/0103, grep the log for
  `Skipping patch 0102`/`0103` and for `Applying patch: .../0102`.
- `op6/tools/mkadt-op6.py` was run. Its `--dump` output was diffed against `s9/tools/mkadt.py
  --scale2`, and only the intended properties differ (section 3). **[V]**
- `op6/shim-m4-wdt-off.patch` passes `patch -p1 --dry-run` on `op6/shim/m4.c`. The patched
  m4.c compiles with the op6 CFLAGS, with 0 ABS64 relocations. **[V]**

## 1. Inventory of S9-hardcoded facts and how they become per-board

Line numbers are in the patched tree (after 0101), with the originating patch and its line in
brackets.

### 1.1 Needed for milestone A (kernel boots to "waiting for root" with a display console)

| # | What / where | S9 value | OP6 value | Mechanism |
|---|---|---|---|---|
| 1 | `osfmk/arm64/start.s:517-533` stage-0 white band [0082:67-72] | `0x9d400000 + 2928*5760`, row 5760 | `0x9d400000 + 2248*4320`, row 4320 | **0102**: computed from `boot_args->Video` (x20, MMU off): `base + (v_height-32)*v_rowBytes`; skipped if base/rowBytes = 0 or height < 32. With unpatched S9 values on OP6 it writes 2 KiB of white into an invisible part of the splash (0x9e415800): harmless but useless. **[V]** |
| 2 | `osfmk/arm64/sweet_early.c:15-23` early console geometry [0082:97-108] | 1440 wide, 2184 rows (block-limited), stride 1440 px | 1080 wide, 2280 rows (the 0x965400-byte frame fits in the 0x9c000000 block: ends 0x9dd65400 < 0x9e000000), stride 1080 px | **0102**: `sweet_early_set_video()` is called first thing in `arm_init` with `args->Video`. It clips rows to the end of the mapped block and turns the console off if the FB is outside the block. With unpatched values on OP6 the console is garbled, though memory-safe (it stays inside the splash). **[V]** |
| 3 | `osfmk/arm64/arm_vm_init.c:2193-2194` relocate offset [0082:54] | `0x9d400000 & (L2-1)` | same 0x1400000 | **0102**: `sweet_early_relocate(block VA)` adds `early_pa - 0x9c000000`. **[V]** |
| 4 | Early V=P block `start.s:694` and kernel alias `arm_vm_init.c:1846` [0010:131, 0012:26] | 0x9c000000, 32 MiB, Normal-NC | **same** (splash also at 0x9d400000 [HW]) | No change. The S9 caveat (TIMA at 0x9f800000 in the next block) does not apply: nothing is mapped there. Unlisted RAM 0x9c000000-0x9d400000 gets a speculative-readable NC mapping, as on the S9 (nothing in the OP6 reserved-memory list lies there). [I] |
| 5 | DRAM span, `s9/tools/mkadt.py:19-20` | `0xa6000000`+`0xd81c0000` | `0xae000000`-`0x180000000` (3.28 GiB; `--mem-top` up to `0x27c000000`) | ADT `/chosen dram-base/size` from **`op6/tools/mkadt-op6.py`**. mkpayload and the shim already take it from the ADT. Why 0xae000000: it is 32 MiB aligned (start.s maps 32 MiB L2 blocks), it sits above the ramoops window, so the shim flight recorder at 0xac480000 survives, and it puts 0x10000000 of hole-free RAM at the span start, where early boot steals pages without any hole check (`pmap_next_page` ignores `pmap_is_bad_ram`). **[V]** (tool) / [I] (steal reasoning) |
| 6 | Carve-out holes `osfmk/arm/pmap/pmap.c:2581-2585` [0091:20-21] | `{0xc0000000,0xd4000000}` IPA fw, `{0xff000000,0x100000000}` SPSS | `{0xf5000000,0xf6000000}` (16 MiB scan window holding rmtfs 0xf5b00000+0x202000). With `--mem-top` the list stays the same: nothing else above 0xa0000000 is firmware [HW scan] | **0103**: ADT `/chosen/sweet-ram-holes` = u64 (start,end) pairs. If present it *replaces* the built-in table (empty = no holes). If absent (every S9 ADT), the S9 table applies unchanged. A malformed property panics. **[V]** Note: an OP6 ADT booted on a kernel *without* 0103 silently uses the S9 holes, so rmtfs is not excluded. Never mix the two. |
| 7 | Panic log `/pram` (mkadt) | 0x17e0c0000 | 0x17ff00000 (top-1 MiB of the span) | mkadt-op6 (same code). **[V]** |
| 8 | ADT `/sweet-fbcon` (fbcon), `mkadt.py:38-40` | `0x9d400000`/`0x1100000`, 1440x2960, 5760 | `0x9d400000`/`0x968000`, 1080x2280, 4320 | mkadt-op6. `pe_serial.c:fbcon_probe` reads it, so no kernel change. **[V]** |
| 9 | boot_args Video (shim `loader.c:121-139`, `fb.h`) | 1440x2960/5760 | 1080x2280/4320 | Already done in `op6/shim/fb.h` [HW]. SweetFramebuffer takes its size from `PE_current_console` (`SweetFramebuffer.cpp:400-404`), so nothing is hardcoded there. **[V]** |
| 10 | Root `compatible`, `mkadt.py:258` | `...xiaomi,sweet, samsung,starqltechn...` | `...xiaomi,sweet, oneplus,enchilada...` | mkadt-op6. `xiaomi,sweet` stays (SweetPlatformExpert IONameMatch, 0016:12). New `/chosen/sweet-board = "oneplus,enchilada"` is the board selector for later driver patches. **[V]** |
| 11 | APSS watchdog (shim m4) | ABL left it off on S9 | unknown whether ABL arms it (op6 M1 logs "watchdog: EN era …" to ramlog; not in my facts) | **`op6/shim-m4-wdt-off.patch`**: m4 prints the WDT EN that ABL left on screen, then writes EN=0 before jumping. XNU never pets it (boot-arg `wdt=-1` only affects Apple's watchdog). **[V]** (patch) / [G] (ABL state) |
| 12 | MAX77705 battery probe `SweetBattery.cpp:630` (`sweetbattery` defaults to 2) + bit-bang `sweet_max77705.c:28-30` [0099:71,868] | GPIO33/34, TLMM 0x03921000, I2C 0x25/0x36/0x69 | OP6 GPIO33/34 = qup14 pins, i2c14 not used. Gauge is bq27411 @0x55 on i2c10 (0xa88000) | Boot-arg **`sweetbattery=0`** (static battery) for now. Later: gate on `sweet-board` (see 1.2 #20). [I] |
| 13 | OSM CPU clocks `SweetCPU.cpp:223` [0089] | silver 1689 / gold 2649 / L3 1478 | same SoC, but which bin and what ABL leaves are unknown | Boot-arg **`sweetcpu=0`** for the first boots (keeps ABL clocks), then the default. [G] |
| 14 | PP autorefresh `SweetFramebuffer.cpp:73-87,503-506` [0095:18-41] | ABL `0x80000002` → write `0x80000001` | ABL already `0x80000001` [HW] | No change. With `sweetfb_hz>=60` it rewrites the same value and logs it. **[V]** (code read) |
| 15 | Zero-copy scanout gate `SweetFramebuffer.cpp:1074`, `S9_SPLASH_PA/END/FBCON_SIZE :119-121` [0096:180-182,590] | needs `fPhys==0x9d400000 && 1440x2960`, 2-VIG split | 1 VIG, 1080 wide | No change for A/B: the gate fails, the log says "zero-copy unavailable", and it falls back to CPU copy (0089/0096 path). Later patch: see 1.2 #22. **[V]** (code read) |
| 16 | `sweetusb`, `sweettouch` default 0; `sweetkeys` default 1 (read-only SPMI) | | | All three are started only from `sweet_ufs_init` *after* the root partition is found (`sweet_ufs.c:661-663`). Milestone A needs nothing here. **[V]** |

### 1.2 Later milestones (B-D): not drafted as diffs, mechanism decided

| # | What / where | S9 value | OP6 value | Proposed mechanism |
|---|---|---|---|---|
| 17 | Vol-down `sweet_keys.c:27,113` [0084:11, 0060:58-59] | PON RT_STS `0x0810` bit1 (RESIN); vol-up PM8998 GPIO6 RT `0xc510` (ppid 0x0c5) | vol-down = **PM8998 GPIO5**: ppid `0x0c4`, RT_STS `0xc410` (GPIO n at `0xc000+(n-1)*0x100`, as the S9's GPIO6 = 0xc5xx), active low. RESIN is disabled in the OP6 DT. Power and vol-up are the same. | **0104-op6-keys-voldown-gpio**: ADT `/buttons voldown-pmic-gpio = <5>` (mkadt-op6 adds it). When it is present, a second apid lookup (`keys_find_apid(0x0c4)`) and `now[2] = !(gpio5 & 1)`. Read-only, as today. [I] |
| 18 | Touch HID panel size `SweetHID.cpp:439-459,480-481` [0085:278-291, 0087:46-48] | literal 1440/2960, clamps 1439/2959; `SurfaceWidth/Height` 68/151 mm (`:462`) | 1080x2280; 68x144 mm (DT `touchscreen-x/y-mm`) | **0105-op6-hid-panel-from-video**: take W/H from `PE_current_console` (as SweetFramebuffer already does) and the mm from ADT `/chosen` or disp0 (`panel-mm`). S9 behavior stays identical. [I] |
| 19 | Touch driver `sweet_touch.c:37-70` [0085, 0088] | sec_ts @0x48, SDA/SCL GPIO31/32, IRQ GPIO120, power S2DOS05 @0x60 on GPIO127/128, coords 0..4095 | Synaptics S3706B **RMI4 @0x20**; SDA/SCL **GPIO49/50 (NORTH tile, 0x03900000+n·0x1000)**, IRQ **GPIO125 (EAST tile 0x03500000+125·0x1000)**, GPIO99 NORTH pull-up (role unknown); vio = **GPIO88 (EAST)** high; vdd = **PM8998 L28A 3.0 V via RPMh**; coords already in panel px 0..1079/0..2279 | **0107-op6-rmi4-touch** (milestone C, section 4) [V tiles: linux `pinctrl-sdm845.c` PINGROUPs 49/50/99 NORTH, 88/125 EAST] |
| 20 | MAX77705 USB D+/D- switch `sweet_usb.c:142,571-606` [0090:228] and battery | runs in `s9_usb_cold_start`; on OP6 it NACKs, so the **cold start FAILS** | no such chip (PMI8998 + bq27411) | **0106-op6-usb-battery-board-gate**: skip `s9_max77705_usb()` and the battery's MAX77705 probe when `/chosen/sweet-board` ≠ S9. Later a bq27411 reader on bit-banged i2c10 pins. [I] |
| 21 | QUSB2 init `sweet_usb.c:147` [0090:231] + RPMh rails `ldoa5/12/24` (`:575-583`) | Samsung `qcom,qusb-phy-init-seq`; L5 0.8 V, L12 1.8 V, L24 3.075 V | Mainline OP6 hsphy: vdd **L1A 0.875 V**, pll L12A 1.8 V, dpdm L24A 3.075 V; tune: hstx-trim 4, preemph 1, width half-bit, imp-res-offset 8 | 0106: first try the S9 sequence unchanged (same SoC and PMIC; tuning only affects signal margin) [G]. If enumeration is flaky, use an ADT `/sweet-usb qusb2-init` byte array. Add an `ldoa1` 880 mV vote. RPMh DRV2 0x179e0000, cmd-db, SMMU SID 0x740 recipe and GCC offsets are SoC-generic [I]. |
| 22 | Zero-copy for a single-pipe layout | 2 VIG halves, QSEED3 per half | VIG0 only, src `0x08e80438` (2280x1080), fmt/unpack the same [HW] | **0108-op6-zero-copy-single-pipe**: accept "VIG0 covers the whole panel, VIG1 unused", flush bits `(1<<17)|(1<<0)`. Slots: `0x9d400000+0x968000` up to `0x9f800000` = 4 slots of 0x5b0000 at @2x 828x1792. Performance only. [I] |
| 23 | Reset / power-off | kernel has none (`PE_halt_restart` NULL: `cpu.c:1115-1124` panics "can't shutdown") | PSCI 1.1 SYSTEM_RESET/OFF work [HW] | **0109 (both boards)**: install `PE_halt_restart` in SweetPlatformExpert → PSCI `0x84000009`/`0x84000008`. `kern.sweet_next_boot` PON reason (0090) is PM8998-generic. Whether OnePlus ABL honors PON `SOFT_RB_SPARE` 1/2 is [G]. |
| 24 | Log ring | kernel: sweetlog in the last 4 MiB+4 KiB of the root partition (generic); shim ramoops zone S9 0xa1340000 | shim already 0xac480000 (`op6/shim/ramlog.c`); ramoops kept out of the XNU span by #5 | Optional later: mirror the kernel console into the ramoops console zone (it survives a warm reset if OP6 RAM is retained [G]). |
| 25 | RPMh / SE indices | touch SE11 (`0xa8c000`) only as pins; RPMh DRV2 | touch SE12 `0xa90000` (only the pins matter: bit-bang); gauge SE10 `0xa88000` | SE GENI is not used (FIFO disabled / GSI-only conclusion carried over [G]); RPMh unchanged. |

Nothing else default-on touches S9-only hardware. Checked: every `ml_io_map` of a constant in
`bsd/dev/sweet_*`, `iokit/Kernel/arm/*` and `pexpert/arm`. The SoC-generic ones (GIC, timer,
SPMI, UFS 0x1d84000, MDSS/PP, OSM, IMEM) match the OP6 DT. **[V]** (grep) / [I]

## 2. Memory placement (shim m4 → XNU)

- ABL put the shim at 0x80080000 and the DTB at 0x853fd000 [HW]. pmOS's 14 MiB initrd went to
  0x84884000-0x855fe6a9. Taken together this suggests ABL puts the ramdisk so that it **ends
  just below ~0x85600000**, under hyp-mem at 0x85700000, and the DTB about 2 MiB below the
  ramdisk start. [I, G]
- A SWEETXNU payload is about 16-20 MiB (S9/sweet images were around 16.5 MB). So it should
  land at about 0x84300000-0x85600000, below 0xa0000000 and far from `dram-base` 0xae000000.
  The payload grows down toward the shim, so the limit is about 80 MiB [G]. If ABL behaves
  differently, `loader.c:101-103` refuses to boot ("ramdisk overlaps kernel memory") rather
  than corrupting anything, and m4 prints `INITRD lo/hi` on screen. **Record those numbers on
  the first m4 boot.**
- The kernel is copied with the MMU off to `0xae000000 + (va - virt_base)`. The ADT and
  boot_args follow it. All of this is in RAM the scan verified [HW] and does not overlap the
  shim, the DTB or the splash.
- No shim m4 code changes are needed for placement: `op6/shim/{loader,m4}.c` are byte-identical
  to s9's and read everything from the payload and the ADT. The only m4 change is the watchdog
  (#11).

## 3. Host tools

| S9 tool | OP6 | Note |
|---|---|---|
| `s9/tools/mkadt.py` | **`op6/tools/mkadt-op6.py`** (new) | Imports `s9/tools/adt.py`. Its dump differs from S9 `--scale2` only in: compatible, dram-base/size, `sweet-ram-holes`, `sweet-board`, `/sweet-fbcon` reg/w/h/stride, `/pram` reg. It refuses `phys-base < 0xa0000000`, a base that is not 32 MiB aligned, a top above 0x27c000000, and a hole within 256 MiB of the base. **[V]** |
| `s9/tools/mkpayload.py` | use as is | Generic. Takes phys_base/mem_size from the ADT. |
| `s9/build.sh m4` | `op6/build.sh m4` (exists) + `op6/shim-m4-wdt-off.patch` | header v0, Image.gz + OP6 DTB appended (msm-id/board-id), `fastboot boot`. |
| `s9/tools/flash-userdata.sh` (TWRP dd) | needs an OP6 variant: `img2simg` + `fastboot flash userdata` | **Destroys pmOS on userdata** (section 5B). |
| `tools/sweet-log-pull.sh` (recovery) | needs an OP6 variant | Read the tail of userdata from pmOS/TWRP-OP6 (`/dev/disk/by-partlabel/userdata`). |
| `s9/tools/s9-log.py` (UDP 5140) | use as is | Once USB works (D). |

### Commands (from the repo root)

```sh
# 0. kernel with 0102+0103: copy them into xnu/patches/ (new files), push enchilada, then
#    gh workflow run xnu.yml -R BJT1698/ios-s9 --ref enchilada     (push trigger is main-only)
#    gh run download <RUN_ID> -R BJT1698/ios-s9 -n xnu-kernel -D xnu/out-op6
#    and check the log: no "Skipping patch .../0102" or 0103.
K=xnu/out-op6/kernel.development.sweet

# 1. D22AP device tree (Apple artifact, not in git; local IPSW, `ipsw` CLI needed, not installed here)
mkdir -p s9/research/ipsw-x
unzip -j ../ipsw/iPhone10,3_16.7.16_20H392_Restore.ipsw Firmware/all_flash/DeviceTree.d22ap.im4p -d s9/research/ipsw-x
ipsw img4 im4p extract s9/research/ipsw-x/DeviceTree.d22ap.im4p -o s9/research/ipsw-x/d22ap.adt

# 2. ADT (XR identity @2x, 828x1792 canvas stretched to 1080x2280)
python3 op6/tools/mkadt-op6.py op6/out/op6-xr2.adt --scale2            # span 0xae000000-0x180000000
# later, for all 8 GB:  --mem-top 0x27c000000

# 3. payload (cmdline <= 607 chars; strings in section 5)
python3 s9/tools/mkpayload.py $K op6/out/op6-xr2.adt op6/out/payload-A1.bin --cmdline "$A1"

# 4. boot image (apply the m4 watchdog patch once), test without flashing
patch -p1 < op6/shim-m4-wdt-off.patch
PAYLOAD=$PWD/op6/out/payload-A1.bin op6/build.sh m4                    # build.sh cd's into op6/
fastboot boot op6/out/boot.img
```

## 4. Milestone C outline: Synaptics RMI4 touch (0107)

Reuse from `sweet_touch.c` (0088): the TLMM open-drain bit-bang I2C engine (OE bit 9, 3 µs
half period), the poll thread bound to CPU1 (0094), the `sweettouch`/`sweettouchdelay`
boot-args, and `sweet_hid_touch()`. Changes:

1. Pins from ADT `/sweet-touch` (`sda`, `scl`, `irq`, `vio-gpio`, `addr`, `kind = "rmi4"`).
   Map two tiles: NORTH 0x03900000 len 0x81000 (49, 50, 99) and EAST 0x03500000 len 0x7e000
   (88, 125). The offset is `n*0x1000` in its tile. IO is at +4, cfg at +0, as the S9 code does.
2. Power: first just probe 0x20. ABL may already have the panel's touch rails up (S9 ABL did
   not) [G]. If it NACKs: drive GPIO88 high (vio 1.8 V), vote `ldoa28` 3000 mV with the RPMh
   VRM helper from `sweet_usb.c` (`s9_rpmh_init`/`s9_vreg`, move them to a shared file), wait
   200 ms (DT `syna,startup-delay-ms`). No reset GPIO is known. An F01 soft reset (cmd reg
   bit0) is the fallback.
3. RMI4 protocol (Linux `drivers/input/rmi4`): page select reg 0xFF. Scan the PDT from 0xE9
   downward in 6-byte entries (query, cmd, ctrl, data base, int-count|version, fn#) until fn#=0.
   Find F01 and F12. Read the F12 query regs 4..8 (register descriptors) to locate data1 (finger
   objects, 8 B each: type, x lo/hi, y lo/hi, z, wx, wy). Write F01 ctrl0 with CONFIGURED|nosleep,
   and enable F12 in F01 ctrl1 (interrupt enable).
4. Loop: while GPIO125 is low, read F01 data1 (IRQ status), then F12 data1 for N objects. type
   0 means lifted, 1 means finger. x/y are already in panel px (clip 1079/2279). Call
   `sweet_hid_touch(i, down, x, y)` with no 4096 scaling. This needs 0105.
5. Risk [G]: GPIO99's role (a possible reset or attention line). Some S3706B firmware needs the
   F34/F54 "configured" handshake; Linux handles it with `rmi_f01` only.

Milestone D (USB ECM): 0106 gate (#20, #21), then `sweetusb=2`. Note that `fastboot boot`
leaves ABL's fastboot USB **running** on the OP6 (the S9 started cold). First run `sweetusb=1`
(report only), then decide whether the cold start must first reset the controller: GCC BCR
0xf000 with clocks off, exactly as the S9 path does, which should cope. [G]

## 5. Milestones and boot-args

**A1, no storage at all.** Expect: "SWEET EARLY 1..20" marks on a correct 1080-wide console,
then fbcon, 1 CPU, then "Still waiting for root device" (`rd=md0` with no RAMDisk in
memory-map, so IOFindBSDRoot waits [I]). No userdata access.
```
A1="debug=0xc serial=3 cpus=1 keepsyms=1 -noprogress wdt=-1 wdt_test=no_user_checkin sweetcpu=0 sweetbattery=0 sweetusb=0 rd=md0"
```
Then the same with `cpus=8`.

**A2, UFS read-only probe** (still no wipe: `ufsrw`/`rootrw` are absent, `su.writable=false`,
and sweetlog stays off): expect `SWEET UFS: "userdata" on LUN 0: first block …, ~112600 MiB,
read-only`, the keys thread, the `sweetusb=1` state dump, then a root mount failure (userdata
holds pmOS, not HFS+), i.e. a "can't mount root" panic.
```
A2="debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress wdt=-1 wdt_test=no_user_checkin sweetcpu=0 sweetbattery=0 sweetusb=1 rd=ufsroot0 ufsroot=userdata"
```

**B, root plus SpringBoard.** Root image as on the S9 (`tools/mkiosroot.sh … ios16-root.img
16G`, `fsck.hfsplus`, `tools/sweet-root-tweaks.py`; optional `s9/tools/prep-ios6.sh`
extras). **Decision for you:** OP6 userdata (sda17, ~110 GiB) holds pmOS today. Choose one:
(i) back up, then `img2simg ios16-root.img root.simg && fastboot flash userdata root.simg`.
This wipes pmOS, and the sweetlog then uses the partition tail.
(ii) keep pmOS: shrink pmOS's inner partition and add a kernel option `ufsroot_off=<4K
blocks>`/`ufsroot_len`, which exposes a sub-range of userdata as `ufsroot0`. That is about 15
lines in `ufs_find_partition` (not drafted).
```
B="debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=ufsroot0 ufsroot=userdata ufsrw=1 rootrw=1 ufswait=1 ufslogsecs=60 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 cs_enforcement_disable=1 sweetfb_hz=60 sweetbattery=0 sweetusb=1"
```
(305 chars). Add `sweetfb_noconsole=1` once SpringBoard is up. Drop `sweetcpu=0` once A has
passed, since CoreAnimation runs on the CPU. Expect the CPU-copy framebuffer path
("zero-copy unavailable"), so fps will be below the S9's until 0108.

**C, touch:** 0105 + 0107, then `sweettouch=1 sweettouchdelay=20`.
**D, USB:** 0106, then `sweetusb=2` + `s9/tools/s9-log.py`.

## 6. Open risks and unknowns

1. Whether ABL arms the APSS WDT (m4 patch prints it, then disables it). [G]
2. ABL ramdisk placement for a ~20 MiB payload (m4 prints it; loader refuses overlap). [G]
3. The RAM scan sampled once per 16 MiB, so an XPU-protected region smaller than 16 MiB could
   be missed. The Linux reserved-memory list shows nothing above 0xa0000000 except
   ramoops/rmtfs (both excluded). A crash in the span would point here. [G]
4. Early boot page stealing ignores holes. That is safe as long as the first hole is 0x47000000
   above dram-base (rmtfs window), and mkadt-op6 enforces at least 256 MiB. [I]
5. `rd=md0` without a ramdisk is expected to wait, not panic. If it panics instead, A1 has
   still reached BSD init, so it is equally conclusive. [G]
6. UFS left running by ABL after `fastboot boot` (it loaded nothing from UFS for the kernel),
   no SMMU programming needed (S9: none), LUN0 GPT read at 4 KiB blocks. A2 tests all three. [G]
7. QEMU (`tools/qemu-sweet.py`): after 0102, the early console and stage-0 band follow the
   boot_args Video and turn off if it lies outside 0x9c000000-0x9e000000. The band used to be
   written blindly to 0x9e415800. [I]
8. 0102/0103 are compile-unverified as a whole (there is no local xnu build). The pieces were
   syntax-checked (section 0). [V partial]
