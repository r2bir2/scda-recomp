/*
 * Geforce NV2A PGRAPH OpenGL Renderer
 *
 * Copyright (c) 2012 espes
 * Copyright (c) 2015 Jannik Vogel
 * Copyright (c) 2018-2024 Matt Borgerson
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 */

#include "qemu/fast-hash.h"
#include "hw/xbox/nv2a/nv2a_int.h"
#include "debug.h"
#include "renderer.h"

/* xboxrecomp diagnostic (Update 358): F11 already preserves sixty displayed
 * frames.  While that bounded capture is active, retain the VSH state which
 * produced each draw.  This distinguishes a real animation/CPU-constant jump
 * from a renderer-side texture or present artifact without logging ordinary
 * gameplay.  It is deliberately observational and capped per F11 burst. */
extern unsigned xbox_FramebufferBurstGeneration(void);

static uint64_t recomp_vsh_hash_used(const PGRAPHState *pg,
                                     const ShaderBinding *binding)
{
    uint64_t h = UINT64_C(1469598103934665603);
    for (unsigned i = 0; i < NV2A_VERTEXSHADER_CONSTANTS; i++) {
        if (binding->vsh_constant_loc[i] == -1) {
            continue;
        }
        const uint8_t *p = (const uint8_t *)&pg->vsh_constants[i];
        size_t bytes = sizeof(pg->vsh_constants[i]);
        h ^= i;
        h *= UINT64_C(1099511628211);
        while (bytes--) {
            h ^= *p++;
            h *= UINT64_C(1099511628211);
        }
    }
    return h;
}

static uint64_t recomp_vsh_hash_program(const ShaderState *state)
{
    uint64_t h = UINT64_C(1469598103934665603);
    const uint8_t *p = (const uint8_t *)state->program_data;
    size_t bytes = state->program_length * sizeof(state->program_data[0]);
    while (bytes--) {
        h ^= *p++;
        h *= UINT64_C(1099511628211);
    }
    return h;
}

/* The 107-token SCDA skinning program does ARL(v10.xyzw * c131), where
 * c131 is 768.  Read the actual indexed v10 bytes during the existing F11
 * capture.  This answers whether dynamic c[A0+132..134] can reach c183/184;
 * a uniform location alone cannot answer that question.  Observational. */
static void recomp_trace_vsh_bone_indices(NV2AState *d, const PGRAPHState *pg,
                                          unsigned record)
{
    const VertexAttribute *attr = &pg->vertex_attributes[10];
    if (!pg->inline_elements_length || attr->size != 1 || !attr->count ||
        attr->count > 4 || !attr->stride) {
        fprintf(stderr, "[GPU-VSH-BONE] n=%u unavailable format=%u size=%u "
                "count=%u stride=%u elements=%u\n", record, attr->format,
                attr->size, attr->count, attr->stride,
                pg->inline_elements_length);
        return;
    }

    hwaddr dma_len = 0;
    uint8_t *base = (uint8_t *)nv_dma_map(
        d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a, &dma_len);
    unsigned lo = 255, hi = 0, ge17 = 0, examined = 0, invalid = 0;
    for (unsigned i = 0; i < pg->inline_elements_length; i++) {
        uint64_t address = (uint64_t)attr->offset +
                           (uint64_t)pg->inline_elements[i] * attr->stride;
        if (!base || address > dma_len || attr->count > dma_len - address) {
            invalid++;
            continue;
        }
        const uint8_t *value = base + address;
        for (unsigned c = 0; c < attr->count; c++) {
            if (value[c] < lo) lo = value[c];
            if (value[c] > hi) hi = value[c];
            if (value[c] >= 17) ge17++;
            examined++;
        }
    }
    fprintf(stderr, "[GPU-VSH-BONE] n=%u format=%u count=%u stride=%u "
            "offset=%08" PRIx64 " bytes=%u raw_min=%u raw_max=%u ge17=%u "
            "invalid=%u\n", record, attr->format, attr->count,
            attr->stride, (uint64_t)attr->offset, examined, lo, hi, ge17,
            invalid);
}

static void recomp_trace_vsh_draw(NV2AState *d, PGRAPHGLState *r,
                                  const char *kind, unsigned count)
{
    static unsigned last_burst, records;
    static uint32_t previous[NV2A_VERTEXSHADER_CONSTANTS][4];
    static bool have_previous;
    PGRAPHState *pg = &d->pgraph;
    unsigned burst = xbox_FramebufferBurstGeneration();

    if (!burst || !r->shader_binding->state.vertex_program)
        return;
    if (burst != last_burst) {
        last_burst = burst;
        records = 0;
        have_previous = false;
        fprintf(stderr, "[GPU-VSH] F11 burst %u begins\n", burst);
    }
    unsigned length = r->shader_binding->state.program_length;
    /* The first burst isolates these two long programs as the stable,
     * repeated character/model pass.  Keep the next burst bounded to that
     * pass, rather than consuming its budget on the static world draws. */
    if (length != 107 && length != 114) {
        return;
    }
    /* RECOMP_VSH_477_ONLY=1 (Update 385): keep only the count=477 skinning
     * draw, one per frame, with a timestamp.  The full trace slows the game
     * to ~8 frames/s, which may itself drive the arm-bone oscillation; this
     * light variant measures it at normal speed.  Observational. */
    static int only477 = -1;
    if (only477 < 0) {
        const char *e = getenv("RECOMP_VSH_477_ONLY");
        only477 = e && *e == '1';
    }
    if (only477 && (length != 107 || count != 477)) {
        return;
    }
    if (records++ >= (only477 ? 1200u : 3000u))
        return;
    if (only477) {
        static int64_t t0;
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        if (records == 1) {
            t0 = now;
        }
        fprintf(stderr, "[GPU-VSH-T] n=%u frame=%u t_us=%" PRId64 "\n",
                records, pg->frame_time, (now - t0) / 1000);
    }

    uint64_t hash = recomp_vsh_hash_used(pg, r->shader_binding);
    uint32_t start = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CSV0_C),
                              NV_PGRAPH_CSV0_C_CHEOPS_PROGRAM_START);
    fprintf(stderr,
            "[GPU-VSH] burst=%u n=%u frame=%u draw=%u kind=%s count=%u "
            "start=%u length=%u program=%016" PRIx64
            " used_constants=%016" PRIx64
            " color=%08" PRIx32 " tex=%08" PRIx32 ",%08" PRIx32
            ",%08" PRIx32 ",%08" PRIx32 " attr0=%08" PRIx64
            "/%u/%u attr1=%08" PRIx64 "/%u/%u\n",
            burst, records, pg->frame_time, pg->draw_time, kind, count,
            start, r->shader_binding->state.program_length,
            recomp_vsh_hash_program(&r->shader_binding->state), hash,
            (uint32_t)(r->color_binding ? r->color_binding->vram_addr : 0),
            pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0),
            pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET1),
            pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET2),
            pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET3),
            (uint64_t)pg->vertex_attributes[0].offset,
            pg->vertex_attributes[0].count,
            pg->vertex_attributes[0].stride,
            (uint64_t)pg->vertex_attributes[1].offset,
            pg->vertex_attributes[1].count,
            pg->vertex_attributes[1].stride);
    if (length == 107 && !only477) {
        recomp_trace_vsh_bone_indices(d, pg, records);
    }
    if (!have_previous || memcmp(previous, pg->vsh_constants,
                                 sizeof(previous)) != 0) {
        for (unsigned i = 0; i < NV2A_VERTEXSHADER_CONSTANTS; i++) {
            if (r->shader_binding->vsh_constant_loc[i] != -1 &&
                (!have_previous || memcmp(previous[i], pg->vsh_constants[i],
                                         sizeof(previous[i])) != 0)) {
                fprintf(stderr,
                        "[GPU-VSH-C] burst=%u n=%u c%u=%08" PRIx32 ",%08" PRIx32
                        ",%08" PRIx32 ",%08" PRIx32 "\n",
                        burst, records, i, pg->vsh_constants[i][0],
                        pg->vsh_constants[i][1], pg->vsh_constants[i][2],
                        pg->vsh_constants[i][3]);
            }
        }
        memcpy(previous, pg->vsh_constants, sizeof(previous));
        have_previous = true;
    }
}

