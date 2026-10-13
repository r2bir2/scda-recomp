/**
 * Execute the parts of the title's pushbuffer that produce visible pixels.
 *
 * The title builds NV2A commands in guest RAM and advances DMA_PUT; without
 * something consuming them the framebuffer stays whatever it was, which is how
 * a fully booted title renders a black screen. This walks the same command
 * stream nv2a_pb_scan.c surveys and carries out the subset that decides what is
 * on screen: which surface is being drawn into, and clearing it.
 *
 * It also rasterises geometry, flat-shaded, straight into the same guest
 * framebuffer the clear writes: batches whose attribute 0 is already in
 * screen space are drawn directly (UI/HUD/2D overlays), and batches that
 * need the programmable transform pipeline are run through a real NV2A
 * vertex-program interpreter (vsh_interp.c) -- see raster_batch(),
 * transform_vertex(). Only fixed-function transform mode and batches with no
 * program loaded yet are still counted and skipped rather than drawn
 * somewhere wrong. The two linear A8R8G8B8 texture/combiner paths observed
 * during SCDA's intro are modelled; depth and other pixel programs are still
 * not. The upgrade path for those (and for performance -- this interpreter
 * runs on the CPU, one vertex at a time) is the D3D11 translator in
 * src/nv2a/nv2a_pgraph_d3d11.c.
 *
 * Everything this does not handle is counted and ranked by
 * nv2a_pb_exec_report(), so what remains is a list rather than a guess.
 *
 * Enabled with RECOMP_PB_EXEC. RECOMP_RASTER_TEST draws one known triangle
 * after every clear, which separates "the pixel path is broken" from "the title
 * has not given us any vertices". RECOMP_FB_DUMP=<prefix> writes the surface to
 * <prefix>NNN.bmp, so the result can be looked at without a display.
 */
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "xbox_memory_layout.h"
#include "vsh_interp.h"
#include <windows.h>

extern ptrdiff_t xbox_GetMemoryOffset(void);
extern void xbox_FramebufferWindowSet(uint32_t fb_va, uint32_t pitch);
extern void xbox_FramebufferWindowStart(void);
extern uint32_t g_xbox_image_lo, g_xbox_image_hi;

/* Would writing this surface land on the title's own image?
 *
 * NV097_SET_SURFACE_COLOR_OFFSET is an offset within the colour DMA object,
 * not a guest virtual address, and this executor has always used it as one.
 * That is harmless while the two happen to agree and catastrophic when they do
 * not: the Xbox Dashboard names surface 0x00088000 at 1280x960x4, so clearing
 * it wrote 4.9 MB of opaque black from 0x00088000 to 0x00538000 -- straight
 * over its own code, its D3D context at 0x000BBFC0 and the register-block
 * pointer at 0x000BE2C4. The symptom was a title that submitted one perfect
 * frame and then spun forever in a pushbuffer-full loop, three layers away,
 * with every D3D global reading 0xFF000000: the clear colour.
 *
 * So refuse, and say so. Getting the address right needs the DMA object base
 * this ignores (NV097_SET_CONTEXT_DMA_COLOR); until that exists, writing
 * nothing is strictly better than writing over the guest, and a title that
 * cannot draw is easier to debug than one that has been overwritten.
 */
static int surface_hits_image(uint32_t base, uint32_t bytes)
{
    if (!g_xbox_image_hi || !bytes)
        return 0;
    return base < g_xbox_image_hi && base + bytes > g_xbox_image_lo;
}

static int surface_write_refused(uint32_t base, uint32_t bytes, const char *what)
{
    static int said;

    if (!surface_hits_image(base, bytes))
        return 0;
    if (!said) {
        said = 1;
        fprintf(stderr,
                "  [GPU] REFUSING to %s surface 0x%08X..0x%08X: that overlaps "
                "the loaded image (0x%08X..0x%08X).\n"
                "  [GPU]   SET_SURFACE_COLOR_OFFSET is a DMA-object offset, not "
                "a guest VA, and this executor treats it as one. Writing here "
                "would destroy the title's own code and globals.\n",
                what, base, base + bytes, g_xbox_image_lo, g_xbox_image_hi);
        fflush(stderr);
    }
    return 1;
}

static uint32_t dma_resolve(uint32_t offset, uint32_t bytes)
{
    uint32_t va = 0;

    if (xbox_ResolveGpuPhysicalRange(offset, bytes ? bytes : 1, &va))
        return va;
    return 0;
}

/* NV097 methods this executor acts on. */
#define NV097_SET_SURFACE_CLIP_HORIZONTAL 0x0200
#define NV097_SET_SURFACE_CLIP_VERTICAL   0x0204
#define NV097_SET_SURFACE_FORMAT          0x0208
#define NV097_SET_SURFACE_PITCH           0x020C
#define NV097_SET_SURFACE_COLOR_OFFSET    0x0210
#define NV097_SET_COLOR_CLEAR_VALUE       0x1D90
#define NV097_CLEAR_SURFACE               0x1D94
#define NV097_SET_VERTEX_DATA_ARRAY_OFFSET 0x1720   /* +i*4, 16 attributes */
#define NV097_SET_VERTEX_DATA_ARRAY_FORMAT 0x1760   /* +i*4 */
#define NV097_SET_BEGIN_END               0x17FC
#define NV097_ARRAY_ELEMENT16             0x1800
/* Implicit vertex run: low 24 bits are the first vertex, high 8 bits are
 * count minus one. Several runs may occur inside one BEGIN_END batch. */
#define NV097_DRAW_ARRAYS                 0x1810

#define NV097_CLEAR_COLOR_MASK            0xF0   /* R,G,B,A bits */

/* Transform (vertex shader) program/constant upload, per nv2a_regs.h and the
 * matching xemu pgraph.c DEF_METHOD_INC handlers (checked directly, not
 * assumed): each is a 32-word window whose four dwords per row land at
 * whatever row program_load/const_load currently point to, and the pointer
 * only advances after a complete row (slot % 4 == 3). The actual write
 * target is therefore the separate load-pointer register, not the method
 * number itself. Uploaded rows/constants are now actually executed for
 * vertex-program batches -- see vsh_interp.h/.c and transform_vertex()
 * below. */
#define NV097_SET_TRANSFORM_PROGRAM        0x0B00   /* +i*4, 32-word window */
#define NV097_SET_TRANSFORM_CONSTANT       0x0B80   /* +i*4, 32-word window */
#define NV097_SET_VIEWPORT_OFFSET          0x0A20   /* four dwords -> c[59] */
#define NV097_SET_VIEWPORT_SCALE           0x0AF0   /* four dwords -> c[58] */
#define NV097_SET_TRANSFORM_EXECUTION_MODE 0x1E94
#define NV097_SET_TRANSFORM_PROGRAM_LOAD   0x1E9C
#define NV097_SET_TRANSFORM_PROGRAM_START  0x1EA0
#define NV097_SET_TRANSFORM_CONSTANT_LOAD  0x1EA4

/* Four texture units, with one 0x40-byte method block per stage. */
#define NV097_SET_TEXTURE_OFFSET            0x1B00
#define NV097_SET_TEXTURE_FORMAT            0x1B04
#define NV097_SET_TEXTURE_ADDRESS           0x1B08
#define NV097_SET_TEXTURE_CONTROL0          0x1B0C
#define NV097_SET_TEXTURE_CONTROL1          0x1B10
#define NV097_SET_TEXTURE_FILTER            0x1B14
#define NV097_SET_TEXTURE_IMAGE_RECT        0x1B1C
#define NV_TEXTURE_STAGE_STRIDE              0x40
#define NV_TEXTURE_STAGES                    4

#define NV097_SET_COMBINER_ALPHA_ICW        0x0260
#define NV097_SET_COMBINER_SPECULAR_FOG_CW0 0x0288
#define NV097_SET_COMBINER_SPECULAR_FOG_CW1 0x028C
#define NV097_SET_COMBINER_FACTOR0          0x0A60
#define NV097_SET_COMBINER_FACTOR1          0x0A80
#define NV097_SET_COMBINER_ALPHA_OCW        0x0AA0
#define NV097_SET_COMBINER_COLOR_ICW        0x0AC0
#define NV097_SET_COMBINER_COLOR_OCW        0x1E40
#define NV097_SET_COMBINER_CONTROL          0x1E60
#define NV097_SET_SHADER_STAGE_PROGRAM      0x1E70
#define NV097_SET_SHADER_OTHER_STAGE_INPUT  0x1E78
#define NV_COMBINER_STAGES                   8

#define NV097_SET_ALPHA_TEST_ENABLE         0x0300
#define NV097_SET_BLEND_ENABLE              0x0304
#define NV097_SET_ALPHA_FUNC                0x033C
#define NV097_SET_ALPHA_REF                 0x0340
#define NV097_SET_BLEND_FUNC_SFACTOR        0x0344
#define NV097_SET_BLEND_FUNC_DFACTOR        0x0348
#define NV097_SET_BLEND_COLOR               0x034C
#define NV097_SET_BLEND_EQUATION            0x0350
#define NV097_SET_COLOR_MASK                0x0358

/* One vertex attribute stream, as the title describes it. Attribute 0 is
 * position; the rest are colours, texture coordinates and so on. */
typedef struct {
    uint32_t dma_offset;  /* physical address stated by the DMA object */
    uint32_t offset;      /* guest address of element 0 */
    uint32_t type;        /* NV097 data type nibble */
    uint32_t size;        /* components per element */
    uint32_t stride;      /* bytes between elements */
} VertexAttr;

typedef struct {
    uint32_t offset, format, address, control0, control1, filter, image_rect;
} TextureStage;

typedef struct {
    uint32_t base, width, height, pitch;
} TextureSample;

#define NV_VERTEX_ATTRS 16
#define NV_MAX_INDICES  4096

static struct {
    VertexAttr attr[NV_VERTEX_ATTRS];
    TextureStage tex[NV_TEXTURE_STAGES];
    uint32_t combiner_control, shader_stage_program, shader_other_stage_input;
    uint32_t final_combiner[2];
    uint32_t combiner_factor[2][NV_COMBINER_STAGES];
    uint32_t combiner_rgb_in[NV_COMBINER_STAGES];
    uint32_t combiner_rgb_out[NV_COMBINER_STAGES];
    uint32_t combiner_alpha_in[NV_COMBINER_STAGES];
    uint32_t combiner_alpha_out[NV_COMBINER_STAGES];
    uint32_t alpha_test_enable, blend_enable, alpha_func, alpha_ref;
    uint32_t blend_sfactor, blend_dfactor, blend_color, blend_equation;
    uint32_t color_mask;
    uint32_t   prim;                    /* SET_BEGIN_END parameter, 0 = ended */
    uint16_t   idx[NV_MAX_INDICES];
    uint32_t   idx_count;
    uint32_t   draws, verts, nonzero_draws;
    float      min_x, max_x, min_y, max_y;
    uint32_t color_offset, color_base, pitch, format;
    uint32_t clip_x, clip_w, clip_y, clip_h;
    uint32_t clear_color;
    uint32_t clears, unhandled_total;
    uint32_t tris_drawn, tris_skipped_offscreen, batches_untransformed;
    uint32_t batches_vertex_program;
    uint32_t batches_textured, batches_pixel_program_unsupported;

    /* Transform program/constant capture -- observed only, never executed,
     * until the captured contents are checked against xemu's decoder. */
    uint32_t program_data[NV2A_MAX_TRANSFORM_PROGRAM_LENGTH][4];
    uint32_t program_load;       /* next row SET_TRANSFORM_PROGRAM writes to */
    uint32_t program_load_high;  /* highest row index actually written + 1 */
    uint32_t program_start;      /* SET_TRANSFORM_PROGRAM_START value */
    uint32_t transform_mode;     /* SET_TRANSFORM_EXECUTION_MODE, low 2 bits */
    uint32_t vsh_constants[NV2A_VERTEXSHADER_CONSTANTS][4];
    uint32_t const_load;         /* next row SET_TRANSFORM_CONSTANT writes to */
    uint8_t  const_written[NV2A_VERTEXSHADER_CONSTANTS];
} s_gpu;

/* The report callback can run after the title has already disabled or
 * repointed its texture stages.  Keep the descriptors from the most recent
 * batch that the software pixel path actually sampled so source/target dumps
 * describe the same rendering path rather than unrelated later live state. */
static TextureStage s_last_draw_tex[2];
static uint32_t s_last_draw_tex_count;

/* Unhandled methods, ranked. The interesting output is not that something was
 * skipped but which things dominate, because that is the order to implement
 * them in. */
#define PB_EXEC_MAX_UNHANDLED 2048
typedef struct { uint32_t method, count; } PbUnhandled;
static PbUnhandled s_unhandled[PB_EXEC_MAX_UNHANDLED];
static int s_unhandled_count;

static void note_unhandled(uint32_t method)
{
    int i;

    s_gpu.unhandled_total++;
    for (i = 0; i < s_unhandled_count; i++) {
        if (s_unhandled[i].method == method) {
            s_unhandled[i].count++;
            return;
        }
    }
    if (s_unhandled_count < PB_EXEC_MAX_UNHANDLED) {
        s_unhandled[s_unhandled_count].method = method;
        s_unhandled[s_unhandled_count].count = 1;
        s_unhandled_count++;
    }
}

/* Read attribute `a` of vertex `index` as floats. Only the float and the
 * normalised-byte types appear in practice; anything else returns 0 so a
 * caller sees a degenerate vertex rather than reading past the array.
 * UB_D3D is BGRA in memory (xemu binds it as GL_BGRA), whereas UB_OGL keeps
 * byte order. */
