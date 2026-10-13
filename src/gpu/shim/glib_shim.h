/* The GLib subset the imported xemu NV2A files use (see src/gpu/PROVENANCE.md).
 *
 * g_autofree relies on GCC's cleanup attribute, which MSVC lacks; here it is
 * empty, so the three short-lived shader-name strings in glsl/psh.c leak a few
 * bytes per generated shader. Shaders are generated once per unique state and
 * cached, so this is bounded and deliberate rather than an oversight. */
#pragma once

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char gchar;
typedef int gboolean;
typedef void *gpointer;
typedef size_t gsize;

#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

#define g_autofree
/* Same limitation as g_autofree: the Vulkan renderer uses it only for
 * extension lists during start-up, so those few arrays are not freed. */
#define g_autoptr(type) type *
#define g_malloc(n) malloc(n)
#define g_malloc0(n) calloc(1, (n))
#define g_malloc_n(n, s) malloc((size_t)(n) * (size_t)(s))
#define g_malloc0_n(n, s) calloc((size_t)(n), (size_t)(s))
#define g_realloc(p, n) realloc((p), (n))
#define g_free(p) free(p)
#define g_new(type, n) ((type *)malloc(sizeof(type) * (size_t)(n)))
#define g_new0(type, n) ((type *)calloc((n), sizeof(type)))

static inline char *g_strdup(const char *s)
{
    size_t n;
    char *r;
    if (!s) return NULL;
    n = strlen(s) + 1;
    r = (char *)malloc(n);
    if (r) memcpy(r, s, n);
    return r;
}

static inline char *g_strdup_vprintf(const char *fmt, va_list ap)
{
    va_list ap2;
    int n;
    char *r;
    va_copy(ap2, ap);
    n = vsnprintf(NULL, 0, fmt, ap2);
    va_end(ap2);
    if (n < 0) return NULL;
    r = (char *)malloc((size_t)n + 1);
    if (r) vsnprintf(r, (size_t)n + 1, fmt, ap);
    return r;
}

static inline char *g_strdup_printf(const char *fmt, ...)
{
    va_list ap;
    char *r;
    va_start(ap, fmt);
    r = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    return r;
}

/* Only used with two or three pieces; NULL-terminated like GLib's. */
static inline char *g_strconcat(const char *first, ...)
{
    va_list ap;
    size_t len = 0;
    const char *s;
    char *r, *p;
    va_start(ap, first);
    for (s = first; s; s = va_arg(ap, const char *)) len += strlen(s);
    va_end(ap);
    r = p = (char *)malloc(len + 1);
    if (!r) return NULL;
    va_start(ap, first);
    for (s = first; s; s = va_arg(ap, const char *)) {
        size_t n = strlen(s);
        memcpy(p, s, n);
        p += n;
    }
    va_end(ap);
    *p = 0;
    return r;
}

#ifdef _MSC_VER
/* glibc's vasprintf (Vulkan debug labels). */
static inline int vasprintf(char **out, const char *fmt, va_list ap)
{
    *out = g_strdup_vprintf(fmt, ap);
    return *out ? (int)strlen(*out) : -1;
}
#endif

/* GArray: the subset the Vulkan renderer uses (instance.c extension lists,
 * shaders.c/glsl.c). Elements are stored contiguously in data. */
typedef struct GArray {
    char *data;
    unsigned int len;
    unsigned int alloc;
    unsigned int elt_size;
    int zero_terminated;
    int clear;
} GArray;

static inline GArray *g_array_sized_new(int zero_terminated, int clear,
                                        unsigned int elt_size,
                                        unsigned int reserved)
{
    GArray *a = (GArray *)calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->elt_size = elt_size;
    a->zero_terminated = zero_terminated;
    a->clear = clear;
    a->alloc = reserved + 1;
    a->data = (char *)calloc(a->alloc, elt_size);
    return a;
}
static inline GArray *g_array_new(int zero_terminated, int clear,
                                  unsigned int elt_size)
{
    return g_array_sized_new(zero_terminated, clear, elt_size, 0);
}
static inline void g_array_reserve_(GArray *a, unsigned int n)
{
    if (n + 1 > a->alloc) {
        unsigned int na = a->alloc * 2 > n + 1 ? a->alloc * 2 : n + 1;
        char *nd = (char *)realloc(a->data, (size_t)na * a->elt_size);
        if (!nd) abort();
        memset(nd + (size_t)a->alloc * a->elt_size, 0,
               (size_t)(na - a->alloc) * a->elt_size);
        a->data = nd;
        a->alloc = na;
    }
}
static inline GArray *g_array_set_size(GArray *a, unsigned int len)
{
    g_array_reserve_(a, len);
    if (len > a->len)
        memset(a->data + (size_t)a->len * a->elt_size, 0,
               (size_t)(len - a->len) * a->elt_size);
    a->len = len;
    memset(a->data + (size_t)a->len * a->elt_size, 0, a->elt_size);
    return a;
}
static inline GArray *g_array_append_vals(GArray *a, const void *v,
                                          unsigned int n)
{
    g_array_reserve_(a, a->len + n);
    memcpy(a->data + (size_t)a->len * a->elt_size, v, (size_t)n * a->elt_size);
    a->len += n;
    memset(a->data + (size_t)a->len * a->elt_size, 0, a->elt_size);
    return a;
}
#define g_array_append_val(a, v) g_array_append_vals((a), &(v), 1)
#define g_array_index(a, t, i) (((t *)(void *)(a)->data)[(i)])
static inline char *g_array_free(GArray *a, int free_segment)
{
    char *d;
    if (!a) return NULL;
    d = a->data;
    if (free_segment) {
        free(d);
        d = NULL;
    }
    free(a);
    return d;
}

/* GByteArray: SPIR-V blobs from glsl.c. */
typedef unsigned char guint8;
typedef struct GByteArray {
    guint8 *data;
    unsigned int len;
} GByteArray;
static inline GByteArray *g_byte_array_new_take(guint8 *data, size_t len)
{
    GByteArray *b = (GByteArray *)malloc(sizeof(*b));
    if (!b) return NULL;
    b->data = data;
    b->len = (unsigned int)len;
    return b;
}
static inline void g_byte_array_unref(GByteArray *b)
{
    if (!b) return;
    free(b->data);
    free(b);
}

/* The on-disk shader cache (gl/shaders.c) is disabled through
 * g_config.perf.cache_shaders = false; these exist so it links. */
#define G_FILE_TEST_EXISTS 1
static inline gboolean g_file_test(const char *path, int test)
{
    FILE *f = fopen(path, "rb");
    (void)test;
    if (!f) return FALSE;
    fclose(f);
    return TRUE;
}
static inline int g_mkdir_with_parents(const char *path, int mode)
{
    (void)path; (void)mode;
    return -1;
}
typedef struct GError GError;
static inline gboolean g_file_set_contents(const char *path, const char *data,
                                           long len, GError **err)
{
    (void)path; (void)data; (void)len; (void)err;
    return FALSE;
}
