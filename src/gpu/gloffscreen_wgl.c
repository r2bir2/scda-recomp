/*
 * WGL implementation of xemu's gloffscreen API (the SDL version is
 * hw/xbox/nv2a/pgraph/thirdparty/gloffscreen/sdl.c in xemu).
 *
 * Same contract as upstream: an OpenGL 4.0 core-profile context on a hidden
 * 640x480 window, created sharing objects with whatever context is current,
 * and made current on return. Local code for xboxrecomp, see
 * src/gpu/PROVENANCE.md.
 */
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>

#include "gloffscreen.h"
#include "GL/wglext.h"

struct _GloContext {
    HWND hwnd;
    HDC hdc;
    HGLRC hglrc;
};

static PFNWGLCREATECONTEXTATTRIBSARBPROC p_wglCreateContextAttribsARB;
static PFNWGLCHOOSEPIXELFORMATARBPROC p_wglChoosePixelFormatARB;
static int s_loader_ready;

static LRESULT CALLBACK glo_wndproc(HWND h, UINT m, WPARAM w, LPARAM l)
{
    return DefWindowProcA(h, m, w, l);
}

static HWND glo_make_window(void)
{
    static ATOM cls;
    if (!cls) {
        WNDCLASSA wc = {0};
        wc.style = CS_OWNDC;
        wc.lpfnWndProc = glo_wndproc;
        wc.hInstance = GetModuleHandleA(NULL);
        wc.lpszClassName = "xboxrecomp_gloffscreen";
        cls = RegisterClassA(&wc);
    }
    return CreateWindowExA(0, "xboxrecomp_gloffscreen", "gloffscreen",
                           WS_OVERLAPPEDWINDOW, 0, 0, 640, 480,
                           NULL, NULL, GetModuleHandleA(NULL), NULL);
}

static int glo_set_basic_pixel_format(HDC hdc)
{
    PIXELFORMATDESCRIPTOR pfd = {0};
    int pf;
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cAlphaBits = 8;
    pfd.cDepthBits = 24;
    pfd.cStencilBits = 8;
    pf = ChoosePixelFormat(hdc, &pfd);
    return pf && SetPixelFormat(hdc, pf, &pfd);
}

/* wglCreateContextAttribsARB is itself an extension: it can only be fetched
 * with some legacy context current. Do that once on a throwaway window. */
static int glo_bootstrap(void)
{
    HWND hwnd;
    HDC hdc;
    HGLRC tmp;
    if (p_wglCreateContextAttribsARB)
        return 1;
    hwnd = glo_make_window();
    hdc = GetDC(hwnd);
    if (!glo_set_basic_pixel_format(hdc))
        return 0;
    tmp = wglCreateContext(hdc);
    if (!tmp || !wglMakeCurrent(hdc, tmp))
        return 0;
    p_wglCreateContextAttribsARB = (PFNWGLCREATECONTEXTATTRIBSARBPROC)
        wglGetProcAddress("wglCreateContextAttribsARB");
    p_wglChoosePixelFormatARB = (PFNWGLCHOOSEPIXELFORMATARBPROC)
        wglGetProcAddress("wglChoosePixelFormatARB");
    wglMakeCurrent(NULL, NULL);
    wglDeleteContext(tmp);
    ReleaseDC(hwnd, hdc);
    DestroyWindow(hwnd);
    return p_wglCreateContextAttribsARB != NULL;
}

static HGLRC s_share_root;

/* docs Update 675: OpenGL 4.0 core first (the OpenGL renderer needs it).
 * Drivers without it (Mali/Panfrost: 3.1, here under Wine on ARM) still get
 * a context for the Vulkan renderer's display window, which only presents
 * through GL; nv2a_gl_legacy_context then tells the OpenGL renderer to stay
 * away. */
static const int s_ctx_versions[][3] = {
    { 4, 0, WGL_CONTEXT_CORE_PROFILE_BIT_ARB },
    { 3, 3, WGL_CONTEXT_CORE_PROFILE_BIT_ARB },
    { 3, 2, WGL_CONTEXT_CORE_PROFILE_BIT_ARB },
    { 3, 1, 0 },
    { 3, 0, 0 },
};
static int s_ctx_version = -1;   /* index into s_ctx_versions once chosen */
bool nv2a_gl_legacy_context;

