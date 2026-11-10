# OnePlus 6 USB (milestone D): CDC-ECM to the PC, then SSH

Labels: **[V]** verified on this phone or in source, **[V-S9]** proven on the Galaxy S9 (same
SoC), **[I]** inferred, **[U]** unknown until tested.

## 1. Which path: warm, with the cold start kept as a fallback

ios-sweet's original gadget (`sweet_usb.c`, Redmi Note 10 Pro, SM7150) started from a DWC3 that
ABL's fastboot left "powered, clocked, in device mode and halted, with the PHY set up". It only
did a DCTL soft reset, set up an event buffer and ep0, applied the QSCRATCH VBUS override, then
run/stop. The S9 code still has that sequence (`sweet_usb_gadget_start` / `ud_controller_start`),
but after 0090, `sweetusb=2` always runs `s9_usb_cold_start` first, and that cold start includes
the MAX77705 switch. On the OP6 the cold start would fail at the MAX77705 step, and the bit-bang
would toggle GPIO33/34, which do something else on this board. **[V]** (source)

What ABL actually leaves on the OP6 after `fastboot boot`, read under XNU with `sweetusb=1`
(`../logs/op6-B2-log.txt:126-137`) **[V]**:

| Register | Value | Meaning |
|---|---|---|
| GDSC usb30_prim 0x10f004 | 0xf8222000 | on |
| CBCR master/sleep/utmi/cfg-noc/aggre/ahb2phy/clkref | all on | ABL left the clocks running |
| GSNPSID | 0x5533300a | DWC3 3.00a |
| GCTL | 0x00112000 | PRTCAPDIR = 2, device mode |
| DCFG | 0x004808dc | DEVSPD = 4 (SuperSpeed), address 27, LPM capable |
| DCTL | 0x10f00000 | **run/stop = 0** |
| DSTS | 0x00d31bcc | **DEVCTRLHLT = 1**, last connect speed 4 = SS, link state 4 |
| GUSB2PHYCFG | 0x00002400 | UTMI 8-bit, SUSPHY 0 |
| GEVNTADR | 0x99725000 | ABL's own event buffer |
| QUSB2 PWR_CTRL1 0x210 | 0x23 | VREF_SUPPLY_TRIM, CLAMP_N_EN, **POWER_DOWN**: Linux's `disable_ctrl` for qusb2-v2 (`phy-qcom-qusb2.c`) |
| QUSB2 PLL_STATUS 0x1a0 | 0x64 | bit 0 CORE_READY = 0: PLL off |
| QUSB2 PORT_TUNE1 0x240 | 0x75 | ABL's tuning still in place (the S9's DT init sequence also writes 0x75 there) |

The QUSB2 registers are 8 bits wide, and a 32-bit read repeats the byte (`0x23232323`).

Conclusions:
- ABL already disconnected from the PC (run/stop 0, halted). The soft-disconnect worry is moot
  on the evidence. 0106 still clears run/stop and waits 200 ms if it ever finds it set. **[V]**
- ABL ran the link at **SuperSpeed** through the QMP PHY. The Linux DT has the QMP PHY disabled
  and uses `qcom,select-utmi-as-pipe-clk` (live DT), so the UTMI clock must drive the pipe side.
  That is exactly the S9's HS-only setup (`s9_utmi_for_pipe` + `s9_dwc3_global`). **[V]** (log, DT)
- The sweet-style warm path alone is **not enough on the OP6**, because ABL powered the
  QUSB2 PHY down. The minimal step is the last step of Linux `qusb2_phy_init`, which clears
  POWER_DOWN, waits 150 us and checks CORE_READY. If that does not lock, 0106 falls back to a
  full init (QUSB2 BCR plus the S9 DT sequence). **[V]** (log + Linux source) / **[U]** whether
  the power-up alone locks.