/* xboxrecomp diagnostic (Update 409): the ice flicker alternates between a
 * state with bright highlight patches and one without, every 1-2 s. With
 * RECOMP_DRAWSET_TRACE=1, after F11 log per NV2A frame which draws (textures,
 * blend state, program, vertex count) appeared or disappeared relative to
 * the previous frame, so the periodically toggling pass can be named.
 * Observational only. */
typedef struct RecompDrawKey {
    uint32_t tex[4], blend, count, combiner, pad;
    uint64_t prog;
} RecompDrawKey;

static int recomp_draw_key_cmp(const void *a, const void *b)
{
    return memcmp(a, b, sizeof(RecompDrawKey));
}

static void recomp_print_draw_key(const char *tag, const RecompDrawKey *k)
{
    fprintf(stderr, " %s[t=%08x,%08x,%08x,%08x b=%08x c=%u rc=%08x p=%016" PRIx64 "]",
            tag, k->tex[0], k->tex[1], k->tex[2], k->tex[3], k->blend,
            k->count, k->combiner, k->prog);
}

/* Update 410: with RECOMP_DRAWSET_TRACE=2, the draw set stays equal across
 * the ice flicker, so compare per draw (matched by key without vertex count
 * and occurrence) the values feeding it: VS constants, fixed-function light
 * contexts, combiner/texture registers 0x1800..0x1BFC, light vectors and the
 * bound textures' content hashes. Logs per frame the changed slot ranges as
 * `[GPU-DRAWVAL] frame= matched= name..name:ndraws(old>new)`. */
enum {
    RV_VSH = NV2A_VERTEXSHADER_CONSTANTS * 4,
    RV_LTA = RV_VSH + NV2A_LTCTXA_COUNT * 4,
    RV_LTB = RV_LTA + NV2A_LTCTXB_COUNT * 4,
    RV_LT1 = RV_LTB + NV2A_LTC1_COUNT * 4,
    RV_REG = RV_LT1 + 256,
    RV_LIT = RV_REG + NV2A_MAX_LIGHTS * 12,
    RV_TEX = RV_LIT + 4,
    /* Update 413: content hash of each vertex attribute's used range plus
     * the index list, for CPU-computed vertex data (e.g. lighting). */
    RV_IDX = RV_TEX + NV2A_VERTEXSHADER_ATTRIBUTES,
    /* Update 415: hash of the guest RAM behind each enabled texture stage
     * (texhash is 0 for render-target textures; in the non-TCG path those
     * are downloaded to RAM, so RAM reflects the GPU content). */
    RV_RAM = RV_IDX + 1,
    RV_END = RV_RAM + 4,
    RV_MAXD = 1024,
};

static void recomp_val_name(unsigned s, char *buf, size_t n)
{
    if (s < RV_VSH) snprintf(buf, n, "c%u.%u", s / 4, s % 4);
    else if (s < RV_LTA) snprintf(buf, n, "lta%u.%u", (s - RV_VSH) / 4, s % 4);
    else if (s < RV_LTB) snprintf(buf, n, "ltb%u.%u", (s - RV_LTA) / 4, s % 4);
    else if (s < RV_LT1) snprintf(buf, n, "lt1_%u.%u", (s - RV_LTB) / 4, s % 4);
    else if (s < RV_REG) snprintf(buf, n, "r%04x", 0x1800 + (s - RV_LT1) * 4);
    else if (s < RV_LIT) snprintf(buf, n, "light%u.%u", (s - RV_REG) / 12,
                                  (s - RV_REG) % 12);
    else if (s < RV_TEX) snprintf(buf, n, "texhash%u", s - RV_LIT);
    else if (s < RV_IDX) snprintf(buf, n, "vtx%u", s - RV_TEX);
    else if (s == RV_IDX) snprintf(buf, n, "idx");
    else snprintf(buf, n, "texram%u", s - RV_RAM);
}

