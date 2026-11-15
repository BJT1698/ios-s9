// SPDX-License-Identifier: MIT
//
// Chlorophyll Metal plugin bundle for OnePlus 6 (Adreno 630 / iOS 16.7.16 20H392).
// Loaded by Metal.framework from /System/Library/Extensions/Chlorophyll.bundle
// when xnu patch 0117 publishes SweetAccelerator : IOAcceleratorES (sweetmetal=1).
//
// Adapted from Chlorophyll/plugin/chlorophyll_plugin.m (@787338f, MIT) with
// OP6 allowlist filtering (/var/local/op6-mtl-clients) and iOS 16.7.16 SPI
// patterns from ios-flame SweetMetal2.mm (MIT).

#include <ctype.h>
#include <errno.h>
#include <os/log.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "../src/bgmtl.m"

#define CHL_ALLOWLIST_PATH "/var/local/op6-mtl-clients"
#define CHL_KILLSWITCH_PATH "/var/local/op6-mtl.off"

static os_log_t
chl_log(void)
{
   static os_log_t lg;
   static dispatch_once_t once;
   dispatch_once(&once, ^{ lg = os_log_create("com.op6.chlorophyll", "plugin"); });
   return lg;
}

static char *
chl_trim(char *s)
{
   while (*s && isspace((unsigned char)*s)) s++;
   size_t n = strlen(s);
   while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
   return s;
}

static const char *
chl_gate_reason(unsigned *minor_out)
{
   if (access(CHL_KILLSWITCH_PATH, F_OK) == 0)
      return "disabled by " CHL_KILLSWITCH_PATH;

   const char *proc = getprogname();
   if (!proc || !*proc)
      return "unknown process name";

   // backboardd stays on the G4 GLES compositor (op6/ca-bridge + 0116 scan-out).
   // Returning nil here makes MTLCreateSystemDefaultDevice() return nil so
   // QuartzCore EAGLServer::renderer() falls back to EAGLContext.
   if (strcmp(proc, "backboardd") == 0)
      return "backboardd stays on G4 GLES compositor";

   int allowed = 0;
   FILE *f = fopen(CHL_ALLOWLIST_PATH, "r");
   if (!f) {
      if (errno == ENOENT) {
         // Default policy before /var/local/op6-mtl-clients is installed:
         // allow only op6-mtl-probe.
         allowed = (strcmp(proc, "op6-mtl-probe") == 0);
         if (!allowed)
            return "not op6-mtl-probe (default allowlist; " CHL_ALLOWLIST_PATH " missing)";
      } else {
         return "cannot open " CHL_ALLOWLIST_PATH;
      }
   } else {
      char buf[256];
      int matched = 0, wildcard = 0, denied = 0;
      while (fgets(buf, sizeof(buf), f)) {
         char *line = chl_trim(buf);
         if (!*line || *line == '#') continue;
         if (line[0] == '-') {
            const char *ex = chl_trim(line + 1);
            if (*ex && strcmp(ex, proc) == 0) {
               denied = 1;
               break;
            }
            continue;
         }
         if (strcmp(line, "*") == 0)
            wildcard = 1;
         else if (strcmp(line, proc) == 0)
            matched = 1;
      }
      fclose(f);
      if (denied)
         return "explicitly excluded in " CHL_ALLOWLIST_PATH;
      if (!matched && !wildcard)
         return "not listed in " CHL_ALLOWLIST_PATH;
   }

   if (!getenv("RWA_SOFTWARE")) {
      int st = bgmtl_gpu_state(minor_out);
      if (st != 1)
         return st == 2 ? "kernel reports GPU lost" : "GPU not ready";
   }
   return NULL;
}

@interface CHLDevice : BGMTLDevice
@end

@implementation CHLDevice

+ (void)load
{
   ensure_protocol(self, "MTLDeviceSPI");
   ensure_protocol(self, "MTLDevice");
}

// Reject here BEFORE [super allocWithZone:]: an uninitialized _MTLDevice cannot
// safely go through -dealloc (dispatch_release(NULL) in Apple's _MTLDevice).
+ (id)allocWithZone:(struct _NSZone *)zone
{
   unsigned minor = 0;
   const char *proc = getprogname() ?: "?";
   const char *why = chl_gate_reason(&minor);
   if (why) {
      os_log(chl_log(), "refused %{public}s: %{public}s", proc, why);
      return nil;
   }
   // Same CoreAnimation / CoreImage safeguards used by Chlorophyll and ios-flame
   // on non-Apple GPUs (avoid precompiled CIKL Metal DAGs and compressed FB/CGTextureData).
   setenv("CA_USE_CGTEXTUREDATA", "0", 0);
   setenv("CI_USE_MTL_DAG_FOR_CIKL_SRC", "0", 0);
   setenv("CA_DISABLE_FRAMEBUFFER_COMPRESSION", "1", 0);
   os_log(chl_log(), "allowing %{public}s (GPU ready, kernel ABI minor %u)", proc, minor);
   return [super allocWithZone:zone];
}

- (instancetype)initWithAcceleratorPort:(uint32_t)port
{
   self = [super initWithAcceleratorPort:port];
   if (!self) return nil;
   if (!getenv("RWA_SOFTWARE") && ![self rwa]) {
      const char *proc = getprogname() ?: "?";
      os_log_error(chl_log(), "device open failed for %{public}s: %{public}s",
                   proc, [[self bgWhy] UTF8String] ?: "unknown");
      [self release];
      return nil;
   }
   return self;
}

// iOS 16.7.16 QuartzCore/UIKit may request Apple-GPU precompiled .pipelinelib files;
// refuse cleanly so callers compile shaders from AIR instead.
- (id)newPipelineLibraryWithFilePath:(NSString *)filepath error:(NSError **)error
{
   return [self bgFail:error with:[NSString stringWithFormat:@"pipeline library %@ not supported",
                                   filepath ?: @"(null)"]];
}

@end
