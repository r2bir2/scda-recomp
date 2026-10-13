/*
 * NV2A vertex-program (vertex shader) interpreter -- a pure, game-agnostic
 * model of the Kelvin (NV097) programmable vertex pipeline's instruction
 * semantics, register file and output-register write-back routing.
 *
 * Every field layout, opcode formula and write-back routing rule here is
 * checked directly against the local xemu reference source
 * (hw/xbox/nv2a/pgraph/glsl/vsh-prog.c: field_mapping[], convert_c_register(),
 * mac_opcode_params[]/ilu_opcode[]/decode_opcode()'s R1-forcing) and against
 * this project's own already-shipping D3D8 vertex-shader translator
 * (src/d3d/d3d8_vsh.c, which independently agrees on the R12/oPos alias).
 * Nothing here is specific to any one title's program.
 */
#ifndef XBOXRECOMP_VSH_INTERP_H
#define XBOXRECOMP_VSH_INTERP_H

#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NV2A_MAX_TRANSFORM_PROGRAM_LENGTH 136   /* instructions, 4 dwords each */
#define NV2A_VERTEXSHADER_CONSTANTS       192
#define NV2A_VSH_NUM_ATTRIBUTES           16
#define NV2A_VSH_NUM_OUTPUTS              16

typedef struct { float x[4]; } Vec4;

/* Output-register indices, from xemu's out_reg_name[]. Slots 1, 2, 13, 14 are
 * reserved/unused on real hardware (xemu itself only names 0,3-12,15); slot
 * 15 (A0.x) is not modelled as a Vec4 here since address-register writes
 * happen through the dedicated ARL opcode, not this array -- see
 * vsh_run_program()'s comment on out_addr==15. */
enum {
    VSH_OREG_POS = 0,
    VSH_OREG_D0  = 3,
    VSH_OREG_D1  = 4,
    VSH_OREG_FOG = 5,
    VSH_OREG_PTS = 6,
    VSH_OREG_B0  = 7,
    VSH_OREG_B1  = 8,
    VSH_OREG_T0  = 9,
    VSH_OREG_T1  = 10,
    VSH_OREG_T2  = 11,
    VSH_OREG_T3  = 12,
};

/* MAC/ILU opcode numbers, matching field values directly (xemu's
 * mac_opcode[]/ilu_opcode[] array order). */
enum {
    VSH_MAC_NOP = 0, VSH_MAC_MOV, VSH_MAC_MUL, VSH_MAC_ADD, VSH_MAC_MAD,
    VSH_MAC_DP3, VSH_MAC_DPH, VSH_MAC_DP4, VSH_MAC_DST, VSH_MAC_MIN,
    VSH_MAC_MAX, VSH_MAC_SLT, VSH_MAC_SGE, VSH_MAC_ARL,
};
enum {
    VSH_ILU_NOP = 0, VSH_ILU_MOV, VSH_ILU_RCP, VSH_ILU_RCC, VSH_ILU_RSQ,
    VSH_ILU_EXP, VSH_ILU_LOG, VSH_ILU_LIT,
};

typedef struct {
    Vec4 oreg[NV2A_VSH_NUM_OUTPUTS]; /* all 16 slots valid; unused ones hold
                                       * the hardware default (0,0,0,1) */
    int steps;        /* instructions actually executed */
    int status;        /* 0 = stopped on FLD_FINAL (normal completion),
                        * 1 = ran past the last captured/loaded row,
                        * -2 = hit a MAC or ILU opcode not implemented here */
    uint32_t stop_row;  /* row index where execution stopped */
    uint32_t stop_mac;  /* MAC opcode at stop_row (meaningful for status==-2) */
    uint32_t stop_ilu;  /* ILU opcode at stop_row (meaningful for status==-2) */
} VshResult;

/* Runs the vertex program starting at row `start_pc`, treating rows
 * [0, program_len) as the only ones actually loaded (matching
 * NV097_SET_TRANSFORM_PROGRAM's capture semantics: an out-of-range pc simply
 * ends the run rather than reading garbage). `consts` is the full 192-entry
 * constant bank, raw dword bit patterns (reinterpreted as float, matching
 * how NV097_SET_TRANSFORM_CONSTANT stores them). `vattr` is v0..v15 for the
 * vertex being evaluated -- callers must fill unbound attribute slots with
 * the hardware default (0,0,0,1), matching xemu's uniform default. If
 * `trace_fp` is non-NULL, one diagnostic line per executed instruction is
 * written to it. */
void vsh_run_program(const uint32_t program[][4], uint32_t program_len,
                      uint32_t start_pc,
                      const uint32_t consts[NV2A_VERTEXSHADER_CONSTANTS][4],
                      const Vec4 vattr[NV2A_VSH_NUM_ATTRIBUTES],
                      VshResult *out, FILE *trace_fp);

#ifdef __cplusplus
}
#endif

#endif /* XBOXRECOMP_VSH_INTERP_H */
