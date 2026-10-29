/*
 * EGL implementation of xemu's gloffscreen API for Linux hosts (SCDA ARM
 * port, docs Update 677); the Windows one is gloffscreen_wgl.c.
 *
 * Same contract: desktop-OpenGL contexts, all in one share group, created
 * sharing with whatever context is current and made current on return.
 * Contexts are surfaceless (EGL_KHR_surfaceless_context; a 16x16 pbuffer
 * otherwise). The framebuffer window is an X11 window: glo_window_dc() wraps
 * it in an EGL window surface, so a context of the share group can be made
 * current on it and swapped, as on Windows. Local code for xboxrecomp, see
 * src/gpu/PROVENANCE.md.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <X11/Xlib.h>

#include "gloffscreen.h"

struct _GloContext {
    EGLContext ctx;
    EGLSurface pbuf;      /* EGL_NO_SURFACE when surfaceless works */
};

static EGLDisplay s_dpy = EGL_NO_DISPLAY;
static EGLConfig s_cfg;
static bool s_surfaceless;
static EGLContext s_share_root = EGL_NO_CONTEXT;
static int s_loader_ready;
static pthread_mutex_t s_init_lock = PTHREAD_MUTEX_INITIALIZER;
bool nv2a_gl_legacy_context;

bool nv2a_gl_loader_init(bool strict);

static const int s_ctx_versions[][3] = {
    { 4, 0, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT },
    { 3, 3, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT },
    { 3, 2, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT },
    { 3, 1, 0 },
    { 3, 0, 0 },
};
static int s_ctx_version = -1;

static bool glo_init_display(void)
{
    pthread_mutex_lock(&s_init_lock);
    if (s_dpy != EGL_NO_DISPLAY) {
        pthread_mutex_unlock(&s_init_lock);
        return true;
    }
    /* X11 when there is a display (the framebuffer window is an X11 window),
     * Mesa's surfaceless platform otherwise */
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    Display *x = getenv("DISPLAY") ? XOpenDisplay(NULL) : NULL;
    if (x && get_platform_display)
        s_dpy = get_platform_display(EGL_PLATFORM_X11_KHR, x, NULL);
    else if (x)
        s_dpy = eglGetDisplay((EGLNativeDisplayType)x);
    else if (get_platform_display)
        s_dpy = get_platform_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
    EGLint major = 0, minor = 0;
    if (s_dpy == EGL_NO_DISPLAY || !eglInitialize(s_dpy, &major, &minor)) {
        fprintf(stderr, "[GPU-GL] EGL initialisation failed (0x%x)\n", eglGetError());
        s_dpy = EGL_NO_DISPLAY;
        pthread_mutex_unlock(&s_init_lock);
        return false;
    }
    const char *exts = eglQueryString(s_dpy, EGL_EXTENSIONS);
    s_surfaceless = exts && strstr(exts, "EGL_KHR_surfaceless_context");

    const EGLint attrs[] = {
        EGL_SURFACE_TYPE, (x ? EGL_WINDOW_BIT : 0) | EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };
    EGLConfig cfgs[64];
    EGLint n = 0;
    if (!eglChooseConfig(s_dpy, attrs, cfgs, 64, &n) || n < 1) {
        fprintf(stderr, "[GPU-GL] no EGL config for desktop OpenGL\n");
        s_dpy = EGL_NO_DISPLAY;
        pthread_mutex_unlock(&s_init_lock);
        return false;
    }
    /* the framebuffer window has the screen's default visual: a config of
     * another visual (32-bit ARGB) cannot render into it */
    s_cfg = cfgs[0];
    if (x) {
        VisualID want = XVisualIDFromVisual(DefaultVisual(x, DefaultScreen(x)));
        for (EGLint i = 0; i < n; i++) {
            EGLint vid = 0;
            if (eglGetConfigAttrib(s_dpy, cfgs[i], EGL_NATIVE_VISUAL_ID, &vid) &&
                (VisualID)vid == want) {
                s_cfg = cfgs[i];
                break;
            }
        }
    }
    fprintf(stderr, "[GPU-GL] EGL %d.%d on %s%s\n", major, minor,
            x ? "X11" : "surfaceless", s_surfaceless ? "" : " (pbuffer contexts)");
    pthread_mutex_unlock(&s_init_lock);
    return true;
}

