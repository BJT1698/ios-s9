#!/usr/bin/env python3
"""Build the minimal Apple Device Tree that the SWEET xnu needs to boot,
OnePlus 6 (enchilada, SDM845, 8 GB) edition of s9/tools/mkadt.py (Galaxy S9),
itself derived from ios-sweet's tools/mkadt.py (MIT, dipa-studio).

Differences from the S9 tree (see op6/XNU-PLAN.md):
  * DRAM span: dram-base 0xae000000 (32 MiB aligned, above the ramoops window
    0xac000000-0xad000000 the shim keeps its flight recorder in), top
    0x180000000 by default (3.28 GiB, the S9's order of size), --mem-top up to
    0x27c000000 (the end of the range the OP6 RAM scan verified).
  * /chosen/sweet-ram-holes: the 16 MiB scan windows inside the span that hold
    firmware memory (rmtfs 0xf5b00000+0x202000); needs xnu patch 0103.
    Always emitted (possibly empty) so the kernel's built-in S9 holes never
    apply to an OnePlus 6.
  * framebuffer 1080x2280 stride 4320 at 0x9d400000 (ABL's splash).
  * root compatible carries oneplus,enchilada instead of samsung,starqltechn
    ('xiaomi,sweet' must stay: SweetPlatformExpert matches it), and
    /chosen/sweet-board = "oneplus,enchilada" for board-specific drivers.

  * --gpu-fw DIR (xnu patch 0107, GPU milestone G1): node /sweet-gpu with the
    Adreno 630 firmware the kernel loads itself (a630_gmu.bin, a630_sqe.fw,
    OnePlus-signed a630_zap.mbn, checked against op6/GPU-NOTES.md sec. 4
    hashes) and dma-region = <u64 pa, u64 size>, the GPU carve-out outside
    the XNU span (--gpu-dma, default 0xa0000000+16 MiB: free RAM per the OP6
    scan, no DT reservation). The firmware is Qualcomm proprietary: the ADT
    (op6/out/, git-ignored) carries it, git never does.

Usage:  mkadt-op6.py OUT.adt [--scale2] [--mem-top 0xADDR] [--phys-base 0xADDR]
                     [--d22ap PATH] [--dump] [--qemu] [--gpu-fw DIR [--gpu-dma PA,SIZE]]
"""
import hashlib, os, struct, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, '..', '..', 's9', 'tools'))
import adt  # noqa: E402  (s9/tools/adt.py, unchanged)

# Kernel-managed RAM. OnePlus 6 8 GB: DRAM 0x80000000-0x27c8a0000. The OP6
# shim RAM scan (MMU off, one sample per 16 MiB) found free, independent RAM
# in [0xa0000000,0xac000000) [0xad000000,0xf5000000) [0xf6000000,0x27c000000).
# Never below 0xa0000000 (Qualcomm carve-outs, ABL staging of shim/DTB/payload,
# splash 0x9d400000-0x9f800000).
PHYS_BASE = 0xae000000          # 32 MiB aligned (start.s maps 32 MiB L2 blocks)
MEM_TOP = 0x180000000           # default top: end of DDR bank 0 (3.28 GiB span)
MEM_TOP_MAX = 0x27c000000       # highest verified address (exclusive)
# Scan windows (16 MiB) that are firmware memory: they stay in the span (XNU
# wants it contiguous) but xnu patch 0103 never puts their pages on the free list.
KEEP_OUT = [
    (0xac000000, 0xad000000),   # ramoops 0xac300000+0x400000 (shim flight recorder)
    (0xf5000000, 0xf6000000),   # rmtfs 0xf5b00000+0x202000 (modem shared memory)
]
# Early boot allocations (vm_pages array, page tables) are stolen from the span
# start without any hole check: keep this much hole-free above dram-base.
HOLE_FREE_LOW = 0x10000000
MEM_SIZE = MEM_TOP - PHYS_BASE
# Panic log buffer, carved from the TOP of the dram range and exposed to XNU as
# /pram + /chosen/embedded-panic-log-size. Without it, pe_init.c's
# check_for_panic_log() leaves gPanicBase == 0, debug_log_init() bails out twice
# and the kernel hangs right after "kdp_core zlib memory" — reproduced on QEMU
# and independently on ios-fajita.
PANIC_LOG_SIZE = 0x100000
# /arm-io children are relative to this; it must not be 0 (serial_init bails)
SOC_BASE = 0x01000000

