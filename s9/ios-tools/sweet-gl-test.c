/*
 * sweet-gl-test: OpenGL ES 2 offscreen rendering test on the OnePlus 6 Adreno 630
 * through Mesa freedreno (libOSMesa.8.dylib) + SweetGPUUserClient (Milestone G3).
 *
 *   sweet-gl-test [FRAMES] [-o out.ppm]
 *
 * Tests:
 *   1. OSMesaCreateContextAttribs(OSMESA_ES_PROFILE, major=2) + OSMesaMakeCurrent;
 *      verifies GL_RENDERER is hardware Adreno (FD630), not softpipe/llvmpipe.
 *   2. 256x256 RGBA FBO + glClearColor(0.2, 0.4, 0.8, 1.0) + glClear + glReadPixels;
 *      verifies readback pixels match expected (51, 102, 204, 255).
 *   3. Vertex + fragment shader compilation (ir3), RGB triangle draw into the FBO,
 *      glReadPixels verification of background corner, red/green/blue vertices, and
 *      triangle pixel coverage (~26% of 256x256).
 *   4. N-frame render + glFinish timing loop and MSM_PARAM_FAULTS == 0 check.
 *
 * Adapted from HackiOS userland/src/gl_test.c (Copyright (c) 2026 OOSEX GANG, MIT)
 * and ios-apollo userland/gl-test/sweet-gl-test.c (Copyright (c) 2026 Leo Malik /
 * dipa-studio, MIT License).
 */
#include <dlfcn.h>
#include <errno.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef mach_port_t io_object_t;
typedef io_object_t io_service_t;
typedef io_object_t io_connect_t;
typedef struct __CFDictionary *CFMutableDictionaryRef;
typedef const struct __CFDictionary *CFDictionaryRef;
extern CFMutableDictionaryRef IOServiceMatching(const char *name);
extern io_service_t IOServiceGetMatchingService(mach_port_t mainPort, CFDictionaryRef matching);
extern kern_return_t IOServiceOpen(io_service_t service, task_port_t owningTask, uint32_t type, io_connect_t *connect);
extern kern_return_t IOServiceClose(io_connect_t connect);
extern kern_return_t IOObjectRelease(io_object_t object);
extern kern_return_t IOConnectCallStructMethod(io_connect_t connection, uint32_t selector, const void *inputStruct,
    size_t inputStructCnt, void *outputStruct, size_t *outputStructCnt);

/* GL/osmesa.h (with 0002-osmesa-es-profile.patch) and GLES2/gl2.h declarations */
typedef struct osmesa_context *OSMesaContext;
extern OSMesaContext OSMesaCreateContextAttribs(const int *attributes, OSMesaContext share);
extern unsigned char OSMesaMakeCurrent(OSMesaContext context, void *buffer, unsigned type, int width, int height);
extern void OSMesaDestroyContext(OSMesaContext context);
extern const unsigned char *glGetString(unsigned name);
extern unsigned glGetError(void);
extern void glGenTextures(int n, unsigned *names);
extern void glBindTexture(unsigned target, unsigned texture);
extern void glTexImage2D(unsigned target, int level, int internal, int width, int height, int border, unsigned format,
    unsigned type, const void *pixels);
extern void glTexParameteri(unsigned target, unsigned name, int value);
extern void glGenFramebuffers(int n, unsigned *names);
extern void glBindFramebuffer(unsigned target, unsigned framebuffer);
extern void glFramebufferTexture2D(unsigned target, unsigned attachment, unsigned textarget, unsigned texture, int level);
extern unsigned glCheckFramebufferStatus(unsigned target);
extern void glViewport(int x, int y, int width, int height);
extern void glClearColor(float red, float green, float blue, float alpha);
extern void glClear(unsigned mask);
extern unsigned glCreateShader(unsigned type);
extern void glShaderSource(unsigned shader, int count, const char *const *strings, const int *lengths);
extern void glCompileShader(unsigned shader);
extern void glGetShaderiv(unsigned shader, unsigned name, int *value);
extern void glGetShaderInfoLog(unsigned shader, int size, int *length, char *log);
extern unsigned glCreateProgram(void);
extern void glAttachShader(unsigned program, unsigned shader);
extern void glBindAttribLocation(unsigned program, unsigned index, const char *name);
extern void glLinkProgram(unsigned program);
extern void glGetProgramiv(unsigned program, unsigned name, int *value);
extern void glUseProgram(unsigned program);
extern void glVertexAttribPointer(unsigned index, int size, unsigned type, unsigned char normalized, int stride,
    const void *pointer);