/* ── c[58]/c[59] provenance trace (Update 242, diagnostic only) ───────────
 *
 * The interpreter has been verified correct (Update 241): it computes
 * oPos = 0 because c[58]/c[59] are never observed as written anywhere in
 * the captured pushbuffer stream, not because it mishandles them. This
 * traces the actual write-triggered path (nv2a_pb_exec_method(), invoked
 * directly from nv2a_pb_scan()'s decode loop -- not the periodic DMA_PUT-
 * diff poll in xbox_memory_layout.c, which only decides WHEN a segment gets
 * scanned, never what happens once it is) to settle three questions before
 * touching anything else: are these two constants ever written at all
 * (case B if not), do they land under the indices this decoder expects
 * (case C if not), and if they are written with sane values, does the
 * interpreter actually see them by the time a draw happens (case A if not).
 *
 * Gated behind RECOMP_VSH_CONST_TRACE, capped, and restricted to a narrow
 * index window so a title that DOES use c[58]/c[59] every frame cannot turn
 * this into hot-path spam. Purely additive: no existing capture behaviour
 * changes when the env var is unset. */
#define CTRACE_LO 55
#define CTRACE_HI 62
#define CTRACE_MAX_EVENTS 80

static int ctrace_enabled(void)
{
    static int e = -1;
    if (e < 0)
        e = getenv("RECOMP_VSH_CONST_TRACE") != NULL;
    return e;
}

static uint32_t ctrace_seq(void)
{
    static uint32_t seq;
    return seq++;
}

/* DMA_PUT/DMA_GET live at fixed guest virtual addresses (XBOX_NV2A_BASE
 * 0xFD000000 + NV2A_USER_DMA_PUT/GET 0x800040/0x800044, both already
 * verified against xbox_memory_layout.c/kernel.h in Update 240's xemu
 * ground-truth session) -- read directly, independent of whatever the
 * polling thread last cached. */
static void ctrace_read_dma(uint32_t *get, uint32_t *put)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    *put = *(const uint32_t *)(mem + 0xFD800040u);
    *get = *(const uint32_t *)(mem + 0xFD800044u);
}

static void ctrace_const_write(uint32_t idx, uint32_t line, uint32_t param,
                                uint32_t subch, uint32_t method, uint32_t va)
{
    static int count;
    uint32_t get, put;

    if (!ctrace_enabled() || idx < CTRACE_LO || idx > CTRACE_HI)
        return;
    if (count >= CTRACE_MAX_EVENTS) {
        static int said;
        if (!said++)
            fprintf(stderr, "  [CTRACE] event cap (%d) reached, no more"
                            " CONST_WRITE events will be logged\n", CTRACE_MAX_EVENTS);
        return;
    }
    count++;
    ctrace_read_dma(&get, &put);
    fprintf(stderr, "  [CTRACE] seq=%u tid=%lu CONST_WRITE c[%u].dword%u"
                    " = 0x%08X (%.6g)  subch=%u method=0x%04X va=0x%08X"
                    " const_load=%u DMA_GET=0x%08X DMA_PUT=0x%08X\n",
            ctrace_seq(), (unsigned long)GetCurrentThreadId(), idx, line, param,
            (double)*(const float *)&param, subch, method, va,
            s_gpu.const_load, get, put);
    fflush(stderr);
}

static void ctrace_program_start(uint32_t param, uint32_t va)
{
    uint32_t get, put;
    if (!ctrace_enabled())
        return;
    ctrace_read_dma(&get, &put);
    fprintf(stderr, "  [CTRACE] seq=%u tid=%lu PROGRAM_START = %u va=0x%08X"
                    " DMA_GET=0x%08X DMA_PUT=0x%08X\n",
            ctrace_seq(), (unsigned long)GetCurrentThreadId(), param, va, get, put);
    fflush(stderr);
}

static void ctrace_draw(uint32_t draw_no)
{
    uint32_t get, put;
    if (!ctrace_enabled())
        return;
    ctrace_read_dma(&get, &put);
    fprintf(stderr, "  [CTRACE] seq=%u tid=%lu DRAW #%u c[58].written=%d"
                    " c[59].written=%d DMA_GET=0x%08X DMA_PUT=0x%08X\n",
            ctrace_seq(), (unsigned long)GetCurrentThreadId(), draw_no,
            s_gpu.const_written[58], s_gpu.const_written[59], get, put);
    fflush(stderr);
}

static int fetch_attr(const VertexAttr *a, uint32_t index, float out[4])
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    const uint8_t *p;
    uint32_t i, element_bytes, base;
    uint64_t needed;

    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    if (!a->offset || !a->size || !a->stride)
        return 0;
    element_bytes = a->type == 2 ? a->size * 4u : a->size;
    needed = (uint64_t)index * a->stride + element_bytes;
    if (needed > UINT32_MAX
            || !xbox_ResolveGpuPhysicalRange(a->dma_offset,
                                              (uint32_t)needed, &base))
        return 0;
    p = mem + base + (size_t)index * a->stride;

    switch (a->type) {
    case 0:                                  /* UB_D3D: BGRA -> RGBA */
        if (a->size != 4)
            return 0;
        out[0] = (float)p[2] / 255.0f;
        out[1] = (float)p[1] / 255.0f;
        out[2] = (float)p[0] / 255.0f;
        out[3] = (float)p[3] / 255.0f;
        return 1;
    case 2:                                  /* float */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = ((const float *)p)[i];
        return 1;
    case 4:                                  /* unsigned byte, normalised */
        for (i = 0; i < a->size && i < 4; i++)
            out[i] = (float)p[i] / 255.0f;
        return 1;
    default:
        return 0;
    }
}

static uint32_t surface_bpp(void)
{
    /* The pitch and the clip width together give the pixel size, which is more
     * reliable than decoding the format field: the format's colour code is
     * only meaningful alongside a type the title also sets, while the pitch is
     * always exactly how many bytes a row occupies. */
    if (!s_gpu.clip_w)
        return 0;
    return s_gpu.pitch / s_gpu.clip_w;
}


/* Write the current surface out as a 24-bit BMP.
 *
 * A framebuffer window needs someone watching it. A file does not, which makes
 * this the only way to check what a title actually rendered on a machine you
 * are not sitting at -- and the only way to put a picture in a bug report.
 *
 * ponytail: bottom-up 24bpp BMP, no palette, no compression. That is the one
 * format every viewer reads and it is 30 lines; PNG would need a dependency.
 */
static void dump_surface_bmp(void)
{
    const char *prefix = getenv("RECOMP_FB_DUMP");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    static int seq;
    char path[512];
    uint32_t w = s_gpu.clip_w, h = s_gpu.clip_h, y, x;
    uint32_t row_bytes, pad, filesz;
    uint8_t hdr[54];
    FILE *f;
    uint32_t base;

    if (!prefix || !w || !h || (bpp != 2 && bpp != 4) || !s_gpu.color_offset)
        return;
    base = dma_resolve(s_gpu.color_offset,
                       (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch);
    if (!base)
        return;

    row_bytes = w * 3;
    pad = (4 - (row_bytes & 3)) & 3;
    filesz = 54 + (row_bytes + pad) * h;

    snprintf(path, sizeof path, "%s%03d.bmp", prefix, seq++);
    f = fopen(path, "wb");
    if (!f)
        return;

    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    memcpy(hdr + 2, &filesz, 4);
    hdr[10] = 54;
    hdr[14] = 40;
    memcpy(hdr + 18, &w, 4);
    memcpy(hdr + 22, &h, 4);
    hdr[26] = 1;
    hdr[28] = 24;
    fwrite(hdr, 1, sizeof hdr, f);

    /* BMP rows run bottom-up. */
    for (y = h; y-- > 0; ) {
        const uint8_t *row = mem + base
                           + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        for (x = 0; x < w; x++) {
            uint8_t bgr[3];
            if (bpp == 4) {
                uint32_t v = ((const uint32_t *)row)[s_gpu.clip_x + x];
                bgr[0] = (uint8_t)(v);
                bgr[1] = (uint8_t)(v >> 8);
                bgr[2] = (uint8_t)(v >> 16);
            } else {
                uint16_t v = ((const uint16_t *)row)[s_gpu.clip_x + x];
                bgr[0] = (uint8_t)(( v        & 0x1F) << 3);
                bgr[1] = (uint8_t)(((v >>  5) & 0x3F) << 2);
                bgr[2] = (uint8_t)(((v >> 11) & 0x1F) << 3);
            }
            fwrite(bgr, 1, 3, f);
        }
        if (pad) {
            static const uint8_t zero[3] = {0, 0, 0};
            fwrite(zero, 1, pad, f);
        }
    }
    fclose(f);
    if (seq == 1)
        fprintf(stderr, "  [GPU] framebuffer dump: %s (%ux%u from 0x%08X %ubpp)\n",
                path, w, h, s_gpu.color_offset, bpp);
}

/* Defined below, next to the rest of the rasteriser; the clear path uses it
 * for RECOMP_RASTER_TEST. */
static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float tex0[3][4],
                            const float tex1[3][4],
                            const float spec[3][4],
                            const TextureSample *sample0,
                            const TextureSample *sample1,
                            float crossfade);
static uint32_t texture_level0_span(const TextureStage *t,
                                    uint32_t *width, uint32_t *height,
                                    uint32_t *pitch, int *linear);

static void clear_surface(uint32_t param)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    uint32_t y, x;

    if (!(param & NV097_CLEAR_COLOR_MASK))
        return;                            /* depth/stencil only */
    if (!s_gpu.color_offset || !s_gpu.pitch || !s_gpu.clip_h || bpp == 0)
        return;
    s_gpu.color_base = dma_resolve(s_gpu.color_offset,
                                   (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch);
    if (!s_gpu.color_base) {
        static int said_unmapped;
        if (!said_unmapped++)
            fprintf(stderr, "  [GPU] REFUSING to clear unmapped physical "
                            "surface 0x%08X\n", s_gpu.color_offset);
        return;
    }
    if (surface_write_refused(s_gpu.color_base,
                              (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch,
                              "clear"))
        return;

    for (y = 0; y < s_gpu.clip_h; y++) {
        uint8_t *row = mem + s_gpu.color_base
                     + (size_t)(s_gpu.clip_y + y) * s_gpu.pitch;
        if (bpp == 4) {
            uint32_t *p = (uint32_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = s_gpu.clear_color;
        } else if (bpp == 2) {
            /* The clear value is always given as A8R8G8B8; a 16-bit surface
             * takes the same colour reduced to 5:6:5. */
            uint16_t v = (uint16_t)(((s_gpu.clear_color >> 8) & 0xF800)
                                  | ((s_gpu.clear_color >> 5) & 0x07E0)
                                  | ((s_gpu.clear_color >> 3) & 0x001F));
            uint16_t *p = (uint16_t *)row + s_gpu.clip_x;
            for (x = 0; x < s_gpu.clip_w; x++)
                p[x] = v;
        }
    }
    s_gpu.clears++;
    /* Progress markers, interleaved with everything else in the log. The
     * summary says drawing stopped; only a marker next to the surrounding
     * activity says what the title was doing when it stopped. */
    if ((s_gpu.clears % 100) == 0)
        fprintf(stderr, "  [GPU] clear #%u\n", s_gpu.clears);
    /* Distinct clear colours actually used. "Cleared to black" and "the clear
     * never ran" look identical in the framebuffer, and only one of them is a
     * bug -- so record what was asked for, not just how often. */
    {
        static uint32_t seen[8];
        static int n;
        int i;
        for (i = 0; i < n; i++)
            if (seen[i] == s_gpu.clear_color) break;
        if (i == n && n < 8) {
            seen[n++] = s_gpu.clear_color;
            fprintf(stderr, "  [GPU] clear colour 0x%08X -> surface 0x%08X"
                            " (%ubpp)\n",
                    s_gpu.clear_color, s_gpu.color_offset, surface_bpp());
        }
    }

    /* Prove the pixel path end to end, independent of whether the title has
     * given us any geometry yet.
     *
     * "Nothing on screen" has three very different causes -- the surface
     * address or pitch is wrong, the rasteriser is broken, or the title's
     * vertex buffers are empty -- and they are indistinguishable from a black
     * window. RECOMP_RASTER_TEST draws one known triangle into the surface
     * just cleared, so a visible triangle rules out the first two and leaves
     * only the third. On Wreckless it is the third: attribute 0 decodes
     * correctly (float, size 2, stride 16) and the buffer it points at stays
     * zero.
     *
     * ponytail: bring-up aid, not a feature. It costs one branch per clear. */
    if (getenv("RECOMP_RASTER_TEST")) {
        static int announced;
        /* Every clear, not once: the title clears each frame and double-buffers,
         * so a triangle drawn a single time is erased before anyone sees it. */
        if (s_gpu.clip_w && s_gpu.clip_h) {
            float a[2], b[2], c[2];
            a[0] = s_gpu.clip_w * 0.5f; a[1] = s_gpu.clip_h * 0.15f;
            b[0] = s_gpu.clip_w * 0.85f; b[1] = s_gpu.clip_h * 0.85f;
            c[0] = s_gpu.clip_w * 0.15f; c[1] = s_gpu.clip_h * 0.85f;
            raster_triangle(a, b, c, 0xFFFF00FFu, NULL, NULL, NULL,
                            NULL, NULL, 0.0f);
                                                    /* magenta: never a clear colour */
            if (announced++ == 0)
            fprintf(stderr, "  [GPU] raster self-test: triangle (%.0f,%.0f)"
                            " (%.0f,%.0f) (%.0f,%.0f) into 0x%08X %ubpp\n",
                    a[0], a[1], b[0], b[1], c[0], c[1],
                    s_gpu.color_offset, surface_bpp());
        }
    }

    /* Show the surface actually being drawn into. A title that double-buffers
     * renders into the back buffer, so following AvSetDisplayMode's address
     * would show the one nothing is writing. */
    xbox_FramebufferWindowSet(s_gpu.color_base, s_gpu.pitch);

    /* And open the window, rather than waiting for AvSetDisplayMode to do it.
     *
     * That was the only caller, so a title which draws before setting a display
     * mode -- or never sets one at all -- got no window however much it
     * rendered. The Xbox Dashboard clears a 1280x960 surface at 0x00088000 on
     * its first frame and had not called AvSetDisplayMode by then, so
     * RECOMP_FB_WINDOW=1 was set, the executor knew the address and the pitch,
     * and nothing appeared.
     *
     * Here is the better trigger anyway: this runs when a surface address is
     * known to be real, because a clear just used it. Idempotent and gated on
     * RECOMP_FB_WINDOW, so the cost is one interlocked compare per clear. */
    xbox_FramebufferWindowStart();
}


/* ── Rasteriser ──────────────────────────────────────────────────────────
 *
 * Fills triangles straight into the guest framebuffer, the same memory
 * clear_surface() writes and the framebuffer window already shows. That is the
 * whole reason it is done on the CPU rather than through D3D: nothing new has
 * to be plumbed for the result to be visible.
 *
 * ponytail: flat-shaded (one colour per triangle, from the first vertex) and
 * no depth buffer or texturing. Two ways a batch's vertices reach here:
 * attribute 0 already in screen space (raster_batch_screen_space(), UI/HUD/
 * 2D overlays), or run through the real NV2A vertex-program interpreter
 * (raster_batch_vertex_program() -> transform_vertex(), vsh_interp.c) for
 * everything else in programmable transform mode. Fixed-function transform
 * mode is still not modelled and is counted and skipped rather than drawn
 * wrong. Upgrade path for texturing/depth/performance is the D3D11
 * translator in src/nv2a/nv2a_pgraph_d3d11.c, which can reuse
 * src/d3d/d3d8_vsh.c's translator once it needs a GPU-side vertex-program
 * path.
 */

static void put_pixel(uint8_t *mem, uint32_t bpp, int x, int y, uint32_t argb)
{
    uint8_t *row;

    if (x < (int)s_gpu.clip_x || x >= (int)(s_gpu.clip_x + s_gpu.clip_w))
        return;
    if (y < (int)s_gpu.clip_y || y >= (int)(s_gpu.clip_y + s_gpu.clip_h))
        return;
    /* Same reason the clear checks: a rasterised triangle writes guest memory
     * too, and a surface address that lands on the image is no safer one pixel
     * at a time than 4.9 MB at once. */
    if (!s_gpu.color_base || surface_hits_image(s_gpu.color_base,
                           (s_gpu.clip_y + s_gpu.clip_h) * s_gpu.pitch))
        return;
    row = mem + s_gpu.color_base + (size_t)y * s_gpu.pitch;
    if (bpp == 4) {
        ((uint32_t *)row)[x] = argb;
    } else if (bpp == 2) {
        ((uint16_t *)row)[x] = (uint16_t)(((argb >> 8) & 0xF800)
                                        | ((argb >> 5) & 0x07E0)
                                        | ((argb >> 3) & 0x001F));
    }
}

/* Half-space fill. Barycentric edge functions rather than scanline slopes:
 * the same test decides both windings, so a title that emits clockwise
 * triangles does not silently render nothing. */
static uint32_t sample_argb8(const TextureSample *s, int x, int y)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)s->width) x = (int)s->width - 1;
    if (y >= (int)s->height) y = (int)s->height - 1;
    return *(const uint32_t *)(mem + s->base + (size_t)y * s->pitch
                              + (size_t)x * 4);
}

