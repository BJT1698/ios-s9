# OnePlus 6 (`enchilada`) Hardware Display Scan-Out & Scaling Plan

Goal: replace the per-frame CPU copy + upscale (`828x1792` `'b3a8'` -> `1080x2280` `0x9d400000`) with **zero-copy MDSS/DPU scan-out and QSEED3 hardware scaling**, keeping `apps_smmu` untouched.

Tag key: `[V]` observed in DT/logs/code in this workspace, `[I]` inferred from SDM845 DPU register layout / DT, `[?]` requires on-device confirmation (addressed by shim `m9.c`).

---

## 1. OP6 MDSS/DPU Pipeline vs Galaxy S9

| Aspect | Galaxy S9 (`starqltechn`) | OnePlus 6 (`enchilada`) | Source / Evidence |
| --- | --- | --- | --- |
| Panel | `S6E3HA8`, `1440x2960` cmd mode `[V]` | `samsung,sofef00-ams628nw01`, `1080x2280` cmd mode `[V]` | `op6-live.dts` (`panel@0`) |
| DSI links | Single DSI0 with DSC `[V]` | Single DSI0 (`dsi@ae94000` `okay`, `dsi@ae96000` `disabled`), 4 lanes, no DSC `[V]` | `op6-live.dts` lines 570–724 |
| Topology | Two 720-wide halves: `LM0` + `LM1`, `VIG0` + `VIG1` `[V]` | Single 1080-wide path: `LM0`, `PP0`, `VIG0` (`0x0ae05000`) `[V]` | `op6/STATUS.md`, `op6/XNU-PLAN.md` |
| ABL FB | `0x9d400000`, `1440x2960`, `ystride0 = 0x1680` (5760) `[V]` | `0x9d400000`, `1080x2280`, `ystride0 = 0x10e0` (4320) `[V]` | `op6/shim/fb.h`, `mkadt-op6.py` |
| ABL SSPP format | `VIG0`/`VIG1`: `fmt = 0x000237ff`, `op = 0x80000000` `[V]` | `VIG0`: `src = 0x08e80438` (`2280x1080`), `fmt = 0x000237ff`, `unpack = 0x03020001` (BGRA) `[V]` | `op6/XNU-PLAN.md`, prompt M1 facts |
| CTL flush mask | `CTL_FLUSH_CTL \| VIG0 \| VIG1` = `(1<<17)\|(1<<1)\|(1<<0)` `[V]` | `CTL_FLUSH_CTL \| VIG0` = `(1<<17)\|(1<<0)` (`0x00020001`) `[I]` | `s9/shim/m9.c`, DPU CTL0 |
| PingPong `PP0` autorefresh | ABL leaves `0x80000002` (30 Hz, every 2nd TE); patch `0095` changes to `0x80000001` `[V]` | ABL already leaves `PP0` at `0x80000001` (60 Hz, every TE, `te-gpios = GPIO10`) `[V]` | `op6/STATUS.md`, `op6-live.dts` |
| GPU -> FB direct? | N/A | Freedreno refuses `0x9d400000` directly because `4320 % 64 == 32 != 0` `[V]` | `op6/GPU-NOTES.md` |

Key takeaway: OP6 already runs 60 Hz autorefresh out of ABL (`0x80000001`) and uses a **single pipe (`VIG0`), single mixer (`LM0`), single pingpong (`PP0`)** rather than dual split pipes.

---

## 2. Single `VIG0` + QSEED3 Scaling (`828x1792 -> 1080x2280`)

Can one `VIG` pipe + `QSEED3` scale `828x1792 -> 1080x2280` inside one Layer Mixer (`LM0`)? **Yes.** `[I]`

