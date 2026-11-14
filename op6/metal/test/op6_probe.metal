// SPDX-License-Identifier: MIT
// Metal compute and render shaders for op6-mtl-probe (milestone G6a).
#include <metal_stdlib>
using namespace metal;

struct ProbeParams {
   uint seed;
   uint step;
   uint count;
   uint pad;
};

kernel void probe_compute(device uint *out [[buffer(0)]],
                          constant ProbeParams &p [[buffer(1)]],
                          uint gid [[thread_position_in_grid]])
{
   if (gid < p.count) {
      out[gid] = p.seed + gid * p.step + (gid ^ 0xa5a5u);
   }
}

struct ProbeVertexOut {
   float4 pos [[position]];
   float4 color;
};

// buffer(0) holds packed (float4 pos, float4 color) pairs per vertex.
vertex ProbeVertexOut probe_vertex(uint vid [[vertex_id]],
                                   device const float4 *data [[buffer(0)]])
{
   ProbeVertexOut out;
   out.pos = data[vid * 2];
   out.color = data[vid * 2 + 1];
   return out;
}

fragment float4 probe_fragment(ProbeVertexOut in [[stage_in]])
{
   return in.color;
}