- If ABL ever leaves GDSC or the clocks off (another ABL version, or a normal boot rather than
  fastboot), 0106 takes the S9 cold path minus the MAX77705 (fallback b).

Same on SDM845 for the S9 and the OP6 **[V]**: DWC3 0xa600000, QSCRATCH 0xa6f8800, QUSB2
0x88e2000, ahb2phy 0x88ee000, GCC offsets (`gcc-sdm845.c`; the S9 code already uses them),
RPMh DRV2 0x179e0000 / TCS +0xd00, apps SMMU 0x15000000, **USB stream ID 0x740** (live DT
`usb@a600000 iommus = <&apps_smmu 0x740 0>`, `../op6-live.dts:233`), TBU1/TBU2 GDSC votes
0x17d034/0x17d038.

What differs from the S9:

| Item | S9 | OP6 | Source |
|---|---|---|---|
| PHY rails | ldoa5 800, ldoa12 1800, ldoa24 3075 mV | **ldoa1 880**, ldoa12 1800, **ldoa24 3088** mV, all HPM (initial-mode 3) | live DT phy@88e2000 vdd/pll/dpdm-supply -> ldo1 0xd6d80, ldo12 0x1b7740, ldo24 0x2f1e80 **[V]** |
| cmd-db | AOP locator 0x0c3f000c | DT reserved `aop-cmd-db-mem@85fe0000` + 0x20000 (proven by shim M5) | TOUCH-NOTES.md **[V]** |
| D+/D- | MAX77705 switch | direct to the SoC, no switch | op6-hardware.md section 8 **[V]** |
| SMMU 0x740 rule | absent (ABL never used USB) | ABL used USB DMA for fastboot, so a rule may survive **[U]** | m7 reports it |

SMMU: the U-Boot recipe (`drivers/iommu/qcom-hyp-smmu.c`, written for SDM845 under Qualcomm's
hypervisor) first looks for an existing SMR with the stream and skips setup if it finds one.
0106 and m7 do the same: a no-op if the hypervisor's view already holds 0x740. Otherwise they
pick a free context bank with S1 translate / S2 bypass, translation off, and S2CR TRANSLATE.
`sweetusbsmmu=0` (kernel) or `SHIM_CFLAGS=-DM7_SMMU=0` (shim) leaves the SMMU alone, in case
ABL's rule survives invisibly and an added SMR conflicts with it. **[U]**

## 2. What was written

- `xnu/patches/0106-op6-usb-warm-start.patch` (copy in `op6/xnu-patches/`), on
  `bsd/dev/sweet_usb.c` only:
  - `/chosen/sweet-board == "oneplus,enchilada"` (same lookup as `sweet_touch.c`) selects
    `op6_usb_start`. The S9 path is byte-for-byte the same.
  - `op6_usb_start` does these steps in order:
    1. RPMh init from the DT cmd-db.
    2. Votes for ldoa1/12/24. These are logged, not fatal.
    3. The S9's BCM votes: SH3, SN11, SN0, SN6, CN0, SN12.
    4. The TBU GDSC votes.
    5. The SMMU 0x740 recipe, unless `sweetusbsmmu=0`.
    6. GCC: if GDSC and the six branches are on, it is left alone. Otherwise the S9 cold GCC +
       QUSB2 init runs.
    7. `op6_qusb2`: power-up, with the full init as a fallback.
    8. `s9_utmi_for_pipe`.
    9. It sets `s9_cold`, so `s9_dwc3_global` (device mode, GUSB3PIPECTL suspend, 19.2 MHz
       GUCTL/GFLADJ) runs after the soft reset, as on the S9.
  - `ud_controller_start` on the OP6: if run/stop is set, it stops, waits for the halt and
    waits 200 ms before the soft reset.
  - USB strings on the OP6: manufacturer "ios-op6", product "OnePlus 6 iOS (USB Ethernet)". VID:PID
    1209:0001, MACs and IPs are the same as the S9 (phone 02:00:5e:77:ee:02 192.168.7.2, host
    02:00:5e:77:ee:01 192.168.7.1).
  - No ADT change is needed, because `sweet-board` already comes from `mkadt-op6.py:148`.