static void recomp_trace_drawval(NV2AState *d, PGRAPHGLState *r,
                                 const void *key, bool new_frame, uint32_t frame,
                                 bool per_draw)
{
    static uint32_t (*cur)[RV_END], (*prev)[RV_END], (*prev2)[RV_END];
    static RecompDrawKey *ckey, *pkey, *p2key;
    static unsigned ncur, nprev, nprev2;
    static unsigned cnt[RV_END];
    static uint32_t ex_old[RV_END], ex_new[RV_END];
    static uint32_t rt_addr[32], rt_count[32];
    static unsigned n_rt;
    PGRAPHState *pg = &d->pgraph;

    if (!cur) {
        cur = calloc(RV_MAXD, sizeof(*cur));
        prev = calloc(RV_MAXD, sizeof(*prev));
        ckey = calloc(RV_MAXD, sizeof(*ckey));
        pkey = calloc(RV_MAXD, sizeof(*pkey));
        prev2 = calloc(RV_MAXD, sizeof(*prev2));
        p2key = calloc(RV_MAXD, sizeof(*p2key));
        if (!cur || !prev || !ckey || !pkey || !prev2 || !p2key)
            return;
    }
    if (new_frame) {
        unsigned matched = 0;
        /* Render targets drawn into this frame, in first-use order. */
        fprintf(stderr, "[GPU-RT] frame=%u", frame);
        for (unsigned k = 0; k < n_rt; k++)
            fprintf(stderr, " %08x:%u", rt_addr[k], rt_count[k]);
        fprintf(stderr, "\n");
        n_rt = 0;
        /* Update 416: the ice samples the small render-target chain at
         * 0x05600000..0x0565FFFF, whose content freezes while the warm
         * patches are gone. In mode 3 log each chain surface and write its
         * RAM content (downloaded first) to rtdump_<frame>_<addr>_...raw. */
        if (per_draw) {
            SurfaceBinding *sb;
            QTAILQ_FOREACH(sb, &r->surfaces, entry) {
                if (sb->vram_addr < 0x05600000 || sb->vram_addr >= 0x05660000)
                    continue;
                pgraph_gl_surface_download_if_dirty(d, sb);
                fprintf(stderr, "[GPU-SURF] frame=%u addr=%08x %ux%u pitch=%u "
                        "bpp=%u color=%d swz=%d draw_time=%d frame_time=%d "
                        "cleared=%d\n", frame, (uint32_t)sb->vram_addr,
                        sb->width, sb->height, sb->pitch, sb->fmt.bytes_per_pixel,
                        sb->color, sb->swizzle, sb->draw_time, sb->frame_time,
                        sb->cleared);
                if (getenv("RECOMP_RTDUMP") && sb->size <= 4u * 1024 * 1024) {
                    char fn[128];
                    snprintf(fn, sizeof(fn), "rtdump_%u_%08x_%ux%u_%u_%u_%d.raw",
                             frame, (uint32_t)sb->vram_addr, sb->width,
                             sb->height, sb->pitch, sb->fmt.bytes_per_pixel,
                             sb->swizzle);
                    FILE *f = fopen(fn, "wb");
                    if (f) {
                        fwrite(d->vram_ptr + sb->vram_addr, 1, sb->size, f);
                        fclose(f);
                    }
                }
            }
        }
        memset(cnt, 0, sizeof(cnt));
        for (unsigned i = 0; i < ncur; i++) {
            unsigned occ = 0, j, o = 0;
            for (j = 0; j < i; j++)
                occ += !memcmp(&ckey[j], &ckey[i], sizeof(ckey[0]));
            for (j = 0; j < nprev; j++) {
                if (!memcmp(&pkey[j], &ckey[i], sizeof(ckey[0])) && o++ == occ)
                    break;
            }
            /* Update 417: draws whose key alternates with the double-
             * buffered render targets (post-processing) never match the
             * previous frame; compare those with the frame before it. */
            uint32_t (*base)[RV_END] = prev;
            int older = 0;
            if (j >= nprev) {
                for (j = 0, o = 0; j < nprev2; j++) {
                    if (!memcmp(&p2key[j], &ckey[i], sizeof(ckey[0])) &&
                        o++ == occ)
                        break;
                }
                if (j >= nprev2)
                    continue;
                base = prev2;
                older = 1;
            }
            matched++;
            char line[2048], nm[32];
            int len = 0, listed = 0, more = 0;
            if (per_draw)
                len = snprintf(line, sizeof(line),
                               "[GPU-DV] f=%u i=%u j%s=%u t0=%08x p=%08x b=%08x",
                               frame, i, older ? "2" : "", j, ckey[i].tex[0],
                               (uint32_t)ckey[i].prog, ckey[i].blend);
            /* Vertex/index hashes first, so the per-draw line cap never
             * hides them behind camera-driven constant changes. */
            for (unsigned q = 0; q < RV_END; q++) {
                unsigned s = (q + RV_TEX) % RV_END;
                if (cur[i][s] != base[j][s]) {
                    if (!cnt[s]++) {
                        ex_old[s] = base[j][s];
                        ex_new[s] = cur[i][s];
                    }
                    if (per_draw && listed < 40 &&
                        len < (int)sizeof(line) - 64) {
                        recomp_val_name(s, nm, sizeof(nm));
                        len += snprintf(line + len, sizeof(line) - len,
                                        " %s=%08x", nm, cur[i][s]);
                        listed++;
                    } else if (per_draw) {
                        more++;
                    }
                }
            }
            if (per_draw && listed) {
                len += snprintf(line + len, sizeof(line) - len, " more=%d\n",
                                more);
                fwrite(line, 1, len, stderr);
            }
        }
        if (nprev) {
            char a[32], b[32];
            unsigned printed = 0;
            fprintf(stderr, "[GPU-DRAWVAL] frame=%u draws=%u matched=%u", frame,
                    ncur, matched);
            for (unsigned s = 0; s < RV_END && printed < 48; s++) {
                unsigned e = s;
                if (!cnt[s])
                    continue;
                while (e + 1 < RV_END && cnt[e + 1] == cnt[s])
                    e++;
                recomp_val_name(s, a, sizeof(a));
                recomp_val_name(e, b, sizeof(b));
                if (e > s)
                    fprintf(stderr, " %s..%s:%u", a, b, cnt[s]);
                else
                    fprintf(stderr, " %s:%u(%08x>%08x)", a, cnt[s], ex_old[s],
                            ex_new[s]);
                printed++;
                s = e;
            }
            fprintf(stderr, "\n");
        }
        void *t = prev2; prev2 = prev; prev = cur; cur = t;
        t = p2key; p2key = pkey; pkey = ckey; ckey = t;
        nprev2 = nprev;
        nprev = ncur;
        ncur = 0;
    }
    if (!key || ncur >= RV_MAXD)
        return;
    /* Update 418: the ice program (VP hash ..d920e4d2) samples stages 1..3
     * at the addresses of the bloom render-target chain. Log the texture
     * shapes it asks for there and what the surface cache holds, to tell
     * intended surface sampling from two allocations sharing memory. */
    {
        static unsigned shape_logs;
        if ((uint32_t)((const RecompDrawKey *)key)->prog == 0xd920e4d2u &&
            shape_logs < 40) {
            shape_logs++;
            for (int t = 0; t < 4; t++) {
                if (!pgraph_is_texture_enabled(pg, t))
                    continue;
                TextureShape sh = pgraph_get_texture_shape(pg, t);
                hwaddr ta = pgraph_get_texture_phys_addr(pg, t);
                SurfaceBinding *sb = pgraph_gl_surface_get(d, ta);
                SurfaceBinding *sw = pgraph_gl_surface_get_within(d, ta);
                fprintf(stderr, "[GPU-ICETEX] frame=%u stage=%d addr=%08x "
                        "fmt=%u cube=%d dim=%u %ux%ux%u levels=%u len=%zu "
                        "surf=%s%08x %ux%u compat=%d within=%08x\n", frame, t,
                        (uint32_t)ta, sh.color_format, sh.cubemap,
                        sh.dimensionality, sh.width, sh.height, sh.depth,
                        sh.levels, pgraph_get_texture_length(pg, &sh),
                        sb ? "" : "none:", sb ? (uint32_t)sb->vram_addr : 0,
                        sb ? sb->width : 0, sb ? sb->height : 0,
                        sb ? pgraph_gl_check_surface_to_texture_compatibility(
                                 sb, &sh) : -1,
                        sw ? (uint32_t)sw->vram_addr : 0);
            }
        }
    }
    uint32_t rt = r->color_binding ? (uint32_t)r->color_binding->vram_addr
                                   : 0xFFFFFFFFu;
    unsigned k;
    for (k = 0; k < n_rt && rt_addr[k] != rt; k++)
        ;
    if (k < 32) {
        if (k == n_rt) {
            rt_addr[n_rt] = rt;
            rt_count[n_rt++] = 0;
        }
        rt_count[k]++;
    }
    uint32_t *v = cur[ncur];
    memcpy(&ckey[ncur], key, sizeof(ckey[0]));
    memcpy(v, pg->vsh_constants, sizeof(pg->vsh_constants));
    /* Constants the bound program does not read are leftovers of earlier
     * draws (e.g. the bone palette); zero them so they do not count. */
    for (unsigned c = 0; c < NV2A_VERTEXSHADER_CONSTANTS; c++) {
        if (!r->shader_binding || r->shader_binding->vsh_constant_loc[c] == -1)
            memset(v + c * 4, 0, 16);
    }
    memcpy(v + RV_VSH, pg->ltctxa, sizeof(pg->ltctxa));
    memcpy(v + RV_LTA, pg->ltctxb, sizeof(pg->ltctxb));
    memcpy(v + RV_LTB, pg->ltc1, sizeof(pg->ltc1));
    for (unsigned k = 0; k < 256; k++)
        v[RV_LT1 + k] = pgraph_reg_r(pg, 0x1800 + k * 4);
    for (unsigned l = 0; l < NV2A_MAX_LIGHTS; l++) {
        memcpy(v + RV_REG + l * 12, pg->light_infinite_half_vector[l], 12);
        memcpy(v + RV_REG + l * 12 + 3, pg->light_infinite_direction[l], 12);
        memcpy(v + RV_REG + l * 12 + 6, pg->light_local_position[l], 12);
        memcpy(v + RV_REG + l * 12 + 9, pg->light_local_attenuation[l], 12);
    }
    for (unsigned t = 0; t < 4; t++)
        v[RV_LIT + t] = r->texture_binding[t]
                            ? (uint32_t)r->texture_binding[t]->data_hash : 0;

    uint32_t lo = 0, hi = 0;
    bool ranged = true;
    if (pg->draw_arrays_length) {
        lo = pg->draw_arrays_min_start;
        hi = pg->draw_arrays_max_count ? pg->draw_arrays_max_count - 1 : 0;
    } else if (pg->inline_elements_length) {
        lo = UINT32_MAX;
        for (unsigned e = 0; e < pg->inline_elements_length; e++) {
            lo = MIN(lo, pg->inline_elements[e]);
            hi = MAX(hi, pg->inline_elements[e]);
        }
        v[RV_IDX] = (uint32_t)fast_hash(
            (uint8_t *)pg->inline_elements, pg->inline_elements_length * 4);
    } else {
        ranged = false;
        if (pg->inline_array_length)
            v[RV_IDX] = (uint32_t)fast_hash(
                (uint8_t *)pg->inline_array, pg->inline_array_length * 4);
    }
    for (unsigned a = 0; a < NV2A_VERTEXSHADER_ATTRIBUTES; a++) {
        const VertexAttribute *attr = &pg->vertex_attributes[a];
        if (!attr->count)
            continue;
        if (!ranged) {
            if (pg->inline_buffer_length && attr->inline_buffer_populated)
                v[RV_TEX + a] = (uint32_t)fast_hash(
                    (uint8_t *)attr->inline_buffer,
                    pg->inline_buffer_length * sizeof(float) * 4);
            continue;
        }
        hwaddr dma_len = 0;
        uint8_t *base = (uint8_t *)nv_dma_map(
            d, attr->dma_select ? pg->dma_vertex_b : pg->dma_vertex_a,
            &dma_len);
        uint64_t start = (uint64_t)attr->offset + (uint64_t)lo * attr->stride;
        uint64_t len = attr->stride ? (uint64_t)(hi - lo + 1) * attr->stride
                                    : attr->size * attr->count;
        if (base && hi >= lo && start < dma_len && len <= dma_len - start &&
            len <= 16u * 1024 * 1024)
            v[RV_TEX + a] = (uint32_t)fast_hash(base + start, len);
    }
    for (int t = 0; t < 4; t++) {
        if (!pgraph_is_texture_enabled(pg, t))
            continue;
        TextureShape shape = pgraph_get_texture_shape(pg, t);
        hwaddr ta = pgraph_get_texture_phys_addr(pg, t);
        size_t tl = pgraph_get_texture_length(pg, &shape);
        if (tl && ta + tl < memory_region_size(d->vram) &&
            tl <= 8u * 1024 * 1024)
            v[RV_RAM + t] = (uint32_t)fast_hash(d->vram_ptr + ta, tl);
    }
    ncur++;
}

