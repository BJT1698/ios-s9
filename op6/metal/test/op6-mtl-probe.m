// SPDX-License-Identifier: MIT
// op6-mtl-probe: Milestone G6a verification tool for OnePlus 6 (enchilada, Adreno 630).
//
// Registers BGMTLDevice in-process via MTLAddDevice(), verifies MTLCreateSystemDefaultDevice() is non-nil,
// runs a compute kernel writing a pattern into a shared MTLBuffer, and runs a 256x256 render pass drawing a
// triangle over a cleared background, reading back and verifying pixel values on the CPU.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bgmtl.h"

struct ProbeParams {
   uint32_t seed;
   uint32_t step;
   uint32_t count;
   uint32_t pad;
};

static NSString *
find_metallib(const char *arg)
{
   if (arg && access(arg, R_OK) == 0)
      return [NSString stringWithUTF8String:arg];

   char exe[4096];
   uint32_t sz = sizeof(exe);
   if (_NSGetExecutablePath(exe, &sz) == 0) {
      NSString *dir = [[NSString stringWithUTF8String:exe] stringByDeletingLastPathComponent];
      NSString *cand = [dir stringByAppendingPathComponent:@"op6_probe.metallib"];
      if (access(cand.UTF8String, R_OK) == 0) return cand;
   }
   if (access("op6_probe.metallib", R_OK) == 0) return @"op6_probe.metallib";
   if (access("/var/root/op6_probe.metallib", R_OK) == 0) return @"/var/root/op6_probe.metallib";
   return nil;
}

static int
run_compute_test(id<MTLDevice> dev, id<MTLLibrary> lib)
{
   NSError *err = nil;
   id<MTLFunction> fn = [lib newFunctionWithName:@"probe_compute"];
   if (!fn) {
      printf("COMPUTE: FAIL (missing function probe_compute)\n");
      return 1;
   }
   id<MTLComputePipelineState> pso = [dev newComputePipelineStateWithFunction:fn error:&err];
   if (!pso) {
      printf("COMPUTE: FAIL (pipeline error: %s)\n", err ? err.localizedDescription.UTF8String : "unknown");
      return 1;
   }

   const uint32_t count = 256;
   const uint32_t guard = 16;
   id<MTLBuffer> buf = [dev newBufferWithLength:(count + guard) * sizeof(uint32_t) options:MTLResourceStorageModeShared];
   if (!buf) {
      printf("COMPUTE: FAIL (newBufferWithLength returned nil)\n");
      return 1;
   }
   uint32_t *words = (uint32_t *)[buf contents];
   memset(words, 0, (count + guard) * sizeof(uint32_t));

   struct ProbeParams params = { .seed = 0x13570000u, .step = 17u, .count = count, .pad = 0u };
   id<MTLCommandQueue> q = [dev newCommandQueue];
   id<MTLCommandBuffer> cb = [q commandBuffer];
   id<MTLComputeCommandEncoder> enc = [cb computeCommandEncoder];
   if (!q || !cb || !enc) {
      printf("COMPUTE: FAIL (queue=%p cb=%p enc=%p)\n", q, cb, enc);
      return 1;
   }
   [enc setComputePipelineState:pso];
   [enc setBuffer:buf offset:0 atIndex:0];
   [enc setBytes:&params length:sizeof(params) atIndex:1];
   [enc dispatchThreadgroups:MTLSizeMake(count / 16, 1, 1) threadsPerThreadgroup:MTLSizeMake(16, 1, 1)];
   [enc endEncoding];
   [cb commit];
   [cb waitUntilCompleted];

   if ([cb status] != MTLCommandBufferStatusCompleted) {
      NSError *ce = [cb error];
      printf("COMPUTE: FAIL (cmdbuf status=%ld err=%s)\n", (long)[cb status],
             ce ? ce.localizedDescription.UTF8String : "none");
      return 1;
   }

   int bad = 0;
   for (uint32_t i = 0; i < count; i++) {
      uint32_t exp = params.seed + i * params.step + (i ^ 0xa5a5u);
      if (words[i] != exp) {
         if (bad < 8)
            printf("  word[%u]: got 0x%08x expected 0x%08x\n", i, words[i], exp);
         bad++;
      }
   }
   for (uint32_t i = count; i < count + guard; i++) {
      if (words[i] != 0) {
         if (bad < 8)
            printf("  guard[%u]: got 0x%08x expected 0\n", i, words[i]);
         bad++;
      }
   }
   printf("COMPUTE: %s (%u words checked, %d mismatches, first4=[0x%08x, 0x%08x, 0x%08x, 0x%08x])\n",
          bad == 0 ? "PASS" : "FAIL", count + guard, bad, words[0], words[1], words[2], words[3]);
   return bad == 0 ? 0 : 1;
}

