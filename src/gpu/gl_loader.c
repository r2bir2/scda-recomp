/*
 * Resolves the OpenGL entry points listed in shim/epoxy/gl_funcs.h (generated
 * by gen_gl_loader.py) -- the job libepoxy does for xemu. Called once, with
 * the first WGL context current (gloffscreen_wgl.c). Local code for
 * xboxrecomp, see src/gpu/PROVENANCE.md.
 */
#if defined(_WIN32)
#include <windows.h>
#else
#include <EGL/egl.h>
#endif
#include <stdio.h>
#include <string.h>

#include <epoxy/gl.h>

#define GLFN(type, name) type xgl_##name;
#include "gl_funcs.h"
#undef GLFN

/* wglGetProcAddress returns 0/1/2/3/-1 for failure, and nothing at all for
 * the OpenGL 1.1 functions opengl32.dll exports directly. */
#if defined(_WIN32)
static void *load_one(HMODULE gl32, const char *name)
{
    void *p = (void *)wglGetProcAddress(name);
    if (p == NULL || p == (void *)1 || p == (void *)2 || p == (void *)3
            || p == (void *)-1)
        p = (void *)GetProcAddress(gl32, name);
    return p;
}
#else
/* Mesa's eglGetProcAddress resolves core entry points too
 * (EGL_KHR_get_all_proc_addresses); gloffscreen_egl.c, docs Update 677 */
typedef void *HMODULE;
static void *load_one(HMODULE unused, const char *name)
{
    (void)unused;
    return (void *)eglGetProcAddress(name);
}
#endif

bool nv2a_gl_loader_init(bool strict)
{
    int missing = 0;
#if defined(_WIN32)
    HMODULE gl32 = GetModuleHandleA("opengl32.dll");

    if (!gl32)
        gl32 = LoadLibraryA("opengl32.dll");
#else
    HMODULE gl32 = NULL;
#endif
    /* EXT entry points (GL_EXT_memory_object, used only by the Vulkan
     * renderer's display interop) are optional: a driver without them can
     * still run the OpenGL renderer. */
#define GLFN(type, name)                                                  \
    xgl_##name = (type)load_one(gl32, #name);                             \
    if (!xgl_##name) {                                                    \
        size_t len_ = strlen(#name);                                      \
        bool optional_ = len_ > 3 && !strcmp(#name + len_ - 3, "EXT");    \
        fprintf(stderr, "[GPU-GL] missing %sentry point %s\n",            \
                optional_ ? "optional " : "", #name);                     \
        if (!optional_ && strict)                                         \
            missing++;                                                    \
    }
#include "gl_funcs.h"
#undef GLFN
    return missing == 0;
}

bool epoxy_has_gl_extension(const char *ext)
{
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n; i++) {
        const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
        if (e && strcmp(e, ext) == 0)
            return true;
    }
    return false;
}

int epoxy_gl_version(void)
{
    GLint major = 0, minor = 0;
    glGetIntegerv(GL_MAJOR_VERSION, &major);
    glGetIntegerv(GL_MINOR_VERSION, &minor);
    return major * 10 + minor;
}

bool epoxy_is_desktop_gl(void)
{
    return true;
}