static void recomp_trace_drawset(NV2AState *d, PGRAPHGLState *r, unsigned count)
{
    enum { MAXD = 4096 };
    static int on = -1;
    static RecompDrawKey cur[MAXD], prev[MAXD];
    static unsigned ncur, nprev, logged;
    static uint32_t frame = UINT32_MAX;
    PGRAPHState *pg = &d->pgraph;

    if (on < 0) {
        const char *e = getenv("RECOMP_DRAWSET_TRACE");
        on = e ? atoi(e) : 0;
    }
    if (!on || !xbox_FramebufferBurstGeneration() || logged >= 600)
        return;
    if (pg->frame_time != frame) {
        if (frame != UINT32_MAX && on >= 2) {
            /* RECOMP_DRAWSET_TRACE=3 also logs every matched draw's changed
             * slots as `[GPU-DV] f i j t0 p b slot=new...` (150 frames). */
            if (on == 3 && logged >= 150)
                return;
            logged++;
            recomp_trace_drawval(d, r, NULL, true, frame, on == 3);
        } else if (frame != UINT32_MAX) {
            unsigned i = 0, j = 0, added = 0, removed = 0;
            qsort(cur, ncur, sizeof(cur[0]), recomp_draw_key_cmp);
            logged++;
            fprintf(stderr, "[GPU-DRAWSET] frame=%u draws=%u prev=%u", frame,
                    ncur, nprev);
            while (i < ncur || j < nprev) {
                int c = i >= ncur ? 1 : j >= nprev ? -1
                        : recomp_draw_key_cmp(&cur[i], &prev[j]);
                if (c < 0) {
                    if (added++ < 12) recomp_print_draw_key("+", &cur[i]);
                    i++;
                } else if (c > 0) {
                    if (removed++ < 12) recomp_print_draw_key("-", &prev[j]);
                    j++;
                } else {
                    i++; j++;
                }
            }
            fprintf(stderr, " added=%u removed=%u\n", added, removed);
            memcpy(prev, cur, ncur * sizeof(cur[0]));
            nprev = ncur;
        }
        ncur = 0;
        frame = pg->frame_time;
    }
    if (ncur < MAXD) {
        RecompDrawKey *k = &cur[ncur++];
        memset(k, 0, sizeof(*k));
        for (int t = 0; t < 4; t++)
            k->tex[t] = pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0 + t * 4);
        k->blend = pgraph_reg_r(pg, NV_PGRAPH_BLEND);
        k->count = count;
        k->combiner = pgraph_reg_r(pg, NV_PGRAPH_COMBINECTL);
        k->prog = r->shader_binding && r->shader_binding->state.vertex_program
                      ? recomp_vsh_hash_program(&r->shader_binding->state) : 0;
        if (on >= 2) {
            RecompDrawKey vk = *k;
            vk.count = 0;
            recomp_trace_drawval(d, r, &vk, false, frame, false);
        }
    }
}