extern void glEnableVertexAttribArray(unsigned index);
extern void glDrawArrays(unsigned mode, int first, int count);
extern void glFinish(void);
extern void glReadPixels(int x, int y, int width, int height, unsigned format, unsigned type, void *pixels);

#define OSMESA_FORMAT                   0x22
#define OSMESA_PROFILE                  0x33
#define OSMESA_CONTEXT_MAJOR_VERSION    0x36
#define OSMESA_ES_PROFILE               0x38
#define GL_TRIANGLES                    0x0004
#define GL_TEXTURE_2D                   0x0de1
#define GL_UNSIGNED_BYTE                0x1401
#define GL_FLOAT                        0x1406
#define GL_RGBA                         0x1908
#define GL_VENDOR                       0x1f00
#define GL_RENDERER                     0x1f01
#define GL_VERSION                      0x1f02
#define GL_NEAREST                      0x2600
#define GL_TEXTURE_MAG_FILTER           0x2800
#define GL_TEXTURE_MIN_FILTER           0x2801
#define GL_COLOR_BUFFER_BIT             0x4000
#define GL_FRAGMENT_SHADER              0x8b30
#define GL_VERTEX_SHADER                0x8b31
#define GL_COMPILE_STATUS               0x8b81
#define GL_LINK_STATUS                  0x8b82
#define GL_FRAMEBUFFER_COMPLETE         0x8cd5
#define GL_COLOR_ATTACHMENT0            0x8ce0
#define GL_FRAMEBUFFER                  0x8d40

#define SIZE 256

#define MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES        7
#define MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES        8
struct memlimit {
	int32_t active;
	uint32_t active_attr;
	int32_t inactive;
	uint32_t inactive_attr;
};

struct drm_msm_param {
	uint32_t pipe;
	uint32_t param;
	uint64_t value;
	uint32_t len;
	uint32_t pad;
};

static void
lift_memory_limit(void)
{
	int (*control)(uint32_t, int32_t, uint32_t, void *, size_t) =
	    (int (*)(uint32_t, int32_t, uint32_t, void *, size_t))dlsym(RTLD_DEFAULT, "memorystatus_control");
	struct memlimit before = { 0, 0, 0, 0 }, none = { -1, 0, -1, 0 }, after = { 0, 0, 0, 0 };

	if (control == NULL) {
		return;
	}
	control(MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES, getpid(), 0, &before, sizeof(before));
	if (control(MEMORYSTATUS_CMD_SET_MEMLIMIT_PROPERTIES, getpid(), 0, &none, sizeof(none)) == 0) {
		control(MEMORYSTATUS_CMD_GET_MEMLIMIT_PROPERTIES, getpid(), 0, &after, sizeof(after));
		printf("sweet-gl-test: memory limit %d MB -> %d MB\n", before.active, after.active);
	}
}

static uint64_t
now_us(void)
{
	static mach_timebase_info_data_t tb;

	if (tb.denom == 0)
		mach_timebase_info(&tb);
	return mach_absolute_time() * tb.numer / tb.denom / 1000;
}

static unsigned
shader(unsigned type, const char *text)
{
	unsigned name = glCreateShader(type);
	int good = 0;
	char log[512] = "";

	glShaderSource(name, 1, &text, NULL);
	glCompileShader(name);
	glGetShaderiv(name, GL_COMPILE_STATUS, &good);
	if (!good) {
		glGetShaderInfoLog(name, sizeof(log), NULL, log);
		printf("sweet-gl-test: shader compile failed: %s\n", log);
	}
	return name;
}

static const uint8_t *
at(const uint8_t *pixels, float x, float y)
{
	int px = (int)((x + 1) / 2 * SIZE), py = (int)((y + 1) / 2 * SIZE);

	return pixels + ((size_t)py * SIZE + (size_t)px) * 4;
}

static int
near(const uint8_t *p, int r, int g, int b)
{
	return abs(p[0] - r) <= 3 && abs(p[1] - g) <= 3 && abs(p[2] - b) <= 3 && p[3] == 255;
}

