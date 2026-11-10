# Mesa freedreno for OnePlus 6 iOS 16 userland (Milestone G3)

`op6/mesa/build.sh` cross-compiles Mesa 25.0.7 (`libOSMesa.8.dylib` with Gallium
`freedreno` + `softpipe` and an OpenGL ES 2/3 profile) for `arm64-apple-ios16.0`.

At runtime on the OnePlus 6 (`enchilada`, SDM845, Adreno 630), `freedreno` talks to
`SweetGPUUserClient` (`xnu/patches/0108-op6-gpu-userclient.patch`) through
`sweet-drm/`, a minimal `libdrm` stand-in that translates `drmOpenWithType("msm")`,
`drmIoctl`, `drmCommandWrite`, `drmCommandWriteRead`, and BO mapping into IOKit
user-client calls on the `SweetGPU` service.

## Outputs

- `out/lib/libOSMesa.8.dylib` (install name `/usr/local/lib/libOSMesa.8.dylib`, ad-hoc signed)
- `mesa-work/meta/gl.xml` and `mesa-work/meta/exported-gl.txt` (for G4 EAGL symbol table generation)

## Origin and License

`patches/0001-freedreno-darwin.patch`, `patches/0002-osmesa-es-profile.patch`,
`sweet-drm/`, and `build.sh` are adapted from:
- [oosexgang/hackios-xnu-userland](https://github.com/oosexgang/hackios-xnu-userland) /
  [oosexgang/hackios-xnu-kernel](https://github.com/oosexgang/hackios-xnu-kernel) `tools/mesa/`
  (`Copyright (c) 2026 OOSEX GANG`, MIT License)
- [aTechGeekDev/ios-apollo](https://github.com/aTechGeekDev/ios-apollo) `userland/mesa/`
  (`Copyright (c) 2026 Leo Malik`, `Copyright (c) 2026 dipa-studio`, MIT License)

Mesa 25.0.7 is MIT-licensed and downloaded at build time from `archive.mesa3d.org`.