void pgraph_gl_clear_surface(NV2AState *d, uint32_t parameter)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    NV2A_DPRINTF("---------PRE CLEAR ------\n");
    pg->clearing = true;

    GLbitfield gl_mask = 0;

    bool write_color = (parameter & NV097_CLEAR_SURFACE_COLOR);
    bool write_zeta =
        (parameter & (NV097_CLEAR_SURFACE_Z | NV097_CLEAR_SURFACE_STENCIL));

    if (write_zeta) {
        GLint gl_clear_stencil;
        GLfloat gl_clear_depth;
        pgraph_get_clear_depth_stencil_value(pg, &gl_clear_depth,
                                             &gl_clear_stencil);

        if (parameter & NV097_CLEAR_SURFACE_Z) {
            gl_mask |= GL_DEPTH_BUFFER_BIT;
            glDepthMask(GL_TRUE);
            glClearDepth(gl_clear_depth);
        }
        if (parameter & NV097_CLEAR_SURFACE_STENCIL) {
            gl_mask |= GL_STENCIL_BUFFER_BIT;
            glStencilMask(0xff);
            glClearStencil(gl_clear_stencil);
        }
    }
    if (write_color) {
        gl_mask |= GL_COLOR_BUFFER_BIT;
        glColorMask((parameter & NV097_CLEAR_SURFACE_R)
                         ? GL_TRUE : GL_FALSE,
                    (parameter & NV097_CLEAR_SURFACE_G)
                         ? GL_TRUE : GL_FALSE,
                    (parameter & NV097_CLEAR_SURFACE_B)
                         ? GL_TRUE : GL_FALSE,
                    (parameter & NV097_CLEAR_SURFACE_A)
                         ? GL_TRUE : GL_FALSE);

        GLfloat rgba[4];
        pgraph_get_clear_color(pg, rgba);
        glClearColor(rgba[0], rgba[1], rgba[2], rgba[3]);
    }

    pgraph_gl_surface_update(d, true, write_color, write_zeta);

    /* FIXME: Needs confirmation */
    unsigned int xmin =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX), NV_PGRAPH_CLEARRECTX_XMIN);
    unsigned int xmax =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTX), NV_PGRAPH_CLEARRECTX_XMAX);
    unsigned int ymin =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY), NV_PGRAPH_CLEARRECTY_YMIN);
    unsigned int ymax =
        GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CLEARRECTY), NV_PGRAPH_CLEARRECTY_YMAX);

    NV2A_DPRINTF(
        "------------------CLEAR 0x%x %d,%d - %d,%d  %x---------------\n",
        parameter, xmin, ymin, xmax, ymax,
        d->pgraph.regs_[NV_PGRAPH_COLORCLEARVALUE]);

    unsigned int scissor_width = xmax - xmin + 1,
                 scissor_height = ymax - ymin + 1;
    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);

    NV2A_DPRINTF("Translated clear rect to %d,%d - %d,%d\n", xmin, ymin,
                 xmin + scissor_width - 1, ymin + scissor_height - 1);

    bool full_clear = !xmin && !ymin &&
                      scissor_width >= pg->surface_binding_dim.width &&
                      scissor_height >= pg->surface_binding_dim.height;

    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    /* FIXME: Respect window clip?!?! */
    glEnable(GL_SCISSOR_TEST);
    glScissor(xmin, ymin, scissor_width, scissor_height);

    /* Dither */
    /* FIXME: Maybe also disable it here? + GL implementation dependent */
    if (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) & NV_PGRAPH_CONTROL_0_DITHERENABLE) {
        glEnable(GL_DITHER);
    } else {
        glDisable(GL_DITHER);
    }

    glClear(gl_mask);

    glDisable(GL_SCISSOR_TEST);

    pgraph_gl_set_surface_dirty(pg, write_color, write_zeta);

    if (r->color_binding) {
        r->color_binding->cleared = full_clear && write_color;
    }
    if (r->zeta_binding) {
        r->zeta_binding->cleared = full_clear && write_zeta;
    }
    
    pg->clearing = false;
}

