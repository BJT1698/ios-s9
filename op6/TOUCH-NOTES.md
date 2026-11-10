# OP6 touch (milestone C prep): shim M5

`bash op6/build.sh m5` -> `op6/out/boot-m5.img` (`fastboot boot`, nothing is flashed).
Code: `shim/m5.c` (stage), `shim/i2c_bb.c` (TLMM pins + bit-bang I2C), `shim/rmi4.c`
(RMI4 PDT/F01/F12/F11), `shim/rpmh.c` (RPMh VRM vote, extracted from `shim/m7.c`).

Labels: **[V-DT]** read in the OP6 live DT (`../op6-live.dts`), **[V-SRC]** read in the cited source,
**[V-S9]** proven on S9 hardware (same SoC), **[I]** inferred, **[U]** untested on the OP6.
No fact below has been verified on the OP6 itself yet: M5 is the test that does that.

## Hardware facts used

| Fact | Value | Source | Status |
|---|---|---|---|
| Touch bus | `i2c@a90000` (alias i2c12), `clock-frequency 0x61a80` (400 kHz), `pinctrl-0 = <0x6e>` | op6-live.dts:3112-3128, alias :5987 | V-DT |
| SDA/SCL pins | phandle 0x6e = `qup-i2c12-default-state`: `gpio49`, `gpio50`, function qup12, 2 mA, **bias-disable** (board pull-ups) | op6-live.dts:1172-1178; mainline sdm845.dtsi `qup_i2c12_default` | V-DT. Which of 49/50 is SDA: [I] the QUP SE order (first pin SDA), same as the S9 (31 SDA / 32 SCL) |
| Device | `synaptics-rmi4-i2c@20`, `reg 0x20`, `syna,startup-delay-ms 200`, `syna,reset-delay-ms 200`, F01 `nosleep-mode 1`, F12 `sensor-type 1`, 68x144 mm | op6-live.dts:3131-3157; mainline sdm845-oneplus-common.dtsi:521-549 | V-DT |
| IRQ | `interrupts-extended = <tlmm 0x7d 2>` = GPIO125, IRQ_TYPE_EDGE_FALLING | op6-live.dts:3136 | V-DT |
| ts-int-state | phandle 0x6f: `gpio99`, `gpio125`, function gpio, 16 mA, **bias-pull-up** | op6-live.dts:1119-1125; mainline dtsi:1038-1043 | V-DT |
| **GPIO99 = touch reset, active low** | downstream `synaptics,reset-gpio = <&tlmm 99 0x00>`, `irq-gpio 125`, `1v8-gpio 88`, `vdd_2v8-supply = pm8998_l28`, `avdd-voltage 3008000` | LineageOS android_kernel_oneplus_sdm845 `arch/arm64/boot/dts/qcom/enchilada.dtsi`:343-365 | V-SRC (not in the mainline DT) |
| OEM power-on order | `tpd_hw_pwron`: enable vdd_2v8 (L28), 10 ms, GPIO88=1, 10 ms, reset 99 = 1, 10 ms, 0, 10 ms, 1; probe then selects `ts_active` pinctrl and waits 100 ms | same repo `drivers/input/touchscreen/synaptics_driver_s3320.c`:603-647, 5774-5786 | V-SRC |
| vio | `ts-1p8-regulator`: regulator-fixed 1.8 V, `gpio = <tlmm 0x58 0>` = GPIO88, `enable-active-high`, no pinctrl, not boot-on ("controlled somehow by a gpio" comment) | op6-live.dts:5948-5956; mainline dtsi:189-204 | V-DT |
| GPIO88 other users | only `qup-uart5`/`qup-spi5` pinctrl states, used by `spi@894000`/`serial@894000`, both `disabled` | op6-live.dts:1265-1281, 1763-1767, 2145-2158, 2373-2384 | V-DT |
| vdd | phandle 0x70 = PM8998 `ldo28`, min 2856000, max 3008000 uV, initial mode 3 (HPM); only user is the touch | op6-live.dts:4546-4551, :3138; mainline dtsi:408-412 | V-DT |
| TLMM | `pinctrl@3400000` size 0xc00000, `gpio-reserved-ranges <0 4>, <81 4>` | op6-live.dts:920-931 | V-DT |
| Tiles | NORTH 0x500000, SOUTH 0x900000, EAST 0x100000 (offsets from 0x03400000); ctl = tile + 0x1000*n, io +4, intr_cfg +8, intr_status +0xc; mux bits 2-5, pull 0-1, drive 6-8, OE 9, in bit 0, out bit 1 | linux `drivers/pinctrl/qcom/pinctrl-sdm845.c`:13-52 | V-SRC |
| Pin tiles | 49 NORTH, 50 NORTH, 99 NORTH, 88 EAST, 125 EAST | pinctrl-sdm845.c:1162, 1163, 1213, 1201, 1239 | V-SRC |
| Pin addresses | 49 ctl 0x03931000, 50 0x03932000, 99 0x03963000, 88 0x03558000, 125 0x0357d000 | computed | V-SRC |
| Wake IRQs | PDC map: GPIO88 -> 77, GPIO125 -> 100 (needed only for an interrupt-driven XNU driver) | pinctrl-sdm845.c:1284, 1288 | V-SRC |
| ts-int-state register value | pull-up 3 | drive (16 mA -> 7) << 6 = 0x1c3, function 0, OE 0 | pinctrl-msm.c encoding (drive = mA/2 - 1) | V-SRC |
| Apps RSC | `rsc@179c0000`, drv-2 reg 0x179e0000, `qcom,drv-id 2`, `qcom,tcs-offset 0xd00`, `qcom,tcs-config` 2 ACTIVE (TCS 0-1), 3 SLEEP, 3 WAKE, 1 CONTROL | op6-live.dts:4374-4383 | V-DT |
| RSC v2 layout | TCS stride 672 (0x2a0), cmd stride 20; IRQ_EN 0, IRQ_STATUS 4, IRQ_CLEAR 8, WAIT 0x10, CONTROL 0x14, STATUS 0x18, ENABLE 0x1c, MSGID 0x30, ADDR 0x34, DATA 0x38, CMD_STATUS 0x3c; AMC enable bit 16, trigger bit 24 | linux `drivers/soc/qcom/rpmh-rsc.c`:85-93, 158-173 | V-SRC, V-S9 (m7.c M7.11) |
| cmd-db | reserved `aop-cmd-db-mem@85fe0000` size 0x20000; header: version, magic `db 30 03 0c`, 8 x rsc_hdr{slv_id, header_offset, data_offset, cnt, ...} from +8, entries after the 144-byte header: id[8], priority[2], addr at +16 | op6-live.dts:6761-6765; linux `drivers/soc/qcom/cmd-db.c`:20-80 | V-DT, V-SRC. The S9 code read the address from the AOP locator 0x0c3f000c instead [V-S9]; M5 uses the DT address and prints `loc!=DT` if they differ |
| LDO28 resource | `ldoa28` (format `ldo%s%d`, pmic-id a), VRM slave 4, pmic4_pldo 1.664-3.312 V in 8 mV steps; VRM regs voltage +0 (mV), enable +4, mode +8; PMIC4 LDO HPM = 7 | linux `drivers/regulator/qcom-rpmh-regulator.c`:57-75, 815-821, 1215 | V-SRC |
| RMI4 I2C | page select: write `[0xFF, page]`; read = write reg byte + repeated-START read | linux `drivers/input/rmi4/rmi_i2c.c`:45, 65 | V-SRC |
| PDT | from 0xE9 down to 0x05 in 6-byte entries; fn 0x00/0xFF ends a page; 2 empty pages end the scan; irq count = byte 4 bits 0-2, version bits 5-6 | rmi_driver.h:30-36, rmi_driver.c:505-560 | V-SRC |
| F01 | query 0 manufacturer (1 = Synaptics), 1 properties, 2-3 product info, 4-6 date, 11-20 product ID; data 0 status (bits 0-3 code, 6 bootloader, 7 unconfigured), data 1.. irq status; ctrl 0 (bits 0-1 sleep, bit 2 nosleep, bit 7 configured), ctrl 1.. irq enable | rmi_f01.c:15-103, 160-185, 422-441; rmi_driver.c:142, 384 | V-SRC |
| F12 | query 0 bit 0 = has descriptors; query/control/data descriptors at query+1/+4/+7; control 8 subpacket 0 = max X, max Y (LE16); data 1 = 8 bytes per object (type, x LE16, y LE16, z, wx, wy); a block read from the data base returns the present data registers back to back | rmi_f12.c:63-135, 211-250, 380-529; rmi_driver.c:610-735 | V-SRC (host-tested parser against a synthetic descriptor set) |
| F11 fallback | query 1 bits 0-2 fingers (5 = 10), bit 4 abs; ctrl +6/+8 max X/Y; data: 2 state bits per finger, then 5 bytes (x hi, y hi, x lo/y lo nibbles, wx/wy, z) | rmi_f11.c:23-24, 560-590, 663, 700-712 | V-SRC, [U]; not expected to run (F12 exists per DT) |
| Expected identity | `manufacturer: Synaptics, product: S3706B, fw id: 2827775` (pmOS dmesg) | ../op6-hardware.md section 5 | log |
| Expected range | coordinates in panel px, 0..1079 / 0..2279 (`syna,clip-x-high 1079` in mainline, clip-y in op6-hardware.md) | mainline dtsi:545; ../op6-hardware.md:154 | [I] max X/Y from control 8 decide |