static uint32_t lerp_argb8(uint32_t a, uint32_t b, float t)
{
    uint32_t out = 0, shift;
    for (shift = 0; shift < 32; shift += 8) {
        float av = (float)((a >> shift) & 0xFFu);
        float bv = (float)((b >> shift) & 0xFFu);
        uint32_t v = (uint32_t)(av + (bv - av) * t + 0.5f);
        if (v > 255) v = 255;
        out |= v << shift;
    }
    return out;
}

/* Linear Xbox textures use texel coordinates.  Xemu normalises them by the
 * texture size before textureProj(); subtracting half a texel reproduces the
 * host linear-filter convention, so a raster sample at x+0.5 lands exactly
 * on source texel x during a 1:1 full-screen copy. */
static uint32_t sample_argb8_linear(const TextureSample *s, float u, float v)
{
    float fx = u - 0.5f, fy = v - 0.5f;
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    float tx = fx - floorf(fx), ty = fy - floorf(fy);
    uint32_t c00 = sample_argb8(s, x0, y0);
    uint32_t c10 = sample_argb8(s, x0 + 1, y0);
    uint32_t c01 = sample_argb8(s, x0, y0 + 1);
    uint32_t c11 = sample_argb8(s, x0 + 1, y0 + 1);
    return lerp_argb8(lerp_argb8(c00, c10, tx),
                      lerp_argb8(c01, c11, tx), ty);
}

static uint32_t add_specular_rgb(uint32_t argb, const float spec[3][4],
                                 float w0, float w1, float w2, float area)
{
    float add[3];
    uint32_t out = argb & 0xFF000000u;
    int c;
    for (c = 0; c < 3; c++)
        add[c] = (w1 * spec[0][c] + w2 * spec[1][c]
                + w0 * spec[2][c]) / area;
    for (c = 0; c < 3; c++) {
        uint32_t shift = (uint32_t)(16 - c * 8);
        float value = (float)((argb >> shift) & 0xFFu) / 255.0f + add[c];
        if (value < 0.0f) value = 0.0f;
        if (value > 1.0f) value = 1.0f;
        out |= (uint32_t)(value * 255.0f + 0.5f) << shift;
    }
    return out;
}

static void raster_triangle(const float a[2], const float b[2],
                            const float c[2], uint32_t argb,
                            const float tex0[3][4],
                            const float tex1[3][4],
                            const float spec[3][4],
                            const TextureSample *sample0,
                            const TextureSample *sample1,
                            float crossfade)
{
    uint8_t *mem = (uint8_t *)xbox_GetMemoryOffset();
    uint32_t bpp = surface_bpp();
    float area;
    int minx, maxx, miny, maxy, x, y;

    if (bpp != 4 && bpp != 2)
        return;

    area = (b[0] - a[0]) * (c[1] - a[1]) - (b[1] - a[1]) * (c[0] - a[0]);
    if (area == 0.0f)
        return;                            /* degenerate */

    minx = (int)floorf(fminf(a[0], fminf(b[0], c[0])));
    maxx = (int)ceilf (fmaxf(a[0], fmaxf(b[0], c[0])));
    miny = (int)floorf(fminf(a[1], fminf(b[1], c[1])));
    maxy = (int)ceilf (fmaxf(a[1], fmaxf(b[1], c[1])));

    if (minx < (int)s_gpu.clip_x) minx = (int)s_gpu.clip_x;
    if (miny < (int)s_gpu.clip_y) miny = (int)s_gpu.clip_y;
    if (maxx > (int)(s_gpu.clip_x + s_gpu.clip_w)) maxx = (int)(s_gpu.clip_x + s_gpu.clip_w);
    if (maxy > (int)(s_gpu.clip_y + s_gpu.clip_h)) maxy = (int)(s_gpu.clip_y + s_gpu.clip_h);
    if (minx >= maxx || miny >= maxy) {
        s_gpu.tris_skipped_offscreen++;
        return;
    }

    for (y = miny; y < maxy; y++) {
        for (x = minx; x < maxx; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (b[0] - a[0]) * (py - a[1]) - (b[1] - a[1]) * (px - a[0]);
            float w1 = (c[0] - b[0]) * (py - b[1]) - (c[1] - b[1]) * (px - b[0]);
            float w2 = (a[0] - c[0]) * (py - c[1]) - (a[1] - c[1]) * (px - c[0]);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0)
             || (w0 <= 0 && w1 <= 0 && w2 <= 0)) {
                uint32_t pixel = argb;
                if (tex0 && sample0) {
                    float tu = (w1 * tex0[0][0] + w2 * tex0[1][0]
                              + w0 * tex0[2][0]) / area;
                    float tv = (w1 * tex0[0][1] + w2 * tex0[1][1]
                              + w0 * tex0[2][1]) / area;
                    float tw = (w1 * tex0[0][3] + w2 * tex0[1][3]
                              + w0 * tex0[2][3]) / area;
                    if (isfinite(tw) && fabsf(tw) > 1e-20f) {
                        pixel = sample_argb8_linear(sample0, tu / tw, tv / tw);
                        if (tex1 && sample1) {
                            float u1 = (w1 * tex1[0][0] + w2 * tex1[1][0]
                                      + w0 * tex1[2][0]) / area;
                            float v1 = (w1 * tex1[0][1] + w2 * tex1[1][1]
                                      + w0 * tex1[2][1]) / area;
                            float q1 = (w1 * tex1[0][3] + w2 * tex1[1][3]
                                      + w0 * tex1[2][3]) / area;
                            if (isfinite(q1) && fabsf(q1) > 1e-20f) {
                                uint32_t other = sample_argb8_linear(
                                    sample1, u1 / q1, v1 / q1);
                                pixel = lerp_argb8(other, pixel, crossfade);
                                pixel = (pixel & 0x00FFFFFFu)
                                      | ((uint32_t)(((pixel >> 24) & 0xFFu)
                                          * crossfade + 0.5f) << 24);
                            }
                        }
                        if (spec)
                            pixel = add_specular_rgb(pixel, spec,
                                                     w0, w1, w2, area);
                    }
                }
                put_pixel(mem, bpp, x, y, pixel);
            }
        }
    }
    s_gpu.tris_drawn++;
}

/* Attribute 3 is diffuse colour in every NV2A layout that sets one. Absent it,
 * white -- a visible wrong colour beats an invisible correct one during
 * bring-up. */
static uint32_t vertex_color(uint32_t index)
{
    float c[4];

    if (!fetch_attr(&s_gpu.attr[3], index, c))
        return 0xFFFFFFFFu;
    return ((uint32_t)(c[3] * 255.0f) << 24)
         | ((uint32_t)(c[0] * 255.0f) << 16)
         | ((uint32_t)(c[1] * 255.0f) <<  8)
         |  (uint32_t)(c[2] * 255.0f);
}

/* ── Vertex-program execution ────────────────────────────────────────────
 *
 * The actual interpreter (field decode, opcode semantics, R12/oPos alias,
 * MAC/ILU write-back routing) lives in vsh_interp.c, checked directly
 * against xemu's source and this project's own src/d3d/d3d8_vsh.c -- nothing
 * SCDA-specific, and covered by tests/vsh_interp's regression tests. This
 * file only adapts it to the captured s_gpu program/constant state and to
 * fetching real per-vertex attributes for actual rendering. */

/* Fetches v0..v15 for `index` via fetch_attr(); unbound streams keep the
 * hardware default (0,0,0,1), matching xemu's uniform-attribute default. */
static void fetch_vattr(uint32_t index, Vec4 vattr[NV2A_VSH_NUM_ATTRIBUTES])
{
    uint32_t i;
    for (i = 0; i < NV2A_VSH_NUM_ATTRIBUTES; i++) {
        vattr[i].x[0] = vattr[i].x[1] = vattr[i].x[2] = 0.0f;
        vattr[i].x[3] = 1.0f;
        if (s_gpu.attr[i].offset)
            fetch_attr(&s_gpu.attr[i], index, vattr[i].x);
    }
}

/* Evaluate the captured program for a synthetic v0 (position only, the rest
 * default) -- used solely by the diagnostic dump below, not by real
 * rendering (which fetches every bound attribute via fetch_vattr()). */
static int vsh_evaluate_capture_only(Vec4 v0, Vec4 *out_pos, int trace)
{
    Vec4 vattr[NV2A_VSH_NUM_ATTRIBUTES];
    VshResult res;
    uint32_t i;

    for (i = 0; i < NV2A_VSH_NUM_ATTRIBUTES; i++)
        vattr[i] = (Vec4){ { 0.0f, 0.0f, 0.0f, 1.0f } };
    vattr[0] = v0;

    vsh_run_program(s_gpu.program_data, s_gpu.program_load_high,
                     s_gpu.program_start, s_gpu.vsh_constants, vattr,
                     &res, trace ? stderr : NULL);
    *out_pos = res.oreg[VSH_OREG_POS];
    return res.status == -2 ? -2 : res.steps;
}

/* Runs the real vertex program for vertex `index` and produces a
 * screen-space point ready for raster_triangle(). NV2A vertex programs leave
 * oPos.xy in screen space; xemu's vsh-prog.c explicitly converts that result
 * back to host clip space and then multiplies by w so the host GPU's later
 * perspective divide recovers the same screen coordinates. A CPU rasterizer
 * must therefore consume oPos.xy directly -- applying another divide and
 * viewport transform would transform the vertex twice. Colour comes from oD0
 * (hardware default (0,0,0,1) = opaque black if the program never writes it,
 * same as real hardware). */
