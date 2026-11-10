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

Next: C touch (Synaptics RMI4), D USB networking, vol-down key, daemon trim, CPU clocks, full 8 GB. See XNU-PLAN.md.