static HGLRC glo_create_versioned(HDC hdc, HGLRC share)
{
    int first = s_ctx_version >= 0 ? s_ctx_version : 0;
    int last = s_ctx_version >= 0 ? s_ctx_version
                                  : (int)(sizeof(s_ctx_versions) /
                                          sizeof(s_ctx_versions[0])) - 1;
    for (int i = first; i <= last; i++) {
        int attribs[8], n = 0;
        attribs[n++] = WGL_CONTEXT_MAJOR_VERSION_ARB;
        attribs[n++] = s_ctx_versions[i][0];
        attribs[n++] = WGL_CONTEXT_MINOR_VERSION_ARB;
        attribs[n++] = s_ctx_versions[i][1];
        if (s_ctx_versions[i][2]) {
            attribs[n++] = WGL_CONTEXT_PROFILE_MASK_ARB;
            attribs[n++] = s_ctx_versions[i][2];
        }
        attribs[n] = 0;
        HGLRC rc = p_wglCreateContextAttribsARB(hdc, share, attribs);
        if (rc) {
            if (s_ctx_version < 0) {
                s_ctx_version = i;
                nv2a_gl_legacy_context = i > 0;
                if (i > 0)
                    fprintf(stderr, "[GPU-GL] OpenGL 4.0 core context not "
                            "available, using %d.%d (display only)\n",
                            s_ctx_versions[i][0], s_ctx_versions[i][1]);
            }
            return rc;
        }
    }
    return NULL;
}

GloContext *glo_context_create(void)
{
    GloContext *c = (GloContext *)calloc(1, sizeof(*c));
    /* Share with the caller's current context, or -- for a thread that has
     * none yet, such as the framebuffer window reading the display texture
     * -- with the first context ever created, so every GloContext is in one
     * share group as xemu's UI and renderer contexts are. */
    HGLRC share = wglGetCurrentContext();
    if (!share)
        share = s_share_root;

    if (!c || !glo_bootstrap()) {
        fprintf(stderr, "[GPU-GL] WGL bootstrap failed (no wglCreateContextAttribsARB)\n");
        exit(1);
    }
    c->hwnd = glo_make_window();
    c->hdc = GetDC(c->hwnd);
    if (!glo_set_basic_pixel_format(c->hdc)) {
        fprintf(stderr, "[GPU-GL] SetPixelFormat failed (%lu)\n", GetLastError());
        exit(1);
    }
    c->hglrc = glo_create_versioned(c->hdc, share);
    if (!c->hglrc) {
        fprintf(stderr, "[GPU-GL] no OpenGL 3.0+ context available (%lu)\n",
                GetLastError());
        exit(1);
    }
    if (!s_share_root)
        s_share_root = c->hglrc;
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
    if (!context)
        wglMakeCurrent(NULL, NULL);
    else
        wglMakeCurrent(context->hdc, context->hglrc);
}

void glo_context_destroy(GloContext *context)
{
    if (!context)
        return;
    glo_set_current(NULL);
    wglDeleteContext(context->hglrc);
    ReleaseDC(context->hwnd, context->hdc);
    DestroyWindow(context->hwnd);
    free(context);
}

/* xboxrecomp (docs Update 479): present straight into the framebuffer
 * window. Every GloContext was created on a DC with the basic pixel format
 * above, so giving the window's DC that same format lets a context of the
 * share group be made current on the window and SwapBuffers there. */
HDC glo_window_dc(HWND hwnd)
{
    HDC hdc = GetDC(hwnd);
    if (!hdc)
        return NULL;
    if (GetPixelFormat(hdc) == 0 && !glo_set_basic_pixel_format(hdc)) {
        ReleaseDC(hwnd, hdc);
        return NULL;
    }
    return hdc;
}

int glo_make_current_on(GloContext *context, HDC hdc)
{
    return context && hdc && wglMakeCurrent(hdc, context->hglrc);
}
