/*
 * Regression tests for the GP/EP DSP port (xemu DSP56300 interpreter,
 * src/apu/dsp; adapted gp_ep in src/apu/apu_dsp.c). docs Update 343.
 *
 * Build/run:  cmake --build build/scda_upgrade --config RelWithDebInfo
 *                   --target apu_dsp_tests
 *             build/scda_upgrade/xboxrecomp/src/apu/RelWithDebInfo/apu_dsp_tests.exe
 *
 * The program used for execution tests:
 *   P:0  0x244200  move #$42,x0        (8-bit short immediate, MSB-aligned)
 *   P:1  0x441000  move x0,x:$10       (X short absolute, register -> memory)
 *   P:2  0x0C0002  jmp  $2             (spin)
 */
#include "apu_state.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern uint8_t *g_apu_ram_ptr;

static int g_fail, g_pass;

#define CHECK(cond, ...) do { \
    if (cond) { g_pass++; } \
    else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); \
           printf(__VA_ARGS__); printf("\n"); } \
} while (0)

static const uint32_t prog[] = { 0x244200, 0x441000, 0x0C0002 };

/* Stand-alone scratch/fifo callbacks for tests that use a bare DSPState. */
static uint32_t g_scratch_words[0x800];
static void test_scratch_rw(void *opaque, uint8_t *ptr, uint32_t addr,
                            size_t len, bool dir)
{
    (void)opaque;
    if (dir)
        memcpy((uint8_t *)g_scratch_words + addr, ptr, len);
    else
        memcpy(ptr, (uint8_t *)g_scratch_words + addr, len);
}
static void test_fifo_rw(void *opaque, uint8_t *ptr, unsigned int index,
                         size_t len, bool dir)
{
    (void)opaque; (void)ptr; (void)index; (void)len; (void)dir;
}

static void test_memory_spaces(void)
{
    DSPState *dsp = dsp_init(NULL, test_scratch_rw, test_fifo_rw, true);
    dsp_write_memory(dsp, 'X', 0x10, 0x123456);
    dsp_write_memory(dsp, 'Y', 0x10, 0x654321);
    dsp_write_memory(dsp, 'P', 0x10, 0xABCDEF);
    CHECK(dsp_read_memory(dsp, 'X', 0x10) == 0x123456, "X ram");
    CHECK(dsp_read_memory(dsp, 'Y', 0x10) == 0x654321, "Y ram");
    CHECK(dsp_read_memory(dsp, 'P', 0x10) == 0xABCDEF, "P ram");
    /* DSP words are 24-bit; upstream asserts on anything wider (QEMU keeps
     * asserts on, and so do we -- see dsp/shim/qemu/osdep.h). */
    /* Never-written DSP RAM holds xemu's 0xCA fill (dsp_c_init). */
    CHECK(dsp_read_memory(dsp, 'Y', 0x7FF) == 0xCACACACA,
          "untouched Y fill 0x%08X", dsp_read_memory(dsp, 'Y', 0x7FF));
    dsp_destroy(dsp);
}

static void test_execution_and_reset(void)
{
    DSPState *dsp = dsp_init(NULL, test_scratch_rw, test_fifo_rw, true);
    for (unsigned i = 0; i < 3; i++)
        dsp_write_memory(dsp, 'P', i, prog[i]);
    dsp_invalidate_opcache(dsp);
    dsp_sync_to_vm(dsp);
    CHECK(dsp->core.pc == 0, "pc after init = 0x%X", dsp->core.pc);

    dsp_set_halt_requested(dsp, false);
    dsp_set_cycle_count(dsp, 0);
    dsp_run(dsp, 200);
    dsp_sync_to_vm(dsp);
    CHECK(dsp_read_memory(dsp, 'X', 0x10) == 0x420000,
          "move #$42,x0 ; move x0,x:$10 -> x:$10 = 0x%06X",
          dsp_read_memory(dsp, 'X', 0x10));
    CHECK(dsp->core.pc == 2, "pc spins at jmp $2, pc=0x%X", dsp->core.pc);
    CHECK(dsp_get_cycle_count(dsp) > 0, "cycles advanced");

    dsp_reset(dsp);
    dsp_sync_to_vm(dsp);
    CHECK(dsp->core.pc == 0, "pc after reset = 0x%X", dsp->core.pc);
    dsp_destroy(dsp);
}

/* A minimal MCPXAPUState with real GP/EP DSPs and a small guest RAM. */
#define TEST_RAM_SIZE 0x04000000u
static MCPXAPUState *make_apu(void)
{
    MCPXAPUState *d = calloc(1, sizeof(*d));
    d->ram_ptr = calloc(1, TEST_RAM_SIZE);
    g_apu_ram_ptr = d->ram_ptr;
    qemu_mutex_init(&d->lock);
    mcpx_apu_dsp_init(d);
    return d;
}