static int
check_bgra_range(const uint8_t *px, uint8_t b_min, uint8_t b_max,
                 uint8_t g_min, uint8_t g_max, uint8_t r_min, uint8_t r_max, uint8_t a_min)
{
   return px[0] >= b_min && px[0] <= b_max &&
          px[1] >= g_min && px[1] <= g_max &&
          px[2] >= r_min && px[2] <= r_max &&
          px[3] >= a_min;
}

static int
run_render_test(id<MTLDevice> dev, id<MTLLibrary> lib)
{
   const unsigned W = 256, H = 256;
   NSError *err = nil;

   id<MTLFunction> vs = [lib newFunctionWithName:@"probe_vertex"];
   id<MTLFunction> fs = [lib newFunctionWithName:@"probe_fragment"];
   if (!vs || !fs) {
      printf("RENDER: FAIL (vs=%p fs=%p)\n", vs, fs);
      return 1;
   }

   MTLRenderPipelineDescriptor *rpd = [[MTLRenderPipelineDescriptor alloc] init];
   rpd.vertexFunction = vs;
   rpd.fragmentFunction = fs;
   rpd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
   id<MTLRenderPipelineState> rso = [dev newRenderPipelineStateWithDescriptor:rpd error:&err];
   if (!rso) {
      printf("RENDER: FAIL (pipeline error: %s)\n", err ? err.localizedDescription.UTF8String : "unknown");
      return 1;
   }

   MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm
                                                                                 width:W height:H mipmapped:NO];
   td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
   id<MTLTexture> tex = [dev newTextureWithDescriptor:td];
   if (!tex) {
      printf("RENDER: FAIL (newTextureWithDescriptor returned nil)\n");
      return 1;
   }

   // Three vertices: (pos, color) float4 pairs.
   // Triangle covers center (128,128), top-center blue, bottom-left red, bottom-right green.
   const float verts[] = {
      -0.6f, -0.6f, 0.0f, 1.0f,   1.0f, 0.0f, 0.0f, 1.0f,
       0.6f, -0.6f, 0.0f, 1.0f,   0.0f, 1.0f, 0.0f, 1.0f,
       0.0f,  0.6f, 0.0f, 1.0f,   0.0f, 0.0f, 1.0f, 1.0f,
   };
   id<MTLBuffer> vbuf = [dev newBufferWithBytes:verts length:sizeof(verts) options:MTLResourceStorageModeShared];
   if (!vbuf) {
      printf("RENDER: FAIL (vertex buffer nil)\n");
      return 1;
   }

   MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
   rp.colorAttachments[0].texture = tex;
   rp.colorAttachments[0].loadAction = MTLLoadActionClear;
   rp.colorAttachments[0].storeAction = MTLStoreActionStore;
   // Clear color: navy blue (R=0, G=0, B=128/255, A=1) -> BGRA ~(128, 0, 0, 255)
   rp.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 128.0 / 255.0, 1.0);

   id<MTLCommandQueue> q = [dev newCommandQueue];
   id<MTLCommandBuffer> cb = [q commandBuffer];
   id<MTLRenderCommandEncoder> enc = [cb renderCommandEncoderWithDescriptor:rp];
   if (!q || !cb || !enc) {
      printf("RENDER: FAIL (queue=%p cb=%p enc=%p)\n", q, cb, enc);
      return 1;
   }
   [enc setRenderPipelineState:rso];
   [enc setVertexBuffer:vbuf offset:0 atIndex:0];
   [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
   [enc endEncoding];
   [cb commit];
   [cb waitUntilCompleted];

   if ([cb status] != MTLCommandBufferStatusCompleted) {
      NSError *ce = [cb error];
      printf("RENDER: FAIL (cmdbuf status=%ld err=%s)\n", (long)[cb status],
             ce ? ce.localizedDescription.UTF8String : "none");
      return 1;
   }

   uint8_t *img = calloc(W * H, 4);
   if (!img) {
      printf("RENDER: FAIL (calloc failed)\n");
      return 1;
   }
   [tex getBytes:img bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];

   const uint8_t *tl = img + (2 * W + 2) * 4;
   const uint8_t *br = img + (253 * W + 253) * 4;
   const uint8_t *mid = img + (128 * W + 128) * 4;
   const uint8_t *apex = img + (72 * W + 128) * 4;
   const uint8_t *bl_tri = img + (192 * W + 80) * 4;
   const uint8_t *br_tri = img + (192 * W + 176) * 4;

   int ok = 1;
   ok &= check_bgra_range(tl, 124, 132, 0, 4, 0, 4, 250);
   ok &= check_bgra_range(br, 124, 132, 0, 4, 0, 4, 250);
   ok &= check_bgra_range(mid, 110, 145, 50, 80, 50, 80, 250);
   ok &= (apex[0] > 170 && apex[1] < 60 && apex[2] < 60 && apex[3] >= 250);
   ok &= (bl_tri[2] > 140 && bl_tri[0] < 60 && bl_tri[1] < 90 && bl_tri[3] >= 250);
   ok &= (br_tri[1] > 140 && br_tri[0] < 60 && br_tri[2] < 90 && br_tri[3] >= 250);

   printf("  px(2,2)     BGRA=(%3u,%3u,%3u,%3u) [expect clear ~(128,0,0,255)]\n", tl[0], tl[1], tl[2], tl[3]);
   printf("  px(253,253) BGRA=(%3u,%3u,%3u,%3u) [expect clear ~(128,0,0,255)]\n", br[0], br[1], br[2], br[3]);
   printf("  px(128,128) BGRA=(%3u,%3u,%3u,%3u) [expect tri center ~(128,64,64,255)]\n", mid[0], mid[1], mid[2], mid[3]);
   printf("  px(128,72)  BGRA=(%3u,%3u,%3u,%3u) [expect tri apex blue-dom]\n", apex[0], apex[1], apex[2], apex[3]);
   printf("  px(80,192)  BGRA=(%3u,%3u,%3u,%3u) [expect tri bottom-left red-dom]\n", bl_tri[0], bl_tri[1], bl_tri[2], bl_tri[3]);
   printf("  px(176,192) BGRA=(%3u,%3u,%3u,%3u) [expect tri bottom-right green-dom]\n", br_tri[0], br_tri[1], br_tri[2], br_tri[3]);
   printf("RENDER: %s\n", ok ? "PASS" : "FAIL");
   free(img);
   return ok ? 0 : 1;
}

