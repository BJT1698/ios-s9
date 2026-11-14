// SPDX-License-Identifier: MIT
#ifndef OP6_BGMTL_H
#define OP6_BGMTL_H

#import <Metal/Metal.h>

#ifdef __cplusplus
extern "C" {
#endif

id bgmtl_create_device(void);
int bgmtl_gpu_state(unsigned *abi_minor);
void bgmtl_render_info(id pipeline, unsigned *vertex_buffers, unsigned *fragment_buffers,
    unsigned *fragment_textures, unsigned *fragment_samplers);
struct rwa_device *bgmtl_rwa(id device);

#ifdef __cplusplus
}
#endif

#endif
