# OnePlus 6 GPU (Adreno 630) bring-up

Labels: **[V]** verified by reading the source or DT named next to it, **[HW-S9]** proven on the Galaxy S9
hardware (same SDM845 / A630 v2), **[HW-F1]** proven on a Poco F1 by Bergamota (facts only, no licence),
**[I]** inferred, **[?]** unknown until the phone answers. M10, M11 and M12 ran on the OP6 (see "Device results"); M13 has not run yet.

Sources: OP6 live DT `../op6-live.dts` (mainline `sdm845.dtsi` + `sdm845-oneplus-common.dtsi` as pmOS boots
it); Linux v6.6 `drivers/gpu/drm/msm/adreno/a6xx_gmu.c`, `a6xx_hfi.c/.h`, `a6xx_gmu.xml.h`, `a6xx.xml.h`,
`drivers/clk/qcom/gpucc-sdm845.c`, `gcc-sdm845.c`, `drivers/soc/qcom/cmd-db.c`,
`drivers/interconnect/qcom/sdm845.c` (all GPL: facts only, no code copied); S9 port `s9/shim/m10.c`,
`s9/shim/m11.c`, `s9/ESTUDO-2026-10-25-gpu.md`, `s9/NOTES.md`.

## 1. Stages

| stage | what | status |
|---|---|---|
| **M10** | CX GDSC + GMU clocks 200 MHz, DTCM test, RSC/PDC micro-code, `a630_gmu.bin` into ITCM, CM3 out of reset -> `0xBABEFACE` | written, builds; S9 same stage passed first time [HW-S9] |
| **M11** | M10 + GPU SMMU (tables, free CB, SID 5/4 rules, ATS1PR self-test) + boot with `HFI_QTBL_ADDR` + HFI v1: INIT, FW_VERSION, PERF_TABLE, BW_TABLE, TEST | written, builds; S9 M11 was flashed but never confirmed; the OP6 one follows mainline instead (sec. 2) |
| **M12** | M11 + GX on through the GMU, Linux v6.6 legacy order: `GMU_RPMH_CTRL`=0xf11 at boot, `BOOT_SLUMBER_OPTION=0` + `GX_VOTE_IDX`/`MX_VOTE_IDX` (highest OPP), OOB `BOOT_SLUMBER` (bit 22 / ack 30), SPTPRAC on, then HFI, DCVS OOB, OOB `GPU_SET` (16/24); read-only core probes; Linux shutdown (VBIF halt, notify_slumber, POWER_STATE 0xf) and CM3 reset | written, builds (sec. 5); the first GX power-up on the OP6 [HW-F1 sweetgpu=5] |
| **M13** | M12 (quiet) + zap shader via TZ (PAS 13: init_image / mem_setup / auth_and_reset), CP: `a630_sqe.fw`, own GPU SMMU context (SID 0, CB 0, 4K PTEs), a630 hw_init subset, ringbuffer, `CP_ME_INIT`, `CP_SET_SECURE_MODE 0`, NOP + `CP_MEM_WRITE 0xC0FFEE01` (sec. 8) | written, builds; not run [HW-F1 etapa 8] |
| D | XNU driver: `op6/GPU-XNU-PLAN.md` (ADT `/sweet-gpu` firmware, NC carve-out 0xa0000000, msm uAPI user client, milestones G1-G6). **G1** = `xnu/patches/0107-op6-gpu-bringup.patch` (M10-M13 in the kernel, `sweetgpu=1/2`) | G1 drafted (apply-check + stub syntax check), not built/run |
| E/F | Mesa freedreno a630 + fake libdrm + our EAGL in backboardd (iOS 16 uses GLES natively when Metal is absent, `s9/ESTUDO` sec. 2), then Metal (Chlorophyll, MIT) | todo |

## 2. S9 -> OP6 differences for everything M10/M11 touch

Same SoC, so every SoC block address is identical; what differs is the software description (S9 = Samsung
kgsl DT + msm-4.9, OP6 = mainline DT + msm DRM) and a few S9 M11 mistakes.

| item | S9 (s9/shim) | OP6 (op6/shim/gpu.c, m11.c) | label |
|---|---|---|---|
| GPU / GMU / gpucc / GCC / GPU SMMU bases | 0x5000000 / 0x506a000 / 0x5090000 / 0x100000 / 0x5040000 | identical (DT `gpu@5000000` 0x40000 + cx_mem 0x509e000/0x10, `gmu@506a000` 0x30000, `clock-controller@5090000` 0x9000, `iommu@5040000` 0x10000) | [V] DT |
| PDC | base 0xb200000, kgsl word offsets (0x21140, 0xA0000) | DT `gmu_pdc` 0xb280000 + `gmu_pdc_seq` 0xb480000, mainline offsets (0x1140, 0): **same physical registers** (0xb284500, 0xb480000) | [V] DT + a6xx.xml.h |
| register numbering | kgsl (GMU window base = 0x1A800) | mainline dword offsets; kgsl = mainline + 0x1A800 for every register used | [V] a6xx_gmu.xml.h |
| CX GDSC / GX GDSC | 0x106c (status 0x1540) / 0x100c | same; `gpu_cx_gdsc` VOTABLE + gds_hw_ctrl 0x1540; GX untouched | [V] gpucc-sdm845.c |
| GMU clock | RCG 0x1120/0x1124 = 0x602 (GPLL0_OUT_MAIN_DIV / 1.5 = 200 MHz) | same; DT gmu opp 200/400 MHz (levels 0x30/0x80); mainline also sets cx_gmu CBCR wake/sleep = 0xf (added) | [V] gpucc-sdm845.c, DT |
| clock branches | gpucc cxo 0x109c, ahb 0x1078, cx_gmu 0x1098; gcc cfg_ahb 0x71004, memnoc_gfx 0x7100c, snoc_dvm 0x71018, ddrss_gpu_axi 0x44038, GPLL0 vote 0x52004 b15/b16 | same. DT gmu clocks = gpucc 0/1 (cx_gmu, cxo) + gcc 18 (ddrss_gpu_axi) + gcc 33 (memnoc_gfx); SMMU clocks gcc 33 + gcc 30 (cfg_ahb, CLK_IS_CRITICAL). gpucc 0x1078 and snoc_dvm are kgsl-only, kept (S9 proven) | [V] DT + bindings; [HW-S9] |
| rails | none voted | none voted by the CPU either: sdm845 gpucc/gmu have no rpmhpd domain; CX/MX are on (CPU, display); GX = `gfx.lvl` ARC (PM8005), voted only by the GMU through the GPU RSC/PDC | [V] DT; [I] |
| PDC TCS addresses | 0x30010 / 0x30000 / 0x30080 | M10/M11: third command 0x30050 (`GPU_PDC_GFX`). **M12: 0x30080 (`GPU_PDC_CMD2`)**, what Linux (a630 `pdc_address_offset`) and the S9 kgsl send; on the OP6 cmd-db 0x30080 = `xo.lvl`, so the GPU PDC's third vote is the XO vote, not a gfx vote: GX is voted by the GMU itself (`GMU_RPMH_CTRL` GFX enable + `GX_VOTE_IDX`). Bergamota used 0x30050 and lit GX too, so it only matters for `GPU_RPMH=1` | [V] a6xx_gmu.c + OP6 cmd-db (M10); [HW-F1] |
| ARC levels | not used | M11 reads `cx.lvl`, `mx.lvl`, `gfx.lvl` aux lists from the OP6 cmd-db and builds the perf votes as `a6xx_gmu_rpmh_arc_votes_init` | [V] a6xx_gmu.c, cmd-db.c |
| GPU OPPs / speed bin | 257..710 MHz (kgsl table) | DT gpu opp-table: 257/342/414/520/596/675/710 MHz, opp-level 0x40/0x80/0xc0/0x100/0x140/0x180/0x1a0, peak kBps 1648000..7216000; **no speed-bin nvmem / opp-supported-hw** (qfprom only has the hstx-trim cells) | [V] DT |
| bus votes | none | none (DDR is busy anyway: CPU + display scanout). Mainline votes `gfx-mem` = MASTER_GFX3D (7) -> SLAVE_EBI1 (14) on mem_noc (BCMs SH0, MC0, ACV) at boot; to add in M12/M13 if the CP needs bandwidth | [V] DT, interconnect/sdm845.c; [I] |
| GMU SID | 4 and 5 (kgsl gmu_user/gmu_kernel) | DT `iommus = <&adreno_smmu 5>`; GPU = SID 0. M11 maps 5 (required) and 4 (harmless) | [V] DT |
| GMU memory | HFI at VA 0x60000000, dump 0x60100000 | mainline legacy: dummy page fixed at 0x60000000, then debug 16K (0x60001000), log 16K (0x60005000), HFI 16K (0x60009000) | [V] a6xx_gmu.c memory_probe |
| HFI layout | 3 queues packed after the table, read index rounded up to 4, writes padded with 0xfafafafa, ack = type 1 | 2 queues at +4K/+8K (cmd id 0, legacy response id 4), no rounding/padding (that is HFI v2), ack = id 126 + echoed seqnum | [V] a6xx_hfi.c (S9 M11 would have mis-parsed) |
| HFI sequence | INIT, FW_VER | INIT, FW_VERSION (supported 1.10), PERF_TABLE v1 (43 words), BW_TABLE (160 words, sdm845 fixed BCM cmds), TEST | [V] a6xx_hfi_start_v1 |
| SMMU PTEs | privileged-only, walks cacheable | AP[1] unprivileged set as io-pgtable does for non-PRIV BOs; walks non-cacheable outer-shareable (no coherency with MMU-off CPU writes needed) | [V] io-pgtable-arm (from memory) [I] |
| SMMU programming | CB0 and SMR0/1 blindly, CLIENTPD cleared first | scan SMRs/CBs, free CB + free SMRs, CB -> S2CR -> SMR -> TLB -> CLIENTPD last, ATS1PR self-test before the GMU may touch RAM | OP6 lesson (m7 smmu_usb) |
| chip id in HFI_SFR_ADDR | 0x06030100 (kgsl qcom,chipid 0x06030001) | 0x06030200 (DT `qcom,adreno-630.2` repacked as a6xx_gmu_fw_start); build flag `GMU_CHIPID` | [V] a6xx_gmu.c; both [I] safe |
| AHB fence | (1<<31) \| (0x28<<18) \| 0x8A0 | (1<<31) \| (0xa<<18) \| 0xa0 (mainline) | [V] |
| extra boot writes | - | `GMU_CM3_CFG`=0x4052, `ICACHE/DCACHE_CONFIG`=1, `PWR_COL_CP_MSG` = log VA \| 3, `PWR_COL_CP_RESP`=0 | [V] a6xx_gmu.c |
| GPU RSC handshake | none (0xBABEFACE anyway) | none by default; `-DGPU_RPMH=1` adds mainline `a6xx_rpmh_stop`/`start` (RSCC_CONTROL_REQ/ACK, SEQ_BUSY) | [HW-S9] without; [V] mainline with |
| zap | Samsung `a630_zap` from NON-HLOS.bin | OnePlus-signed `a630_zap.mbn` (sec. 4); DT `zap-shader` memory-region = reserved `gpu@8c415000` 0x2000 no-map | [V] DT |
| Samsung-specific | RKP/hyp worry for the GPU SMMU | none. Mainline treats `qcom,adreno-smmu` (smmu-v2) as directly programmable (the apps SMMU is the hyp-emulated one on this firmware); M11 reads every write back anyway | [I] [?] |