GICD, GICD_SIZE = 0x17a00000, 0x10000
GICR, GICR_SIZE = 0x17a60000, 0x100000
# splash@9d400000 (0x2400000); OP6 shim M1 read the scanout: SSPP VIG0 at
# 0x9d400000, 1080x2280, ystride 4320, B G R A (fmt 0x000237ff, unpack 0x03020001)
FB_W, FB_H, FB_STRIDE = 1080, 2280, 1080 * 4
FB, FB_SIZE = 0x9d400000, (FB_STRIDE * FB_H + 0x3fff) & ~0x3fff     # 0x968000
SWEET_ECID = 0x5357454554000001  # 'SWEET' + 1, synthetic
# Internal UFS host (ufshc@1d84000 "ufs_mem"); QEMU virt has none
UFS, UFS_SIZE = 0x01d84000, 0x3000
TIMEBASE = 19200000
VTIMER_INTID = 19
# QEMU virt (-M virt,gic-version=3 -cpu cortex-a76): GICD/GICR and the generic timer's CNTFRQ
QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE = 0x08000000, 0x080a0000, 62500000
QEMU_VTIMER_INTID = 27

# Canvas (pixels) and artwork subtype per display-scale. UIKit picks the
# status bar layout (_UIStatusBarVisualProvider_Split<width>) by the native
# pixel width AND the notch size, and the notch comes from SpringBoard's
# screen type, looked up from points, scale and artwork-device-subtype
# (SBHScreenTypeForCurrentDevice). So @2x must be a whole iPhone XR screen:
# 828 px wide (Split828) and subtype 1792, or the bar falls back to red.
CANVAS = {3: (1125, 2436, 2436), 2: (828, 1792, 1792)}

BOOT_MANIFEST_HASH = hashlib.sha384(b'sweet boot manifest').digest()

# The iOS userland is the iPhone X (iPhone10,3 / D22AP) IPSW. MobileGestalt
# derives product type, device class, home-button-less UI, artwork etc. from
# the root identity and /product of the real D22AP device tree. That tree is
# an Apple artifact (kept out of git): extract it from the IPSW with
#   tools/remotezip.py URL DeviceTree.d22ap.im4p DIR
#   ipsw img4 im4p extract DIR/Firmware/all_flash/DeviceTree.d22ap.im4p -o d22ap.adt
D22AP_ADT = os.path.join(HERE, '..', '..', 's9', 'research', 'ipsw-x', 'd22ap.adt')


def d22ap_product(path=D22AP_ADT):
    """/product properties of the real D22AP tree, without the syscfg
    placeholders iBoot would fill in (flag bit set) and the node bookkeeping."""
    if not os.path.exists(path):
        print(f'warning: {path} missing, no /product node', file=sys.stderr)
        return None
    root, _ = adt.parse(open(path, 'rb').read())
    for c in root.children:
        if dict((k, v) for k, f, v in c.props).get('name') == b'product\0':
            return [(k, v) for k, f, v in c.props if not f and k not in ('name', 'AAPL,phandle')]
    raise SystemExit(f'{path}: no /product node')

u32 = lambda *v: struct.pack('<%dI' % len(v), *v)
u64 = lambda *v: struct.pack('<%dQ' % len(v), *v)
cstr = lambda s: s.encode() + b'\0'


def node(name, props=(), children=()):
    n = adt.Node()
    n.props = [('name', 0, cstr(name))] + [(k, 0, v) for k, v in props]
    n.children = list(children)
    return n