1. **LM width budget**: SDM845 DPU Layer Mixer maximum width is 2560 pixels (`LM0` already receives full-panel `1080x2280` from `VIG0` in ABL). `1080 <= 2560`, so no dual-LM split or DSC is needed.
2. **QSEED3 availability**: Only VIG pipes (`VIG0..VIG3`, offsets `0x5000..0xb000`) include QSEED3 (`QS = +0xa00`; DMA0..DMA3 do not). ABL uses `VIG0` (`0x0ae05000`), so QSEED3 (`0x0ae05a00`) is directly on the active pipe.
3. **Scale ratios & Q11.21 phase step math (`PHASE_STEP_SHIFT = 21`)**:
   - Horizontal: `828 -> 1080` (`1.3043x` upscale, `stx = ((uint64_t)828 << 21) / 1080 = 0x188888`).
   - Vertical: `1792 -> 2280` (`1.2723x` upscale, `sty = ((uint64_t)1792 << 21) / 2280 = 0x19269f`).
   - Both ratios are mild upscales (~1.3x, well inside SDM845 QSEED3 upscale limits and <=2560 line buffer width).
   - Aspect ratio difference: `(828/1792) = 0.4621` vs `(1080/2280) = 0.4737` (~2.5% wider when filling `1080x2280`, smaller than S9's ~5.2% stretch).
   - **No split seam**: unlike S9 (`414->720` on two halves), single-pipe scaling has zero center seam.

---

## 3. SSPP Requirements for Backboardd `'b3a8'` (`828x1792`)

Observed `'b3a8'` layout (`op6/GPU-NOTES.md`, patch `0113`) `[V]`:
- Total IOSurface allocation: `7,454,720` B (`0x71c000`, `1820` x 4 KiB pages).
- **Plane 0**: `828 x 1792`, `bpe = 4` (BGRA8888), offset `+0x0`, **`bytesPerRow = 3328`** (`832` px = `52 * 64` bytes), size `5,963,776` B (`0x5b0000`).
- **Plane 1**: `832 x 1792`, `bpe = 1`, offset `+0x5b0000`, size `1,490,944` B (`0x16c000`, opacity-0 auxiliary plane unused by CPU present `0113` and Freedreno `0003`).

SSPP programming against plane 0 `[I]`:
- `SSPP_FORMAT` / `SSPP_UNPACK`: ABL already programs BGRA8888 (`fmt = 0x000237ff`, `unpack = 0x03020001`), matching `'b3a8'` plane 0 byte order (`B, G, R, A`).
- `SSPP_YSTRIDE0`: **`3328` (`0x0d00`)**. `3328` is 64-byte aligned, satisfying **both** Freedreno's 64-byte render-target pitch rule (`GPU-NOTES.md`) and SSPP's linear stride alignment.
- `SSPP_SRC_SIZE`: `(1792 << 16) | 828` (`0x0700033c`) so SSPP fetches only the visible `828` pixels per `3328`-byte row; `SSPP_SRC_XY = 0`.
- `SSPP_OUT_SIZE`: `(2280 << 16) | 1080` (`0x08e80438`), `SSPP_OUT_XY = 0`.
- `SSPP_PE_*_REQ` + `SSPP_OP_MODE |= OP_PE_OVERRIDE`: `(1792 << 16) | 828` so pixel extension fetches the entire `828x1792` source before QSEED3.

---

## 4. SMMU Analysis & Scan-Out Buffer Design

### Hardware SMMU state (`op6-live.dts`)
- `display-subsystem@ae00000` specifies:
  ```
  iommus = <0x32 0x880 0x08 0x32 0xc80 0x08>;
  ```
  where `0x32` is `iommu@15000000` (`apps_smmu`, `qcom,sdm845-smmu-500`), served by `tbu@150cd000` (`qcom,stream-id-range = <0x32 0x800 0x400>`) `[V]`.
- Therefore MDSS DMA (`SSPP_SRC0_ADDR`) **does** pass through the **hypervisor-emulated apps SMMU (`0x15000000`)** on SIDs `0x880` / `0xc80` (mask `0x08`) `[V]`.
- Under Qualcomm's SDM845 hypervisor (`HANDOFF.md`, `m7.c`), `S2CR BYPASS` becomes `FAULT`. ABL configures MDSS with the hypervisor-compatible S1-translate/S2-bypass context bank and **`CB_SCTLR.M = 0`** (identity mapping, IOVA == PA) so ABL's splash (`0x9d400000`) scans out `[V/I]`.

### Why SSPP cannot read backboardd's normal `'b3a8'` surface directly
Backboardd's default `IOSurface` (`0x71c000` B = `1820` x 4 KiB pages, or `455` x 16 KiB XNU pages) is allocated from non-contiguous XNU VM pages `[V]`. With `CB_SCTLR.M = 0` (identity PA), SSPP increments linearly from `SSPP_SRC0_ADDR` through `SSPP_SRC0_ADDR + 0x5affff`; pointing it at a fragmented surface would scan arbitrary unrelated physical RAM after page 0.

### Options considered

1. **Option A — Reprogram `apps_smmu` (`0x15000000`) Stage-1 (`M = 1`) for MDSS (`0x880`/`0xc80`)**:
   - Build dynamic ARMv8 page tables per `'b3a8'` IOSurface, switch ABL's live MDSS CB to `M = 1`, manage IOVA allocation and TLB invalidations while DPU autorefreshes autonomously at 60 Hz.
   - **Rejected (high risk, unnecessary complexity)**: `apps_smmu` sits behind the Qualcomm hypervisor (reading `sCR0` hangs; rewriting/splitting active MDSS SMRs or dynamic page-table updates racing TE fetches risks an immediate hypervisor crashdump).

2. **Option B — Physically contiguous `'b3a8'` display scan-out slots (CHOSEN)**:
   - Keep ABL's `apps_smmu` (`M = 0`, identity PA) **completely untouched**, and back backboardd's `'b3a8'` render-target `IOSurface`s with **physically contiguous slots** (the exact strategy proven on S9 in `xnu/patches/0096`).
   - Where to place the slots (`0x720000` B = `7,471,104` B each, 64 KiB-aligned to cover the full `0x71c000` `'b3a8'` surface + plane 1):
     - **Preferred: tail of `splash@9d400000`** (`0x9d400000..0x9f800000`, `36 MiB` `no-map` outside XNU DRAM):
       - ABL boot framebuffer uses `0x9d400000..0x9dd68000` (`0x968000` B = `9.41 MiB`) `[V]`.
       - Unused tail `0x9de00000..0x9f800000` has **`26.0 MiB` free**, fitting **3 contiguous `'b3a8'` slots** (`0x9de00000`, `0x9e520000`, `0x9ec40000`, ending at `0x9f360000 < 0x9f800000`) `[I]`.
       - Because `0109-op6-gpu-iosurface.patch` imports `IOSurface` physical pages straight into **GPU SMMU (`0x05040000`) Stage-1 page tables** `[V]`, Adreno 630 renders directly into the contiguous splash-tail slot (zero-copy), and MDSS `VIG0` scans directly out of that same slot (zero-copy) with **0 bytes stolen from `--gpu-dma` (`0xa0000000..0xac000000`)** — so even 32 MiB `--gpu-dma` configs work.
     - **Alternative (if GPU carve-out locality is ever desired)**: take 2–3 slots (`14.25`–`21.38 MiB`) from the top of the `192 MiB` GPU carve-out (`0xa0000000..0xac000000`), keeping the exact same `SweetFramebuffer` scan-out logic.
   - **Kernel rollout (`sweetfbscale=1`, future patch)**:
     - Keep CPU copy/scale as default (`sweetfbscale=0`) until `m9` and runtime slot rendering are confirmed.
     - Under `sweetfbscale=1`: `SweetSurface` gives `828x1792` `'b3a8'` surfaces one of the contiguous `0x720000` slots (starting with 2 or 3 slots for triple-buffering); `presentSurface()` verifies physical contiguity / slot membership + `ystride0 = 3328`, switches `VIG0` + `QSEED3` (`828x1792 -> 1080x2280`) once, and on each present simply updates `SSPP_SRC0_ADDR` + `CTL0_FLUSH = 0x00020001`, falling back cleanly to the CPU path for any non-slot surface.

---

## 5. Bare-Metal Verification Stage (`op6/shim/m9.c`)

Before writing any XNU patch, `op6/shim/m9.c` verifies every hardware assumption bare-metal on the OP6 (without touching `apps_smmu`):
1. Read-only MDSS dump (`CTL0`, `LM0`/`LM1`, active SSPPs, `QSEED3` version & op mode, `PP0` autorefresh/TE counters).
2. **Test A (zero-copy contiguous `1080x2280`)**: copy ABL splash to `0xb0000000`, draw purple `"A: OP6 ZERO-COPY OK"` banner, update `VIG0` `SSPP_SRC0_ADDR` with readback verify + `CTL0` flush `(1<<17)|(1<<0)`.
3. **Test B (single-pipe QSEED3 `828x1792 -> 1080x2280`, `bpr = 3328`)**: draw test pattern at `0xb2000000` with **`stride = 3328`** (exact `'b3a8'` plane 0 pitch), configure single `VIG0` + `QSEED3`, **read back and verify every written register** (refuse & restore on mismatch), flush, then run a live **frame counter** inside the `828x1792` buffer for ~20 s so hardware scaling + live buffer updates are visually observable on the glass.
4. **Restore & Test C**: restore saved ABL `VIG0` + `QSEED3` registers with readback verify + flush, then measure `PP0` frames/s and TEs/s (`0x80000001` should show ~60/s).