static int transform_vertex(uint32_t index, float out_xy[2], uint32_t *out_color,
                            float out_tex0[4], float out_tex1[4],
                            float out_spec[4])
{
    Vec4 vattr[NV2A_VSH_NUM_ATTRIBUTES];
    VshResult res;
    Vec4 pos, color;

    fetch_vattr(index, vattr);
    vsh_run_program(s_gpu.program_data, s_gpu.program_load_high,
                     s_gpu.program_start, s_gpu.vsh_constants, vattr,
                     &res, NULL);
    if (res.status == -2)
        return 0;

    pos = res.oreg[VSH_OREG_POS];
    if (!isfinite(pos.x[0]) || !isfinite(pos.x[1]))
        return 0;
    out_xy[0] = pos.x[0];
    out_xy[1] = pos.x[1];

    color = res.oreg[VSH_OREG_D0];
    if (out_tex0)
        memcpy(out_tex0, res.oreg[VSH_OREG_T0].x,
               sizeof res.oreg[VSH_OREG_T0].x);
    if (out_tex1)
        memcpy(out_tex1, res.oreg[VSH_OREG_T1].x,
               sizeof res.oreg[VSH_OREG_T1].x);
    if (out_spec)
        memcpy(out_spec, res.oreg[VSH_OREG_D1].x, sizeof res.oreg[VSH_OREG_D1].x);
    {
        float r = color.x[0], g = color.x[1], b = color.x[2], a = color.x[3];
        if (r < 0.0f) r = 0.0f; else if (r > 1.0f) r = 1.0f;
        if (g < 0.0f) g = 0.0f; else if (g > 1.0f) g = 1.0f;
        if (b < 0.0f) b = 0.0f; else if (b > 1.0f) b = 1.0f;
        if (a < 0.0f) a = 0.0f; else if (a > 1.0f) a = 1.0f;
        *out_color = ((uint32_t)(a * 255.0f) << 24) | ((uint32_t)(r * 255.0f) << 16)
                   | ((uint32_t)(g * 255.0f) << 8)  |  (uint32_t)(b * 255.0f);
    }
    return 1;
}

/* Is attribute 0 already in screen space? Measured, not assumed: every vertex
 * of the batch has to land inside the surface. Object-space positions are
 * small numbers around the origin and fail this immediately, which is what
 * keeps an untransformed batch from being smeared across the top-left corner.
 */
static int batch_is_screen_space(void)
{
    float p[4];
    uint32_t i;

    if (!s_gpu.clip_w || !s_gpu.clip_h)
        return 0;
    for (i = 0; i < s_gpu.idx_count; i++) {
        if (!fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], p))
            return 0;
        if (p[0] < (float)s_gpu.clip_x - 1.0f
         || p[0] > (float)(s_gpu.clip_x + s_gpu.clip_w) + 1.0f
         || p[1] < (float)s_gpu.clip_y - 1.0f
         || p[1] > (float)(s_gpu.clip_y + s_gpu.clip_h) + 1.0f)
            return 0;
    }
    return 1;
}

/* Capture-only dump for the transform program active at a given draw: mode,
 * start PC, every loaded instruction row, every constant row a load
 * actually touched, and a capture-only evaluation with three deliberately
 * different v0 inputs (falsifiable: does oPos depend on the vertex position
 * at all, or is it constant regardless -- this does not depend on attribute
 * 0's type-0 decoding, which fetch_attr does not implement, so reading the
 * title's real buffer here would only prove "a fixed zero produces a fixed
 * answer").
 *
 * Called at several draw counts, not only the first: the first draw's
 * constants could simply not have been uploaded yet (a warm-up/placeholder
 * draw before the real camera/projection constants arrive), so a single
 * snapshot cannot tell "never written" apart from "not written yet". */
static void dump_transform_program_if_due(void)
{
    static const uint32_t due_at[] = { 1, 10, 50, 200, 500, 2000 };
    uint32_t i, r, c;
    int is_due = 0;

    for (i = 0; i < sizeof(due_at) / sizeof(due_at[0]); i++)
        if (s_gpu.draws == due_at[i]) { is_due = 1; break; }
    if (!is_due)
        return;

    fprintf(stderr, "  [GPU] transform program at draw #%u:"
                    " mode=%u start=%u rows_loaded=%u\n",
            s_gpu.draws, s_gpu.transform_mode, s_gpu.program_start,
            s_gpu.program_load_high);
    for (r = 0; r < s_gpu.program_load_high
             && r < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH; r++)
        fprintf(stderr, "  [GPU]   prog[%3u] = %08X %08X %08X %08X\n",
                r, s_gpu.program_data[r][0], s_gpu.program_data[r][1],
                s_gpu.program_data[r][2], s_gpu.program_data[r][3]);
    for (c = 0; c < NV2A_VERTEXSHADER_CONSTANTS; c++) {
        if (!s_gpu.const_written[c])
            continue;
        fprintf(stderr, "  [GPU]   const[%3u] = %08X %08X %08X %08X\n",
                c, s_gpu.vsh_constants[c][0], s_gpu.vsh_constants[c][1],
                s_gpu.vsh_constants[c][2], s_gpu.vsh_constants[c][3]);
    }
    {
        static const Vec4 probes[3] = {
            {{0.0f, 0.0f, 0.0f, 1.0f}},
            {{1.0f, 2.0f, 3.0f, 1.0f}},
            {{100.0f, -200.0f, 300.0f, 1.0f}},
        };
        int p;
        for (p = 0; p < 3; p++) {
            Vec4 pos;
            int ended = vsh_evaluate_capture_only(probes[p], &pos, p == 1);
            fprintf(stderr, "  [GPU]   eval v0=(%.1f,%.1f,%.1f,%.1f) ->"
                            " oPos=(%.6f,%.6f,%.6f,%.6f) [ended row=%d]\n",
                    probes[p].x[0], probes[p].x[1], probes[p].x[2], probes[p].x[3],
                    pos.x[0], pos.x[1], pos.x[2], pos.x[3], ended);
        }
    }
    fflush(stderr);
}

/* NV097 primitive types that are triangles under some winding. */
#define NV_PRIM_TRIANGLES      5
#define NV_PRIM_TRIANGLE_STRIP 6
#define NV_PRIM_TRIANGLE_FAN   7
#define NV_PRIM_QUADS          8
#define NV_PRIM_QUAD_STRIP     9

/* Raster path for batches whose attribute 0 is already screen-space
 * (pre-transformed UI/HUD/2D geometry): flat-shaded from attribute 3. */
static void raster_batch_screen_space(void)
{
    float p[3][4];
    uint32_t i;

#define VTX(slot, index) \
    (fetch_attr(&s_gpu.attr[0], (index), p[slot]) ? 1 : 0)

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count; i += 3)
            if (VTX(0, s_gpu.idx[i]) && VTX(1, s_gpu.idx[i+1])
             && VTX(2, s_gpu.idx[i+2]))
                raster_triangle(p[0], p[1], p[2], vertex_color(s_gpu.idx[i]),
                                NULL, NULL, NULL, NULL, NULL, 0.0f);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.idx_count; i++)
            if (VTX(0, s_gpu.idx[i]) && VTX(1, s_gpu.idx[i+1])
             && VTX(2, s_gpu.idx[i+2]))
                raster_triangle(p[0], p[1], p[2], vertex_color(s_gpu.idx[i]),
                                NULL, NULL, NULL, NULL, NULL, 0.0f);
        break;
    case NV_PRIM_TRIANGLE_FAN:
        for (i = 1; i + 1 < s_gpu.idx_count; i++)
            if (VTX(0, s_gpu.idx[0]) && VTX(1, s_gpu.idx[i])
             && VTX(2, s_gpu.idx[i+1]))
                raster_triangle(p[0], p[1], p[2], vertex_color(s_gpu.idx[0]),
                                NULL, NULL, NULL, NULL, NULL, 0.0f);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < s_gpu.idx_count; i += 4) {
            if (VTX(0, s_gpu.idx[i]) && VTX(1, s_gpu.idx[i+1])
             && VTX(2, s_gpu.idx[i+2]))
                raster_triangle(p[0], p[1], p[2], vertex_color(s_gpu.idx[i]),
                                NULL, NULL, NULL, NULL, NULL, 0.0f);
            if (VTX(0, s_gpu.idx[i]) && VTX(1, s_gpu.idx[i+2])
             && VTX(2, s_gpu.idx[i+3]))
                raster_triangle(p[0], p[1], p[2], vertex_color(s_gpu.idx[i]),
                                NULL, NULL, NULL, NULL, NULL, 0.0f);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < s_gpu.idx_count; i += 2) {
            if (VTX(0, s_gpu.idx[i]) && VTX(1, s_gpu.idx[i+1])
             && VTX(2, s_gpu.idx[i+2]))
                raster_triangle(p[0], p[1], p[2], vertex_color(s_gpu.idx[i]),
                                NULL, NULL, NULL, NULL, NULL, 0.0f);
            if (VTX(0, s_gpu.idx[i+2]) && VTX(1, s_gpu.idx[i+1])
             && VTX(2, s_gpu.idx[i+3]))
                raster_triangle(p[0], p[1], p[2], vertex_color(s_gpu.idx[i]),
                                NULL, NULL, NULL, NULL, NULL, 0.0f);
        }
        break;
    default:
        break;                             /* points and lines: not yet */
    }
#undef VTX
}

/* Change-triggered pixel-state chronology.  Hash every register that affects
 * the currently modelled texture/combiner/blend path, emit full raw state only
 * once per variant, and keep batch counts for the periodic report. */
#define PIXEL_TRACK_MAX 32
typedef struct {
    uint32_t hash, first_draw, batches;
    int supported;
    char label[4];
} PixelTrack;
static PixelTrack s_pixel_track[PIXEL_TRACK_MAX];
static int s_pixel_track_count;

static void pixel_hash_word(uint32_t *h, uint32_t v)
{
    *h ^= v;
    *h *= 16777619u;
}

static uint32_t pixel_state_hash(void)
{
    uint32_t h = 2166136261u, i, j;
    pixel_hash_word(&h, s_gpu.shader_stage_program);
    pixel_hash_word(&h, s_gpu.shader_other_stage_input);
    pixel_hash_word(&h, s_gpu.combiner_control);
    pixel_hash_word(&h, s_gpu.final_combiner[0]);
    pixel_hash_word(&h, s_gpu.final_combiner[1]);
    for (i = 0; i < NV_TEXTURE_STAGES; i++) {
        const uint32_t *p = (const uint32_t *)&s_gpu.tex[i];
        for (j = 0; j < sizeof s_gpu.tex[i] / sizeof *p; j++)
            pixel_hash_word(&h, p[j]);
    }
    for (i = 0; i < NV_COMBINER_STAGES; i++) {
        pixel_hash_word(&h, s_gpu.combiner_rgb_in[i]);
        pixel_hash_word(&h, s_gpu.combiner_rgb_out[i]);
        pixel_hash_word(&h, s_gpu.combiner_alpha_in[i]);
        pixel_hash_word(&h, s_gpu.combiner_alpha_out[i]);
        pixel_hash_word(&h, s_gpu.combiner_factor[0][i]);
        pixel_hash_word(&h, s_gpu.combiner_factor[1][i]);
    }
    pixel_hash_word(&h, s_gpu.alpha_test_enable);
    pixel_hash_word(&h, s_gpu.blend_enable);
    pixel_hash_word(&h, s_gpu.alpha_func);
    pixel_hash_word(&h, s_gpu.alpha_ref);
    pixel_hash_word(&h, s_gpu.blend_sfactor);
    pixel_hash_word(&h, s_gpu.blend_dfactor);
    pixel_hash_word(&h, s_gpu.blend_color);
    pixel_hash_word(&h, s_gpu.blend_equation);
    pixel_hash_word(&h, s_gpu.color_mask);
    return h;
}

static void pixel_trace_batch(int supported)
{
    uint32_t h, stages, i;
    PixelTrack *p;
    if (!getenv("RECOMP_PIXEL_TRACE"))
        return;
    h = pixel_state_hash();
    for (i = 0; i < (uint32_t)s_pixel_track_count; i++)
        if (s_pixel_track[i].hash == h) {
            s_pixel_track[i].batches++;
            return;
        }
    if (s_pixel_track_count >= PIXEL_TRACK_MAX)
        return;
    p = &s_pixel_track[s_pixel_track_count];
    memset(p, 0, sizeof *p);
    p->hash = h;
    p->first_draw = s_gpu.draws;
    p->batches = 1;
    p->supported = supported;
    p->label[0] = (char)('A' + s_pixel_track_count % 26);
    p->label[1] = 0;
    s_pixel_track_count++;
    stages = s_gpu.combiner_control & 0xFFu;
    if (stages > NV_COMBINER_STAGES) stages = NV_COMBINER_STAGES;
    fprintf(stderr,
            "  [PIXTRACE] NEW %s hash=%08X draw=%u prim=%u indices=%u supported=%d"
            " shader=%08X modes=%02X,%02X,%02X,%02X other=%08X"
            " combiner=%08X final=%08X,%08X blend=%u sf=%08X df=%08X"
            " eq=%08X color=%08X mask=%08X atest=%u afunc=%08X aref=%08X\n",
            p->label, h, s_gpu.draws, s_gpu.prim, s_gpu.idx_count, supported,
            s_gpu.shader_stage_program,
            s_gpu.shader_stage_program & 0x1Fu,
            (s_gpu.shader_stage_program >> 5) & 0x1Fu,
            (s_gpu.shader_stage_program >> 10) & 0x1Fu,
            (s_gpu.shader_stage_program >> 15) & 0x1Fu,
            s_gpu.shader_other_stage_input, s_gpu.combiner_control,
            s_gpu.final_combiner[0], s_gpu.final_combiner[1],
            s_gpu.blend_enable, s_gpu.blend_sfactor, s_gpu.blend_dfactor,
            s_gpu.blend_equation, s_gpu.blend_color, s_gpu.color_mask,
            s_gpu.alpha_test_enable, s_gpu.alpha_func, s_gpu.alpha_ref);
    for (i = 0; i < NV_TEXTURE_STAGES; i++)
        fprintf(stderr,
                "  [PIXTRACE]   tex%u off=%08X fmt=%08X addr=%08X ctl0=%08X"
                " ctl1=%08X filter=%08X rect=%08X\n", i,
                s_gpu.tex[i].offset, s_gpu.tex[i].format,
                s_gpu.tex[i].address, s_gpu.tex[i].control0,
                s_gpu.tex[i].control1, s_gpu.tex[i].filter,
                s_gpu.tex[i].image_rect);
    for (i = 0; i < stages; i++)
        fprintf(stderr,
                "  [PIXTRACE]   rc%u rgb=%08X/%08X alpha=%08X/%08X"
                " c0=%08X c1=%08X\n", i,
                s_gpu.combiner_rgb_in[i], s_gpu.combiner_rgb_out[i],
                s_gpu.combiner_alpha_in[i], s_gpu.combiner_alpha_out[i],
                s_gpu.combiner_factor[0][i], s_gpu.combiner_factor[1][i]);
    fflush(stderr);
}