static EGLContext glo_create_versioned(EGLContext share)
{
    int first = s_ctx_version >= 0 ? s_ctx_version : 0;
    int last = s_ctx_version >= 0 ? s_ctx_version
                                  : (int)(sizeof(s_ctx_versions) /
                                          sizeof(s_ctx_versions[0])) - 1;
    eglBindAPI(EGL_OPENGL_API);
    for (int i = first; i <= last; i++) {
        EGLint attribs[8], n = 0;
        attribs[n++] = EGL_CONTEXT_MAJOR_VERSION;
        attribs[n++] = s_ctx_versions[i][0];
        attribs[n++] = EGL_CONTEXT_MINOR_VERSION;
        attribs[n++] = s_ctx_versions[i][1];
        if (s_ctx_versions[i][2]) {
            attribs[n++] = EGL_CONTEXT_OPENGL_PROFILE_MASK;
            attribs[n++] = s_ctx_versions[i][2];
        }
        attribs[n] = EGL_NONE;
        EGLContext c = eglCreateContext(s_dpy, s_cfg, share, attribs);
        if (c != EGL_NO_CONTEXT) {
            if (s_ctx_version < 0) {
                s_ctx_version = i;
                nv2a_gl_legacy_context = i > 0;
                if (i > 0)
                    fprintf(stderr, "[GPU-GL] OpenGL 4.0 core context not "
                            "available, using %d.%d (display only)\n",
                            s_ctx_versions[i][0], s_ctx_versions[i][1]);
            }
            return c;
        }
    }
    return EGL_NO_CONTEXT;
}

GloContext *glo_context_create(void)
{
    GloContext *c = (GloContext *)calloc(1, sizeof(*c));
    if (!c || !glo_init_display())
        exit(1);
    EGLContext share = eglGetCurrentContext();
    if (share == EGL_NO_CONTEXT)
        share = s_share_root;
    c->ctx = glo_create_versioned(share);
    if (c->ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "[GPU-GL] no OpenGL 3.0+ context available (0x%x)\n",
                eglGetError());
        exit(1);
    }
    c->pbuf = EGL_NO_SURFACE;
    if (!s_surfaceless) {
        const EGLint pb[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
        c->pbuf = eglCreatePbufferSurface(s_dpy, s_cfg, pb);
    }
    if (s_share_root == EGL_NO_CONTEXT)
        s_share_root = c->ctx;
    glo_set_current(c);
    if (!s_loader_ready) {
        if (!nv2a_gl_loader_init(!nv2a_gl_legacy_context)) {
            fprintf(stderr, "[GPU-GL] required GL entry points missing\n");
            exit(1);
        }
        s_loader_ready = 1;
        fprintf(stderr, "[GPU-GL] OpenGL %s (%s)\n",
                (const char *)glGetString(GL_VERSION),
                (const char *)glGetString(GL_RENDERER));
    }
    return c;
}

void glo_set_current(GloContext *context)
{
    eglBindAPI(EGL_OPENGL_API);
    if (!context)
        eglMakeCurrent(s_dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    else
        eglMakeCurrent(s_dpy, context->pbuf, context->pbuf, context->ctx);
}

void glo_context_destroy(GloContext *context)
{
    if (!context)
        return;
    glo_set_current(NULL);
    if (context->pbuf != EGL_NO_SURFACE)
        eglDestroySurface(s_dpy, context->pbuf);
    eglDestroyContext(s_dpy, context->ctx);
    free(context);
}

/* The framebuffer window (an X11 Window id, see fb_present_sdl.c) as an EGL
 * window surface; the presenter's "HDC". */
void *glo_window_dc(void *hwnd)
{
    Window xid = (Window)(uintptr_t)hwnd;
    if (!xid || !glo_init_display())
        return NULL;
    EGLSurface s = eglCreateWindowSurface(s_dpy, s_cfg, (EGLNativeWindowType)xid, NULL);
    if (s == EGL_NO_SURFACE) {
        fprintf(stderr, "[GPU-GL] EGL window surface failed (0x%x)\n", eglGetError());
        return NULL;
    }
    return (void *)s;
}

int glo_make_current_on(GloContext *context, void *hdc)
{
    eglBindAPI(EGL_OPENGL_API);
    return context && hdc &&
           eglMakeCurrent(s_dpy, (EGLSurface)hdc, (EGLSurface)hdc, context->ctx);
}

int glo_swap_window(void *hdc)
{
    return eglSwapBuffers(s_dpy, (EGLSurface)hdc);
}

void glo_swap_interval(int interval)
{
    eglSwapInterval(s_dpy, interval);
}