def ram_holes(phys_base, top):
    """KEEP_OUT windows clipped to [phys_base, top), checked against the scan."""
    if phys_base < 0xa0000000 or phys_base & 0x1ffffff:
        raise SystemExit(f'phys-base {phys_base:#x}: must be >= 0xa0000000 and 32 MiB aligned')
    if not phys_base + 0x20000000 <= top <= MEM_TOP_MAX or top & 0x3fff:
        raise SystemExit(f'mem-top {top:#x}: must be 16K aligned, >= base + 512 MiB, <= {MEM_TOP_MAX:#x}')
    holes = [(max(s, phys_base), min(e, top)) for s, e in KEEP_OUT if s < top and e > phys_base]
    for s, e in holes:
        if s < phys_base + HOLE_FREE_LOW:
            raise SystemExit(f'hole {s:#x}-{e:#x} within {HOLE_FREE_LOW:#x} of phys-base: '
                             'early boot allocations would land in it')
    return holes


# GPU firmware for /sweet-gpu (op6/GPU-NOTES.md sec. 4; same hashes as tools/gpu-fw-embed*.py)
GPU_FW = [
    ('gmu-firmware', 'a630_gmu.bin', 'da8d9b1b1f5c1a0b311f32567093b4828f3c80031dd8435f91ac13c664e173a6'),
    ('sqe-firmware', 'a630_sqe.fw', '1c21b527d9183487cc550dabbb3f43e555df5a977a461934fc61f0635a9aa90c'),
    ('zap-firmware', 'a630_zap.mbn', '9780956733759d24740b3df5bd804e5dddc48bc0e956271b0288631890080df9'),
]
GPU_DMA = (0xa0000000, 0x1000000)   # below dram-base, in the scan's free [0xa0000000,0xac000000)


def gpu_node(fw_dir, dma, phys_base, top, ph):
    """/sweet-gpu: firmware blobs + carve-out (sweet_gpu.c setup())."""
    pa, size = dma
    if pa & 0xffff or not 0x100000 <= size <= 0x10000000:
        raise SystemExit(f'gpu-dma {pa:#x},{size:#x}: 64 KiB aligned, 1..256 MiB')
    if pa < 0xa0000000 or pa + size > 0xac000000:
        raise SystemExit(f'gpu-dma {pa:#x},{size:#x}: must stay inside the free [0xa0000000,0xac000000)')
    if pa < top and pa + size > phys_base:
        raise SystemExit(f'gpu-dma {pa:#x},{size:#x} overlaps the XNU span {phys_base:#x}-{top:#x}')
    props = [ph()]
    for prop, name, sha in GPU_FW:
        data = open(os.path.join(fw_dir, name), 'rb').read()
        if hashlib.sha256(data).hexdigest() != sha:
            raise SystemExit(f'{name}: sha256 differs from op6/GPU-NOTES.md sec. 4 (wrong file / OEM zap?)')
        props.append((prop, data + b'\0' * (-len(data) & 3)))
    props.append(('dma-region', u64(pa, size)))
    return node('sweet-gpu', props)