## What M5 does (and writes)

1. Reads only: TLMM ctl/io of 49, 50, 99, 88 and ctl/io/intr_cfg/intr_status of 125.
2. Puts 49/50 in GPIO mode (input, no pull, 2 mA, latch 0) and bit-bangs at ~5 us half period.
   If SDA is held low with SCL high, 9-clock bus recovery.
3. Probe 0x20 (address + W, STOP; 3 tries). Then, only while it NACKs:
   a. ts-int-state on 99 and 125 (input pull-up 16 mA, ctl 0x1c3), 200 ms, probe;
   b. GPIO88 output 1, 10 ms, reset pulse on 99 (out 1, 10 ms 0, 10 ms 1, left driving 1), 200 ms, probe;
   c. **RPMh**: RSC id/TCS count sanity check, cmd-db magic, lookup `ldoa28` (must be unique,
      slave 4, 16-aligned), one ACTIVE TCS (0 or 1, only if idle) as AMC with 3 commands:
      voltage 3008 mV, mode 7, enable 1; then 10 ms, reset pulse, 200 ms, probe, +300 ms probe.
   If it ACKed at once, GPIO99 is not touched; only 125 gets input pull-up.
4. PDT scan, F01 queries and status; F01 ctrl0 = (ctrl0 & ~3) | nosleep | configured; F01 irq
   enable |= F12 (abs) bit, read back.
