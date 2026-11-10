# OnePlus 6 (enchilada) status

Labels: device-verified unless noted.

| Date | Milestone | Result |
|---|---|---|
| 2026-11-06 | M1 shim | ABL accepts header v0 + appended DTB via `fastboot boot`; FB 0x9d400000 1080x2280 stride 4320 BGRA, PP0 autorefresh 0x80000001 |
| 2026-11-06 | M2 | PSCI 1.1, 8/8 CPUs (A55 0-3, A75 4-7), vtimer 30/30, SGI 10/10 |
| 2026-11-06 | M8 | RAM 0xa0000000-0x27c000000 free (474/474 16-MiB samples) |
| 2026-11-10 | A1 | XNU (macos-26/Xcode 26 build + 0000/0102/0103) reaches BSD init (`rd=md0` panic) |
| 2026-11-10 | A2 | 8/8 CPUs under XNU, AppleCLCD 1080x2280 -> 828x1792, UFS `ufsroot0` read-only |
| 2026-11-10 | B | iOS 16.7.16 root flashed to userdata; **lock screen shown** (boot-B2: B args + `sweetfb_noconsole=1`), first boot ~30 min on the Apple logo (data migration) |
| 2026-11-10 | M5 | bare-metal touch: ABL leaves it off; GPIO88 + RPMh ldoa28 3008 mV + reset GPIO99 -> S3706B ACK at 0x20, F12 10 fingers, max 1079x2279, aligned with the panel |
| 2026-11-10 | C | kernel with 0104/0105 (boot-C): **touch works in iOS (unlock, taps), volume up/down work** |
| 2026-11-10 | M7 | bare-metal USB warm start: QUSB2 power-up, rail/bus votes, SMMU rule SID 0x740 (U-Boot recipe; without it the first DMA -> Qualcomm CrashDump) -> PC enumerates CDC-ECM 1209:0001 |
| 2026-11-10 | D | kernel with 0106 (boot-D, `sweetusb=2`): **iOS USB Ethernet, ping 192.168.7.2 from the PC ~1 ms** |

Next: SSH (bootstrap + openssh via pmOS usb-storage), GPU, daemon trim, CPU clocks, full 8 GB. See XNU-PLAN.md.