static void pixel_trace_report(void)
{
    int i;
    if (!getenv("RECOMP_PIXEL_TRACE"))
        return;
    for (i = 0; i < s_pixel_track_count; i++)
        fprintf(stderr, "  [PIXTRACE] %s hash=%08X first_draw=%u batches=%u"
                        " supported=%d\n", s_pixel_track[i].label,
                s_pixel_track[i].hash, s_pixel_track[i].first_draw,
                s_pixel_track[i].batches, s_pixel_track[i].supported);
}

/* Raster path for batches that need the programmable transform pipeline:
 * runs the real vertex program per vertex (transform_vertex()) instead of
 * reading attribute 0 as if it were already screen space. A vertex that
 * cannot be placed (unimplemented opcode, or w too close to zero) drops
 * just its triangle, not the whole batch -- the geometric equivalent of
 * "this batch has not given us this vertex", not a reason to fall back to
 * skipping everything. */
static void raster_batch_vertex_program(void)
{
    float xy[3][2];
    float tex[2][3][4];
    float spec[3][4];
    uint32_t argb[3];
    uint32_t i;
    TextureSample sample[2];
    TextureSample *sample0 = NULL, *sample1 = NULL;
    float crossfade = 0.0f;
    int two_stage = 0;

    /* Exact state observed at SCDA draw #1, decoded with xemu's psh.c:
     * PROJECT2D on stage 0; T0*C0 (white) -> R0; final RGB=R0+V1 and A=R0.a.
     * Stages with any other program remain deliberately unsupported. */
    if (s_gpu.shader_stage_program == 0x00000001u
     && (s_gpu.combiner_control & 0xFFu) == 1u
     && s_gpu.combiner_rgb_in[0] == 0xC8C10000u
     && s_gpu.combiner_rgb_out[0] == 0x000000C0u
     && s_gpu.combiner_alpha_in[0] == 0xD8D11010u
     && s_gpu.combiner_alpha_out[0] == 0x000000C0u
     && s_gpu.combiner_factor[0][0] == 0xFFFFFFFFu
     && s_gpu.final_combiner[0] == 0x0000000Eu
     && s_gpu.final_combiner[1] == 0x00001C80u
     && ((s_gpu.tex[0].control0 >> 30) & 1u)
     && ((s_gpu.tex[0].format >> 8) & 0xFFu) == 0x12u
     && (s_gpu.tex[0].address & 7u) == 3u
     && ((s_gpu.tex[0].address >> 8) & 7u) == 3u) {
        uint32_t span;
        int linear;
        span = texture_level0_span(&s_gpu.tex[0], &sample[0].width,
                                   &sample[0].height, &sample[0].pitch, &linear);
        sample[0].base = linear && span
                       ? dma_resolve(s_gpu.tex[0].offset, span) : 0;
        if (sample[0].base && sample[0].width && sample[0].height
                && sample[0].pitch) {
            sample0 = &sample[0];
            s_gpu.batches_textured++;
        }
    } else if (s_gpu.shader_stage_program == 0x00000021u
     && (s_gpu.combiner_control & 0xFFu) == 2u
     && s_gpu.combiner_rgb_in[0] == 0x11C8C931u
     && s_gpu.combiner_rgb_out[0] == 0x00000C00u
     && s_gpu.combiner_alpha_in[0] == 0x11D8D931u
     && s_gpu.combiner_alpha_out[0] == 0x00000C00u
     && s_gpu.combiner_rgb_in[1] == 0xCCC10000u
     && s_gpu.combiner_rgb_out[1] == 0x000000C0u
     && s_gpu.combiner_alpha_in[1] == 0xDCD11010u
     && s_gpu.combiner_alpha_out[1] == 0x000000C0u
     && (s_gpu.combiner_factor[0][0] & 0x00FFFFFFu) == 0x00FFFFFFu
     && s_gpu.final_combiner[0] == 0x0000000Eu
     && s_gpu.final_combiner[1] == 0x00001C80u) {
        int ok = 1;
        two_stage = 1;
        crossfade = (float)(s_gpu.combiner_factor[0][0] >> 24) / 255.0f;
        for (i = 0; i < 2; i++) {
            uint32_t span;
            int linear;
            if (!((s_gpu.tex[i].control0 >> 30) & 1u)
             || ((s_gpu.tex[i].format >> 8) & 0xFFu) != 0x12u
             || (s_gpu.tex[i].address & 7u) != 3u
             || ((s_gpu.tex[i].address >> 8) & 7u) != 3u) {
                ok = 0;
                break;
            }
            span = texture_level0_span(&s_gpu.tex[i], &sample[i].width,
                                       &sample[i].height, &sample[i].pitch,
                                       &linear);
            sample[i].base = linear && span
                           ? dma_resolve(s_gpu.tex[i].offset, span) : 0;
            if (!sample[i].base || !sample[i].width || !sample[i].height
                    || !sample[i].pitch) {
                ok = 0;
                break;
            }
        }
        if (ok) {
            sample0 = &sample[0];
            sample1 = &sample[1];
            s_gpu.batches_textured++;
        }
    } else if (s_gpu.shader_stage_program != 0) {
        s_gpu.batches_pixel_program_unsupported++;
    }

    if (!sample0 && s_gpu.shader_stage_program != 0
            && (s_gpu.shader_stage_program == 0x00000001u || two_stage))
        s_gpu.batches_pixel_program_unsupported++;
    if (sample0) {
        s_last_draw_tex[0] = s_gpu.tex[0];
        s_last_draw_tex_count = 1;
        if (sample1) {
            s_last_draw_tex[1] = s_gpu.tex[1];
            s_last_draw_tex_count = 2;
        }
    }
    pixel_trace_batch(sample0 != NULL);

#define VTX(slot, index) transform_vertex((index), xy[slot], &argb[slot], \
                                           tex[0][slot], tex[1][slot], \
                                           spec[slot])

    switch (s_gpu.prim) {
    case NV_PRIM_TRIANGLES:
        for (i = 0; i + 2 < s_gpu.idx_count; i += 3)
            if (VTX(0, s_gpu.idx[i]) && VTX(1, s_gpu.idx[i+1])
             && VTX(2, s_gpu.idx[i+2]))
                raster_triangle(xy[0], xy[1], xy[2], argb[0], tex[0], tex[1],
                                spec, sample0, sample1, crossfade);
        break;
    case NV_PRIM_TRIANGLE_STRIP:
        for (i = 0; i + 2 < s_gpu.idx_count; i++)
            if (VTX(0, s_gpu.idx[i]) && VTX(1, s_gpu.idx[i+1])
             && VTX(2, s_gpu.idx[i+2]))
                raster_triangle(xy[0], xy[1], xy[2], argb[0], tex[0], tex[1],
                                spec, sample0, sample1, crossfade);
        break;
    case NV_PRIM_TRIANGLE_FAN:
        for (i = 1; i + 1 < s_gpu.idx_count; i++)
            if (VTX(0, s_gpu.idx[0]) && VTX(1, s_gpu.idx[i])
             && VTX(2, s_gpu.idx[i+1]))
                raster_triangle(xy[0], xy[1], xy[2], argb[0], tex[0], tex[1],
                                spec, sample0, sample1, crossfade);
        break;
    case NV_PRIM_QUADS:
        for (i = 0; i + 3 < s_gpu.idx_count; i += 4) {
            if (VTX(0, s_gpu.idx[i]) && VTX(1, s_gpu.idx[i+1])
             && VTX(2, s_gpu.idx[i+2]))
                raster_triangle(xy[0], xy[1], xy[2], argb[0], tex[0], tex[1],
                                spec, sample0, sample1, crossfade);
            if (VTX(0, s_gpu.idx[i]) && VTX(1, s_gpu.idx[i+2])
             && VTX(2, s_gpu.idx[i+3]))
                raster_triangle(xy[0], xy[1], xy[2], argb[0], tex[0], tex[1],
                                spec, sample0, sample1, crossfade);
        }
        break;
    case NV_PRIM_QUAD_STRIP:
        for (i = 0; i + 3 < s_gpu.idx_count; i += 2) {
            if (VTX(0, s_gpu.idx[i]) && VTX(1, s_gpu.idx[i+1])
             && VTX(2, s_gpu.idx[i+2]))
                raster_triangle(xy[0], xy[1], xy[2], argb[0], tex[0], tex[1],
                                spec, sample0, sample1, crossfade);
            if (VTX(0, s_gpu.idx[i+2]) && VTX(1, s_gpu.idx[i+1])
             && VTX(2, s_gpu.idx[i+3]))
                raster_triangle(xy[0], xy[1], xy[2], argb[0], tex[0], tex[1],
                                spec, sample0, sample1, crossfade);
        }
        break;
    default:
        break;                             /* points and lines: not yet */
    }
#undef VTX
}

static void raster_batch(void)
{
    if (s_gpu.idx_count < 3)
        return;

    /* The execution mode is authoritative. Normalised programmable inputs
     * such as UB_D3D commonly lie inside a 640x480 clip rectangle and would
     * therefore fool the screen-space heuristic if it ran first. */
    if (s_gpu.transform_mode == 2 && s_gpu.program_load_high > 0) {
        s_gpu.batches_vertex_program++;
        raster_batch_vertex_program();
    } else if (batch_is_screen_space()) {
        raster_batch_screen_space();
    } else {
        /* Fixed-function transform, or programmable mode with no program
         * loaded yet: neither is modelled here. */
        s_gpu.batches_untransformed++;
        return;
    }

    if (s_gpu.tris_drawn && (s_gpu.tris_drawn % 500) == 0)
        fprintf(stderr, "  [GPU] %u triangles rasterised\n", s_gpu.tris_drawn);
}

/* What a batch actually contains. Before anything can be rasterised, the
 * question is what space attribute 0 arrives in: a title running a vertex
 * program hands over object-space positions that mean nothing without running
 * it, while pre-transformed screen-space coordinates can be drawn directly. */
/* ── Change-triggered vertex-program chronology (Update 243) ─────────────
 *
 * Update 242 closed the c[58]/c[59] question for the ONE early program
 * Update 239-241 had been fixated on: it is genuinely silent on both this
 * recomp and xemu, not a capture bug. The open question now is different --
 * which is the first program/batch that SHOULD produce visible geometry at
 * all, out of however many distinct vertex programs the title loads over
 * time. Dumping every draw is unreadable; dumping at fixed draw counts (the
 * old dump_transform_program_if_due()) can straddle a program change or
 * miss one entirely. This instead hashes the currently active program
 * (rows + start + mode) and only prints when that hash changes, building a
 * one-line-per-program chronology plus one detailed snapshot at the moment
 * each new program first appears. Gated behind RECOMP_PROG_TRACE; costs one
 * hash comparison per draw when enabled, nothing when it is not.
 *
 * Per-vertex screen-space classification (valid: finite x/y; plausible:
 * within a generous margin around the active clip rectangle) is a
 * diagnostic-only duplicate of transform_vertex()'s own interpretation, run
 * for at most the first three batches of each new
 * program -- deliberately NOT reusing/altering transform_vertex() itself,
 * so the actual render path is untouched. */
static int ctrace_program_trace_enabled(void)
{
    static int e = -1;
    if (e < 0)
        e = getenv("RECOMP_PROG_TRACE") != NULL;
    return e;
}

typedef struct {
    int valid;
    int plausible;
    float screen_x, screen_y;
} VertexClass;

static VertexClass classify_vertex(uint32_t index)
{
    Vec4 vattr[NV2A_VSH_NUM_ATTRIBUTES];
    VshResult res;
    VertexClass c = { 0, 0, 0.0f, 0.0f };
    Vec4 pos;

    fetch_vattr(index, vattr);
    vsh_run_program(s_gpu.program_data, s_gpu.program_load_high,
                     s_gpu.program_start, s_gpu.vsh_constants, vattr,
                     &res, NULL);
    if (res.status == -2)
        return c;
    pos = res.oreg[VSH_OREG_POS];
    if (!isfinite(pos.x[0]) || !isfinite(pos.x[1]))
        return c;
    c.valid = 1;
    c.screen_x = pos.x[0];
    c.screen_y = pos.x[1];
    c.plausible = c.screen_x >= (float)s_gpu.clip_x - (float)s_gpu.clip_w
               && c.screen_x <= (float)(s_gpu.clip_x + 2 * s_gpu.clip_w)
               && c.screen_y >= (float)s_gpu.clip_y - (float)s_gpu.clip_h
               && c.screen_y <= (float)(s_gpu.clip_y + 2 * s_gpu.clip_h);
    return c;
}

static uint32_t hash_active_program(void)
{
    uint32_t h = 2166136261u;
    uint32_t i, j;
    for (i = 0; i < s_gpu.program_load_high; i++)
        for (j = 0; j < 4; j++) {
            h ^= s_gpu.program_data[i][j];
            h *= 16777619u;
        }
    h ^= s_gpu.program_start; h *= 16777619u;
    h ^= s_gpu.transform_mode; h *= 16777619u;
    return h;
}

