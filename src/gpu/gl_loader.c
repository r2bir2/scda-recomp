/*
 * Resolves the OpenGL entry points listed in shim/epoxy/gl_funcs.h (generated
 * by gen_gl_loader.py) -- the job libepoxy does for xemu. Called once, with
 * the first WGL context current (gloffscreen_wgl.c). Local code for
 * xboxrecomp, see src/gpu/PROVENANCE.md.
 */
#include <windows.h>
#include <stdio.h>
#include <string.h>

#include <epoxy/gl.h>

#define GLFN(type, name) type xgl_##name;
#include "gl_funcs.h"
#undef GLFN

/* wglGetProcAddress returns 0/1/2/3/-1 for failure, and nothing at all for
 * the OpenGL 1.1 functions opengl32.dll exports directly. */
static void *load_one(HMODULE gl32, const char *name)
{
    void *p = (void *)wglGetProcAddress(name);
    if (p == NULL || p == (void *)1 || p == (void *)2 || p == (void *)3
            || p == (void *)-1)
        p = (void *)GetProcAddress(gl32, name);
    return p;
}

bool nv2a_gl_loader_init(void)
{
    HMODULE gl32 = GetModuleHandleA("opengl32.dll");
    int missing = 0;

    if (!gl32)
        gl32 = LoadLibraryA("opengl32.dll");
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
        if (!optional_)                                                   \
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