def build(seed=None, qemu=False, scale=3, phys_base=PHYS_BASE, top=MEM_TOP, d22ap=D22AP_ADT,
          gpu_fw=None, gpu_dma=GPU_DMA):
    """qemu=True: same tree, but with QEMU virt's GIC addresses and CNTFRQ (tools/qemu-sweet.py)."""
    mem_size = top - phys_base
    holes = ram_holes(phys_base, top)
    # Carve the panic log off the top so the kernel never allocates over it.
    panic_base = phys_base + mem_size - PANIC_LOG_SIZE
    mem_size -= PANIC_LOG_SIZE
    gicd, gicr, timebase = (QEMU_GICD, QEMU_GICR, QEMU_TIMEBASE) if qemu else (GICD, GICR, TIMEBASE)
    seed = seed if seed is not None else os.urandom(64)
    phandle = iter(range(1, 1000))
    ph = lambda: ('AAPL,phandle', u32(next(phandle)))

    chosen = node('chosen', [
        ph(),
        ('dram-base', u64(phys_base)),
        ('dram-size', u64(mem_size)),
        # xnu 0103: (start, end) pairs never handed to the VM free list.
        ('sweet-ram-holes', b''.join(u64(s, e) for s, e in holes)),
        # Board selector for board-specific kernel drivers (keys, touch,
        # battery: patches after 0103); ignored by kernels up to 0103.
        ('sweet-board', cstr('oneplus,enchilada')),
        ('random-seed', seed),
        ('debug-enabled', u32(1)),
        ('embedded-panic-log-size', u32(PANIC_LOG_SIZE)),
        ('firmware-version', cstr('sweet-shim')),
        ('system-firmware-version', cstr('sweet-shim')),
        # iBoot's SHA-384 of the boot manifest; /sbin/mount -P 1 exits unless
        # it exists (bind mounts /private/preboot/<hex>/usr/standalone/firmware,
        # tolerating a missing directory). Fixed stand-in value.
        ('boot-manifest-hash', BOOT_MANIFEST_HASH),
        # libimg4 (cryptexd) aborts "failed to get crypto algorithm" without
        # it; real A11+ device trees say sha2-384.
        ('crypto-hash-method', cstr('sha2-384')),
        # MobileGestalt's screen class: display-scale (u32, default 0) comes
        # from /chosen, the pixel size from IOMobileFramebuffer. Scale 0 makes
        # SpringBoard's FBSDisplayMode "infxinf@0x" invalid. 1080x2400 @3x =
        # 360x800 points (like iPhone X class devices).
        # Galaxy S9 --scale2: the iPhone XR/11 screen, 414x896 points at @2x
        # (828x1792), 46% fewer pixels than @3x for CoreAnimation's CPU
        # renderer; the kernel reads the canvas size from disp0 below and
        # scales it to the panel. 375x812 @2x (750x1624) is no real iPhone:
        # UIKit has no status bar provider for it (red "rdar:45025538" bar).
        ('display-scale', u32(scale)),
        # XNU derives IOPlatformUUID (gethostuuid) from these on arm; without
        # them CFPreferences cannot use kCFPreferencesCurrentHost (no ByHost
        # files), so e.g. DataMigration never records LastSystemVersion and
        # migrates on every boot. Synthetic ECID (not the phone's); chip-id
        # of the A11 that the iPhone X userland expects.
        ('unique-chip-id', struct.pack('<Q', SWEET_ECID)),
        ('chip-id', u32(0x8015)),
        # Security status as on a dev-fused, undemoted board (tuple 0,0,1,1):
        # MobileGestalt reads these from /chosen, and mobileactivationd's
        # DeviceType then takes its native "Hactivation" branch (state
        # Activated without an activation record; no SEP, no baseband here).
        ('certificate-production-status', u32(0)),
        ('effective-production-status-ap', u32(0)),
        ('certificate-security-mode', u32(1)),
        ('effective-security-mode-sep', u32(1)),
    ], [node('memory-map', [ph()]),
        # Local boot policy (kern_csr.c): SIP relaxed like `csrutil disable`
        # (lp-sip0: untrusted kexts, unrestricted FS, task_for_pid, kernel
        # debugger, dtrace, nvram), lp-sip1: unauthenticated (unsealed) root.
        # The iOS shared cache is in no trust cache, so dyld can only map it
        # with CSR_ALLOW_UNRESTRICTED_FS (vm_unix.c shared region checks).
        node('asmb', [ph(), ('lp-sip0', u64(0x6f)), ('lp-sip1', u32(1))])])

    # OnePlus 6 = Galaxy S9 SoC (shim M2 on the OP6: CPU_ON 0x100..0x700 OK).
    # Galaxy S9 SMP (shim M2): eight cores in physical-core order, 0-3 Cortex-A55
    # (MIDR 0x517F803C), 4-7 Cortex-A75 (0x516F802D), one homogeneous cluster.
    # 'qcom,kryo470' stays: KernelConfigTables.cpp matches the CPU driver on it. The kernel defaults to one
    # and uses cpus=N to select an initial prefix (N <= 8).
    # QEMU retains its original single-CPU tree: its MPIDRs differ from sweet.
    cpu_nodes = []
    for core in range(1 if qemu else 8):
        cpu_nodes.append(node(f'cpu{core}', [
            ph(),
            ('device_type', cstr('cpu')),
            ('compatible', cstr('qcom,kryo470') + cstr('ARM,v8')),
            ('reg', u32(core << 8)),  # actual MPIDR Aff1:Aff0, NOT logical ID
            ('cpu-id', u32(core)),   # descriptive: XNU assigns IDs by DT order
            ('cluster-id', u32(0)),  # descriptive; see ml_parse_cpu_topology
            ('cluster-type', u32(0)),  # homogeneous CLUSTER_TYPE_SMP, not AMP
            ('die-cluster-id', u32(0)),
            ('cluster-core-id', u32(core)),
            ('l2-cache-id', u32(core)),  # distinct per-core L2 nodes in live FDT
            ('l3-cache-id', u32(0)),     # shared L3
            ('state', cstr('running' if core == 0 else 'stopped')),
            ('timebase-frequency', u32(timebase)),
            ('clock-frequency', u32(2803200000 if core >= 4 else 1766400000)),
            ('bus-frequency', u32(100000000)),
            ('memory-frequency', u32(1866000000)),
            ('peripheral-frequency', u32(timebase)),
            ('fixed-frequency', u32(timebase)),
        ]))
    cpus = node('cpus', [ph(), ('#address-cells', u32(1)),
                         ('#size-cells', u32(0))], cpu_nodes)

    gic = node('gic', [
        ph(),
        ('compatible', cstr('arm,gic-v3')),
        ('interrupt-controller', b''),
        ('virtual-timer-intid', u32(QEMU_VTIMER_INTID if qemu else VTIMER_INTID)),
        ('reg', u64(gicd - SOC_BASE, GICD_SIZE, gicr - SOC_BASE, GICR_SIZE)),
    ])
    arm_io = node('arm-io', [
        ph(),
        ('device_type', cstr('sweet-io')),
        ('compatible', cstr('sweet-io') + cstr('arm-io')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
        ('ranges', u64(0, SOC_BASE, 0x100000000 - SOC_BASE)),
    ], [gic,
        # No hardware: watchdogd checks IODeviceTree:/arm-io/wdt for no-pmu and
        # otherwise crashes trying to set the PMU boot stage (customer builds
        # ignore the -wdt_no_pmu boot-arg).
        node('wdt', [ph(), ('no-pmu', u32(1))]),
        # Main display: IOMobileFramebuffer.framework counts /arm-io children
        # with this device_type and reads the canvas size from the node
        # (the presented iPhone X size, see SweetFramebuffer.cpp);
        # the kernel's AppleCLCD matches the compatible string.
        node('disp0', [
            ph(),
            ('device_type', cstr('display-subsystem')),
            ('compatible', cstr('sweet,display')),
            ('canvas-width', u32(CANVAS[scale][0])),
            ('canvas-height', u32(CANVAS[scale][1])),
        ])])

    fbcon = node('sweet-fbcon', [
        ph(),
        ('reg', u64(FB, FB_SIZE)),
        ('width', u32(FB_W)),
        ('height', u32(FB_H)),
        ('stride', u32(FB_STRIDE)),
    ])

    # bsd/dev/sweet_ufs.c: the UFS host ABL leaves running (absolute physical reg).
    ufs = node('sweet-ufs', [
        ph(),
        ('compatible', cstr('qcom,ufshc')),
        ('reg', u64(UFS, UFS_SIZE)),
    ])

    # pmap_compute_max_asids requires the node even when no overrides are set.
    # An absent pmap-max-asids property selects XNU's own MAX_ASIDS default.
    # No effaceable storage: keybagd then reads /private/var/keybags/systembag.kb
    # as a plain plist instead of decrypting it with the effaceable key.
    defaults = node('defaults', [ph(), ('no-effaceable-storage', u32(1))])

    # MobileGestalt HomeButtonType = IODeviceTree:/buttons home-button-type
    # (D22AP: 2, no Home button; absent = 0 = Home-button device).
    buttons = node('buttons', [
        ph(),
        ('button-names', cstr('volup') + cstr('voldown') + cstr('hold')),
        ('home-button-type', u32(2)),
        ('opposed-power-vol-buttons', u32(1)),
        # OP6 volume-down is PM8998 GPIO5 (RT_STS 0xc410, active low), not
        # PON RESIN: read by sweet_keys.c (patch 0104) when present.
        ('voldown-pmic-gpio', u32(5)),
    ])

    product_props = d22ap_product(d22ap)
    if product_props and scale != 3:
        over = {'artwork-device-subtype': u32(CANVAS[scale][2]), 'artwork-scale-factor': u32(scale)}
        product_props = [(k, over.get(k, v)) for k, v in product_props]
    product = [node('product', [ph()] + product_props)] if product_props else []

    # Panic log region. /pram reg = <phys size>; it sits just above the
    # dram-size we expose, so XNU maps it but never allocates into it.
    pram = node('pram', [ph(), ('reg', u64(panic_base) + u64(PANIC_LOG_SIZE))])

    # Identity of the iPhone X whose userland runs here: compatible[0] is
    # hw.model/hw.target (D22AP), model is hw.machine/hw.product
    # (iPhone10,3). 'xiaomi,sweet' stays for SweetPlatformExpert matching.
    return node('device-tree', [
        ph(),
        ('compatible', cstr('D22AP') + cstr('iPhone10,3') + cstr('xiaomi,sweet') + cstr('oneplus,enchilada') + cstr('qcom,sdm845') + cstr('AppleARM')),
        ('model', cstr('iPhone10,3')),
        ('target-type', cstr('D22')),
        ('target-sub-type', cstr('D22AP')),
        ('manufacturer', cstr('Apple Inc.')),
        ('#address-cells', u32(2)),
        ('#size-cells', u32(2)),
    ], [chosen, defaults, cpus, arm_io, fbcon, buttons, pram] + product + ([] if qemu else [ufs]) +
       ([gpu_node(gpu_fw, gpu_dma, phys_base, top, ph)] if gpu_fw else []))


if __name__ == '__main__':
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument('out')
    ap.add_argument('--scale2', action='store_true', help='828x1792 @2x canvas (iPhone XR identity)')
    ap.add_argument('--qemu', action='store_true')
    ap.add_argument('--dump', action='store_true')
    ap.add_argument('--phys-base', type=lambda s: int(s, 0), default=PHYS_BASE)
    ap.add_argument('--mem-top', type=lambda s: int(s, 0), default=MEM_TOP)
    ap.add_argument('--d22ap', default=D22AP_ADT, help='D22AP device tree extracted from the iPhone X IPSW')
    ap.add_argument('--gpu-fw', metavar='DIR', help='a630_gmu.bin/a630_sqe.fw/a630_zap.mbn -> /sweet-gpu (xnu 0107)')
    ap.add_argument('--gpu-dma', default='%#x,%#x' % GPU_DMA, help='GPU carve-out PA,SIZE (outside the span)')
    a = ap.parse_args()
    dma = tuple(int(x, 0) for x in a.gpu_dma.split(','))
    root = build(qemu=a.qemu, scale=2 if a.scale2 else 3, phys_base=a.phys_base, top=a.mem_top, d22ap=a.d22ap,
                 gpu_fw=a.gpu_fw, gpu_dma=dma)
    blob = adt.serialize(root)
    open(a.out, 'wb').write(blob)
    if a.dump:
        adt.dump(adt.parse(blob)[0])
    print(f'{a.out}: {len(blob)} bytes, dram {a.phys_base:#x}-{a.mem_top:#x} '
          f'(minus {PANIC_LOG_SIZE:#x} panic log), holes {[(hex(s), hex(e)) for s, e in ram_holes(a.phys_base, a.mem_top)]}',
          file=sys.stderr)
