/* Stand-in for libepoxy's <epoxy/gl.h> (see src/gpu/PROVENANCE.md).
 *
 * Declares the Khronos core-profile API and routes every entry point the
 * renderer uses through a function pointer that src/gpu/gl_loader.c fills
 * once a WGL context is current. The list is generated
 * (src/gpu/gen_gl_loader.py -> gl_funcs.h). */
#pragma once

#include <stdbool.h>

#ifndef APIENTRY
#if defined(_WIN32)
#define APIENTRY __stdcall
#else
#define APIENTRY
#endif
#endif
#ifndef APIENTRYP
#define APIENTRYP APIENTRY *
#endif
#ifndef GLAPI
#define GLAPI extern
#endif

#include "KHR/khrplatform.h"
#include "GL/glcorearb.h"
#include "GL/glext_memory_object.h"

#define GLFN(type, name) extern type xgl_##name;
#include "gl_funcs.h"
#undef GLFN

/* glFoo -> xgl_glFoo, generated alongside gl_funcs.h. */
#include "gl_remap.h"

/* Extension tokens outside the core header. */
#ifndef GL_COMPRESSED_RGB_S3TC_DXT1_EXT
#define GL_COMPRESSED_RGB_S3TC_DXT1_EXT   0x83F0
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT  0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT3_EXT  0x83F2
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT  0x83F3
#endif
#ifndef GL_TEXTURE_MAX_ANISOTROPY_EXT
#define GL_TEXTURE_MAX_ANISOTROPY_EXT     0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT 0x84FF
#endif

/* GREMEDY frame terminator: a debugger hook, absent on real drivers. */
#define glFrameTerminatorGREMEDY() ((void)0)

bool epoxy_has_gl_extension(const char *ext);
int epoxy_gl_version(void);
bool epoxy_is_desktop_gl(void);
/* strict: every non-EXT entry point required (OpenGL renderer); false for
 * a pre-4.0 context that only presents (docs Update 675) */
bool nv2a_gl_loader_init(bool strict);
/* true when only a pre-4.0 context could be created (gloffscreen_wgl.c) */
extern bool nv2a_gl_legacy_context;