## 3. Files

- `op6/shim/gpu.c`, `gpu.h` - shared: screen/ramlog `step()`/`say()`, power, cmd-db readout, RSC/PDC
  micro-code, firmware load + boot. `m10.c`, `m11.c` - the stages. `rpmh.c` gained `rh_aux()` / `rh_name()`
  (cmd-db reads only).
- `op6/tools/gpu-fw-embed.py <a630_gmu.bin>` writes `op6/shim/fw_a630_gmu.h` (git-ignored; `--remove` deletes it).
  Without it both stages build and say "SEM FIRMWARE" on screen (M11 then stops before writing the SMMU).
- `m12.c` - `#include "m11.c"` (with `M11_LIB`, so M11's code is reused unchanged and its `shim_main` left out)
  + the GX sequence. `gpu.c` gained `gpu_quiet` (M11 lines to the ramlog only, last 6 shown on failure: the
  screen has ~47 rows), `gpu_rpmh_ctrl` (default 0 = m10/m11 behaviour; m12 sets 0xf11 and it is read back
  before the CM3 is released), `gpu_rpmh_stop()`, `GPU_PDC_CMD2`.
- `bash op6/build.sh m12` -> `boot-m12.img` (renamed to `boot-m12-gx.img`; `SHIM_CFLAGS=-DGPU_RPMH=1` ->
  `boot-m12-gx-rpmh.img`). `-DM12_OPP=1` boots GX at 257 MHz / lvl 0x40 instead of Linux's 710 MHz / 0x1a0.
- `m13.c` - `#include "m12.c"` (with `M12_LIB`; m12.c gained `m12_hush`, 0 = unchanged M12 behaviour) + zap + CP.
  `op6/tools/gpu-fw-embed-cp.py <a630_sqe.fw> <a630_zap.mbn>` writes `shim/fw_a630_sqe.h` + `fw_a630_zap.h`
  (git-ignored by `op6/.gitignore`; `--remove`). Without any of the three headers m13 builds and says
  `SEM FIRMWARE: ...` before powering or writing anything. `bash op6/build.sh m13` also checks that the appended
  DT still has `gpu@8c415000` 0x2000. Images: `boot-m13-cp.img` (full), `boot-m13-zap.img` (`-DM13_STOP=1`:
  stops right after `auth_and_reset`, no CP).
- `bash op6/build.sh m10` / `m11` -> `op6/out/boot-m10.img` / `boot-m11.img`. Variants:
  `SHIM_CFLAGS=-DGPU_RPMH=1` (GPU RSC handshake), `-DGMU_CHIPID=0x06030100u` (S9 value),
  `-DGPU_PDC_GFX=0x30050u` (Bergamota's gfx.lvl, only if cmd-db says so).

## 4. Firmware (none of it in git)

| file | size | sha256 | official source | needed by |
|---|---|---|---|---|
| `a630_gmu.bin` | 32768 | `da8d9b1b...73a6` | https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/qcom/a630_gmu.bin | M10, M11 (embedded) |
| `a630_sqe.fw` | 34188 | `1c21b527...a90c` | https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/qcom/a630_sqe.fw | M13 (CP microcode; > 0x8000, Bergamota's lesson) |
| `a630_zap.mbn` | 14256 | `97809567...0df9` | OnePlus-signed, pmOS package `firmware-oneplus-sdm845`: https://gitlab.com/sdm845-mainline/firmware-oneplus-sdm845/-/raw/prepackaged-release/lib/firmware/qcom/sdm845/OnePlus/enchilada/a630_zap.mbn (also on the phone's stock `/vendor/firmware` as `a630_zap.mdt/.bNN`) | M13 (PAS 13, loaded into 0x8c415000) |

All three are already in `../gpu-fw/` and match these sizes and hashes [V 2026-11-10: sha256sum; GitLab
`x-gitlab-content-sha256` for the zap; HTTP content-length for linux-firmware]. The DT names the zap
`qcom/sdm845/OnePlus/enchilada/a630_zap.mbn`. The Xiaomi or Samsung zap will not do: TZ checks the OEM
signature.

## 5. Test checklist (one `fastboot boot` each, nothing flashed)

Before: phone in fastboot on USB. `fastboot boot op6/out/boot-m10-gmu.img` (then `boot-m11-hfi.img`, `boot-m12-gx.img`; `*-rpmh.img` = built with `GPU_RPMH=1`). Each screen counts 60 s and reboots
(PSCI). A hang shows the last **PASSO** line; the watchdog bites 18 s after the last pet. A Qualcomm
CrashDump screen needs a long power-key press (no damage, lesson from M7). Photograph the whole screen
when "Tire uma foto" or a hang appears; if it reboots into something with pstore, `ramoops` keeps the
full log (`ramlog` at 0xac480000).

**M10, what to read in the photo:**
1. `GCC` / `GPUCC` lines (ABL state; `cfg_ahb desligado` is fine).
2. `ARC` lines: `cx.lvl @30000:...`, `mx.lvl @30010:...`, `gfx.lvl @300?0:...` (the level lists; write them
   down - they replace the [?] in sec. 2), and `PDC TCS 30000=cx.lvl 30010=mx.lvl 30080=<name>`.
3. `CX gdscr=...` with bit 31 (`8.......` or `F.......`) = CX on; `CLK ... gmu=........` bit 31 clear.
4. `SMMU idr0=... idr1=... scr0=...` (read-only facts for M11).
5. `DTCM back=600D0001 0A110002` = GMU memory works.
6. `FW palavras=8192 erradas=0`, then `GMU init=BABEFACE` and **"O FIRMWARE DO GMU RODA (0xBABEFACE)"** = success.
   `init=00000000` = firmware did not start (then try `boot-m10-gmu-rpmh.img`).

**M11, success looks like:**
1. `VOTOS gx ...` (8 votes, first 000000) / `cx ...` (3), `BW BCM 00=.. 5c=.. 0c=.. 34=.. 7c=.. 4c=..` names.
2. `SMMU smr=.. cb=.. pg=4K n=8 SMS ATOS`, `smr validos=.. cb em uso=.... livre=N`.
3. `SMMU hfi pa=... ttbr=...`, `cbar=0001F300 sctlr=1027 ...`, `sid 5 smrX=80000005 s2cr=0000000N`,
   `sCR0 ...` with bit 0 clear, **`ATS ... -> buffer HFI: OK`**.
4. `GMU init=BABEFACE`, "GMU rodando (0xBABEFACE) com a SMMU".
5. `HFI status=00000001`, then `5a H2F INIT: ACK ok`, `5b FW_VERSION: ACK ok`, `GMU fw versao 01xxxxxx`,
   **`5c PERF_TABLE: ACK ok`**, `5d BW_TABLE: ACK ok`, `5e TEST: ACK ok`,
   **"O GMU CONVERSA POR HFI (INIT/VER/PERF/BW)"**. Any `FALHA fsr=...` line = the GMU hit an unmapped
   address (FAR tells which); `SEM RESPOSTA` without FALHA = GMU silent (try `boot-m11-hfi-rpmh.img`).

**M12, success looks like** (`boot-m12-gx.img` first; `boot-m12-gx-rpmh.img` = same + GPU-RSC wake before the
firmware and RSC sleep at the end):
1. `GX antes gd=... cf=... sp=.. p=..` (GX as ABL left it; `cf` bit 16 = POWER_UP_COMPLETE, expected off),
   `M11 SMMU+ATS ok, GMU 0xBABEFACE`, `RPMH ctrl=00000F11 pwr=..`.
2. `VOTO opp 710MHz gx=N mx=N lvl=1A0`, then **`OOB BOOT ack NNNus i=40000000 ...`** (bit 30) and
   `GX gd=... cf=0001.... sp=..` with `sp` bits 6/7 clear = the GMU powered GX. `SPTPRAC st=28 ... ligado`.
3. `HFI INIT/VER/PERF/BW ok`, `OOB DCVS ack ...`, `DCVS return=00000000 ok`, `OOB GPU ack ...` (bit 24).
4. `RBBM st=...` (F1 saw 0x00000001), `RBBM1-3`, `CP flt= st1= vbif=`, `SQE cntl= base=0` (no microcode yet;
   a6xx has no readable SQE version register, Linux reads it from the firmware file),
   `AO GMU d=~1920000 em ~100000us conta` (19.2 MHz), `AO CP v=... d=...` (`PARADO` is the F1 observation,
   not a failure), **"A GX LIGOU: O NUCLEO DA GPU RESPONDE"**.
5. `VBIF halt ack, solto`, `SLUMBER ack ...` with `p=0F`, `DESLIGA slumber ok (0xf)`, `GX fim ... cf=` bit 16
   clear (or `GDSC desligado pela CPU` if the GMU left it on), countdown.
- Failure readings: `OOB BOOT SEM ACK` = the GMU did not power GX (default image: try `-rpmh`; the GMU may need
  the RSC awake to vote); `RECUSA <reg> w= r=` = a readback mismatch, the stage stopped before the next
  step; `GX nao confirmada` = acked but gpucc/SPTPRAC disagree, core not read (photo the `GX` line);
  hang on PASSO `10 nucleo` = GX reads stall (bus) -> watchdog; CrashDump = needs a long power press.

## 6. Risks (what these stages write outside the GPU block)

- RPMh: nothing from the CPU (cmd-db reads only). With `GPU_RPMH=1` the GPU's own PDC sends its wake TCS
  (mx.lvl idx 2, cx.lvl idx 3, 0x30080 idx 3) as Linux does on every GPU resume on this phone.
- PMIC: nothing. GX GDSC: never written. `GMU_RPMH_CTRL` = 0: the GMU cannot vote rails/DDR.
- GPU SMMU: one context bank, up to two SMR/S2CR pairs, sCR0.CLIENTPD cleared if set. Not the apps SMMU.
- The GMU DMA only after the ATS self-test passed; on any SMMU readback mismatch the stage stops before
  releasing the CM3. Both stages put the CM3 back in reset before the countdown.
- **M12** adds: `GMU_RPMH_CTRL`=0xf11, so the **GMU firmware sends RPMh votes through the GPU RSC** (gfx.lvl
  for GX at the highest OPP level 0x1a0, mx.lvl, cx.lvl; DDR/LLC as its BW table says - one zero level). The
  CPU itself still votes nothing, writes no PMIC (the PM8005 GFX rail moves because AOSS aggregates the GMU's
  vote), no apps SMMU, no SCM/TZ. CPU writes in the GPU block: GMU vote/OOB/DCVS registers, SPTPRAC control,
  GX GDSCR retain-FF bit (Linux sptprac_disable), VBIF XIN halt/unhalt (only with GX confirmed on), and - only
  if GX is still on after slumber - GX GDSC SW_COLLAPSE + clamp IO (`a6xx_gmu_stop`'s gxpd put).
- Not done vs Linux: the CPU's own `gfx-mem` interconnect vote (`a6xx_gmu_set_initial_bw`: DDR is busy
  anyway), LLCC activation, GMU interrupts (polled), and in `force_off` the GX bus clear / sw reset.
- If the GMU dies mid-way its RPMh votes stay until the PSCI reboot (the countdown); a hung GX read is the
  watchdog's job. Linux boots GX at the highest OPP on every resume; `-DM12_OPP=1` is the gentler variant.

## 7. Roadmap details for M12/M13 (from mainline legacy path, to verify on screen)

- (done in m12.c; kept for reference) M12 order (a6xx_gmu_fw_start legacy): `GMU_RPMH_CTRL` |= 0xf11 (interface, LLC, DDR, MX, CX, GFX vote enable)
  in power_config, boot, then `gfx_rail_on`: `BOOT_SLUMBER_OPTION`(0x23f8)=0, `GX_VOTE_IDX`(0x23f9) =
  vote[7]&0xff, `MX_VOTE_IDX`(0x23fa) = (vote[7]>>8)&0xff, OOB BOOT_SLUMBER; `a6xx_sptprac_enable`
  (`GMU_GX_SPTPRAC_POWER_CONTROL` 0x81 = 0x778000, poll `SPTPRAC_PWR_CLK_STATUS` 0x50d0 until
  `& 0x38 == 0x28`); OOB BOOT_SLUMBER = set bit 22, ack/clear bit 30 [V a6xx_gmu.c]; then HFI as M11;
  then OOB GPU and the first `RBBM_STATUS` (0x210) read. Needs `GPU_RPMH=1` behaviour.
- (done in m13.c, sec. 8) M13: SMC `qcom_scm_pas_init_image/mem_setup/auth_and_reset` with PAS 13 on the zap loaded at 0x8c415000
  (outside XNU RAM, which starts at 0xae000000), `a630_sqe.fw` into a 4K-paged SMMU mapping for SID 0
  (GPU pages are 4K, XNU's 16K: one PTE per 4K [HW-F1]), ring + `CP_ME_INIT`, `CP_SET_SECURE_MODE 0`,
  NOP + `CP_MEM_WRITE`; read CP_ALWAYS_ON through the GMU always-on counter (CPU-side read looked frozen
  on the F1 [HW-F1]).

## 8. M13 (zap + CP) - sequence, screen, risks

Sources (facts only): Linux v6.6 `drivers/soc/qcom/mdt_loader.c`, `drivers/firmware/qcom_scm.c`, `qcom_scm.h`,
`qcom_scm-smc.c`, `drivers/gpu/drm/msm/adreno/adreno_gpu.c` (zap_shader_load_mdt, adreno_fw_create_bo),
`a6xx_gpu.c` (hw_init, a6xx_cp_init, a6xx_ucode_check_version, a6xx_flush/idle), `a6xx.xml.h`,
`adreno_pm4.xml.h`, `arm-smmu-qcom.c` behaviour (GPU on CB 0, SCTLR.HUPCF) from memory [I].

**Zap facts [V]:** `a630_zap.mbn` = ELF32 Hexagon, 3 phdrs: [0] NULL 0x94 (ELF + phdrs), [1] hash (flags 0x02200000)
file 0x1000 + 0x1988, [2] PT_LOAD paddr 0x5000 filesz = memsz 0x7b0, flags 0x08000007 (bit 27 = relocatable).
`qcom_mdt_get_size` = 0x1000 (4K-aligned) <= DT 0x2000: fits. Metadata = 0x94 + 0x1988 = 0x1a1c bytes (hash
read from p_offset, the file is not split). The code segment lands at 0x8c415000 + 0 (relocated to min paddr).

**Sequence** (each step on screen + ramlog before it runs):
1. M11 + M12 exactly as M12 (ramlog only; on failure the last 6 lines are shown) except: GMU context bank moves
   off CB 0 (CB 0 is kept for the GPU, as arm-smmu-qcom does) -> one line `M12 GX 710MHz GPU_SET ok RBBM st=.. gmu cbN`.
2. **Z1-Z7 zap** (SMC64, SIP, std call; x0 = 1 INTERRUPTED re-issued with x0 = 1 + x6 as returned, -12 EBUSY retried
   20x / 30 ms; success = x0 == 0 && x1 == 0):
   | step | x0 | x1 (arginfo) | x2.. | Linux |
   |---|---|---|---|---|
   | Z2 convention probe (fast) | 0xC2000601 | 0x01 | 0x02000601 | `__get_convention` (expects x1 = 1) |
   | Z3 is_call_available | 0x42000601 | 0x01 | 0x02000207 | `__qcom_scm_is_call_available` (info only) |
   | Z3 pas_supported | 0x42000207 | 0x01 | 13 | `qcom_scm_pas_supported` (info only) |
   | Z4 init_image | 0x42000201 | 0x82 (VAL, RW) | 13, PA of 4K-aligned metadata buffer | `qcom_scm_pas_init_image` |
   | Z5 mem_setup | 0x42000202 | 0x03 | 13, 0x8c415000, 0x1000 | `qcom_scm_pas_mem_setup` (relocatable) |
   | Z6 (CPU) copy 0x7b0 bytes to 0x8c415000, read back | - | - | - | `__qcom_mdt_load` |
   | Z7 auth_and_reset | 0x42000205 | 0x01 | 13 | `qcom_scm_pas_auth_and_reset` |
   No `set_remote_state`: in v6.6 `a6xx_zap_shader_init` loads once per boot and never sends
   SCM_GPU_ZAP_SHADER_RESUME. Order differs from Linux (which loads the zap after CP_ME_INIT) and matches kgsl
   (zap before the CP starts); Linux and kgsl both work, GX is held on by OOB GPU_SET in either case.
   Any refusal: stop -> OOB clear, M12 shutdown, countdown.
3. **C1-C8 CP:** SQE (file minus word 0, 8546 words, version word & 0xfff = 0x207 >= 0x190 so CP_WHERE_AM_I is used)
   at GPU VA 0x01000000 (read-only PTEs), ring 32 KiB at 0x01010000, scratch page at 0x01020000 (rptr shadow +0,
   magic +0x100); GPU context bank (CB 0), S1 39-bit 4K, TTBR0 only, SCTLR M|TRE|AFE|CFRE|ASIDPNE|HUPCF,
   SMR/S2CR SID 0 (mask 0), **sCR0.USFCFG set** (unmatched SIDs fault instead of bypassing to PA = VA; only when
   both GMU SIDs 5 and 4 have rules), ATS1PR of ring and SQE+0x8000 must give the buffers' PAs (when ATOS exists). hw_init a630 subset (sec. table in m13.c: SECVID_TSB 0, 12x
   ADDR_MODE_CNTL = 1, VBIF QoS 3, VBIF_GATE_OFF_WRREQ_EN 9, UCHE ranges/filter/ways, ROQ thresholds, MEM_POOL 128,
   PC_DBG_ECO 0x180000, CP_AHB_CNTL 1, UBWC NC modes (hbb_lo 2), hang detect, UCHE_CLIENT_PF); not done: hwcg, perf
   counters, CP protect (left off), RBBM_INT_0_MASK (polled). CP_SQE_INSTR_BASE, CP_RB_BASE, CP_RB_CNTL =
   0x0800020c (BUFSZ 12, BLKSZ 2, NO_UPDATE), CP_RB_RPTR_ADDR (read back, refuse on mismatch), CP_SQE_CNTL = 1,
   ring: `CP_ME_INIT` (0x70c80008 + 0x2f, 3, 0x20000000, 0 x5) + `CP_WHERE_AM_I` -> idle; `CP_SET_SECURE_MODE 0`
   (0x70e60001, 0) -> idle; `CP_NOP` + `CP_MEM_WRITE` (0x703d8003, 0x01020100, 0, 0xC0FFEE01) -> idle, then the
   scratch word is polled 100 ms. Idle = CP_RB_RPTR == WPTR, shadow == WPTR, RBBM_STATUS only bit 0, no hang irq.
4. Fault dump (GPU CB FSR/FAR/FSYNR0, GFSR/GFSYNR0/GFSYNR1 = SID), OOB clear, M12 shutdown, CM3 reset, countdown.

**Expected screen (success, `boot-m13-cp.img`):** `M12 GX 710MHz GPU_SET ok RBBM st=00000001 gmu cb1`;
`ZAP n=3 hash=1 load=1 rel tam=1000/2000`, `ZAP meta 1A1C pa=... w0=464C457F`; `SCM conv a0=00000000
a1=00000001 ... ok`, `SCM disp`, `SCM sup13` (info), `SCM init a0=00000000 a1=00000000 ... ok`, `SCM mem ... ok`,
`ZAP copiado 07B0 bytes, erradas=0`, `SCM auth ... ok`, **`ZAP ACEITO PELO TRUSTZONE (PAS 13)`**,
`SMMU pos-zap smr validos=2 cb on=02 cr0=...` (anything else = TZ touched the GPU SMMU); `SQE ver=207
palavras=8546 erradas=0`, `SMMU cb0 sid0 smrN gmu cb1 cr0 X>Y` (Y has bit 10), `ATS anel ... sqe+8000 ...`,
`INIT escritas=41 nao lidas=4 diferentes=N` (N > 0 is logged, not fatal), `CP rptr=0 wptr=0 cntl=0800020C`,
`ME_INIT rptr=12 sombra=12 wptr=12 st=00000001 ...us ok`, `SECURE0 rptr=17 sombra=17 wptr=17 ... ok`,
`MEMWRITE rptr=26 ... ok`, `MEM valor=C0FFEE01 ...`, **"O CP ESCREVEU C0FFEE01 NA RAM PELA SMMU"** = success;
then `VBIF halt ack, solto`, `SLUMBER ack`, `DESLIGA slumber ok (0xf)`, `GX fim ...`, countdown.
`boot-m13-zap.img` ends after `ZAP ACEITO` with `13 M13_STOP: CP nao iniciado` and the same shutdown.

**Failure readings:** `SCM conv` not ok = TZ does not speak SMC64/SIP to us (QHEE?); `SCM init ... RECUSA` = TZ
refused the metadata (wrong OEM signature / PAS 13 state); `SCM auth ... RECUSA` = image refused; `ZAP ...
erradas` = carve-out not writable; `TEMPO` lines after ME_INIT = the CP did not consume the ring (with `FALHA
fsr=... far=...` = GPU SMMU fault at that VA; `gsyn1(sid)=` = an unmatched stream ID, e.g. a secure SID);
`sombra=0` with `rptr` advancing = CP runs but its writes do not land; `valor=00000000` after `MEMWRITE ok` = the
write went elsewhere.

**Risks (M13 on top of M12's sec. 6):**
- TZ: three PAS SMCs + three info SMCs. A rejected image is harmless (Linux tolerates it). An accepted zap stays
  loaded until the PSCI reboot. A wrong address/size in mem_setup is refused by TZ, not fatal.
- Memory outside the GPU block: the carve-out 0x8c415000..0x8c4157af (written before auth, never read or written
  after: TZ locks it - touching it would be an XPU violation -> Qualcomm CrashDump, long power press); the shim's
  own 4K-aligned metadata buffer is lent to TZ during init_image (not touched afterwards). If ABL/TZ already
  protected the carve-out, the Z6 write faults (panic screen or CrashDump).
- GPU SMMU: a second context bank (CB 0), one more SMR/S2CR (SID 0), sCR0.USFCFG set (Linux's default; skipped if
  the SID 4 rule is missing). If the GMU uses some other SID that used to bypass, it now faults (`FALHA gsyn1`).
- Independent review (2026-11-10, Opus, against the v6.6 sources): no crash finding; fixed: ATS gated on ATOS,
  USFCFG gated on SID 4/5 rules, no CB fault read before gpu_ctx, convention probe requires x1 == 1. Open: WAITQ_SLEEP
  (x0 = 2) is reported as a failure (no resume), as is INTERRUPTED after 1M resumes.
- GPU DMA: only inside the three mapped buffers (SQE read-only); unmatched SIDs fault instead of bypassing.
  Never touched: RBBM_SECVID_TRUST_CNTL (secure; Linux writes it only without a zap), the apps SMMU, PMIC, CPU RPMh.
- A CP that never goes idle is left as is (Linux does not stop the CP either); the M12 shutdown collapses GX.
  Its VBIF halt may then report `SEM ack` (logged, the stage continues).

## Device results

- 2026-11-10 M10 (boot-m10-gmu.img): **GMU firmware runs, init=0xBABEFACE**; 8192 words loaded, 0 errors; DTCM readback 600D0001 0A110002; CX gdscr=F8222000. cmd-db on the OP6: **gfx.lvl = 0x30050** (levels 000,010,030,040,080,0C0,100,140,180,1A0), cx.lvl = 0x30000, mx.lvl = 0x30010, **0x30080 = xo.lvl** (so mainline's GPU PDC TCS address 0x30080 is NOT gfx on this firmware; the -rpmh variants must use 0x30050 before being tried). BW BCMs: MC0 SN7 ACV SH2 SN15 SN3.
- 2026-11-10 M11 (boot-m11-hfi.img): **GPU SMMU + HFI work**. GPU SMMU idr0=48016605 (5 SMR, 5 CB, 4K pages, ATOS), no SMR valid before; CB with TTBR0=0x8009f000, cbar=0001F300 sctlr=1027 tcr=00802019; SID 5 and 4 mapped; ATOS VA 0x60009000 -> HFI buffer PA 0x80092B80 OK. GMU init=0xBABEFACE under the SMMU, HFI status=1, **GMU firmware v1.6 (0x10060000)**; H2F INIT, FW_VERSION, PERF_TABLE, BW_TABLE, TEST all ACKed (seq 1-5). No CrashDump.
- 2026-11-10 M12 written (`boot-m12-gx.img`, `boot-m12-gx-rpmh.img`), not yet run.
- Next: M12 = GX power through the GMU OOB (GPU core registers readable: RBBM_STATUS, CP_ALWAYS_ON), then M13 zap via TZ + SQE microcode + CP NOP/MEM_WRITE through the GPU SMMU.
- 2026-11-10 M12 (boot-m12-gx.img): **GX ON, GPU core responds.** RPMH ctrl=0xF11; boot vote opp 710 MHz (gx idx 9, mx 4, lvl 0x1A0); OOB BOOT ack 916 us; GX gdscr=F8222000 cf=00070400, SPTPRAC st=0x28 on; HFI INIT/VER/PERF/BW ok; OOB DCVS ack 44 us, DCVS return 0; OOB GPU ack 9 us. Core reads: RBBM_STATUS=0x00000001, INT0=0, CP fault 0, CP st1=0x01000000, VBIF version 0x200A0000, SQE cntl/base 0. Always-on: GMU d=1920078 in 100003 us and **CP d=1920074 (counting, unlike the Poco F1 report)** = 19.2 MHz. Shutdown: VBIF halt ack, SLUMBER ack 50 us (p=0xF), GX GDSC off (gd=00222801). No CrashDump.
- Next: M13 = zap shader via TZ (PAS id 13, a630_zap.mbn into gpu@8c415000 region / its mdt segments), SQE microcode a630_sqe.fw, CP init + ringbuffer NOP / CP_MEM_WRITE through the GPU SMMU.
- 2026-11-10 M13 zap-only (boot-m13-zap.img): **zap shader ACCEPTED by TrustZone (PAS 13).** ELF n=3 hash=1 load=1 rel, size 0x1000/0x2000; metadata 0x1A1C at PA 0x800A9000. SMC: convention probe a1=1 ok; is_call_available a1=1 ok; **pas_is_supported(13) returned a1=0 (informational "RECUSA" — yet init_image 31915 us, mem_setup 176 us and auth_and_reset 1277 us all returned 0/0)**, so do not gate on is_supported on this firmware. Segment copied 0x7B0 bytes, 0 mismatches. Post-zap GPU SMMU: 2 valid SMRs, CB on=02, sCR0=00200626. Clean slumber, no CrashDump.
- 2026-11-10 M13 full (boot-m13-cp.img): **the CP executes our commands.** Zap accepted again (PAS 13). SQE ver=0x207, 8546 words, 0 errors (PA 0x800B6000, GPU VA 0x01000000); GPU CB0 for SID 0, GMU on CB1; ATS ring and SQE+0x8000 translate; INIT 41 writes (5 write-only, 2 differ, logged); CP_PROTECT 0, RBBM st=1. Ring: ME_INIT rptr 12 (17 us), SET_SECURE_MODE 0 rptr 17 (25 us), NOP + CP_MEM_WRITE rptr 26 (4 us), shadow == wptr each time; **memory at GPU VA 0x01020100 (PA 0x800C7100) = 0xC0FFEE01 after 8 us**. Clean VBIF halt + slumber, GX off. No CrashDump.
- Shim GPU bring-up phase complete (M10-M13). Next phase: XNU kernel GPU driver (GMU/HFI/GX/zap/CP/ringbuffer + IOKit user client for submissions, GPU SMMU 4K pages under 16K XNU pages), then userland Mesa freedreno + EAGL bridge in backboardd (see s9/ESTUDO-2026-10-25-gpu.md; HackiOS APSL kernel + MIT userland as references).

## XNU driver credits (code origin)

- `xnu/patches/0107-op6-gpu-bringup.patch` (`bsd/dev/sweet_gpu.c`, `iokit/Kernel/arm/SweetGPU.cpp`) is a port of
  this project's shim code `op6/shim/gpu.c`, `m10.c`, `m11.c`, `m12.c`, `m13.c`, `rpmh.c` (ios-op6, MIT). No code
  from HackiOS (oosexgang/hackios-xnu-kernel @63fc2b4, `drivers/` APSL 2.0 - reusable with notice, not used),
  guacamole (no licence: facts only), Linux (GPL: facts only) or Bergamota (no licence: facts only).
- Design facts taken from the survey (GPU-XNU-PLAN.md sec. 1): ADT firmware node (guacamole), msm uAPI over
  IOUserClient + fake libdrm (HackiOS, guacamole), Mesa gallium freedreno has no KGSL backend (Mesa main
  `src/freedreno/drm/` = msm + virtio, checked 2026-11-10).
- 2026-11-10 **G1 in XNU (boot-G1.img, kernel run 42831415559, patch 0107, `sweetgpu=1 sweetgpudelay=30`): PASS.** iOS boots normally (USB net after ~30 s); `ioreg -c SweetGPU`: SweetGPUStage = 7, SweetGPUMagic = 0xC0FFEE01 (shown sign-extended as 18446744072652582401: store it as an unsigned 32-bit number), SweetGPUState = "off". The SWEET GPU dmesg lines had already rotated out of the 1225-line buffer (backboardd IOAcceleratorES/AGXAccelerator matching spam) — use the persistent log ring for the full trace. backboardd probes IOAcceleratorES / IOAcceleratorESLegacy / AGXAccelerator: the hooks for G4.
- 2026-11-10 **G2 in XNU (boot-G2.img, kernel run 42847871854, patch 0108, `sweetgpu=2 sweetgpudelay=30`): PASS.** `ioreg -l -c SweetGPU`: `SweetGPUStage = 7`, `SweetGPUMagic = 3237998081` (`0xc0ffee01`, 64-bit `OSNumber` fix verified), `SweetGPUFirmwareGMU = 268828672` (`0x10060000`), `SweetGPUFirmwareSQE = 519` (`0x207`), `SweetGPUState = "on"`, `IOUserClientClass = "SweetGPUUserClient"`. Userland `/var/root/sweetgpu-test`: `GET_PARAM` returns `GPU_ID=630`, `GMEM_SIZE=0x100000`, `CHIP_ID=0x6030002`, `MAX_FREQ=710000000`, `TIMESTAMP` advancing at 19.2 MHz, `FAULTS=0`, `HIGHEST_BANK_BIT=15`; `GEM_NEW` + `GEM_INFO` + `IOConnectMapMemory64` maps `cmd BO` (`handle=1, iova=0x10000000, size=0x4000`) and `dst BO` (`handle=2, iova=0x10004000, size=0x4000`); single IB submit (`CP_NOP` + `CP_MEM_WRITE 0xc0ffee02` via `CP_INDIRECT_BUFFER`) + `WAIT_FENCE` completes in 69 us with `dst[0]=0xc0ffee02`; 100-submit loop (`fences=2..101`) completes in 5303 us (53 us/submit); second invocation (`-n 1000`) reclaims freed BO IOVAs `0x10000000`/`0x10004000` after `clientClose` TLB invalidation and completes 1000 submits (`fences=103..1102`, wrapping the 32 KiB / 8192-dword ringbuffer) in 51207 us (51 us/submit) with `FAULTS=0`. UAPI dispatch and ring submission reference `oosexgang/hackios-xnu-kernel` @ `63fc2b4` (`drivers/gpu/hakios.adreno.c`, `drivers/gpu/hakios.gpu.cpp`, APSL 2.0) and Mesa's `drm-uapi/msm_drm.h` (MIT).
- 2026-11-10 **G3 in XNU (`boot-G3.img`, `--gpu-dma 0xa0000000,0x04000000` 64 MiB carve-out, Mesa 25.0.7 `libOSMesa.8.dylib` + `op6/mesa/sweet-drm/` + `/var/root/sweet-gl-test`): PASS.** `sweet-gl-test` opens `SweetGPU` via `sweet-drm` (`drmOpenWithType("msm")`, version 1.7 softpin), initializes an OpenGL ES context (`freedreno; FD630; OpenGL ES 3.2 Mesa 25.0.7 (the GPU)`), verifies 256x256 FBO `glClear(0.2, 0.4, 0.8, 1.0)` + `glReadPixels` (`pixel[0]=51 102 204 255 (CLEAR OK)` in 491 us), compiles vertex and fragment shaders with `ir3` and draws an RGB triangle (`corner 26 26 51 255, near red 213 22 20 255, near green 22 213 20 255, near blue 14 15 226 255; 17086 of 65536 pixels = 26%`, saved to `op6/out/sweet-gl-triangle.ppm`/`.png`), and passes both 100-frame (`207 us/frame`) and 500-frame (`180 us/frame`, ~5555 fps) benchmark loops with `faults 0; error 0: MESA DRAWS WITH THE GPU`, followed by `/var/root/sweetgpu-test -n 100` reclaiming `0x10000000`/`0x10004000` (`fences=609..709`, 52 us/submit). `op6/mesa/` and `s9/ios-tools/sweet-gl-test.c` are adapted from HackiOS `tools/mesa/` (`Copyright (c) 2026 OOSEX GANG`, MIT) and `aTechGeekDev/ios-apollo` `userland/mesa/` (`Copyright (c) 2026 Leo Malik / dipa-studio`, MIT).
- 2026-11-14 **G4 first runs.** CI now builds Mesa + the CA bridge (op6-mesa on macos-26, run 43580196150); the unverified prebuilt libs Gemini had installed were moved to /var/root/old-libs on the phone. G3 re-passes with the CI libs (186 us/frame). Kernel 0108/0109 build OK (run 43577964732). 0109's sweetinject added the env var in process_signature(), after exec_copyout_strings(): never reached the process; launchd also ignores DYLD_* in launchd.plist for system daemons. **Patch 0110** moves it into exec_extract_strings() (kernel run 43581416374, boot-G4b.img): `SWEET: injected DYLD_INSERT_LIBRARIES=/usr/local/lib/libsweet-ca.dylib into backboardd` — works. libsweet-ca now dup2()s backboardd's stderr to /private/var/tmp/sweet-gl.log (but the launchd-started backboardd did not write it: probably sandbox/tmp path — to check).
- Result: **CoreAnimation renders through the bridge** (crash trace: CA::OGL::render_layers -> GLESContext::bind_image_impl -> update_image -> my_glTexImage2D -> libOSMesa), Mesa compiler threads ir3q0-3 / gdrv0 run inside backboardd. backboardd crashed once (SIGSEGV, KERN_INVALID_ADDRESS 0x14) inside libOSMesa during that glTexImage2D (a CPU CA::Render::Image texture upload), then restarted and stayed up. Mesa from CI is stripped: next, build a non-stripped Mesa (keep symbols / -g) to symbolize, and log the format/type/size my_glTexImage2D passes (CA images include 'LA08' and planar 'b3a8' surfaces).
- 2026-11-14 **Observability for G4 (written, not yet built or run).** (1) Kernel patch **0111** (`xnu/patches/0111-op6-gpu-counters-klog.patch`, `SweetGPU.cpp` only): IORegistry counters on the SweetGPU service — `SweetGPUSubmits` (successful GEM_SUBMITs), `SweetGPUImports` (IOSurfaces newly mapped into the GPU by selector 0x7e), `SweetGPUClients` (GPU user clients open now), `SweetGPUOpens`, `SweetGPULastClient` (process name), `SweetGPUFaults` (SMMU faults + hangs), `SweetGPUHangs` (failed submits + fence waits >= 500 ms that timed out), `SweetGPULogLines`/`SweetGPULogDropped`; atomic counters, published with no GPU lock held, at most every 256 events or 1 s, and on every open/close. New selector **0x7f kSweetGpuLog**: structureInput = text (<= 512 bytes, NUL optional) printed as `sweet-gl[<process>]: <text>`, 200 lines/s per process (the rest dropped and counted, "N lines dropped" said the next second); handled before the GPU-ready and size checks. IOServiceOpen **type 'SLOG' (0x534c4f47)** gives a log-only client: no wait for the GPU, no BO table, only 0x7f, not counted in SweetGPUClients. (2) Bridge: `op6/ca-bridge/sweet_klog.c` (lazy log-only open of `SweetGPU`, retried at most once a second, gives up if 0x7f fails = older kernel); `say()`/`eagl_trace()` in eagl.c and sweet_ca.c send every line to the kernel log as well as stderr; sweet_ca tries `/private/var/tmp/sweet-gl.log` then `/var/mobile/Library/Logs/sweet-gl.log` and reports which (or the errno) plus its uid; eagl.c logs every glTexImage2D/glTexSubImage2D *before* calling Mesa for the first 64 calls and every new (internal format, format, type), with the internal format actually passed to Mesa. (3) Mesa built `debugoptimized` (-O2 -g, NDEBUG, `-Dstrip=false -Dsplit-debug=disabled`): artifact `lib/libOSMesa.8.dylib` (`strip -S` only: all function names stay), `unstripped/libOSMesa.8.dylib`, `dSYM/libOSMesa.8.dylib.dSYM` (build.sh fails if the UUIDs differ or the DWARF is < 1 MB).
  **Test:** boot a kernel with 0111, install the new `lib/*` (libOSMesa, libsweet-gl, libsweet-ca), restart backboardd. `ioreg -l -c SweetGPU | grep SweetGPU` — GPU really used by CoreAnimation = `SweetGPULastClient = "backboardd"`, `SweetGPUClients >= 1` and `SweetGPUSubmits`/`SweetGPUImports` growing while the UI changes (softpipe fallback = no submits). `dmesg | grep sweet-gl` (and the persistent log ring) for the bridge's lines: renderer string, which log file opened, `upload #N NEW glTexImage2D(...)` — the last upload line before a crash is the one that kills Mesa. **Symbolize** a `.ips` from the phone (check its `usedImages` UUID for libOSMesa.8.dylib == `dwarfdump --uuid dSYM/...`; a dylib's __TEXT vmaddr is 0, so address = imageOffset):
  `tail -n +2 backboardd-*.ips | jq -r '.usedImages as $u | .threads[] | select(.triggered) | .frames[] | select($u[.imageIndex].name == "libOSMesa.8.dylib") | .imageOffset' | xargs printf '0x%x\n' | llvm-symbolizer --obj=out/dSYM/libOSMesa.8.dylib.dSYM/Contents/Resources/DWARF/libOSMesa.8.dylib` (on macOS: `xcrun atos -o <same DWARF file> -l 0x0 <offsets>`).
  **Risks:** a log-only open on a kernel *without* 0111 becomes a normal client whose start() waits up to 60 s for the GPU (sweet_ca waits for SweetGPU anyway) and then 0x7f fails -> the bridge stops klogging; per-line IOConnectCall cost (~µs) while gltrace is on; libOSMesa with symbols is larger (only the debug map is stripped).
- 2026-11-14 **G4 works on device** (kernel run 43583724699 with 0110 sweetinject + 0111 counters/klog, Mesa/bridge run 43583728286 with symbols + dSYM, boot-G4c/G4d.img). backboardd started at boot renders CoreAnimation through libsweet-gl -> Mesa freedreno FD630: `SweetGPULastClient = backboardd`, home screen, Spotlight blur, Control Center, Settings drawn correctly (user video). Crash at 64 MiB carve-out (boot-G4c): symbolized with the dSYM = `my_glTexImage2D -> st_TexImage -> fd_resource_create -> fd_bo_new -> fd_bo_heap_alloc` returns NULL (GPU memory exhausted) then `fd_bo_ref(NULL)` (Mesa does not check). With `--gpu-dma 0xa0000000,0x08000000` (128 MiB, boot-G4d.img): 5+ min of use, 7447 submits, 0 faults, 0 hangs, no backboardd/SpringBoard crash. Frame time 38-63 ms (~16-26 fps, same feel as the CPU path): every frame is rendered off-screen and "1 picture copied out, 0 drawn in place" — the final copy to the display is the bottleneck.
- Next: (1) draw in place / zero-copy into the AppleCLCD framebuffer surface (or import the display IOSurface with selector 0x7e) instead of copying the picture out each frame; (2) GPU memory: make GEM_NEW failure clean (sweet-drm/Mesa NULL check) and add BO eviction or a larger GPU VA/page-table pool (driver pool = 64 page-table pages ~ 128 MiB); (3) IRQs instead of polling (G5).
- 2026-11-14 **G4e: zero-copy present — why SweetGPUImports = 0, and the fix (written, not yet built or run).**
  **Root cause (from code + G4d log): the import was never asked for.** Selector 0x7e's request struct differs
  between kernel and userland. Kernel (0109 `struct sweet_surface_request`): `surface u32, handle u32, size u64,
  offset u64, rowBytes, width, height, pixelFormat, planes, pad` = 48 bytes. sweet-drm (`op6/mesa/sweet-drm/xf86drm.h`
  `struct sweet_drm_surface`): same but `offset` is **u32** = 40 bytes. The kernel accepts any size <= its union and
  copies its 48-byte answer back truncated to 40, so Mesa's describe call (`osmesa_surface_resource()` ->
  `sweet_drm_surface()`, handle 0xffffffff) reads everything after `offset` 4 bytes late: `row_bytes` = 0 (high half of
  the u64 offset), `width` = 3328 (bpr), `height` = 828, `pixel_format` = 1792 (the height), `planes` = 0x42475241
  ('BGRA'). `pixel_format != 'BGRA' || planes != 0` -> NULL -> `glEGLImageTargetTexture2DOES` fails (GL error) ->
  `surface_mirrored()` and `surface_shared()` fail -> every frame goes through `target_to_surface()`'s
  `glReadPixels` (45 ms of the 38-63 ms frame: "45678 copying out"). `drmPrimeFDToHandle()` (the real import) is
  never reached, which is exactly `SweetGPUImports = 0` and "0 IOSurfaces were the textures' memory, 119 had to be
  copied". Nothing in the kernel logged 0x7e, so the log could not show it.
  **Where the display surfaces live:** on the OnePlus 6 there are no display slots (`SWEET FB: zero-copy unavailable
  (splash 0x9d400000 1080x2280)`), so backboardd's 828x1792 'BGRA' surfaces (bpr 3328, 5,963,776 bytes) are
  `IOBufferMemoryDescriptor`s of ordinary XNU RAM (wired 16K pages, anywhere in 0x80000000..0x27d800000). 0109's
  import path already walks the descriptor (`getPhysicalSegment` per 4K) and installs 4K PTEs at GPU VA 0x20000000+
  (1.5 GiB window, own free list); the GPU context bank's TCR2 PASize = 36 bits covers all of RAM; only page-table
  pages come from the carve-out pool (`GPU_PT_PAGES` = 512 = 2 MiB, reserved at bring-up), no BO pages. freedreno's
  `resource_from_handle` pitch check passes (3328 = 52 x 64, a630 pitchalign 64 B).
  **Changes:**
  - Kernel `xnu/patches/0112-op6-gpu-import-fix-klog.patch` (SweetGPU.cpp only): 0x7e accepts the 48-byte layout and
    the old 40-byte layout (answered in kind: a 0112 kernel fixes even the G4d libOSMesa), anything else refused and
    logged; every result logged (all for the first 96 lines, then 1/256 successes, 1/16 refusals):
    `SWEET GPU: import backboardd surface N describe: 828x1792 fmt 0x42475241 bpr 3328 planes 0 size 5963776 -> ok (...)`,
    `SWEET GPU: import backboardd surface N import: ... -> handle H iova 0x20000000, 1456 pages from pa 0x..., mapped`,
    `... import: already handle H at iova ...`, or `-> REFUSED, <why> (page +0x.. pa 0x..)` (no such IOSurface / no
    memory behind it / larger than 64 MiB / BO table full / GPU VA for surfaces used up / a page with no usable
    physical address / GPU page-table pool exhausted). New ioreg counters `SweetGPUImportFails`, `SweetGPUImportedKB`
    (surface memory mapped into the GPU now; drops on GEM_CLOSE). Present side: `SWEET GPU: present of surface N:
    GPU fence F not done after 50 ms, frame skipped (k times)` (1/64).
  - `op6/mesa/sweet-drm/xf86drm.h`: `struct sweet_drm_surface` = the kernel's 48 bytes (u64 offset + pad), with a
    static assert. Works with 0109+ kernels.
  - `op6/ca-bridge/sweet_gl_osmesa.c`: one line on first use (`IOSurfaces as GPU memory: glEGLImageTargetTexture2DOES
    found, renderer FD630: will be tried (selector 0x7e)`), then each `IOSurface N as GPU texture memory (EGLImage):
    ok | REFUSED, copied instead, GL error 0x..` (first 32, then 1/256 ok, 1/16 refused). Kernel "describe ok" with no
    "import" line + bridge REFUSED = Mesa refused (format/planes or freedreno pitch/size).
  - `op6/ca-bridge/eagl.c`: surfaces that are only *shown* (texImageIOSurface) stay copied by default: apps write them
    with the CPU without moving the seed, and the kernel cleans a surface for the GPU only when its seed moves, so
    zero-copy there could show stale pixels. Boot-arg `glshareshown=1` enables it (test). The display path
    (attachImage -> `surface_mirrored()`: CA draws into a Mesa texture, the GPU blits it into the imported surface,
    "drawn in place") is the one this fixes; it was already written that way (HackiOS/apollo: direct rendering into
    the surface lost parts of frames with framebuffer fetch).
  - Fallbacks unchanged and automatic: any kernel refusal or Mesa NULL -> GL error -> copy path as in G4d; boot-args
    `glcopy=1` (no sharing at all) or `glblit=0`.
  **Expected on device:** `ioreg -l -c SweetGPU | grep SweetGPU`: `SweetGPUImports` > 0 and growing slowly with
  re-attachments (~50 per 1600 frames in G4d), `SweetGPUImportFails` = 0, `SweetGPUImportedKB` ~ 5824-17472 (1-3
  display surfaces); `dmesg | grep -e 'SWEET GPU: import' -e 'sweet-gl'`: describe ok + import mapped lines for surfaces
  2/3, `-attachImage: texture T is IOSurface 2 ('BGRA' 828 x 1792, 3328 bytes a row): a texture, copied into its memory
  by the GPU`, frame lines `0 pictures copied out, 1 drawn in place`, "copying out" ~0, frame time ~8-15 ms (G4d: 56 ms
  of which 45 copying out); summary `N IOSurfaces were the textures' memory` > 0; `SweetGPUFaults` = 0. Per-frame CPU
  left: the kernel present (828x1792 -> 1080x2280 scale, own thread, unchanged) plus a clean+invalidate of the 5.7 MiB
  surface at the submit after each seed change (0109 `sweet_gpu_to_memory`, ~1 ms) and after the fence at present.
  **Test plan:** rebuild the kernel (xnu workflow; it copies all `xnu/patches/*.patch`, 0112 included) -> boot-G4e.img
  with the same ADT `op6/out/op6-gpu128.adt` and the same boot-args as G4d. Step 1: kernel only, G4d userland libs
  (exercises the 40-byte compat path). Step 2: op6-mesa run (new xf86drm.h + bridge logs), install `lib/*` to
  `/usr/local/lib`, restart backboardd. If imports are refused, the REFUSED reason line says which layer; if pictures
  are wrong (tearing / stale rows), compare with `glcopy=1`.
  **Stretch goal (GPU scaled blit into the panel at 0x9d400000) not done:** the panel framebuffer's stride is 4320
  bytes (1080 x 4), not a multiple of 64, so freedreno's `resource_from_handle` refuses it as a render target
  (pitchalign 64 B on a630); it would need a 64-aligned scan-out buffer (display engine stride change) or a kernel-side
  GPU blit path. The bridge's `scan_frame()` (xnu 0159 flip frames) only applies to >= 1000x2000 canvases.
  **Risks:** first time the GPU writes RAM outside the carve-out (SMMU faults would show as SweetGPUFaults and
  `SWEET GPU` fault lines; a wrong PTE could hang the GPU -> backboardd restart); unmaps wait up to 200 ms for the
  BO's fence before the surface pages are released.
- 2026-11-14 **G4f: 'b3a8' display surfaces drawn in place (written, not yet built or run).**
  **Root cause (G4e log + code): the display surfaces are 'b3a8', and every layer refused 'b3a8'.** G4e (kernel 0112,
  48-byte struct) still has `SweetGPUImports = 0`, no `SWEET GPU: import` line and no bridge `IOSurface N as GPU texture
  memory` line after the 0112 fix, because the import was never even tried for the screen: backboardd's frame surfaces are
  `IOSurface 2/3 ('b3a8' 828 x 1792, 3328 bytes a row)` (G4e 219.36 s / 219.44 s), not 'BGRA'. Three rejects in a row:
  1. `op6/ca-bridge/eagl.c` `surface_mirrored()` and `surface_shared()` returned false for `image->deep` ('b3a8') before
     calling `sweet_gl_surface_texture()` -> no EGLImage, no 0x7e, silent -> `target_to_surface()` glReadPixels every frame
     ("1 picture copied out, 0 drawn in place", ~45 ms of 50-57 ms). (Same in ios-apollo's bridge: it never zero-copied 'b3a8' either.)
  2. Mesa `osmesa_surface_resource()` (op6/mesa/patches/0001) accepted only `pixel_format == 'BGRA' && planes == 0`.
  3. Kernel 0x7e describe reported the surface-level offset/rowBytes only (no plane-0 geometry).
  Separately, `-attachImage: ... an IOSurface of 'b3a8': not done` (G4e 146.92 s, 146.99 s; textures 229/234) came from the
  `deep` test asking IOSurface.framework's per-plane accessors (plane 0 bpe == 4, plane width/height == surface's), which
  our kernel's lock result does not reliably feed (image_base() already avoids them: apollo boot 36 "0 bytes a row"); the
  texture then had no storage and CoreAnimation's draws into its FBO failed: `glDrawElements: error 0x506` x2.
  **'b3a8' layout here [V = from code/logs, I = inferred]:** Apple's kCVPixelFormatType_30RGBLE_8A_BiPlanar (plane 0 RGB
  10:10:10 in 32-bit words, plane 1 8-bit alpha), but on this system plane 0 is **BGRA8**: the bridge draws it as a
  blue-first 8-bit texture and SweetFramebuffer (0109 `AppleCLCD::present`/`presentScaled`) reads only `planes[0]`
  (offset, bytesPerRow, bpe 4, 1x1 elements) and copies 4-byte pixels to the BGRA panel [V]; plane 1 is never read by the
  scan-out [V] and stays as the kernel's bzero left it (opacity 0) - no consumer here needs it. Sizes for 828x1792
  (SWEET SURF create, size 7454720): plane 0 = 3328 x 1792 = 5963776 at +0 [V: bpr 3328 logged, sizes add up exactly],
  plane 1 = 832 x 1792 = 1490944 at +0x5b0000 (bpr 828 rounded to 64, 1 byte/pixel) [I; 0113's describe line prints it].
  **Changes:**
  - `op6/ca-bridge/eagl.c`: new `share_refused()` (one list of reasons for mirror/shared); 'b3a8' (deep) is allowed in both
    -> the display path becomes: CA draws into its own texture, the GPU blits into an EGLImage texture over plane 0 of the
    imported surface (`surface_mirrored`, "drawn in place"); glblit=0 renders into plane 0 directly. `deep` is now decided
    from the surface's own numbers only (`'b3a8'`, bpr >= 4 x width, bpr x height <= alloc size). Refused attaches give the
    texture BGRA storage of the surface's size (no more 0x506) and forget a stale image of that texture. `refusal()` says
    every reason once per surface + format + path (`sweet-gl: IOSurface N ('fmt' W x H, B bytes a row, P planes, S bytes)
    mirror|shared|shown|attach: not in place, <why>`; per format only for reasons common to all such surfaces: fmt-0
    opacity-first pictures, shown-surface policy). Copy fallback unchanged and automatic.
  - Mesa `op6/mesa/patches/0003-osmesa-planar-b3a8.patch` (after 0001): accepts 'b3a8' with 2 planes as a
    B8G8R8A8 texture of plane 0 (offset/row_bytes from the kernel; whole surface is the BO); checks rows inside the
    surface and 64-byte pitch; every refusal said once a surface in the kernel log via `sweet_drm_log()` (selector 0x7f):
    `sweet-gl[backboardd]: mesa: IOSurface N not taken as GPU memory: <why> (fmt.. planes.. WxH, bpr at +off, size, ...)`.
  - `op6/mesa/sweet-drm/`: `SWEET_DRM_PLANE0`, `SWEET_DRM_FORMAT_*`, `sweet_drm_log()`.
  - Kernel `xnu/patches/0113-op6-gpu-import-planar-b3a8.patch` (SweetGPU.cpp only, optional): for a planar surface whose
    plane 0 is 4 bytes/pixel, 1x1 elements, the surface's size, rows inside the allocation, the 48-byte 0x7e answer gives
    plane 0's offset/rowBytes and `pad = 'PL0B'` (0x504c3042); else unchanged. Old userland ignores pad; the 40-byte layout
    is answered as before. The describe log line gains `, p0 +0x0 bpr 3328 bpe 4 828x1792, p1 +0x5b0000 bpr 832 bpe 1 ...
    (plane 0 described)`. The import still maps the surface's own pages (all planes, 1820 x 4K for 7454720 B); no
    carve-out BO pages.
  **Rebuild:** op6-mesa (libOSMesa with 0003 + sweet-drm, libsweet-gl with the new eagl.c) is required; the kernel with
  0113 is optional (without it Mesa uses the surface offset 0 / bpr 3328, which is plane 0 for IOSurface.framework's layout,
  the same assumption the copy path and image_base() already make).
  **Expected on device:** bridge `-attachImage: texture T is IOSurface 2 ('b3a8' 828 x 1792, 3328 bytes a row): a texture,
  copied into its memory by the GPU` (and 3), `IOSurface 2 as GPU texture memory (EGLImage): ok`; kernel `SWEET GPU: import
  backboardd surface 2 describe: 828x1792 fmt 0x62336138 ... -> ok` then `import: ... -> handle H iova 0x2..., 1820 pages ...
  mapped`; frame lines `0 pictures copied out, 1 drawn in place`, "copying out" ~0, frame time ~8-15 ms; `ioreg -l -c
  SweetGPU`: `SweetGPUImports` >= 2, `SweetGPUImportFails` 0, `SweetGPUImportedKB` ~14560 (2 x 7280), `SweetGPUFaults` 0;
  no `glDrawElements: error 0x506`; any remaining 'not done' now has its reason + plane dump.
  **Test plan:** (1) op6-mesa run, install `lib/*` to /usr/local/lib on the G4e kernel, restart backboardd; check the lines
  above (`dmesg | grep -e 'SWEET GPU: import' -e 'sweet-gl'`). (2) Kernel with 0113 -> boot-G4f.img, same ADT/boot-args:
  describe line shows the planes and "(plane 0 described)". (3) Compare picture with `glcopy=1` (tearing/stale rows, colours:
  red/blue swapped would mean plane 0 is not BGRA). (4) Snapshot-heavy UI (app switcher, Control Center) for 'not done' lines.
  **Risks:** the GPU now writes backboardd's screen surfaces directly (SMMU faults -> SweetGPUFaults; a wrong offset/pitch ->
  garbled screen: fall back with glcopy=1); per frame the kernel cleans the 7.1 MiB surface at the submit after each seed
  change (~1-2 ms) and after the fence at present; plane 1 is not written (opacity 0) - harmless for our scan-out, but any
  future consumer of 'b3a8' opacity (a real display engine plane, screenshots that composite plane 1) would see transparent
  pixels; refused attaches now get empty storage (draws land in a texture nobody shows instead of failing).
- 2026-11-14 **G4 zero-copy works on device (boot-G4f.img: kernel run 43590108694 with 0112 + 0113; op6-mesa run 43590111044 with the 48-byte 0x7e struct, Mesa 0003 'b3a8' plane-0 support and the eagl.c sharing fix).** Kernel imports backboardd's 'b3a8' screen surface (828x1792, p0 +0x0 bpr 3328, p1 +0x5b0000 bpr 832, 1820 pages mapped from the surface's own RAM, e.g. pa 0x11bd04000 -> iova 0x21620000); bridge `IOSurface 20 as GPU texture memory (EGLImage): ok`; frames "0 pictures copied out, 1 drawn in place", **10-19 ms per frame (was 47-64 ms)**, SweetGPUImports 18, ImportFails 0, Faults 0, Hangs 0. User: colours correct, much smoother.
- 2026-11-14 **G4g: GPU memory - counters, full page-table pool, clean out-of-memory (written, not yet built or run).**
  **Facts from the code (before this change):** the page-table pool was already 512 pages (2 MiB, 0109), not 64 (0108):
  the "~128 MiB" limit in the earlier Next line is stale. Pool pages are never freed (pt_unmap only clears PTEs; tables
  are reused when the same VA is mapped again), BO VA = 0x10000000 + carve-out page index (bounded by the carve-out) and
  surface VA is first-fit from 0x20000000 (1.5 GiB window), so the worst case is bounded: 1 L1 + 2 L2 + 1 L3
  (SQE/ring/scratch) + 1 L3 per 2 MiB of carve-out + 768 L3 for the surface window. With 512 pages a surface VA
  high-water mark above ~820 MiB could exhaust it; with 128 MiB it was never the cause. **The G4c crash at 64 MiB was the
  carve-out itself running out** (GEM_NEW -> kIOReturnNoMemory -> ENOMEM -> fd_bo_new NULL inside fd_bo_heap_alloc ->
  fd_bo_ref(NULL)). GEM_NEW already left no partial state (bitmap untouched on failure, IOMemoryDescriptor failure frees).
  **Changes:**
  - Kernel `xnu/patches/0114-op6-gpu-memory-counters-pt-pool.patch` (bsd/dev/sweet_gpu.c + SweetGPU.cpp, after 0113):
    - pool = the worst case above, computed from the carve-out size at bring-up, still from the carve-out (the context
      bank walks tables non-cacheable, TCR IRGN0/ORGN0 = 0; the carve-out's Normal-NC view needs no cache maintenance per
      PTE write, cacheable kernel memory would): 192 MiB -> 868 pages (3.4 MiB), 128 MiB -> 836, 16 MiB -> 780. pt_new()
      can no longer fail; bring-up logs `SWEET GPU: GPU page-table pool: N pages (K KiB) for a M MiB carve-out + 1536 MiB
      surface VA`. Cost vs 0109: +1.4 MiB of carve-out at 192 MiB.
    - sweet_gpu_bo_alloc() returns a reason (GPU not ready/size, carve-out full, carve-out fragmented = enough free pages
      but no contiguous 16K run, page-table pool exhausted); first fit unchanged; new sweet_gpu_mem_stats().
    - GEM_NEW failure: still kIOReturnNoMemory (BO table full: kIOReturnNoResources), now counted and logged (first 16,
      then 1 in 64) under the GPU lock: `SWEET GPU: GEM_NEW <proc> <bytes> bytes flags 0x.. REFUSED (#n): <reason>;
      carve-out used U KiB, free F KiB, largest free L KiB of T KiB; page tables P/C; B live BOs` followed by one
      `SWEET GPU:   holder <proc>: N BOs X KiB of carve-out, M imports Y KiB of surfaces` per open client (top consumers;
      up to 16 clients tracked). No eviction.
    - ioreg (0111 throttled publish; carve-out/pool values read only if the GPU lock is free at that instant, IOLockTryLock,
      else the previous values stay): `SweetGPUCarveUsedKB`, `SweetGPUCarveFreeKB`, `SweetGPUCarveLargestFreeKB`,
      `SweetGPUPTPagesUsed`, `SweetGPUPTPagesFree`, `SweetGPUAllocFails`, `SweetGPUBOs` (live GEM_NEW BOs, imports not
      counted: those are `SweetGPUImportedKB`). Import/zero-copy path unchanged (only per-client accounting added).
  - `op6/mesa/sweet-drm/sweet_drm.c`: kIOReturnNoResources -> ENOMEM as well; drmCommandRead/Write/WriteRead now also
    set errno (like libdrm), so Mesa's messages show the real error.
  - Mesa `op6/mesa/patches/0004-freedreno-gpu-oom.patch` (after 0001-0003, Mesa MIT): fd_bo_heap_alloc() gives the range
    back and returns NULL when its block cannot be allocated (was fd_bo_ref(NULL), freedreno_bo_heap.c:241); bo_new() on a
    refused GEM_NEW calls fd_device_purge() (bo_cache + ring_cache) and retries exactly once, then returns NULL with a
    rate-limited `mesa_loge` + kernel log line `sweet-gl[<proc>]: mesa: GPU memory exhausted: N-byte BO (flags 0x..)
    failed: Cannot allocate memory, also after purging the BO caches (failure #n): GL_OUT_OF_MEMORY`;
    fd_bo_cache_cleanup(time 0) is no longer skipped when the previous cleanup also had time 0 (upstream bug that made a
    second fd_device_purge() a no-op); realloc_bo() allocates the new BO before dropping the old one and returns false
    (fd_resource_create -> NULL -> st raises GL_OUT_OF_MEMORY in glTexImage2D); a DISCARD_WHOLE_RESOURCE map whose busy
    BO cannot be replaced keeps the old BO and synchronizes like a normal map; fd_bo_new() no longer names a NULL BO in
    !NDEBUG builds.
  - `op6/tools/mkadt-op6.py`: `--gpu-dma 0xa0000000,0x0c000000` (the whole free window below the span, ends at the
    ramoops keep-out 0xac000000) was already accepted; now also refuses sizes < 8 MiB or not 64 KiB aligned.
  **Verified here (not on device):** 0114 `git apply --check` on the tree rebuilt from 0107..0113 (`--include` the two
  files); sweet_gpu.c and SweetGPU.cpp `clang -fsyntax-only -Wall` clean with stub headers; Mesa 0001-0004 apply with
  `patch` on the sha256-checked mesa-25.0.7 tarball; freedreno_bo.c (also with the `__APPLE__` branch), freedreno_bo_heap.c,
  freedreno_bo_cache.c, freedreno_resource.c and sweet_drm.c `-fsyntax-only` clean (generated register/format headers).
  **Expected on device (192 MiB):** at idle after boot `SweetGPUCarveUsedKB` 0, `SweetGPUCarveFreeKB` =
  `SweetGPUCarveLargestFreeKB` ~ 192,000 (192 MiB minus firmware/ring/GMU buffers and the 3.4 MiB pool),
  `SweetGPUPTPagesUsed` 3 (L1, L2, SQE L3) + `SweetGPUPTPagesFree` 865 (sum always 868), `SweetGPUAllocFails` 0,
  `SweetGPUBOs` 0. With backboardd running: CarveUsedKB in the tens of MiB (G4d needed > 64 MiB peak), PTPagesUsed ~ 3 +
  (highest BO address - 0x10000000)/2 MiB + (surface VA high-water)/2 MiB, CarveLargestFreeKB close to CarveFreeKB
  (fragmentation shows as a gap between the two).
  **Test plan:** (1) xnu run with 0114 -> boot-G4g.img; ADT `mkadt-op6.py ... --gpu-fw ../gpu-fw --gpu-dma
  0xa0000000,0x0c000000` (op6/out/op6-gpu192.adt); boot-args as G4f. `dmesg | grep 'page-table pool'`; `ioreg -l -c
  SweetGPU | grep SweetGPU` at idle and while using the UI. (2) op6-mesa run (0004 + sweet-drm), install `lib/*`, restart
  backboardd. (3) Out-of-memory path on purpose: boot the same kernel with a small ADT (`--gpu-dma 0xa0000000,0x02000000`,
  32 MiB) and open Control Center / app switcher / Settings: expected `SweetGPUAllocFails` > 0, the GEM_NEW REFUSED +
  holder lines, the `mesa: GPU memory exhausted` line, missing/black layers at worst, **no backboardd SIGSEGV** in
  /var/mobile/Library/Logs/CrashReporter. (4) Stress at 192 MiB: sweet-gl-test has no texture-stress mode yet (it only
  takes `-o` and a frame count; s9/ios-tools was outside this change) - add one that allocates 1024x1024 RGBA textures
  (4 MiB each) with glTexImage2D until GL_OUT_OF_MEMORY, frees them, repeats 10x, and checks CarveUsedKB returns to the
  start value; meanwhile the UI apps above for fragmentation (CarveLargestFreeKB vs CarveFreeKB).
  **Risks:** 1.4 MiB more carve-out for tables (pays for a pool that can never run out); the 192 MiB window is outside
  the XNU span, so XNU loses nothing, but nothing else may use [0xa0000000,0xac000000) any more; Mesa: callers outside the patched paths still do not check
  fd_bo_new() (fd6 LRZ, VSC streams, tess, border colours, query resize, autotune) - with the purge+retry they fail far
  less often but would still crash on NULL; purging on every refusal frees cached BOs that would have been reused (only
  when memory is already exhausted); kernel GEM_CLOSE of a busy purged BO waits up to 200 ms for its fence (existing
  releaseBOLocked behaviour) inside the allocating thread.
- 2026-11-14 **G4g memory tests (kernel run 43592841933 with 0114, op6-mesa run 43592844370 with Mesa 0004).** 192 MiB (boot-G4g-192.img): after boot 66 MiB used / 281 BOs; after 1-2 min of use 78 MiB used, 107 MiB largest free of 111 MiB free, 382 BOs, PT pages 69/868, 12856 submits, 42 imports, AllocFails 0, Faults 0, no crash -> **normal use needs ~80 MiB; 192 MiB is the default from now on** (64 MiB could not even finish booting the UI). 32 MiB stress (boot-G4g-32.img): kernel refuses cleanly with reasons + holder lines (AllocFails 234), Mesa 0004 purges and returns GL_OUT_OF_MEMORY ("also after purging the BO caches"), but backboardd still crashed once: `my_glFlush -> tc_flush -> fd_draw_vbo -> fd6_draw_vbos -> fd6_emit_3d_state -> fd6_build_bindless_state (fd6_image.cc:208) -> fd_bo_mark_for_dump(NULL)` (KERN_INVALID_ADDRESS 0x18): a draw still references a texture/image whose BO allocation failed. Next hardening: skip/guard NULL BOs in fd6_build_bindless_state / fd6 texture-state emit (and the other unchecked fd_bo_new callers listed in the 0004 notes), or make the draw be dropped when a bound resource has no BO.
- 2026-11-14 **Mesa 0005 (`0005-freedreno-skip-null-bo-draws.patch`, op6-mesa run 43598167134): PASS.** Added NULL BO entry-point skips (`fd_draw_vbo`, `fd_clear`, `fd_clear_render_target`, `fd_clear_depth_stencil`, `fd_launch_grid`, `fd_blit`, `fd_resource_copy_region`), `upload_shader_variant` / `draw_vbos` / `fd6_build_bindless_state` / `fd6_clear` (`subpass->lrz`) guards, and `freedreno_ringbuffer_sp` OOM scratch fallback + incomplete-submit drop (needed because earlier 32 MiB tests hit `fd_submit_sp_new_ringbuffer -> fd_bo_map(NULL)` at `0x1c` in `build_prog_fb_rast` and `ir3_shader_variant -> fd_bo_mark_for_dump(NULL)` at `0x18`). Verified on `boot-G4g-32.img` (120s): `backboardd` and `SpringBoard` alive, 0 new `backboardd-*.ips` crashes, `SweetGPUAllocFails = 95`, `SweetGPUFaults = 0`, `SweetGPUHangs = 0`, `SweetGPUSubmits = 41`, `SweetGPUCarveUsedKB = 28928`, `GL_OUT_OF_MEMORY` logged in `dmesg`. Restored `boot-G4g-192.img` as default.