static int
query_gpu_faults(uint64_t *faults_out)
{
	io_service_t service = IOServiceGetMatchingService(0, IOServiceMatching("SweetGPU"));
	io_connect_t conn = 0;
	struct drm_msm_param p = { .pipe = 0x01, .param = 0x09 /* MSM_PARAM_FAULTS */ };
	size_t out_len = sizeof(p);

	if (service == 0)
		return -1;
	if (IOServiceOpen(service, mach_task_self(), 0, &conn) != 0) {
		IOObjectRelease(service);
		return -1;
	}
	IOObjectRelease(service);
	kern_return_t kr = IOConnectCallStructMethod(conn, 0x40 /* GET_PARAM */, &p, sizeof(p), &p, &out_len);
	IOServiceClose(conn);
	if (kr != 0)
		return -1;
	*faults_out = p.value;
	return 0;
}

static int
write_ppm(const char *path, const uint8_t *pixels, int width, int height)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return -1;
	fprintf(f, "P6\n%d %d\n255\n", width, height);
	for (int y = height - 1; y >= 0; y--) {
		const uint8_t *row = pixels + (size_t)y * width * 4;
		for (int x = 0; x < width; x++) {
			fwrite(row + x * 4, 1, 3, f);
		}
	}
	fclose(f);
	return 0;
}