static uint32_t texture_bytes_per_pixel(uint32_t color)
{
    switch (color) {
    case 0x00: case 0x01: case 0x13: case 0x19: case 0x1B: case 0x1F:
        return 1;
    case 0x02: case 0x03: case 0x04: case 0x05:
    case 0x10: case 0x11: case 0x17: case 0x1A: case 0x1C: case 0x1D:
    case 0x20: case 0x24: case 0x25: case 0x28: case 0x29: case 0x2C:
    case 0x30: case 0x31: case 0x35:
        return 2;
    case 0x06: case 0x07: case 0x2E: case 0x2F:
    case 0x3A: case 0x3B: case 0x3C: case 0x3F: case 0x40: case 0x41:
        return 4;
    default:
        return 0;
    }
}

/* Size of level zero only. This is enough to bounds-check the source sampled
 * by the first draw without claiming support for all mip and cube layouts. */
static uint32_t texture_level0_span(const TextureStage *t,
                                    uint32_t *width, uint32_t *height,
                                    uint32_t *pitch, int *linear)
{
    uint32_t color = (t->format >> 8) & 0xFFu;
    uint32_t bpp = texture_bytes_per_pixel(color);
    uint64_t bytes;

    *linear = color == 0x10 || color == 0x11 || color == 0x12
           || color == 0x13 || color == 0x17 || color == 0x1B
           || color == 0x1C || color == 0x1D || color == 0x1E
           || color == 0x1F || color == 0x20 || color == 0x24
           || color == 0x25 || color == 0x2E || color == 0x2F
           || color == 0x30 || color == 0x31 || color == 0x35
           || color == 0x3F || color == 0x40 || color == 0x41;
    if (*linear) {
        *width = t->image_rect >> 16;
        *height = t->image_rect & 0xFFFFu;
        *pitch = t->control1 >> 16;
        if (!*pitch && bpp)
            *pitch = *width * bpp;
        bytes = (uint64_t)*height * *pitch;
    } else {
        *width = 1u << ((t->format >> 20) & 0xFu);
        *height = 1u << ((t->format >> 24) & 0xFu);
        *pitch = 0;
        if (color == 0x0C)
            bytes = (uint64_t)((*width + 3) / 4) * ((*height + 3) / 4) * 8;
        else if (color == 0x0E || color == 0x0F)
            bytes = (uint64_t)((*width + 3) / 4) * ((*height + 3) / 4) * 16;
        else
            bytes = (uint64_t)*width * *height * bpp;
    }
    return bytes > UINT32_MAX ? 0 : (uint32_t)bytes;
}

static void dump_texture_sources(void)
{
    const char *prefix = getenv("RECOMP_TEX_SOURCE_DUMP");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    static uint32_t sequence;
    uint32_t stage;

    if (!prefix || !s_last_draw_tex_count)
        return;
    for (stage = 0; stage < s_last_draw_tex_count; stage++) {
        const TextureStage *t = &s_last_draw_tex[stage];
        uint32_t width, height, pitch, span, base, row_bytes, pad, filesz, y, x;
        int linear;
        uint8_t hdr[54] = {0};
        char path[MAX_PATH];
        FILE *f;

        if (!((t->control0 >> 30) & 1u)
                || ((t->format >> 8) & 0xFFu) != 0x12u)
            continue;
        span = texture_level0_span(t, &width, &height, &pitch, &linear);
        base = linear && span ? dma_resolve(t->offset, span) : 0;
        if (!base || !width || !height || !pitch)
            continue;
        row_bytes = width * 3;
        pad = (4 - (row_bytes & 3)) & 3;
        filesz = 54 + (row_bytes + pad) * height;
        hdr[0] = 'B'; hdr[1] = 'M';
        memcpy(hdr + 2, &filesz, 4);
        { uint32_t off = 54, dib = 40; uint16_t planes = 1, bits = 24;
          int32_t sw = (int32_t)width, sh = (int32_t)height;
          memcpy(hdr + 10, &off, 4); memcpy(hdr + 14, &dib, 4);
          memcpy(hdr + 18, &sw, 4); memcpy(hdr + 22, &sh, 4);
          memcpy(hdr + 26, &planes, 2); memcpy(hdr + 28, &bits, 2); }
        snprintf(path, sizeof path, "%s_s%u_%03u.bmp", prefix, stage, sequence);
        f = fopen(path, "wb");
        if (!f)
            continue;
        fwrite(hdr, 1, sizeof hdr, f);
        for (y = height; y-- > 0;) {
            const uint32_t *row = (const uint32_t *)(mem + base
                                  + (size_t)y * pitch);
            for (x = 0; x < width; x++) {
                uint8_t bgr[3] = { (uint8_t)row[x],
                                   (uint8_t)(row[x] >> 8),
                                   (uint8_t)(row[x] >> 16) };
                fwrite(bgr, 1, 3, f);
            }
            if (pad) fwrite("\0\0\0", 1, pad, f);
        }
        fclose(f);
        fprintf(stderr, "  [TEXTRACE] source dump: %s (%ux%u from 0x%08X)\n",
                path, width, height, base);
    }
    sequence++;
}

static void trace_first_draw_textures(void)
{
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    uint32_t stage;

    if (!getenv("RECOMP_TEX_TRACE") || s_gpu.draws != 1)
        return;
    fprintf(stderr, "  [TEXTRACE] texture state and shader outputs at draw #1\n");
    for (stage = 0; stage < NV_TEXTURE_STAGES; stage++) {
        const TextureStage *t = &s_gpu.tex[stage];
        uint32_t width, height, pitch;
        uint32_t span, base, n, i;
        int linear;
        span = texture_level0_span(t, &width, &height, &pitch, &linear);
        base = ((t->control0 >> 30) & 1u) && span
             ? dma_resolve(t->offset, span) : 0;
        fprintf(stderr,
                "  [TEXTRACE] stage%u enabled=%u offset=%08X format=%08X"
                " address=%08X control0=%08X control1=%08X filter=%08X"
                " rect=%08X color=%02X dim=%u %s %ux%u pitch=%u"
                " span=%u resolved=%08X\n",
                stage, (t->control0 >> 30) & 1u, t->offset, t->format,
                t->address, t->control0, t->control1, t->filter,
                t->image_rect, (t->format >> 8) & 0xFFu,
                (t->format >> 4) & 0xFu, linear ? "linear" : "swizzled",
                width, height, pitch, span, base);
        if (!base)
            continue;
        n = span < 32 ? span : 32;
        fprintf(stderr, "  [TEXTRACE]   source @%08X:", base);
        for (i = 0; i < n; i++)
            fprintf(stderr, " %02X", mem[base + i]);
        fprintf(stderr, "\n");
    }
    {
        uint32_t count = s_gpu.combiner_control & 0xFFu;
        uint32_t i;
        if (count > NV_COMBINER_STAGES)
            count = NV_COMBINER_STAGES;
        fprintf(stderr,
                "  [TEXTRACE] pixel shaderprog=%08X modes=%02X,%02X,%02X,%02X"
                " other=%08X combiner=%08X stages=%u flags=%06X"
                " final=%08X,%08X\n",
                s_gpu.shader_stage_program,
                (s_gpu.shader_stage_program >> 0) & 0x1Fu,
                (s_gpu.shader_stage_program >> 5) & 0x1Fu,
                (s_gpu.shader_stage_program >> 10) & 0x1Fu,
                (s_gpu.shader_stage_program >> 15) & 0x1Fu,
                s_gpu.shader_other_stage_input, s_gpu.combiner_control,
                count, s_gpu.combiner_control >> 8,
                s_gpu.final_combiner[0], s_gpu.final_combiner[1]);
        for (i = 0; i < count; i++)
            fprintf(stderr,
                    "  [TEXTRACE]   combiner%u rgb_in=%08X rgb_out=%08X"
                    " alpha_in=%08X alpha_out=%08X c0=%08X c1=%08X\n",
                    i, s_gpu.combiner_rgb_in[i], s_gpu.combiner_rgb_out[i],
                    s_gpu.combiner_alpha_in[i], s_gpu.combiner_alpha_out[i],
                    s_gpu.combiner_factor[0][i], s_gpu.combiner_factor[1][i]);
    }
    {
        uint32_t k;
        for (k = 0; k < s_gpu.idx_count && k < 4; k++) {
            Vec4 vattr[NV2A_VSH_NUM_ATTRIBUTES];
            VshResult res;
            uint32_t index = s_gpu.idx[k];
            int t;
            fetch_vattr(index, vattr);
            vsh_run_program(s_gpu.program_data, s_gpu.program_load_high,
                            s_gpu.program_start, s_gpu.vsh_constants, vattr,
                            &res, NULL);
            fprintf(stderr,
                    "  [TEXTRACE]   idx=%u status=%d oD1=(%.6g,%.6g,%.6g,%.6g)",
                    index, res.status,
                    res.oreg[VSH_OREG_D1].x[0], res.oreg[VSH_OREG_D1].x[1],
                    res.oreg[VSH_OREG_D1].x[2], res.oreg[VSH_OREG_D1].x[3]);
            for (t = 0; t < 4; t++) {
                const Vec4 *o = &res.oreg[VSH_OREG_T0 + t];
                fprintf(stderr, " oT%d=(%.6g,%.6g,%.6g,%.6g)", t,
                        o->x[0], o->x[1], o->x[2], o->x[3]);
            }
            fprintf(stderr, "\n");
        }
    }
    fflush(stderr);
}

#define PROG_TRACK_MAX 32
typedef struct {
    uint32_t hash;
    uint32_t first_draw;
    uint32_t batches, tris, verts;
    uint32_t sample_verts, sample_valid, sample_plausible;
    float screen_min_x, screen_max_x, screen_min_y, screen_max_y;
    char label[4];
} ProgramTrack;

static ProgramTrack s_prog_track[PROG_TRACK_MAX];
static int s_prog_track_count;
static int s_prog_current = -1;
static uint32_t s_prog_first_batches_left;

static void ptrace_flush_summary(int idx)
{
    ProgramTrack *p;
    float spread_x, spread_y;
    int collapsed;
    const char *verdict;

    if (idx < 0)
        return;
    p = &s_prog_track[idx];
    spread_x = p->screen_max_x - p->screen_min_x;
    spread_y = p->screen_max_y - p->screen_min_y;
    /* All sampled vertices landing within a hairline of each other (e.g.
     * every vertex collapsing to the exact same screen-space point) passes
     * the lenient per-vertex "plausible" check individually while still
     * being fully degenerate as a batch: no triangle it forms can have a
     * nonzero area. That is exactly the case the naive per-vertex check
     * cannot tell apart from real geometry, so report it separately. */
    collapsed = p->sample_valid > 1 && spread_x < 1e-4f && spread_y < 1e-4f;

    if (p->tris)
        verdict = "SICHTBAR";
    else if (collapsed)
        verdict = "degenerate -- alle Sample-Vertices kollabieren auf denselben Bildschirm-Punkt";
    else if (p->sample_plausible)
        verdict = "0 Dreiecke trotz plausibler, gestreuter Vertices -- Rasterizer/Clip/Viewport pruefen";
    else
        verdict = "degenerate/ausserhalb des Sichtvolumens";

    fprintf(stderr, "  [PTRACE] Programm %s (hash=0x%08X, ab Draw #%u): %u"
                    " Batches, %u Vertices gesamt (Sample erste Batches:"
                    " %u/%u valide, %u/%u plausibel, Screen x-spread=%.6f"
                    " y-spread=%.6f), %u Dreiecke rasterisiert -- %s\n",
            p->label, p->hash, p->first_draw, p->batches, p->verts,
            p->sample_valid, p->sample_verts, p->sample_plausible, p->sample_verts,
            spread_x, spread_y, p->tris, verdict);
    fflush(stderr);
}