void pgraph_gl_draw_begin(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    NV2A_GL_DGROUP_BEGIN("NV097_SET_BEGIN_END: 0x%x", pg->primitive_mode);

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    pgraph_gl_surface_update(d, true, true, depth_test || stencil_test);

    if (is_nop_draw) {
        return;
    }

    assert(r->color_binding || r->zeta_binding);

    pgraph_gl_bind_textures(d);
    pgraph_gl_bind_shaders(pg);

    glColorMask(mask_red, mask_green, mask_blue, mask_alpha);
    glDepthMask(!!(control_0 & NV_PGRAPH_CONTROL_0_ZWRITEENABLE));
    glStencilMask(GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                           NV_PGRAPH_CONTROL_1_STENCIL_MASK_WRITE));

    if (pgraph_reg_r(pg, NV_PGRAPH_BLEND) & NV_PGRAPH_BLEND_EN) {
        glEnable(GL_BLEND);
        uint32_t sfactor = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                                    NV_PGRAPH_BLEND_SFACTOR);
        uint32_t dfactor = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                                    NV_PGRAPH_BLEND_DFACTOR);
        assert(sfactor < ARRAY_SIZE(pgraph_blend_factor_gl_map));
        assert(dfactor < ARRAY_SIZE(pgraph_blend_factor_gl_map));
        glBlendFunc(pgraph_blend_factor_gl_map[sfactor],
                    pgraph_blend_factor_gl_map[dfactor]);

        uint32_t equation = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_BLEND),
                                     NV_PGRAPH_BLEND_EQN);
        assert(equation < ARRAY_SIZE(pgraph_blend_equation_gl_map));
        glBlendEquation(pgraph_blend_equation_gl_map[equation]);

        uint32_t blend_color = pgraph_reg_r(pg, NV_PGRAPH_BLENDCOLOR);
        float gl_blend_color[4];
        pgraph_argb_pack32_to_rgba_float(blend_color, gl_blend_color);
        glBlendColor(gl_blend_color[0], gl_blend_color[1], gl_blend_color[2],
                     gl_blend_color[3]);
    } else {
        glDisable(GL_BLEND);
    }

    /* Face culling */
    if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER)
            & NV_PGRAPH_SETUPRASTER_CULLENABLE) {
        uint32_t cull_face = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER),
                                      NV_PGRAPH_SETUPRASTER_CULLCTRL);
        assert(cull_face < ARRAY_SIZE(pgraph_cull_face_gl_map));
        glCullFace(pgraph_cull_face_gl_map[cull_face]);
        glEnable(GL_CULL_FACE);
    } else {
        glDisable(GL_CULL_FACE);
    }

    /* Front-face select */
    /* Winding is reverse here because clip-space y-coordinates are inverted */
    glFrontFace(pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER)
                    & NV_PGRAPH_SETUPRASTER_FRONTFACE
                        ? GL_CW : GL_CCW);

    /* Polygon offset */
    /* FIXME: GL implementation-specific, maybe do this in VS? */
    if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
            NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE) {
        glEnable(GL_POLYGON_OFFSET_FILL);
    } else {
        glDisable(GL_POLYGON_OFFSET_FILL);
    }
    if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
            NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE) {
        glEnable(GL_POLYGON_OFFSET_LINE);
    } else {
        glDisable(GL_POLYGON_OFFSET_LINE);
    }
    if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
            NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE) {
        glEnable(GL_POLYGON_OFFSET_POINT);
    } else {
        glDisable(GL_POLYGON_OFFSET_POINT);
    }
    if (pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
            (NV_PGRAPH_SETUPRASTER_POFFSETFILLENABLE |
             NV_PGRAPH_SETUPRASTER_POFFSETLINEENABLE |
             NV_PGRAPH_SETUPRASTER_POFFSETPOINTENABLE)) {
        uint32_t zfactor_u32 = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETFACTOR);
        GLfloat zfactor = *(float*)&zfactor_u32;
        uint32_t zbias_u32 = pgraph_reg_r(pg, NV_PGRAPH_ZOFFSETBIAS);
        GLfloat zbias = *(float*)&zbias_u32;
        // FIXME: with Linux and Mesa, zbias must be multiplied by 0.5 in
        // order to have the same depth value offset as Xbox.
        glPolygonOffset(zfactor, zbias);
    }

    /* Depth testing */
    if (depth_test) {
        glEnable(GL_DEPTH_TEST);

        uint32_t depth_func = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0),
                                       NV_PGRAPH_CONTROL_0_ZFUNC);
        assert(depth_func < ARRAY_SIZE(pgraph_depth_func_gl_map));
        glDepthFunc(pgraph_depth_func_gl_map[depth_func]);
    } else {
        glDisable(GL_DEPTH_TEST);
    }

    glEnable(GL_DEPTH_CLAMP);

    if (GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_3),
                 NV_PGRAPH_CONTROL_3_SHADEMODE) ==
        NV_PGRAPH_CONTROL_3_SHADEMODE_FLAT) {
        glProvokingVertex(GL_FIRST_VERTEX_CONVENTION);
    }

    if (stencil_test) {
        glEnable(GL_STENCIL_TEST);

        uint32_t stencil_func = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                    NV_PGRAPH_CONTROL_1_STENCIL_FUNC);
        uint32_t stencil_ref = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                    NV_PGRAPH_CONTROL_1_STENCIL_REF);
        uint32_t func_mask = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1),
                                NV_PGRAPH_CONTROL_1_STENCIL_MASK_READ);
        uint32_t op_fail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                NV_PGRAPH_CONTROL_2_STENCIL_OP_FAIL);
        uint32_t op_zfail = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                NV_PGRAPH_CONTROL_2_STENCIL_OP_ZFAIL);
        uint32_t op_zpass = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_CONTROL_2),
                                NV_PGRAPH_CONTROL_2_STENCIL_OP_ZPASS);

        assert(stencil_func < ARRAY_SIZE(pgraph_stencil_func_gl_map));
        assert(op_fail < ARRAY_SIZE(pgraph_stencil_op_gl_map));
        assert(op_zfail < ARRAY_SIZE(pgraph_stencil_op_gl_map));
        assert(op_zpass < ARRAY_SIZE(pgraph_stencil_op_gl_map));

        glStencilFunc(
            pgraph_stencil_func_gl_map[stencil_func],
            stencil_ref,
            func_mask);

        glStencilOp(
            pgraph_stencil_op_gl_map[op_fail],
            pgraph_stencil_op_gl_map[op_zfail],
            pgraph_stencil_op_gl_map[op_zpass]);

    } else {
        glDisable(GL_STENCIL_TEST);
    }

    /* Dither */
    /* FIXME: GL implementation dependent */
    if (pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0) &
            NV_PGRAPH_CONTROL_0_DITHERENABLE) {
        glEnable(GL_DITHER);
    } else {
        glDisable(GL_DITHER);
    }

    glEnable(GL_PROGRAM_POINT_SIZE);

    bool anti_aliasing = GET_MASK(pgraph_reg_r(pg, NV_PGRAPH_ANTIALIASING), NV_PGRAPH_ANTIALIASING_ENABLE);

    /* Edge Antialiasing */
    if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                              NV_PGRAPH_SETUPRASTER_LINESMOOTHENABLE) {
        glEnable(GL_LINE_SMOOTH);
        glLineWidth(MIN(r->supported_smooth_line_width_range[1], pg->surface_scale_factor));
    } else {
        glDisable(GL_LINE_SMOOTH);
        glLineWidth(MIN(r->supported_aliased_line_width_range[1], pg->surface_scale_factor));
    }
    if (!anti_aliasing && pgraph_reg_r(pg, NV_PGRAPH_SETUPRASTER) &
                              NV_PGRAPH_SETUPRASTER_POLYSMOOTHENABLE) {
        glEnable(GL_POLYGON_SMOOTH);
    } else {
        glDisable(GL_POLYGON_SMOOTH);
    }

    unsigned int vp_width = pg->surface_binding_dim.width,
                 vp_height = pg->surface_binding_dim.height;
    pgraph_apply_scaling_factor(pg, &vp_width, &vp_height);
    glViewport(0, 0, vp_width, vp_height);

    /* Surface clip */
    /* FIXME: Consider moving to PSH w/ window clip */
    unsigned int xmin = pg->surface_shape.clip_x,
                 ymin = pg->surface_shape.clip_y;

    unsigned int scissor_width = pg->surface_shape.clip_width,
                 scissor_height = pg->surface_shape.clip_height;

    pgraph_apply_anti_aliasing_factor(pg, &xmin, &ymin);
    pgraph_apply_anti_aliasing_factor(pg, &scissor_width, &scissor_height);
    pgraph_apply_scaling_factor(pg, &xmin, &ymin);
    pgraph_apply_scaling_factor(pg, &scissor_width, &scissor_height);

    glEnable(GL_SCISSOR_TEST);
    glScissor(xmin, ymin, scissor_width, scissor_height);

    /* Visibility testing */
    if (pg->zpass_pixel_count_enable) {
        r->gl_zpass_pixel_count_query_count++;
        r->gl_zpass_pixel_count_queries = (GLuint*)g_realloc(
            r->gl_zpass_pixel_count_queries,
            sizeof(GLuint) * r->gl_zpass_pixel_count_query_count);

        GLuint gl_query;
        glGenQueries(1, &gl_query);
        r->gl_zpass_pixel_count_queries[
            r->gl_zpass_pixel_count_query_count - 1] = gl_query;
        glBeginQuery(GL_SAMPLES_PASSED, gl_query);
    }
}