5. F12 descriptors -> data1 offset / finger count / max X/Y, F11 if no F12.
6. 60 s: read F01 irq status + the data packet when GPIO125 is low, else every 10 ms; square
   + crosshair per object (green, red, yellow, blue by slot), two live coordinate lines.
7. Countdown 30 s, PSCI reset. ramlog (ramoops console zone 0xac480000) has every step and the
   first 40 finger samples.

## Expected screen (top to bottom)

```
OP6 SHIM M5: TOUCH RMI4
PASSO   <last step started>
GPIO    49 c=XXXX io=X  50 c=XXXX io=X
GPIO    99 c=XXXX io=X  88 c=XXXX io=X
GPIO    125 c=XXXX io=X cfg=XXXX st=X
LINHAS  sda=1 scl=1 irq=1
SONDA   inicial: ACK|NACK|linha em 0 sda= scl= irq=
[SONDA  pinctrl: ...]
[VIO 88 c=.. io=.. / SONDA gpio88=1: ...]
[RPMH   id=.. tcs=.. ncpt=.. loc=.. v.. / RPMH ldoa28 addr=.. rc=0 db=85FE0000 / RPMH 3008 mV modo 7 on: 0 ok
 RESET  99 c=.. / SONDA ldo28: ...]
ENERGIA 0x20 responde apos nada (ABL) | pinctrl | gpio88 | ldo28   (or: sem resposta em 0x20)
PDT     funcoes=N irqs=M erro=0
PDT     F01 p0 qXX kXX cXX dXX i1@n v.   (one line per function)
F01     fab=01 prop=.. id=S3706B...
F01     data 20YY/MM/DD info=....
F01     status=.. (cod .) ctrl0=..
F01     ctrl0 .. -> .. lido .. erro=0
F01     irq en .. -> .. erro=0
F12     q0=.. regs q/c/d=../../.. erro=0
F12     data1 off=.. tam=.. dedos=..
F12     max x=1079 y=2279 (expected)
TOUCH   lei= bit= err=        TOUCH irq0= obj= t=Ns
DEDO    <slot> t<type> x.. y.. z.. >px,py     (two lines)
        TOUCH LIDO. Tire uma foto.   /  Tire uma foto desta tela!
TEMPO   countdown
```