static void test_gp_ep_windows_separate(void)
{
    MCPXAPUState *d = make_apu();

    /* DSP memory through the MMIO windows. */
    mcpx_apu_gp_write(d, NV_PAPU_GPXMEM + 4 * 0x20, 0x111111, 4);
    mcpx_apu_ep_write(d, NV_PAPU_EPXMEM + 4 * 0x20, 0x222222, 4);
    CHECK(mcpx_apu_gp_read(d, NV_PAPU_GPXMEM + 4 * 0x20, 4) == 0x111111,
          "GP X via window");
    CHECK(mcpx_apu_ep_read(d, NV_PAPU_EPXMEM + 4 * 0x20, 4) == 0x222222,
          "EP X via window");
    CHECK(dsp_read_memory(d->gp.dsp, 'X', 0x20) == 0x111111 &&
          dsp_read_memory(d->ep.dsp, 'X', 0x20) == 0x222222,
          "GP and EP keep separate X memory");

    /* P words must be initialised before they are read (upstream asserts). */
    mcpx_apu_ep_write(d, NV_PAPU_EPPMEM + 4 * 5, 0x000001, 4);
    mcpx_apu_gp_write(d, NV_PAPU_GPPMEM + 4 * 5, 0x0C0005, 4);
    CHECK(mcpx_apu_gp_read(d, NV_PAPU_GPPMEM + 4 * 5, 4) == 0x0C0005,
          "GP P via window");
    CHECK(mcpx_apu_ep_read(d, NV_PAPU_EPPMEM + 4 * 5, 4) == 0x000001,
          "EP P untouched by GP write (EP P:5=0x%06X)",
          (unsigned)mcpx_apu_ep_read(d, NV_PAPU_EPPMEM + 4 * 5, 4));

    /* Plain registers are stored per processor. */
    mcpx_apu_gp_write(d, 0x100, 0xAAAA, 4);
    mcpx_apu_ep_write(d, 0x100, 0xBBBB, 4);
    CHECK(mcpx_apu_gp_read(d, 0x100, 4) == 0xAAAA &&
          mcpx_apu_ep_read(d, 0x100, 4) == 0xBBBB, "GP/EP plain regs");
}

/* GPRST 0 -> both bits: the bootrom path DMAs 0x800 words of scratch into
 * P memory through the GP scratch scatter-gather list in guest RAM. Then one
 * APU frame runs the program. */
static void test_gp_bootstrap_and_frame(void)
{
    MCPXAPUState *d = make_apu();
    const uint32_t sge_table = 0x00100000, scratch_page = 0x00200000;
    float mixbins[NUM_MIXBINS][NUM_SAMPLES_PER_FRAME];

    memset(mixbins, 0, sizeof(mixbins));
    /* SGE entry 0 -> scratch page; the program is its first words. */
    *(uint32_t *)(d->ram_ptr + sge_table) = scratch_page;
    d->regs[NV_PAPU_GPSADDR] = sge_table;
    d->regs[NV_PAPU_GPSMAXSGE] = 1;
    for (unsigned i = 0; i < 3; i++)
        *(uint32_t *)(d->ram_ptr + scratch_page + 4 * i) = prog[i];
    /* Scratch spans 0x800 words = 2 pages; point entry 1 somewhere valid. */
    *(uint32_t *)(d->ram_ptr + sge_table + 8) = scratch_page + 0x1000;

    mcpx_apu_gp_write(d, NV_PAPU_GPRST, 0, 4);
    mcpx_apu_gp_write(d, NV_PAPU_GPRST,
                      NV_PAPU_GPRST_GPRST | NV_PAPU_GPRST_GPDSPRST, 4);
    CHECK(mcpx_apu_gp_read(d, NV_PAPU_GPPMEM + 4 * 1, 4) == prog[1],
          "bootstrap copied scratch into P memory (P:1=0x%06X)",
          (unsigned)mcpx_apu_gp_read(d, NV_PAPU_GPPMEM + 4 * 1, 4));

    mcpx_apu_dsp_frame(d, mixbins);
    CHECK(mcpx_apu_gp_read(d, NV_PAPU_GPXMEM + 4 * 0x10, 4) == 0x420000,
          "one frame executed the program (X:$10=0x%06X)",
          (unsigned)mcpx_apu_gp_read(d, NV_PAPU_GPXMEM + 4 * 0x10, 4));
    CHECK(g_dbg.gp.cycles > 0, "GP cycle counter %u", g_dbg.gp.cycles);
    CHECK(dsp_read_memory(d->ep.dsp, 'X', 0x10) == 0xCACACACA,
          "EP did not run the GP program (EP X:$10=0x%06X)",
          dsp_read_memory(d->ep.dsp, 'X', 0x10));
}

int main(void)
{
    test_memory_spaces();
    test_execution_and_reset();
    test_gp_ep_windows_separate();
    test_gp_bootstrap_and_frame();
    printf("apu_dsp_tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