void pgraph_gl_draw_end(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    uint32_t control_0 = pgraph_reg_r(pg, NV_PGRAPH_CONTROL_0);
    bool mask_alpha = control_0 & NV_PGRAPH_CONTROL_0_ALPHA_WRITE_ENABLE;
    bool mask_red = control_0 & NV_PGRAPH_CONTROL_0_RED_WRITE_ENABLE;
    bool mask_green = control_0 & NV_PGRAPH_CONTROL_0_GREEN_WRITE_ENABLE;
    bool mask_blue = control_0 & NV_PGRAPH_CONTROL_0_BLUE_WRITE_ENABLE;
    bool color_write = mask_alpha || mask_red || mask_green || mask_blue;
    bool depth_test = control_0 & NV_PGRAPH_CONTROL_0_ZENABLE;
    bool stencil_test =
        pgraph_reg_r(pg, NV_PGRAPH_CONTROL_1) & NV_PGRAPH_CONTROL_1_STENCIL_TEST_ENABLE;
    bool is_nop_draw = !(color_write || depth_test || stencil_test);

    if (is_nop_draw) {
        // FIXME: Check PGRAPH register 0x880.
        // HW uses bit 11 in 0x880 to enable or disable a color/zeta limit
        // check that will raise an exception in the case that a draw should
        // modify the color and/or zeta buffer but the target(s) are masked
        // off. This check only seems to trigger during the fragment
        // processing, it is legal to attempt a draw that is entirely
        // clipped regardless of 0x880. See xemu#635 for context.
        return;
    }

    pgraph_gl_flush_draw(d);

    /* End of visibility testing */
    if (pg->zpass_pixel_count_enable) {
        nv2a_profile_inc_counter(NV2A_PROF_QUERY);
        glEndQuery(GL_SAMPLES_PASSED);
    }

    pg->draw_time++;
    if (r->color_binding && pgraph_color_write_enabled(pg)) {
        r->color_binding->draw_time = pg->draw_time;
    }
    if (r->zeta_binding && pgraph_zeta_write_enabled(pg)) {
        r->zeta_binding->draw_time = pg->draw_time;
    }

    pgraph_gl_set_surface_dirty(pg, color_write, depth_test || stencil_test);
    NV2A_GL_DGROUP_END();
}

/* Update 420: every input of every draw stays equal across the ice-patch
 * fades, yet the image changes. With RECOMP_WARM_TRACE=1, after F11 (60
 * frames) download the main color target after each draw and log how much
 * that draw changed a sparse "warmth" sum (max(0,R-B)) and luminance:
 * `[GPU-WARM] f i t0 p b n dW dL`. Comparing a frame with and without the
 * patches names the draw that paints them. Observational (slow). */
static void recomp_trace_warm(NV2AState *d, PGRAPHGLState *r, unsigned count)
{
    static int on = -1;
    static uint32_t frame = UINT32_MAX, logged_frames;
    static unsigned index;
    static uint32_t last_addr;
    static int64_t last_w, last_l, last_a;
    PGRAPHState *pg = &d->pgraph;

    if (on < 0) {
        const char *e = getenv("RECOMP_WARM_TRACE");
        on = e && *e == '1';
    }
    if (!on || !xbox_FramebufferBurstGeneration() || logged_frames > 60)
        return;
    if (pg->frame_time != frame) {
        frame = pg->frame_time;
        logged_frames++;
        index = 0;
        last_addr = 0;
    }
    index++;
    SurfaceBinding *sb = r->color_binding;
    if (!sb || sb->width < 512 || sb->swizzle || sb->fmt.bytes_per_pixel != 4)
        return;
    pgraph_gl_surface_download_if_dirty(d, sb);
    const uint8_t *px = d->vram_ptr + sb->vram_addr;
    int64_t w = 0, l = 0, a = 0;
    for (unsigned y = 0; y < sb->height; y += 4) {
        const uint8_t *row = px + (size_t)y * sb->pitch;
        for (unsigned x = 0; x < sb->width; x += 4) {
            const uint8_t *p = row + x * 4;   /* B G R A */
            int dw = (int)p[2] - (int)p[0];
            w += dw > 0 ? dw : 0;
            l += p[0] + p[1] + p[2];
            a += p[3];
        }
    }
    /* Update 422: the patches come from the additive light pass (blend
     * 0x16A, large meshes). Log what each of its texture stages samples:
     * shape, and the surface there (color/zeta, its draw_time) against the
     * bound texture's draw_time -- a shadow/depth map at 0x05605000 shares
     * memory with the bloom chain and is recreated every frame. */
    if (pgraph_reg_r(pg, NV_PGRAPH_BLEND) == 0x16a && count >= 1000) {
        /* Update 424: the light pass reads c132.. (per-light matrices that
         * move every frame; the c138.. set switches between different
         * lights). Log c120..c155 with the draw so the patch intensity
         * (dW of the following [GPU-WARM] line) can be tied to them. */
        {
            char cl[4096];
            int cn = snprintf(cl, sizeof(cl), "[GPU-LIGHTC] f=%u i=%u n=%u",
                              frame, index, count);
            for (unsigned c = 120; c < 156 && cn < (int)sizeof(cl) - 64; c++) {
                float v[4];
                memcpy(v, pg->vsh_constants[c], sizeof(v));
                cn += snprintf(cl + cn, sizeof(cl) - cn, " c%u=%.4g,%.4g,%.4g,%.4g",
                               c, v[0], v[1], v[2], v[3]);
            }
            cn += snprintf(cl + cn, sizeof(cl) - cn, "\n");
            fwrite(cl, 1, cn, stderr);
        }
        for (int t = 0; t < 4; t++) {
            if (!pgraph_is_texture_enabled(pg, t))
                continue;
            TextureShape sh = pgraph_get_texture_shape(pg, t);
            hwaddr ta = pgraph_get_texture_phys_addr(pg, t);
            SurfaceBinding *ts = pgraph_gl_surface_get(d, ta);
            SurfaceBinding *tw = pgraph_gl_surface_get_within(d, ta);
            TextureBinding *tb = r->texture_binding[t];
            fprintf(stderr, "[GPU-LIGHTTEX] f=%u i=%u n=%u stage=%d addr=%08x "
                    "fmt=%u %ux%u cube=%d surf=%s:%08x %ux%u dt=%d ft=%d "
                    "compat=%d within=%08x tex_dt=%d hash=%08x "
                    "shadow=%08x\n", frame, index, count, t, (uint32_t)ta,
                    sh.color_format, sh.width, sh.height, sh.cubemap,
                    ts ? (ts->color ? "color" : "zeta") : "none",
                    ts ? (uint32_t)ts->vram_addr : 0, ts ? ts->width : 0,
                    ts ? ts->height : 0, ts ? ts->draw_time : -1,
                    ts ? ts->frame_time : -1,
                    ts ? pgraph_gl_check_surface_to_texture_compatibility(
                             ts, &sh) : -1,
                    tw ? (uint32_t)tw->vram_addr : 0,
                    tb ? tb->draw_time : -1,
                    tb ? (uint32_t)tb->data_hash : 0,
                    pgraph_reg_r(pg, NV_PGRAPH_SHADOWCTL));
        }
    }
    if (sb->vram_addr == last_addr) {
        char line[256];
        int len = snprintf(line, sizeof(line),
                           "[GPU-WARM] f=%u i=%u t0=%08x p=%08x b=%08x n=%u "
                           "dW=%lld dL=%lld dA=%lld A=%lld\n", frame, index,
                           pgraph_reg_r(pg, NV_PGRAPH_TEXOFFSET0),
                           r->shader_binding && r->shader_binding->state.vertex_program
                               ? (uint32_t)recomp_vsh_hash_program(
                                     &r->shader_binding->state) : 0,
                           pgraph_reg_r(pg, NV_PGRAPH_BLEND), count,
                           (long long)(w - last_w), (long long)(l - last_l),
                           (long long)(a - last_a), (long long)a);
        fwrite(line, 1, len, stderr);
    }
    last_addr = (uint32_t)sb->vram_addr;
    last_w = w;
    last_l = l;
    last_a = a;
}