static void ptrace_on_draw_start(void)
{
    trace_first_draw_textures();
    if (!ctrace_program_trace_enabled())
        return;
    {
        uint32_t h = hash_active_program();
        if (s_prog_current >= 0 && s_prog_track[s_prog_current].hash == h)
            return;                      /* unchanged: nothing to do */

        if (s_prog_current >= 0)
            ptrace_flush_summary(s_prog_current);

        /* A program that recurs later (e.g. re-bound after another object's
         * program was active) reuses its existing slot and tally rather
         * than starting a misleading duplicate entry. */
        {
            int i;
            for (i = 0; i < s_prog_track_count; i++) {
                if (s_prog_track[i].hash == h) {
                    s_prog_current = i;
                    fprintf(stderr, "  [PTRACE] zurueck zu Programm %s"
                                    " (hash=0x%08X) bei Draw #%u\n",
                            s_prog_track[i].label, h, s_gpu.draws);
                    fflush(stderr);
                    return;
                }
            }
        }

        if (s_prog_track_count >= PROG_TRACK_MAX) {
            static int said;
            if (!said++)
                fprintf(stderr, "  [PTRACE] Programm-Tabelle voll (%d) --"
                                " weitere Programmwechsel werden nicht mehr"
                                " einzeln verfolgt\n", PROG_TRACK_MAX);
            s_prog_current = -1;
            return;
        }

        {
            int idx = s_prog_track_count++;
            ProgramTrack *p = &s_prog_track[idx];
            uint32_t r, c;
            memset(p, 0, sizeof *p);
            p->hash = h;
            p->first_draw = s_gpu.draws;
            p->screen_min_x = p->screen_min_y = 1e30f;
            p->screen_max_x = p->screen_max_y = -1e30f;
            p->label[0] = (char)('A' + (idx % 26));
            p->label[1] = 0;
            s_prog_current = idx;
            s_prog_first_batches_left = 3;

            fprintf(stderr, "  [PTRACE] NEUES PROGRAMM %s bei Draw #%u:"
                            " mode=%u start=%u rows=%u\n",
                    p->label, s_gpu.draws, s_gpu.transform_mode,
                    s_gpu.program_start, s_gpu.program_load_high);
            for (r = 0; r < s_gpu.program_load_high
                     && r < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH; r++)
                fprintf(stderr, "  [PTRACE]   prog[%3u] = %08X %08X %08X %08X\n",
                        r, s_gpu.program_data[r][0], s_gpu.program_data[r][1],
                        s_gpu.program_data[r][2], s_gpu.program_data[r][3]);
            for (c = 0; c < NV2A_VERTEXSHADER_CONSTANTS; c++) {
                if (!s_gpu.const_written[c])
                    continue;
                fprintf(stderr, "  [PTRACE]   const[%3u] = %08X %08X %08X %08X\n",
                        c, s_gpu.vsh_constants[c][0], s_gpu.vsh_constants[c][1],
                        s_gpu.vsh_constants[c][2], s_gpu.vsh_constants[c][3]);
            }
            {
                float v[4];
                uint32_t k;
                for (k = 0; k < s_gpu.idx_count && k < 2; k++)
                    if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[k], v))
                        fprintf(stderr, "  [PTRACE]   v0[idx=%u] = %.3f %.3f"
                                        " %.3f %.3f\n",
                                s_gpu.idx[k], v[0], v[1], v[2], v[3]);
            }
            fprintf(stderr, "  [PTRACE]   surface: clip %ux%u+%u+%u pitch=%u"
                            " fmt=0x%X color_off=0x%08X\n",
                    s_gpu.clip_w, s_gpu.clip_h, s_gpu.clip_x, s_gpu.clip_y,
                    s_gpu.pitch, s_gpu.format, s_gpu.color_offset);
            fflush(stderr);
        }
    }
}

/* Called once per draw, after raster_batch() -- tallies batch/vertex/
 * triangle counts for the currently active program (cheap: additions only)
 * and, for the first three batches of a new program, runs the diagnostic
 * per-vertex classification above. */
static void ptrace_on_draw_end(uint32_t tris_before)
{
    ProgramTrack *p;
    if (!ctrace_program_trace_enabled() || s_prog_current < 0)
        return;
    p = &s_prog_track[s_prog_current];
    p->batches++;
    p->verts += s_gpu.idx_count;
    p->tris += s_gpu.tris_drawn - tris_before;

    if (s_prog_first_batches_left > 0) {
        uint32_t k;
        s_prog_first_batches_left--;
        for (k = 0; k < s_gpu.idx_count; k++) {
            VertexClass vc = classify_vertex(s_gpu.idx[k]);
            p->sample_verts++;
            if (vc.valid) {
                p->sample_valid++;
                if (vc.screen_x < p->screen_min_x) p->screen_min_x = vc.screen_x;
                if (vc.screen_x > p->screen_max_x) p->screen_max_x = vc.screen_x;
                if (vc.screen_y < p->screen_min_y) p->screen_min_y = vc.screen_y;
                if (vc.screen_y > p->screen_max_y) p->screen_max_y = vc.screen_y;
            }
            if (vc.plausible) p->sample_plausible++;
        }
    }
}

static void draw_primitive(void)
{
    float v[4];
    uint32_t i;
    uint32_t tris_before;

    if (!s_gpu.prim || !s_gpu.idx_count)
        return;
    s_gpu.draws++;
    if (s_gpu.draws <= 5 || (s_gpu.draws % 200) == 0)
        ctrace_draw(s_gpu.draws);
    if ((s_gpu.draws % 200) == 0)
        fprintf(stderr, "  [GPU] draw #%u\n", s_gpu.draws);
    s_gpu.verts += s_gpu.idx_count;

    ptrace_on_draw_start();
    tris_before = s_gpu.tris_drawn;

    /* How many batches carry coordinates at all, and what range they span.
     * A pipeline that decodes perfectly and draws nothing is indistinguishable
     * from one that never ran, unless the vertices themselves are measured. */
    {
        float p[4];
        if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[0], p)) {
            if (p[0] != 0.0f || p[1] != 0.0f || p[2] != 0.0f) {
                s_gpu.nonzero_draws++;
                if (p[0] < s_gpu.min_x) s_gpu.min_x = p[0];
                if (p[0] > s_gpu.max_x) s_gpu.max_x = p[0];
                if (p[1] < s_gpu.min_y) s_gpu.min_y = p[1];
                if (p[1] > s_gpu.max_y) s_gpu.max_y = p[1];
            }
        }
    }

    raster_batch();
    ptrace_on_draw_end(tris_before);

    if (getenv("RECOMP_PB_EXEC_VERBOSE"))
        dump_transform_program_if_due();

    if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
        static int shown;
        if (shown++ < 6) {
            fprintf(stderr, "  [GPU] prim %u, %u indices, pos attr:"
                            " off 0x%08X type %u size %u stride %u\n",
                    s_gpu.prim, s_gpu.idx_count, s_gpu.attr[0].offset,
                    s_gpu.attr[0].type, s_gpu.attr[0].size, s_gpu.attr[0].stride);
            {
                /* Every attribute the batch has, not just position. If the
                 * other streams carry data and position does not, the problem
                 * is one buffer rather than the whole vertex path. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t a, k;
                for (a = 0; a < NV_VERTEX_ATTRS; a++) {
                    const VertexAttr *at = &s_gpu.attr[a];
                    uint32_t nz = 0;
                    if (!at->offset || !at->size)
                        continue;
                    for (k = 0; k < 64; k++)
                        if (mem[at->offset + k]) nz++;
                    fprintf(stderr, "  [GPU]   attr%-2u off 0x%08X type %u"
                                    " size %u stride %-3u  %u/64 bytes set\n",
                            a, at->offset, at->type, at->size, at->stride, nz);
                }
            }
            {
                /* Raw bytes at the array, in case the values read as zero:
                 * that looks the same whether the offset is wrong or the
                 * buffer genuinely has not been filled yet. */
                const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
                uint32_t k;
                fprintf(stderr, "  [GPU]   bytes @0x%08X:", s_gpu.attr[0].offset);
                for (k = 0; k < 32; k++)
                    fprintf(stderr, " %02X", mem[s_gpu.attr[0].offset + k]);
                fprintf(stderr, "\n");
                fprintf(stderr, "  [GPU]   indices:");
                for (k = 0; k < s_gpu.idx_count && k < 8; k++)
                    fprintf(stderr, " %u", s_gpu.idx[k]);
                fprintf(stderr, "\n");
            }
            for (i = 0; i < s_gpu.idx_count && i < 3; i++) {
                if (fetch_attr(&s_gpu.attr[0], s_gpu.idx[i], v))
                    fprintf(stderr, "  [GPU]   v[%u] = %.3f %.3f %.3f %.3f\n",
                            s_gpu.idx[i], v[0], v[1], v[2], v[3]);
            }
        }
    }
}

void nv2a_pb_exec_method(uint32_t subch, uint32_t method, uint32_t param,
                          uint32_t va)
{
    static int inited;
    if (!inited) {
        inited = 1;
        s_gpu.min_x = s_gpu.min_y = 1e30f;
        s_gpu.max_x = s_gpu.max_y = -1e30f;
    }
    /* Bring-up: the first parameters each surface method carries. A wrong
     * pitch or clip is indistinguishable from a method never arriving unless
     * the values are visible. */
    if (getenv("RECOMP_PB_EXEC_VERBOSE")) {
        static int shown[8];
        int slot = -1;
        switch (method) {
        case NV097_SET_SURFACE_CLIP_HORIZONTAL: slot = 0; break;
        case NV097_SET_SURFACE_CLIP_VERTICAL:   slot = 1; break;
        case NV097_SET_SURFACE_FORMAT:          slot = 2; break;
        case NV097_SET_SURFACE_PITCH:           slot = 3; break;
        case NV097_SET_SURFACE_COLOR_OFFSET:    slot = 4; break;
        case NV097_SET_COLOR_CLEAR_VALUE:       slot = 5; break;
        case NV097_CLEAR_SURFACE:               slot = 6; break;
        default: break;
        }
        if (slot >= 0 && shown[slot]++ < 4)
            fprintf(stderr, "  [GPU] subch %u method 0x%04X param 0x%08X\n",
                    subch, method, param);
    }

    if (subch != 0) {                      /* 3D class lives on subchannel 0 */
        note_unhandled(method);
        return;
    }
    switch (method) {
    case NV097_SET_SURFACE_CLIP_HORIZONTAL:
        s_gpu.clip_x = param & 0xFFFF;
        s_gpu.clip_w = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_CLIP_VERTICAL:
        s_gpu.clip_y = param & 0xFFFF;
        s_gpu.clip_h = (param >> 16) & 0xFFFF;
        break;
    case NV097_SET_SURFACE_FORMAT:
        s_gpu.format = param;
        break;
    case NV097_SET_SURFACE_PITCH:
        s_gpu.pitch = param & 0xFFFF;      /* colour pitch; zeta is the top half */
        break;
    case NV097_SET_SURFACE_COLOR_OFFSET:
        s_gpu.color_offset = param;
        s_gpu.color_base = dma_resolve(param, 1);
        break;
    case NV097_SET_COLOR_CLEAR_VALUE:
        s_gpu.clear_color = param;
        break;
    case NV097_CLEAR_SURFACE:
        clear_surface(param);
        break;

    case NV097_SET_BEGIN_END:
        if (param) {
            s_gpu.prim = param;
            s_gpu.idx_count = 0;
        } else {
            draw_primitive();
            s_gpu.prim = 0;
        }
        break;

    case NV097_ARRAY_ELEMENT16:
        /* Two 16-bit indices per parameter word. */
        if (s_gpu.prim && s_gpu.idx_count + 2 <= NV_MAX_INDICES) {
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param & 0xFFFF);
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(param >> 16);
        }
        break;
    case NV097_DRAW_ARRAYS: {
        uint32_t start = param & 0x00FFFFFFu;
        uint32_t count = ((param >> 24) & 0xFFu) + 1u;
        uint32_t i;

        if (!s_gpu.prim)
            break;
        for (i = 0; i < count && s_gpu.idx_count < NV_MAX_INDICES; i++)
            s_gpu.idx[s_gpu.idx_count++] = (uint16_t)(start + i);
        break;
    }

    case NV097_SET_TRANSFORM_EXECUTION_MODE:
        s_gpu.transform_mode = param & 0x3u;
        break;
    case NV097_SET_TRANSFORM_PROGRAM_LOAD:
        s_gpu.program_load = param < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH
                            ? param : NV2A_MAX_TRANSFORM_PROGRAM_LENGTH - 1;
        break;
    case NV097_SET_TRANSFORM_PROGRAM_START:
        s_gpu.program_start = param;
        ctrace_program_start(param, va);
        break;
    case NV097_SET_TRANSFORM_CONSTANT_LOAD:
        s_gpu.const_load = param < NV2A_VERTEXSHADER_CONSTANTS
                          ? param : NV2A_VERTEXSHADER_CONSTANTS - 1;
        break;
    case NV097_SET_COMBINER_SPECULAR_FOG_CW0:
        s_gpu.final_combiner[0] = param;
        break;
    case NV097_SET_COMBINER_SPECULAR_FOG_CW1:
        s_gpu.final_combiner[1] = param;
        break;
    case NV097_SET_COMBINER_CONTROL:
        s_gpu.combiner_control = param;
        break;
    case NV097_SET_SHADER_STAGE_PROGRAM:
        s_gpu.shader_stage_program = param;
        break;
    case NV097_SET_SHADER_OTHER_STAGE_INPUT:
        s_gpu.shader_other_stage_input = param;
        break;
    case NV097_SET_ALPHA_TEST_ENABLE:
        s_gpu.alpha_test_enable = param;
        break;
    case NV097_SET_BLEND_ENABLE:
        s_gpu.blend_enable = param;
        break;
    case NV097_SET_ALPHA_FUNC:
        s_gpu.alpha_func = param;
        break;
    case NV097_SET_ALPHA_REF:
        s_gpu.alpha_ref = param;
        break;
    case NV097_SET_BLEND_FUNC_SFACTOR:
        s_gpu.blend_sfactor = param;
        break;
    case NV097_SET_BLEND_FUNC_DFACTOR:
        s_gpu.blend_dfactor = param;
        break;
    case NV097_SET_BLEND_COLOR:
        s_gpu.blend_color = param;
        break;
    case NV097_SET_BLEND_EQUATION:
        s_gpu.blend_equation = param;
        break;
    case NV097_SET_COLOR_MASK:
        s_gpu.color_mask = param;
        break;

    default:
        /* SET_TRANSFORM_PROGRAM/CONSTANT are increasing-method uploads: the
         * method offset (checked directly against xemu's DEF_METHOD_INC, not
         * assumed) is this call's running word index within the current
         * pushbuffer command, not an absolute row -- the row is whatever
         * program_load/const_load already holds, advanced by one only after
         * a complete four-word row. Capture only; nothing here is executed
         * as a vertex program yet. */
        if (method >= NV097_SET_VIEWPORT_OFFSET
                && method < NV097_SET_VIEWPORT_OFFSET + 0x10) {
            uint32_t slot = (method - NV097_SET_VIEWPORT_OFFSET) / 4;
            ctrace_const_write(59, slot, param, subch, method, va);
            s_gpu.vsh_constants[59][slot] = param;
            s_gpu.const_written[59] = 1;
        } else if (method >= NV097_SET_VIEWPORT_SCALE
                && method < NV097_SET_VIEWPORT_SCALE + 0x10) {
            uint32_t slot = (method - NV097_SET_VIEWPORT_SCALE) / 4;
            ctrace_const_write(58, slot, param, subch, method, va);
            s_gpu.vsh_constants[58][slot] = param;
            s_gpu.const_written[58] = 1;
        } else if (method >= NV097_SET_TRANSFORM_PROGRAM
                && method < NV097_SET_TRANSFORM_PROGRAM + 0x80) {
            uint32_t word = (method - NV097_SET_TRANSFORM_PROGRAM) / 4;
            uint32_t line = word % 4;
            if (s_gpu.program_load < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH) {
                s_gpu.program_data[s_gpu.program_load][line] = param;
                if (s_gpu.program_load + 1 > s_gpu.program_load_high)
                    s_gpu.program_load_high = s_gpu.program_load + 1;
            }
            if (line == 3 && s_gpu.program_load + 1 < NV2A_MAX_TRANSFORM_PROGRAM_LENGTH)
                s_gpu.program_load++;
        } else if (method >= NV097_SET_TRANSFORM_CONSTANT
                && method < NV097_SET_TRANSFORM_CONSTANT + 0x80) {
            uint32_t word = (method - NV097_SET_TRANSFORM_CONSTANT) / 4;
            uint32_t line = word % 4;
            if (s_gpu.const_load < NV2A_VERTEXSHADER_CONSTANTS) {
                ctrace_const_write(s_gpu.const_load, line, param, subch, method, va);
                s_gpu.vsh_constants[s_gpu.const_load][line] = param;
                s_gpu.const_written[s_gpu.const_load] = 1;
            }
            if (line == 3 && s_gpu.const_load + 1 < NV2A_VERTEXSHADER_CONSTANTS)
                s_gpu.const_load++;
        } else if (method >= NV097_SET_COMBINER_ALPHA_ICW
                && method < NV097_SET_COMBINER_ALPHA_ICW
                              + NV_COMBINER_STAGES * 4) {
            s_gpu.combiner_alpha_in[
                (method - NV097_SET_COMBINER_ALPHA_ICW) / 4] = param;
        } else if (method >= NV097_SET_COMBINER_FACTOR0
                && method < NV097_SET_COMBINER_FACTOR0
                              + NV_COMBINER_STAGES * 4) {
            s_gpu.combiner_factor[0][
                (method - NV097_SET_COMBINER_FACTOR0) / 4] = param;
        } else if (method >= NV097_SET_COMBINER_FACTOR1
                && method < NV097_SET_COMBINER_FACTOR1
                              + NV_COMBINER_STAGES * 4) {
            s_gpu.combiner_factor[1][
                (method - NV097_SET_COMBINER_FACTOR1) / 4] = param;
        } else if (method >= NV097_SET_COMBINER_ALPHA_OCW
                && method < NV097_SET_COMBINER_ALPHA_OCW
                              + NV_COMBINER_STAGES * 4) {
            s_gpu.combiner_alpha_out[
                (method - NV097_SET_COMBINER_ALPHA_OCW) / 4] = param;
        } else if (method >= NV097_SET_COMBINER_COLOR_ICW
                && method < NV097_SET_COMBINER_COLOR_ICW
                              + NV_COMBINER_STAGES * 4) {
            s_gpu.combiner_rgb_in[
                (method - NV097_SET_COMBINER_COLOR_ICW) / 4] = param;
        } else if (method >= NV097_SET_COMBINER_COLOR_OCW
                && method < NV097_SET_COMBINER_COLOR_OCW
                              + NV_COMBINER_STAGES * 4) {
            s_gpu.combiner_rgb_out[
                (method - NV097_SET_COMBINER_COLOR_OCW) / 4] = param;
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_OFFSET
                && method < NV097_SET_VERTEX_DATA_ARRAY_OFFSET + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[
                (method - NV097_SET_VERTEX_DATA_ARRAY_OFFSET) / 4];
            a->dma_offset = param;
            a->offset = dma_resolve(param, 1);
        } else if (method >= NV097_SET_VERTEX_DATA_ARRAY_FORMAT
                && method < NV097_SET_VERTEX_DATA_ARRAY_FORMAT + NV_VERTEX_ATTRS * 4) {
            VertexAttr *a = &s_gpu.attr[(method - NV097_SET_VERTEX_DATA_ARRAY_FORMAT) / 4];
            a->type   =  param        & 0x0F;
            a->size   = (param >> 4)  & 0x0F;
            a->stride = (param >> 8)  & 0xFF;
        } else if (method >= NV097_SET_TEXTURE_OFFSET
                && method < NV097_SET_TEXTURE_OFFSET
                              + NV_TEXTURE_STAGE_STRIDE * NV_TEXTURE_STAGES) {
            uint32_t rel = method - NV097_SET_TEXTURE_OFFSET;
            uint32_t stage = rel / NV_TEXTURE_STAGE_STRIDE;
            uint32_t reg = rel % NV_TEXTURE_STAGE_STRIDE;
            TextureStage *t = &s_gpu.tex[stage];
            switch (reg) {
            case 0x00: t->offset = param; break;
            case 0x04: t->format = param; break;
            case 0x08: t->address = param; break;
            case 0x0C: t->control0 = param; break;
            case 0x10: t->control1 = param; break;
            case 0x14: t->filter = param; break;
            case 0x1C: t->image_rect = param; break;
            default: note_unhandled(method); break;
            }
        } else {
            note_unhandled(method);
        }
        break;
    }
}