- `op6/shim/m7.c` (replaces the unused S9 copy) plus `bash op6/build.sh m7` gives
  `op6/out/boot-m7.img`. It uses the same sequence as 0106, polled with the MMU off, and stays
  up for 180 s, then reboots. `op6/shim/rpmh.c` gained `rh_bcm_on`. `build.sh` gained the
  stage and a `SHIM_CFLAGS` pass-through.
- `op6/tools/op6-prep-usb-ssh.sh` (section 4) and `op6/tools/op6ssh.sh`.

Checks (2026-11-10):
- `git apply --check` of 0106 on the post-0105 tree: OK.
- `clang -fsyntax-only -Wall -Wextra` of the patched `sweet_usb.c` with header stubs: no errors.
  The only warnings are stub artefacts (uint64 `%llu`, the stubbed SYSCTL macro), the same ones
  the unpatched file shows.
- `bash op6/build.sh m7`, with and without `-DM7_SMMU=0`: no warnings, 0 ABS64 relocations in
  C objects.
- `op6-prep-usb-ssh.sh root` ran against a fake root with fake debs in a user namespace: the
  files and jobs came out as expected, and a second run is idempotent.

Not tested on the phone: everything in this file marked [U].

## 3. Test plan

### 3a. Shim m7 (seconds, no kernel build)

```
bash op6/build.sh m7                      # op6/out/boot-m7.img
sudo dmesg -w                             # PC, in another terminal (or journalctl -kf)
fastboot boot op6/out/boot-m7.img         # cable stays in
```

The screen shows the following rows. Take a photo of them.
- `ABL` rows: a read-only dump, expected to match the table in section 1 (dctl 10f00000,
  dsts ..d3.., qusb2 pwr=23 pll=64).
- `RSC`/`RPMH`: three votes `ok`. `BCM`: six names without `=X`. `TBU`: two values ending
  in 8000xxxx.