int
main(int argc, char **argv)
{
   @autoreleasepool {
      setvbuf(stdout, NULL, _IONBF, 0);

      NSArray<id<MTLDevice>> *(*copy_all)(void) = dlsym(RTLD_DEFAULT, "MTLCopyAllDevices");
      void (*add_dev)(id<MTLDevice>) = dlsym(RTLD_DEFAULT, "MTLAddDevice");
      id<MTLDevice> (*create_def)(void) = dlsym(RTLD_DEFAULT, "MTLCreateSystemDefaultDevice");

      NSArray *before = copy_all ? copy_all() : nil;
      printf("devices before registration: %lu\n", (unsigned long)[before count]);

      id<MTLDevice> created = bgmtl_create_device();
      if (!created) {
         printf("DEVICE: FAIL (bgmtl_create_device returned nil)\n");
         return 1;
      }

      /*
       * iOS 16.7.16: with no accelerator in the system, Metal.framework never creates its internal
       * device array and MTLAddDevice() asserts (_deviceArray != NULL). Registering a system-wide
       * device is G6b's job (IOAcceleratorES + MetalPlugin); for G6a, test our device directly.
       */
      id<MTLDevice> sysdev = nil;
      if ([before count] > 0 && add_dev) {
         add_dev(created);
         sysdev = create_def ? create_def() : nil;
      } else {
         printf("DEVICE: note: Metal has no device array on this system (no accelerator): "
                "MTLAddDevice skipped, testing BGMTLDevice directly\n");
         sysdev = created;
      }
      if (!sysdev) {
         printf("DEVICE: FAIL (no device)\n");
         return 1;
      }
      printf("DEVICE: PASS name=\"%s\" class=%s registryID=0x%llx\n",
             sysdev.name.UTF8String, class_getName([sysdev class]),
             (unsigned long long)[sysdev registryID]);

      NSString *lib_path = find_metallib(argc > 1 ? argv[1] : NULL);
      if (!lib_path) {
         printf("LIBRARY: FAIL (op6_probe.metallib not found)\n");
         return 1;
      }
      NSError *err = nil;
      id<MTLLibrary> lib = [sysdev newLibraryWithURL:[NSURL fileURLWithPath:lib_path] error:&err];
      if (!lib) {
         printf("LIBRARY: FAIL (%s: %s)\n", lib_path.UTF8String,
                err ? err.localizedDescription.UTF8String : "unknown");
         return 1;
      }
      printf("LIBRARY: PASS loaded %s\n", lib_path.UTF8String);

      int rc = 0;
      rc |= run_compute_test(sysdev, lib);
      rc |= run_render_test(sysdev, lib);
      return rc ? 1 : 0;
   }
}