void pgraph_gl_flush_draw(NV2AState *d)
{
    PGRAPHState *pg = &d->pgraph;
    PGRAPHGLState *r = pg->gl_renderer_state;

    if (!(r->color_binding || r->zeta_binding)) {
        return;
    }
    assert(r->shader_binding);

    if (pg->draw_arrays_length) {
        NV2A_GL_DPRINTF(false, "Draw Arrays");
        nv2a_profile_inc_counter(NV2A_PROF_DRAW_ARRAYS);
        assert(pg->inline_elements_length == 0);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        recomp_trace_vsh_draw(d, r, "arrays", pg->draw_arrays_length);
        recomp_trace_drawset(d, r, pg->draw_arrays_length);
        pgraph_gl_bind_vertex_attributes(d, pg->draw_arrays_min_start,
                                      pg->draw_arrays_max_count - 1,
                                      false, 0,
                                      pg->draw_arrays_max_count - 1);
        glMultiDrawArrays(r->shader_binding->gl_primitive_mode,
                          pg->draw_arrays_start,
                          pg->draw_arrays_count,
                          pg->draw_arrays_length);
    } else if (pg->inline_elements_length) {
        NV2A_GL_DPRINTF(false, "Inline Elements");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ELEMENTS);
        assert(pg->inline_buffer_length == 0);
        assert(pg->inline_array_length == 0);

        recomp_trace_vsh_draw(d, r, "elements", pg->inline_elements_length);
        recomp_trace_drawset(d, r, pg->inline_elements_length);
        uint32_t min_element = (uint32_t)-1;
        uint32_t max_element = 0;
        for (int i=0; i < pg->inline_elements_length; i++) {
            max_element = MAX(pg->inline_elements[i], max_element);
            min_element = MIN(pg->inline_elements[i], min_element);
        }

        pgraph_gl_bind_vertex_attributes(
                d, min_element, max_element, false, 0,
                pg->inline_elements[pg->inline_elements_length - 1]);

        VertexKey k;
        memset(&k, 0, sizeof(VertexKey));
        k.count = pg->inline_elements_length;
        k.gl_type = GL_UNSIGNED_INT;
        k.gl_normalize = GL_FALSE;
        k.stride = sizeof(uint32_t);
        uint64_t h = fast_hash((uint8_t*)pg->inline_elements,
                               pg->inline_elements_length * 4);

        LruNode *node = lru_lookup(&r->element_cache, h, &k);
        VertexLruNode *found = container_of(node, VertexLruNode, node);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, found->gl_buffer);
        if (!found->initialized) {
            nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_4);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                         pg->inline_elements_length * 4,
                         pg->inline_elements, GL_STATIC_DRAW);
            found->initialized = true;
        } else {
            nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_4_NOTDIRTY);
        }
        glDrawElements(r->shader_binding->gl_primitive_mode,
                       pg->inline_elements_length, GL_UNSIGNED_INT,
                       (void *)0);
    } else if (pg->inline_buffer_length) {
        NV2A_GL_DPRINTF(false, "Inline Buffer");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_BUFFERS);
        assert(pg->inline_array_length == 0);

        recomp_trace_vsh_draw(d, r, "inline-buffer", pg->inline_buffer_length);
        recomp_trace_drawset(d, r, pg->inline_buffer_length);
        if (pg->compressed_attrs) {
            pg->compressed_attrs = 0;
            pgraph_gl_bind_shaders(pg);
        }

        for (int i = 0; i < NV2A_VERTEXSHADER_ATTRIBUTES; i++) {
            VertexAttribute *attr = &pg->vertex_attributes[i];
            if (attr->inline_buffer_populated) {
                nv2a_profile_inc_counter(NV2A_PROF_GEOM_BUFFER_UPDATE_3);
                glBindBuffer(GL_ARRAY_BUFFER, r->gl_inline_buffer[i]);
                glBufferData(GL_ARRAY_BUFFER,
                             pg->inline_buffer_length * sizeof(float) * 4,
                             attr->inline_buffer, GL_STREAM_DRAW);
                glVertexAttribPointer(i, 4, GL_FLOAT, GL_FALSE, 0, 0);
                glEnableVertexAttribArray(i);
                attr->inline_buffer_populated = false;
                memcpy(attr->inline_value,
                       attr->inline_buffer + (pg->inline_buffer_length - 1) * 4,
                       sizeof(attr->inline_value));
            } else {
                glDisableVertexAttribArray(i);
                glVertexAttrib4fv(i, attr->inline_value);
            }
        }

        glDrawArrays(r->shader_binding->gl_primitive_mode,
                     0, pg->inline_buffer_length);
    } else if (pg->inline_array_length) {
        NV2A_GL_DPRINTF(false, "Inline Array");
        nv2a_profile_inc_counter(NV2A_PROF_INLINE_ARRAYS);

        recomp_trace_vsh_draw(d, r, "inline-array", pg->inline_array_length);
        recomp_trace_drawset(d, r, pg->inline_array_length);
        unsigned int index_count = pgraph_gl_bind_inline_array(d);
        glDrawArrays(r->shader_binding->gl_primitive_mode,
                     0, index_count);
    } else {
        NV2A_GL_DPRINTF(true, "EMPTY NV097_SET_BEGIN_END");
        NV2A_UNCONFIRMED("EMPTY NV097_SET_BEGIN_END");
    }
    recomp_trace_warm(d, r, pg->draw_arrays_length + pg->inline_elements_length
                                + pg->inline_buffer_length
                                + pg->inline_array_length);
}