- `SMMU`: either `ja existe smr N` (ABL's rule survived) or `usb: smr N=80000740 cb M`.
- `GCC ligado como o ABL deixou (quente)`.
- `QUSB2 antes pwr=23 agora pwr=22 pll=..(bit0 = 1) (so power-up) PRONTO`.
- `DWC3 id=5533300A reset ok rodando erro=0`, then gctl/dcfg/dsts.
- `USB eventos=.. reset>=1 conexao>=1 vel=0 setup>=8 end=N config=1`.

On the PC:
```
usb 1-N: new high-speed USB device number K using xhci_hcd
usb 1-N: New USB device found, idVendor=1209, idProduct=0001, bcdDevice= 1.00
usb 1-N: Product: OnePlus 6 iOS (USB Ethernet)
usb 1-N: Manufacturer: ios-op6
cdc_ether 1-N:1.0 eth0: register 'cdc_ether' at usb-..., CDC Ethernet Device, 02:00:5e:77:ee:01
```
In m7 the link stays NO-CARRIER: only the kernel sends NETWORK_CONNECTION.

If something fails, the screen says where:
- **Hang on a `PASSO smmu: ...` step**, or a reset there: rebuild with
  `SHIM_CFLAGS=-DM7_SMMU=0 bash op6/build.sh m7` (report only). If the SMR list then shows no
  0x740 rule and the run hangs at `dwc3: 1o DMA`, the recipe is needed but its write is what
  hangs. Send the photo.
- **`SEM LOCK`**: the PHY needs more than power-up (the full init already ran). Check the
  RPMH rows. L24 or L1 failing would explain it.
- **`reset PRESO`**: the controller's pipe clock domain is not clocked.
- **`rodando`, but the PC shows nothing or `device descriptor read/64, error -71`**: suspect
  the D+ pull-up or VBUS override (`qs hs=` row), or the SS terminations of the QMP PHY that
  ABL left on (section 5).

### 3b. Kernel, milestone D

0106 goes through the CI build with the other patches. Then make the payload and boot image
as in XNU-PLAN section 4 (mkadt-op6.py `--scale2`, mkpayload, `PAYLOAD=... op6/build.sh m4`),
with:
```
D="debug=0xc serial=3 cpus=8 keepsyms=1 -noprogress rd=ufsroot0 ufsroot=userdata ufsrw=1 rootrw=1 ufswait=1 ufslogsecs=60 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin launchd_unsecure_cache=1 wdt=-1 cs_enforcement_disable=1 sweetfb_noconsole=1 sweetfb_hz=60 sweetcpu=0 sweetbattery=0 sweetusb=2 sweettouch=1 sweettouchdelay=20"
```
That is C with `sweetusb=2`. Add `sweetusbsmmu=0` only if m7 showed that the recipe hurts.
`sweetusblog=1` (the default) streams the console over UDP.

Host side, once: `sudo op6/tools/op6-prep-usb-ssh.sh host`. This creates NM profile `op6-usb`
(MAC 02:00:5E:77:EE:01, 192.168.7.1/24) and a NAT dispatcher. Delete an `s9-usb` profile if
there is one, since it has the same MAC. Without NetworkManager:
```
IF=$(ip -o link | awk -F': ' '/02:00:5e:77:ee:01/{print $2}')
sudo ip addr add 192.168.7.1/24 dev $IF && sudo ip link set $IF up
# NAT (optional): sudo sysctl -w net.ipv4.ip_forward=1
#   sudo iptables -t nat -A POSTROUTING -s 192.168.7.0/24 ! -o $IF -j MASQUERADE
```
Then:
```
python3 s9/tools/s9-log.py op6-D.log &    # UDP 5140, live console from the phone
fastboot boot op6/out/boot-D.img
ip addr show                              # enp...u.. with 192.168.7.1 once the phone is up
ping 192.168.7.2
```
The kernel log (sweetlog, or `op6-D.log`) should show, in order:
- `SWEET USB OP6: cmd-db at 0x85fe0000`
- `ldoa1 880 mV ...: 0`, ldoa12, ldoa24, six BCM lines
- `SMMU ... stream 0x740`
- `GDSC and clocks on as ABL left them (warm start)`
- `QUSB2 as ABL left it: PWR_CTRL1 0x23 PLL_STATUS 0x64`, then `QUSB2 powered up, PLL ready`
- `start done (0)`
- `soft reset done`, `VBUS override on`, `running`
- events, `bus reset`, `connected, speed 0`, setups, `address N`
- `ECM endpoints configured`, `ue0 192.168.7.2/24 up`

`ping` should work as soon as the host sets SET_INTERFACE alt 1. That needs no userland,
because the kernel puts 192.168.7.2 on ue0 itself.

## 4. SSH

### How the S9 does it (`s9/tools/prep-ios6.sh`, `s9/jailbreak/`)

- `s9/jailbreak/fetch-jailbreak.sh` (user, downloads) fills `s9/jailbreak/jb/` with:
  - palera1n's Procursus rootful bootstrap `bootstrap-1800.tar.zst`;
  - ElleKit, Sileo, NewTerm, libpcapa, network-cmds;
  - **openssh-client/-server/-sftp-server 9.7p1** (Procursus 1700 pool);
  - the project's `s9net`, `S9Wallpaper` (upstream release);
  - `authorized_keys` from `~/.ssh/ios-s9.pub`.
  
  It also writes SHA256SUMS.
- `sudo s9/tools/prep-ios6.sh` sets up the PC side and builds root image v2.
  - PC: NM profile `s9-usb` (host MAC 02:00:5E:77:EE:01, 192.168.7.1/24, never-default) plus
    `/etc/NetworkManager/dispatcher.d/90-ios-s9-share` (ip_forward + MASQUERADE + FORWARD).
  - Root image v2 = v1 + the following:
    - the bootstrap, unpacked onto the HFS+ root, and ElleKit's files;
    - the debs, put in `/var/ios-s9/debs`;
    - `/usr/libexec/s9net`;
    - `/var/root/.ssh/authorized_keys`;
    - `/usr/libexec/ios-s9-jbsetup`, the first boot job: `prep_bootstrap.sh`, `pw` sets
      root/mobile to `alpine`, `dpkg -i` the debs, uicache, respring;
    - the respring script, the wallpaper and the Sileo sources.
  - `tools/jb-launchd.py` writes these jobs into `System/Library/xpc/launchd.plist`: with
    `launchd_unsecure_cache=1` only that file counts. The jobs are jbsetup, s9net (publishes
    the ue0 service to configd), ellekit loader, respring, and **com.openssh.sshd**: inetd
    style on ports 22 and 2222, `/usr/libexec/sshd-keygen-wrapper -i`, which makes missing
    host keys on first use. It also sets SpringBoard's DYLD_INSERT_LIBRARIES.
  - `trim-daemons.py` trims about 82 daemons.
- The phone IP is static in the kernel (192.168.7.2/24 on ue0, `ud_if_create`). The S9 logs in
  with `s9/tools/s9ssh.sh` = `ssh -i ~/.ssh/ios-s9 root@192.168.7.2`.

### OP6: leanest route

The pmOS kernel cannot write HFS+. Its IKCONFIG (extracted from `op6-backup/boot_b.img`, Linux
7.1.0-rc1-sdm845) has `# CONFIG_HFSPLUS_FS is not set` and `# CONFIG_HFS_FS is not set`
**[V]**. However, its initramfs has configfs and `usb_f_mass_storage`
(`CONFIG_USB_CONFIGFS_MASS_STORAGE=y`). On failure it falls into the debug shell
(`fail_halt_boot` -> telnet 172.16.42.1), which has `setup_usb_storage_configfs DEV`
(`init_functions.sh:1005`) **[V]**. So the phone can **export userdata to the PC as a USB disk**.
The PC (with hfsprogs) then edits the already-booted iOS root in place. That costs no 16 GiB
reflash and no new 30-minute first-boot migration.

Steps:

1. The user downloads once, as the normal user:
   `ssh-keygen -t ed25519 -f ~/.ssh/ios-op6 -N ''` (local), then
   `bash s9/jailbreak/fetch-jailbreak.sh`, which downloads from static.palera.in, ellekit.space,
   github (Sileo, the s9net release), repo.chariz.com and apt.procurs.us. Only the bootstrap,
   the 3 openssh debs and s9net are used by default.
2. Run `sudo op6/tools/op6-prep-usb-ssh.sh host` once.
3. Boot pmOS: `fastboot boot ~/Progetti/enchilada-work/op6-backup/boot_b.img`. It finds no pmOS
   boot partition, and after about 40 s it is in the debug shell.
   - On the PC: `telnet 172.16.42.1`.
   - On the phone:
     ```
     for d in /sys/class/block/sd*; do grep -q PARTNAME=userdata $d/uevent && echo /dev/${d##*/}; done   # expect /dev/sda17
     setup_usb_storage_configfs /dev/sda17
     ```
4. The PC sees a new disk `/dev/sdX`. Check its size: it is the whole userdata, about 110 GiB,
   with the 16 GiB HFS+ volume at its start.
   - Optional backup: `sudo dd if=/dev/sdX of=../root/userdata-head.img bs=16M count=1024 status=progress`.
   - Then run `sudo PUBKEY=$HOME/.ssh/ios-op6.pub op6/tools/op6-prep-usb-ssh.sh dev /dev/sdX`.
   - The script runs fsck, `mount -t hfsplus -o rw,force`, the root changes, umount, then fsck.
5. In telnet: `sync`. Unplug or hold power, then `fastboot boot op6/out/boot-D.img`.
6. First iOS boot: jbsetup runs (log `/var/ios-s9/jbsetup.log`). sshd already exists, because
   the openssh files were unpacked on the PC. Then `op6/tools/op6ssh.sh`, i.e.
   `ssh -i ~/.ssh/ios-op6 root@192.168.7.2`. Password `alpine` once jbsetup is done.

The fallback, if mass storage does not show up: `sudo op6/tools/op6-prep-usb-ssh.sh image
../root/ios16-root.img`, then `img2simg` and `fastboot flash userdata` the `.simg`. That loses
the migrated state and repeats the long first boot.

`FULL=1` adds everything the S9 has (ElleKit, Sileo, NewTerm, respring, SpringBoard injection)
via `s9/tools/jb-launchd.py`.

Steps that need the user with sudo: `host`, `dev`/`image`, `dd`. Downloads by the user:
`fetch-jailbreak.sh` only. Device steps: the fastboot boots and the telnet commands.

## 5. Risks

1. **SMMU** [U]: ABL's 0x740 rule may survive invisibly, so the added SMR could conflict. Or
   the hypervisor on this OnePlus firmware may differ from the S9's (writes hang).
   Mitigations: m7 first, and the `M7_SMMU=0` / `sweetusbsmmu=0` switches.
2. **PHY power-up** [U]: if CORE_READY stays 0, the full init with the S9's sequence runs. Its
   tuning is the S9's, which affects signal margin only.
3. **QMP SS PHY left on by ABL** [I]: the host's USB3 port may try SuperSpeed first (Rx
   terminations), then fall back to USB2 HS. It is not powered down here. If enumeration
   fails while run/stop is set, the next step is the QMP PCS power-down (0x88e8000 block).