## Test checklist (what to photograph)

1. Right after boot (first ~2 s): the GPIO lines and the first SONDA line. This is the ABL state of
   every pin. Key question: does 0x20 answer with power untouched?
2. After the power ladder: the ENERGIA line and every RPMH line (if any). If it hangs, the PASSO
   line names the step. A hang in `rpmh:` steps = report before retrying anything.
3. The PDT list and the F01/F12 block (product id, max x/y, data1 offset, number of fingers).
4. During the 60 s: touch the four corners and the centre, one finger at a time. Photograph a
   corner with the finger lifted to check that the square is under the finger (axis swap/flip?).
   Then two fingers, then five (colours by slot). Note whether `irq0` and `bit` grow (interrupt
   line and F01 status work) or only `lei` (poll mode).
5. The last screen before the reset. Afterwards, if TWRP/pmOS is booted next, `/sys/fs/pstore`
   holds the ramlog.

If a panic screen appears: photograph it (ETAPA = step name, ESR/ELR/FAR).

## Risks

- **RPMh/PMIC write** (step 3c only, only if the chip did not answer): an ACTIVE-only vote from
  DRV2 for LDO28 at 3008 mV (inside the DT range; the OEM value). LDO28 has no other user in the
  DT. The code is the S9 M7.11 one (proven there for ldoa5/12/24 on the same SoC and PMIC) [V-S9],
  but **untested on the OP6** [U]. It refuses if the RSC id/TCS count looks wrong, the cmd-db magic is
  missing, `ldoa28` is not found exactly once, or no ACTIVE TCS is idle. The vote ends at the reboot.
- GPIO88 drives a rail Linux models as a GPIO-enabled fixed regulator; wiring unknown ("controlled
  somehow by a gpio"). The OEM driver drives it high the same way.
- Vio before vdd in step b (OEM order is vdd first). Chosen so that the PMIC is written only if the
  cheap steps fail; the mainline Linux driver also enables both at once without a fixed order.
- GPIO99 reset pulse resets the controller (harmless: it reinitialises from its flash).
- Which of 49/50 is SDA is inferred. If swapped, the probe NACKs on every step and LINHAS still reads
  1/1; swap TS_SDA/TS_SCL in m5.c and retry.
- F01 ctrl0/irq-enable writes are what Linux writes. No F34 (flash) or F54 commands are sent.

## What the XNU driver (later patch 0107) needs beyond M5

- Everything here, as a thread (0088/0094 style), with the pins and power from the ADT (`/sweet-touch`).
- The power ladder as found by M5 (only what was actually needed), plus the reset pulse.
- RPMh: an ACTIVE-only vote is enough while the CPU never power-collapses the cluster. If XNU idle
  ever lets the apps RSC enter sleep, LDO28 also needs SLEEP/WAKE TCS votes (rpmh-rsc "sleep/wake"
  sets) or it may drop. Move the VRM helper (`sweet_usb.c` s9_rpmh_init/s9_vreg) to a shared file.
- Re-init after a controller reset: F01 status code 1 (reset) or the unconfigured bit -> redo
  ctrl0 (nosleep | configured) and irq enable.
- Tracking: F12 object type 0 = lifted; slot index = finger id; types 1 finger, 2 stylus, 3 palm.
- Coordinate transform (axis swap/flip) from the M5 photos; 0105 HID panel size from the video mode.
- Interrupt-driven (optional): GPIO125 falling edge through TLMM intr_cfg (target KPSS, edge) and the
  PDC wake IRQ 100; polling at 100 Hz on the 0094 thread is the fallback.
- Suspend/resume (sleep mode in ctrl0) when the screen turns off.
