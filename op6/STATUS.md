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
| 2026-11-10 | SSH | Procursus 1800 bootstrap + OpenSSH 9.7 written into the live root via pmOS usb-storage (backup userdata-16G-before-ssh.img): `ssh -i ~/.ssh/ios-op6 root@192.168.7.2` -> root shell, iOS 16.7.16 20H392, 8 CPUs, 3.28 GiB |

| 2026-11-10 | GPU M10/M11 | GMU firmware boots (0xBABEFACE, fw v1.6); GPU SMMU context + HFI INIT/FW_VERSION/PERF/BW/TEST ACKed |

| 2026-11-10 | GPU M12 | GX powered via the GMU at 710 MHz, RBBM_STATUS=1, GMU and CP always-on counters at 19.2 MHz, clean slumber |

| 2026-11-10 | GPU M13 | zap shader accepted by TrustZone (PAS 13); SQE microcode loaded; CP ran ME_INIT, SET_SECURE_MODE 0, NOP + CP_MEM_WRITE: 0xC0FFEE01 in RAM through the GPU SMMU |

| 2026-11-10 | GPU G1 (XNU) | kernel driver (0107) powers the A630 during iOS boot, zap + CP_MEM_WRITE 0xC0FFEE01, GPU back off; iOS unaffected |
| 2026-11-10 | GPU G2 (XNU) | `SweetGPUUserClient` (0108, `sweetgpu=2`): `SweetGPUMagic=3237998081` (`0xC0FFEE01`), `SweetGPUState="on"`; userland `sweetgpu-test` allocates/maps carve-out BOs (`0x10000000`, `0x10004000`), submits IB `CP_NOP` + `CP_MEM_WRITE` `0xC0FFEE02` (69 us), and passes 100-submit (53 us/submit) + 1000-submit ring-wrap (51 us/submit) loops with 0 faults |
| 2026-11-10 | GPU G3 (Mesa) | Mesa 25.0.7 `libOSMesa.8.dylib` (`freedreno` `FD630`, OpenGL ES 3.2) + `sweet-drm` shim + `/var/root/sweet-gl-test` on `boot-G3.img` (`--gpu-dma 0xa0000000,0x04000000` 64 MiB carve-out): `glClear(0.2, 0.4, 0.8, 1.0)` + `glReadPixels` exact match (`51 102 204 255` in 491 us), `ir3` vertex/fragment shader RGB triangle draw + readback verified (`17086/65536` = 26% pixels, corner `26 26 51 255`), 100-frame (207 us/frame) and 500-frame (180 us/frame, ~5555 fps) loops pass with `faults 0; error 0: MESA DRAWS WITH THE GPU` |

| 2026-11-14 | GPU G4 | CoreAnimation in backboardd renders through Mesa freedreno on the Adreno 630 (0110 sweetinject, 0111 counters; 128 MiB GPU carve-out): home, blur, Control Center, Settings OK, 7447 submits 0 faults, ~16-26 fps (picture copied out each frame) |

| 2026-11-14 | GPU G4 zero-copy | backboardd's b3a8 screen surface imported into the GPU (0112/0113 + Mesa 0003): drawn in place, 10-19 ms/frame (was 47-64), colours correct, much smoother |
| 2026-11-14 | GPU OOM (Mesa 0004/0005) | Mesa 0004 + 0005 (`op6-mesa` run `43597009184`): 32 MiB carve-out stress test (`boot-G4g-32.img`) survives `SweetGPUAllocFails=49` / `GL_OUT_OF_MEMORY` with 0 `backboardd` crashes and 0 GPU faults; `boot-G4g-192.img` (192 MiB) restored as default |

Next: IRQs (G5), daemon trim, CPU clocks, full 8 GB. See XNU-PLAN.md.
