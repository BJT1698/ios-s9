# CoreAnimation / EAGLContext Bridge (`op6/ca-bridge`)

Milestone **G4** userland bridge that switches `backboardd`'s CoreAnimation render server (`QuartzCore`) from the CPU software rasterizer to OpenGL ES 3.2 on the Adreno 630 GPU via Mesa's `libOSMesa.8.dylib` (`freedreno`).

## Components

- `libsweet-gl.dylib` (`eagl.c`, `sweet_gl_osmesa.c`, `sweet_gl.h`, `eagl_slots.txt`, `mkeagl.py`):
  Replaces `EAGLContext`'s methods (`-initWithAPI:properties:`, `-getMacroContextPrivate`, `-texImageIOSurface:...`, `-attachImage:toCoreSurface:invertedRender:`) with a dispatch table backed by `libOSMesa.8.dylib` (`freedreno` on `SweetGPUUserClient`), sharing `'BGRA'` `IOSurface`s zero-copy with the Adreno 630 SMMU via `glEGLImageTargetTexture2DOES` (selector `0x7e`) and translating CoreAnimation's GLSL shaders where needed.
- `libsweet-ca.dylib` (`sweet_ca.c`):
  Injected into `backboardd` via the kernel boot-arg `sweetinject=/usr/local/lib/libsweet-ca.dylib:backboardd`. Hooks `CA::WindowServer::Server::context()` and `CARenderOGLNewContext` so `backboardd` creates a `CA::OGL::EAGLContext` instead of falling back to the software rasterizer.

## License & Attribution

Adapted from `aTechGeekDev/ios-apollo` (`userland/ca-bridge/`, MIT License) and `oosexgang/hackios-xnu-kernel` (`userland/src/`, MIT License; see `LICENSE.HackiOS`).