int
main(int argc, char **argv)
{
	static const int attributes[] = { OSMESA_FORMAT, GL_RGBA, OSMESA_PROFILE, OSMESA_ES_PROFILE,
		                          OSMESA_CONTEXT_MAJOR_VERSION, 2, 0 };
	static const float corners[] = {
		-0.8f, -0.6f, 1, 0, 0,
		0.8f, -0.6f, 0, 1, 0,
		0.0f, 0.7f, 0, 0, 1,
	};
	static uint8_t window[16 * 16 * 4], pixels[SIZE * SIZE * 4];
	unsigned frames = 100;
	const char *ppm_path = NULL;
	unsigned texture = 0, framebuffer = 0, program, coloured = 0, error;
	const char *renderer;
	const uint8_t *corner, *red, *green, *blue;
	uint64_t began, clear_us, first_us, frames_us, faults = 0;
	io_service_t service;
	OSMesaContext context;
	int good = 0, gpu, clear_ok = 1, tri_ok, ok;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
			ppm_path = argv[++i];
		} else {
			frames = (unsigned)strtoul(argv[i], NULL, 0);
		}
	}
	if (frames == 0 || frames > 100000)
		frames = 100;

	setvbuf(stdout, NULL, _IOLBF, 0);
	lift_memory_limit();
	printf("sweet-gl-test: waiting for the SweetGPU service\n");
	for (unsigned waited = 0; (service = IOServiceGetMatchingService(0, IOServiceMatching("SweetGPU"))) == 0; waited += 2) {
		if (waited >= 180) {
			printf("sweet-gl-test: no SweetGPU service after 3 minutes (boot with sweetgpu=2)\n");
			return 1;
		}
		sleep(2);
	}
	IOObjectRelease(service);

	context = OSMesaCreateContextAttribs(attributes, NULL);
	if (context == NULL || !OSMesaMakeCurrent(context, window, GL_UNSIGNED_BYTE, 16, 16)) {
		printf("sweet-gl-test: no OpenGL ES context\n");
		return 1;
	}
	renderer = (const char *)glGetString(GL_RENDERER);
	gpu = renderer != NULL && strstr(renderer, "softpipe") == NULL && strstr(renderer, "llvmpipe") == NULL;
	printf("sweet-gl-test: %s; %s; %s (%s)\n", (const char *)glGetString(GL_VENDOR), renderer,
	    (const char *)glGetString(GL_VERSION), gpu ? "the GPU" : "SOFTWARE, not the GPU");

	glGenTextures(1, &texture);
	glBindTexture(GL_TEXTURE_2D, texture);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, SIZE, SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glGenFramebuffers(1, &framebuffer);
	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
	if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
		printf("sweet-gl-test: the framebuffer is not complete\n");
		return 1;
	}
	glViewport(0, 0, SIZE, SIZE);

	/* Test 1: glClear + glReadPixels verification */
	began = now_us();
	glClearColor(0.2f, 0.4f, 0.8f, 1.0f);
	glClear(GL_COLOR_BUFFER_BIT);
	glFinish();
	clear_us = now_us() - began;
	memset(pixels, 0xaa, sizeof(pixels));
	glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	for (size_t i = 0; i < SIZE * SIZE; i++) {
		if (!near(pixels + 4 * i, 51, 102, 204)) {
			clear_ok = 0;
			break;
		}
	}
	printf("sweet-gl-test: glClear(0.2, 0.4, 0.8, 1.0) in %llu us: pixel[0]=%u %u %u %u (%s)\n",
	    (unsigned long long)clear_us, pixels[0], pixels[1], pixels[2], pixels[3],
	    clear_ok ? "CLEAR OK" : "CLEAR MISMATCH");

	/* Test 2: Shader compile + RGB triangle draw + glReadPixels verification */
	began = now_us();
	program = glCreateProgram();
	glAttachShader(program, shader(GL_VERTEX_SHADER,
	    "attribute vec2 where; attribute vec3 colour; varying vec3 c;\n"
	    "void main() { gl_Position = vec4(where, 0.0, 1.0); c = colour; }\n"));
	glAttachShader(program, shader(GL_FRAGMENT_SHADER,
	    "precision mediump float; varying vec3 c;\n"
	    "void main() { gl_FragColor = vec4(c, 1.0); }\n"));
	glBindAttribLocation(program, 0, "where");
	glBindAttribLocation(program, 1, "colour");
	glLinkProgram(program);
	glGetProgramiv(program, GL_LINK_STATUS, &good);
	if (!good) {
		printf("sweet-gl-test: the program does not link\n");
		return 1;
	}
	glUseProgram(program);
	glClearColor(0.1f, 0.1f, 0.2f, 1.0f);
	glVertexAttribPointer(0, 2, GL_FLOAT, 0, 5 * sizeof(float), corners);
	glVertexAttribPointer(1, 3, GL_FLOAT, 0, 5 * sizeof(float), corners + 2);
	glEnableVertexAttribArray(0);
	glEnableVertexAttribArray(1);
	glClear(GL_COLOR_BUFFER_BIT);
	glDrawArrays(GL_TRIANGLES, 0, 3);
	glFinish();
	first_us = now_us() - began;

	memset(pixels, 0x55, sizeof(pixels));
	glReadPixels(0, 0, SIZE, SIZE, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
	error = glGetError();
	for (size_t i = 0; i < SIZE * SIZE; i++)
		coloured += !near(pixels + 4 * i, 26, 26, 51);
	corner = at(pixels, -0.98f, -0.98f);
	red = at(pixels, -0.6f, -0.5f);
	green = at(pixels, 0.6f, -0.5f);
	blue = at(pixels, 0.0f, 0.55f);
	/* 0.1, 0.1, 0.2 -> 26 26 51; triangle area is 1.6 * 1.3 / 2 of the 2x2 NDC square = 26% */
	tri_ok = near(corner, 26, 26, 51) && red[0] > 128 && red[0] > red[1] + 64 && red[0] > red[2] + 64 &&
	    green[1] > 128 && green[1] > green[0] + 64 && green[1] > green[2] + 64 &&
	    blue[2] > 128 && blue[2] > blue[0] + 64 && blue[2] > blue[1] + 64 &&
	    coloured * 100 >= 24u * SIZE * SIZE && coloured * 100 <= 28u * SIZE * SIZE;
	printf("sweet-gl-test: first frame %llu us; error %#x; red green blue alpha: corner %u %u %u %u, "
	    "near the red point %u %u %u %u, the green %u %u %u %u, the blue %u %u %u %u; %u of %u pixels in the triangle (26%% by geometry)\n",
	    (unsigned long long)first_us, error, corner[0], corner[1], corner[2], corner[3], red[0], red[1], red[2], red[3],
	    green[0], green[1], green[2], green[3], blue[0], blue[1], blue[2], blue[3], coloured, SIZE * SIZE);

	if (ppm_path != NULL) {
		if (write_ppm(ppm_path, pixels, SIZE, SIZE) == 0) {
			printf("sweet-gl-test: wrote %dx%d PPM to %s\n", SIZE, SIZE, ppm_path);
		}
	}

	/* Test 3: N-frame benchmark + fault check */
	began = now_us();
	for (unsigned i = 0; i < frames; i++) {
		glClear(GL_COLOR_BUFFER_BIT);
		glDrawArrays(GL_TRIANGLES, 0, 3);
		glFinish();
	}
	frames_us = now_us() - began;
	(void)query_gpu_faults(&faults);
	ok = gpu && error == 0 && clear_ok && tri_ok && faults == 0;
	printf("sweet-gl-test: %u frames of %d x %d, %llu us each (clear, triangle, wait); faults %llu; error %#x: %s\n",
	    frames, SIZE, SIZE, (unsigned long long)(frames_us / frames), (unsigned long long)faults, glGetError(),
	    ok ? "MESA DRAWS WITH THE GPU" : gpu ? "FAILED (wrong pixels or faults)" : "FAILED (software, not the GPU)");
	OSMesaDestroyContext(context);
	return ok ? 0 : 1;
}