/* Find where the title actually wrote its quad.
 *
 * The GPU is pointed at a buffer that stays zero, which says the data went
 * somewhere else -- and the only way to find somewhere else is to look for the
 * data. A screen-space quad for a 640x480 target contains 640.0f and 480.0f as
 * floats, which is a distinctive enough pair to search guest RAM for. Whatever
 * address that turns up is where the title's writes are landing, and the
 * difference from the programmed offset is the bug. */
static void find_quad_vertices(void)
{
    const uint32_t W = 0x44200000u;   /* 640.0f */
    const uint32_t H = 0x43F00000u;   /* 480.0f */
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for 640.0f/480.0f pairs...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 8 && hits < 12; i++) {
        if (ram[i] != W && ram[i] != H)
            continue;
        /* Both values within a few words of each other: a lone 640.0f is
         * common, the pair much less so. */
        {
            int has_w = 0, has_h = 0;
            uint32_t k;
            for (k = 0; k < 8; k++) {
                if (ram[i + k] == W) has_w = 1;
                if (ram[i + k] == H) has_h = 1;
            }
            if (!has_w || !has_h)
                continue;
        }
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 8; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 8;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none found -- the quad is not in RAM in"
                        " that form\n");
    fflush(stderr);
}

/* Locate NaN-filled transform matrices in guest RAM.
 *
 * A matrix arriving as NaN says the maths went wrong somewhere upstream, and
 * the only way to find where is to find the matrix and watch who writes it.
 * Three consecutive real-indefinite values is a distinctive enough signature:
 * ordinary data does not contain runs of 0xFFC00000. */
static void find_nan_matrices(void)
{
    const uint32_t NAN_NEG = 0xFFC00000u;
    const uint32_t *ram = (const uint32_t *)xbox_GetMemoryOffset();
    uint32_t i, hits = 0;

    fprintf(stderr, "[GPU] searching guest RAM for NaN matrices...\n");
    for (i = 0x1000 / 4; i < (0x04000000u / 4) - 20 && hits < 10; i++) {
        if (ram[i] != NAN_NEG || ram[i + 1] != NAN_NEG || ram[i + 2] != NAN_NEG)
            continue;
        hits++;
        fprintf(stderr, "  [GPU]   0x%08X:", i * 4);
        {
            uint32_t k;
            for (k = 0; k < 16; k++)
                fprintf(stderr, " %08X", ram[i + k]);
        }
        fprintf(stderr, "\n");
        i += 16;
    }
    if (!hits)
        fprintf(stderr, "  [GPU]   none in RAM -- the NaNs are computed into"
                        " registers, not stored\n");
    fflush(stderr);
}

/* Print guest dwords named by RECOMP_PEEK, as hex and as float.
 *
 * Chasing a value backwards means reading it, and a value that is only wrong
 * for one frame in a thousand cannot be caught by stopping. Both
 * interpretations are printed because the question is usually "is this a
 * pointer or a number", and guessing wrong costs a run. */
static void peek_addresses(void)
{
    const char *spec = getenv("RECOMP_PEEK");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256];
    char *tok, *ctx = NULL;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;
    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t va = (uint32_t)strtoul(tok, NULL, 0);
        uint32_t v;
        float f;
        if (va < 0x1000u || va >= 0x04000000u)
            continue;
        v = *(const uint32_t *)(mem + va);
        memcpy(&f, &v, 4);
        fprintf(stderr, "  [PEEK] 0x%08X = %08X  (%g)\n", va, v, f);
    }
    fflush(stderr);
}

/* Walk a pointer chain and print every step.
 *
 * RECOMP_PEEK_CHAIN="0x1315A8,8,0x10,0" starts at that address, and for each
 * offset dereferences the current pointer and adds it. Following a chain by
 * hand costs one run per level; this costs one run for the whole chain, and
 * prints where it goes wrong when a level is null.
 */
static void peek_chain(void)
{
    const char *spec = getenv("RECOMP_PEEK_CHAIN");
    const uint8_t *mem = (const uint8_t *)xbox_GetMemoryOffset();
    char buf[256], *tok, *ctx = NULL;
    uint32_t cur = 0;
    int step = 0;

    if (!spec)
        return;
    strncpy(buf, spec, sizeof(buf) - 1);
    buf[sizeof(buf) - 1] = 0;

    for (tok = strtok_s(buf, ",", &ctx); tok; tok = strtok_s(NULL, ",", &ctx)) {
        uint32_t off = (uint32_t)strtoul(tok, NULL, 0);
        if (step == 0) {
            cur = off;
            fprintf(stderr, "  [CHAIN] start 0x%08X\n", cur);
        } else {
            if (cur < 0x1000u || cur + 4 >= 0x04000000u) {
                fprintf(stderr, "  [CHAIN] step %d: 0x%08X is not a guest"
                                " pointer -- chain ends\n", step, cur);
                return;
            }
            cur = *(const uint32_t *)(mem + cur) + off;
            fprintf(stderr, "  [CHAIN] step %d: deref +0x%X -> 0x%08X\n",
                    step, off, cur);
        }
        step++;
    }
    if (cur >= 0x1000u && cur + 4 < 0x04000000u)
        fprintf(stderr, "  [CHAIN] final value at 0x%08X = 0x%08X\n",
                cur, *(const uint32_t *)(mem + cur));
    fflush(stderr);
}

void nv2a_pb_exec_report(void)
{
    if (ctrace_program_trace_enabled() && s_prog_current >= 0)
        ptrace_flush_summary(s_prog_current);
    pixel_trace_report();
    dump_texture_sources();
    peek_addresses();
    peek_chain();
    if (getenv("RECOMP_FIND_NAN")) {
        /* Every report, not once: the matrix is fine early on and only turns
         * to NaN later, so a single scan at startup finds nothing and says
         * nothing. */
        find_nan_matrices();
    }
    if (getenv("RECOMP_FIND_QUAD")) {
        static int done;
        if (!done) { done = 1; find_quad_vertices(); }
    }
    int i, j;

    fprintf(stderr, "[GPU] surface 0x%08X pitch %u clip %ux%u+%u+%u"
                    " clears %u | %u unhandled methods (%d distinct)\n",
            s_gpu.color_offset, s_gpu.pitch, s_gpu.clip_w, s_gpu.clip_h,
            s_gpu.clip_x, s_gpu.clip_y, s_gpu.clears,
            s_gpu.unhandled_total, s_unhandled_count);
    fprintf(stderr, "[GPU] draws %u (%u with coordinates), %u indices;"
                    " x %.1f..%.1f  y %.1f..%.1f\n",
            s_gpu.draws, s_gpu.nonzero_draws, s_gpu.verts,
            s_gpu.min_x, s_gpu.max_x, s_gpu.min_y, s_gpu.max_y);
    /* One picture per report rather than per clear: a title clears hundreds of
     * times a second and nobody wants that many files. */
    dump_surface_bmp();

    /* Drawn and skipped separately: "nothing appeared" and "every batch needed
     * a vertex program we do not run" look identical on screen, and only one
     * of them means the rasteriser is broken. */
    fprintf(stderr, "[GPU] rasterised %u triangles (%u batches via the vertex-"
                    "program interpreter); %u batches skipped (fixed-function"
                    " or no program loaded), %u triangles fully off-surface;"
                    " %u batches textured, %u unsupported pixel programs\n",
            s_gpu.tris_drawn, s_gpu.batches_vertex_program,
            s_gpu.batches_untransformed, s_gpu.tris_skipped_offscreen,
            s_gpu.batches_textured, s_gpu.batches_pixel_program_unsupported);

    /* Top ten by frequency: selection sort over a small table, once every few
     * seconds, is not worth a better algorithm. */
    for (i = 0; i < 10 && i < s_unhandled_count; i++) {
        int best = i;
        for (j = i + 1; j < s_unhandled_count; j++)
            if (s_unhandled[j].count > s_unhandled[best].count)
                best = j;
        if (best != i) {
            PbUnhandled t = s_unhandled[i];
            s_unhandled[i] = s_unhandled[best];
            s_unhandled[best] = t;
        }
        fprintf(stderr, "  [GPU]   0x%04X x%u\n",
                s_unhandled[i].method, s_unhandled[i].count);
    }
    fflush(stderr);
}
