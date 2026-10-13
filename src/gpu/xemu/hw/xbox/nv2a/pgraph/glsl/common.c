/*
 * Geforce NV2A PGRAPH GLSL Shader Generator
 *
 * Copyright (c) 2024 Matt Borgerson
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

#include "common.h"
#include <stdlib.h>

/* xboxrecomp (docs Update 623): the NV2A computes a vertex position the
 * same way in every pass, and multipass titles rely on that with
 * depth func EQUAL (lighting/shadow passes over a depth prepass). Each
 * pass is a different host shader, and without "invariant" the host
 * compiler may optimise the position math differently per program, so
 * EQUAL fails on scattered pixels and they flicker as the view moves.
 * RECOMP_GLSL_INVARIANT=0 switches it off. */
const char *pgraph_glsl_invariant_position(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_GLSL_INVARIANT");
        on = !(e && e[0] == '0');
    }
    return on ? "invariant gl_Position;\n\n" : "";
}

/* xboxrecomp (docs Update 625): RECOMP_DEPTH_FLOOR=1, see psh.c. */
bool pgraph_glsl_depth_floor(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DEPTH_FLOOR");
        on = e && e[0] == '1';
    }
    return on;
}

/* xboxrecomp (docs Update 626): RECOMP_DEPTH_EXACT=1, see psh.c. */
bool pgraph_glsl_depth_exact(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("RECOMP_DEPTH_EXACT");
        on = e && e[0] == '1';
    }
    return on;
}

MString *pgraph_get_glsl_vtx_header(MString *out, bool location, bool smooth, bool in, bool prefix, bool array)
{
    const char *smooth_s = "";
    const char *flat_s = "flat ";
    const char *qualifier_s = smooth ? smooth_s : flat_s;
    const char *in_out_s = in ? "in" : "out";
    const char *float_s = "float";
    const char *vec4_s = "vec4";
    const char *prefix_s = prefix ? "v_" : "";
    const char *suffix_s = array ? "[]" : "";
    const struct {
        const char *qualifier, *type, *name;
    } attr[] = {
        { qualifier_s, vec4_s,  "vtxD0"  },
        { qualifier_s, vec4_s,  "vtxD1"  },
        { qualifier_s, vec4_s,  "vtxB0"  },
        { qualifier_s, vec4_s,  "vtxB1"  },
        { smooth_s,    float_s, "vtxFog" },
        { smooth_s,    vec4_s,  "vtxT0"  },
        { smooth_s,    vec4_s,  "vtxT1"  },
        { smooth_s,    vec4_s,  "vtxT2"  },
        { smooth_s,    vec4_s,  "vtxT3"  },
        /* xboxrecomp (docs Update 626): screen-space z, interpolated
         * linearly in screen space like the NV2A rasterizer does, for
         * RECOMP_DEPTH_EXACT (see psh.c). */
        { "noperspective ", float_s, "vtxZ" },
    };

    for (int i = 0; i < ARRAY_SIZE(attr); i++) {
        if (location) {
            mstring_append_fmt(out, "layout(location = %d) ", i);
        }
        mstring_append_fmt(out, "%s%s %s %s%s%s;\n", attr[i].qualifier,
                           in_out_s, attr[i].type, prefix_s, attr[i].name,
                           suffix_s);
    }

    return out;
}