4. **RPMh from the kernel on the OP6** [I]: the same TCS code is proven in shim M5 (DT cmd-db)
   and on the S9. Rail votes are not fatal, but an `s9_rpmh_init` failure stops the start
   before the SMMU, which is safe.
5. **HFS+ journal** [U]: iOS mounts the root read-write. After a hard power-off the journal may
   hold transactions that Linux `hfsplus` (`force`) ignores and hfsprogs fsck may not replay.
   The script runs fsck before mounting. Take the `dd` backup of the head first.
6. **configd vs ue0** [I]: without s9net, configd may leave ue0 alone (SSH works on the
   kernel's static IP) or start DHCP on it. s9net is installed if fetched. If the address
   vanishes, check `ifconfig ue0` over the screen console.
7. Review findings left as risks [I]:
   - An SMR already matching 0x740 is accepted without checking its S2CR or context bank
     (SCTLR.M=1 with ABL page tables would be bad). Masked SMRs are not matched either. m7's
     `ja existe` line prints the S2CR to judge from.
   - A timed-out ACTIVE TCS is never reused. Two timeouts mean every later vote fails, and the
     log shows it.
   - The minimal root installs only the openssh debs. Their libraries must already be in
     bootstrap 1800, as was true on the S9. If sshd fails, check `jbsetup.log` for dpkg
     dependency errors.
   - GNU tar follows absolute symlinks that already exist in the target. That is only an issue
     with FULL=1 on a root that already has ElleKit.
   - Already fixed after the review:
     - The bus-vote failure now aborts before the SMMU.
     - "GDSC on, clocks partial" now aborts instead of a BCR with clocks running.
     - The stop-if-running path moves the event buffer off ABL's RAM first.
     - The script refuses a non-HFS+ or mounted device.
     - The plist is now written atomically.
8. Battery: `sweetbattery=0` stays mandatory on the OP6. SweetBattery's S9 default reads the
   MAX77705, and 0106 does not gate it.
