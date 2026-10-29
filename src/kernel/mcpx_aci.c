/*
 * MCPX Audio Codec Interface (AC'97 controller + codec) at guest VA
 * 0xFEC00000, ported from xemu hw/audio/ac97.c (VirtualBox OSE / QEMU,
 * GPL v2+) and hw/xbox/mcpx/aci.c (LGPL v2+) at xemu 0.8.66 HEAD
 * 8667193001e, XBOX build (no PCM-in / mic voices).
 *
 * Why (docs Update 355): SCDA's DirectSound resets its PCM-out bus master by
 * writing CR_RR to 0xFEC0011B and then spins `test cl,cl / jne $-2` on the
 * value it read back (sub_004D88ED, XBE 0x4D891A..0x4D8925). The controller
 * clears RR as part of the reset, so on hardware and in xemu the read gives
 * 0. Backed as plain memory the bit stayed set and the main thread spun
 * there forever at DISPATCH_LEVEL -- which also kept the D3D DPC (flip /
 * interrupt re-enable) from ever running.
 *
 * Layout as in aci.c: native audio mixer (codec registers) at +0x000
 * (0x100 bytes), native audio bus master at +0x100 (0x80 bytes). The page is
 * PAGE_NOACCESS; faults are decoded by src/kernel/mmio_decode.c.
 *
 * Differences from xemu, all at the audio-backend boundary:
 *  - PCM out has no host voice yet: a pacing thread consumes the DMA stream
 *    at the codec's 48 kHz x 16-bit stereo rate, as xemu's audio backend
 *    does, and discards the samples (buffer completion, CIV/PICB and the
 *    interrupt behave as in xemu). Feeding them to XAudio2 is the sound task.
 *  - Unknown mixer writes / byte reads are logged instead of assert(0).
 *  - IRQ: the PCI line is the title's level-6 interrupt.
 */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "mmio_decode.h"

extern void xbox_irq_set_line(unsigned line, int asserted);
extern void xbox_irq_deliver_from_mmio(void);

#define ACI_IRQ_LINE  6
#define ACI_MMIO_SIZE 0x1000u
#define ACI_NABM_BASE 0x100u
#define ACI_NABM_SIZE 0x80u
#define ACI_PHYS_LIMIT 0x08000000u   /* 128 MiB: largest Xbox RAM */

/* ---- hw/audio/ac97.h ---- */
enum {
    AC97_Reset                     = 0x00,
    AC97_Master_Volume_Mute        = 0x02,
    AC97_Headphone_Volume_Mute     = 0x04,
    AC97_Master_Volume_Mono_Mute   = 0x06,
    AC97_Master_Tone_RL            = 0x08,
    AC97_PC_BEEP_Volume_Mute       = 0x0A,
    AC97_Phone_Volume_Mute         = 0x0C,
    AC97_Mic_Volume_Mute           = 0x0E,
    AC97_Line_In_Volume_Mute       = 0x10,
    AC97_CD_Volume_Mute            = 0x12,
    AC97_Video_Volume_Mute         = 0x14,
    AC97_Aux_Volume_Mute           = 0x16,
    AC97_PCM_Out_Volume_Mute       = 0x18,
    AC97_Record_Select             = 0x1A,
    AC97_Record_Gain_Mute          = 0x1C,
    AC97_Record_Gain_Mic_Mute      = 0x1E,
    AC97_General_Purpose           = 0x20,
    AC97_3D_Control                = 0x22,
    AC97_AC_97_RESERVED            = 0x24,
    AC97_Powerdown_Ctrl_Stat       = 0x26,
    AC97_Extended_Audio_ID         = 0x28,
    AC97_Extended_Audio_Ctrl_Stat  = 0x2A,
    AC97_PCM_Front_DAC_Rate        = 0x2C,
    AC97_PCM_Surround_DAC_Rate     = 0x2E,
    AC97_PCM_LFE_DAC_Rate          = 0x30,
    AC97_PCM_LR_ADC_Rate           = 0x32,
    AC97_MIC_ADC_Rate              = 0x34,
    AC97_6Ch_Vol_C_LFE_Mute        = 0x36,
    AC97_6Ch_Vol_L_R_Surround_Mute = 0x38,
    AC97_Vendor_Reserved           = 0x58,
    AC97_Sigmatel_Analog           = 0x6c,
    AC97_Sigmatel_Dac2Invert       = 0x6e,
    AC97_Vendor_ID1                = 0x7c,
    AC97_Vendor_ID2                = 0x7e
};

#define EACS_VRA 1
#define EACS_VRM 8

/* ---- hw/audio/ac97_int.h ---- */
enum {
    PI_INDEX = 0,   /* PCM in */
    PO_INDEX,       /* PCM out */
    MC_INDEX,       /* Mic in */
    SO_INDEX = 7,   /* SPDIF out */
    LAST_INDEX
};

typedef struct BD {
    uint32_t addr;
    uint32_t ctl_len;
} BD;

typedef struct AC97BusMasterRegs {
    uint32_t bdbar;
    uint8_t civ;
    uint8_t lvi;
    uint16_t sr;
    uint16_t picb;
    uint8_t piv;
    uint8_t cr;
    unsigned int bd_valid;
    BD bd;
} AC97BusMasterRegs;

typedef struct AC97LinkState {
    uint32_t glob_cnt;
    uint32_t glob_sta;
    uint32_t cas;
    uint32_t last_samp;
    AC97BusMasterRegs bm_regs[LAST_INDEX];
    uint8_t mixer_data[256];
    int po_active;              /* AUD_set_active_out(voice_po) */
    int invalid_freq[LAST_INDEX];
    uint8_t silence[128];
    int bup_flag;
} AC97LinkState;

/* ---- hw/audio/ac97.c ---- */
#define SR_FIFOE 16
#define SR_BCIS  8
#define SR_LVBCI 4
#define SR_CELV  2
#define SR_DCH   1
#define SR_VALID_MASK ((1 << 5) - 1)
#define SR_WCLEAR_MASK (SR_FIFOE | SR_BCIS | SR_LVBCI)
#define SR_RO_MASK (SR_DCH | SR_CELV)
#define SR_INT_MASK (SR_FIFOE | SR_BCIS | SR_LVBCI)

#define CR_IOCE  16
#define CR_FEIE  8
#define CR_LVBIE 4
#define CR_RR    2
#define CR_RPBM  1
#define CR_VALID_MASK ((1 << 5) - 1)
#define CR_DONT_CLEAR_MASK (CR_IOCE | CR_FEIE | CR_LVBIE)

#define GC_WR    4
#define GC_CR    2
#define GC_VALID_MASK ((1 << 6) - 1)

#define GS_MD3   (1 << 17)
#define GS_AD3   (1 << 16)
#define GS_RCS   (1 << 15)
#define GS_B3S12 (1 << 14)
#define GS_B2S12 (1 << 13)
#define GS_B1S12 (1 << 12)
#define GS_S1R1  (1 << 11)
#define GS_S0R1  (1 << 10)
#define GS_S1CR  (1 << 9)
#define GS_S0CR  (1 << 8)
#define GS_MINT  (1 << 7)
#define GS_POINT (1 << 6)
#define GS_PIINT (1 << 5)
#define GS_SOINT (1 << 4)
#define GS_RSRVD (1 << 3)
#define GS_MOINT (1 << 2)
#define GS_MIINT (1 << 1)
#define GS_GSCI  1
#define GS_RO_MASK (GS_B3S12 | GS_B2S12 | GS_B1S12 | GS_S1CR | GS_S0CR | \
                    GS_MINT | GS_POINT | GS_PIINT | GS_SOINT | GS_RSRVD | \
                    GS_MOINT | GS_MIINT)
#define GS_VALID_MASK ((1 << 18) - 1)
#define GS_WCLEAR_MASK (GS_RCS | GS_S1R1 | GS_S0R1 | GS_GSCI)

#define BD_IOC (1u << 31)
#define BD_BUP (1u << 30)

#define REC_MASK 7

enum {
    BUP_SET = 1,
    BUP_LAST = 2
};

#define MKREGS(prefix, start)                   \
enum {                                          \
    prefix ## _BDBAR = start,                   \
    prefix ## _CIV = start + 4,                 \
    prefix ## _LVI = start + 5,                 \
    prefix ## _SR = start + 6,                  \
    prefix ## _PICB = start + 8,                \
    prefix ## _PIV = start + 10,                \
    prefix ## _CR = start + 11                  \
}

MKREGS(PI, PI_INDEX * 16);
MKREGS(PO, PO_INDEX * 16);
MKREGS(MC, MC_INDEX * 16);
MKREGS(SO, SO_INDEX * 16);

enum {
    GLOB_CNT = 0x2c,
    GLOB_STA = 0x30,
    CAS      = 0x34
};

#define GET_BM(index) (((index) >> 4) & 7)

static AC97LinkState s_ac97;
static CRITICAL_SECTION s_lock;
static uint8_t *s_phys;                  /* host address of physical 0 */
static uintptr_t s_mmio_lo, s_mmio_hi;   /* host range of the trapped page */
static volatile LONG s_attached;

static void aci_log_once(const char *what, uint32_t addr, uint32_t val)
{
    static volatile LONG logged;
    if (InterlockedIncrement(&logged) <= 32) {
        fprintf(stderr, "[ACI] %s 0x%03X (0x%X)\n", what, addr, val);
        fflush(stderr);
    }
}

/* dma_memory_read on Xbox physical addresses. */
static void phys_read(uint32_t addr, void *buf, uint32_t len)
{
    if (addr < ACI_PHYS_LIMIT && len <= ACI_PHYS_LIMIT - addr)
        memcpy(buf, s_phys + addr, len);
    else
        memset(buf, 0, len);
}

static void pci_irq_set(int level)
{
    xbox_irq_set_line(ACI_IRQ_LINE, level);
}

static void fetch_bd(AC97LinkState *s, AC97BusMasterRegs *r)
{
    uint8_t b[8];

    phys_read(r->bdbar + r->civ * 8, b, 8);
    r->bd_valid = 1;
    r->bd.addr = (*(uint32_t *)&b[0]) & ~3u;
    r->bd.ctl_len = *(uint32_t *)&b[4];
    r->picb = r->bd.ctl_len & 0xffff;
    (void)s;
}

static void update_sr(AC97LinkState *s, AC97BusMasterRegs *r, uint32_t new_sr)
{
    int event = 0;
    int level = 0;
    uint32_t new_mask = new_sr & SR_INT_MASK;
    uint32_t old_mask = r->sr & SR_INT_MASK;
    static const uint32_t masks[] = {GS_PIINT, GS_POINT, GS_MINT};
    ptrdiff_t index = r - s->bm_regs;

    if (new_mask ^ old_mask) {
        if (!new_mask) {
            event = 1;
            level = 0;
        } else {
            if ((new_mask & SR_LVBCI) && (r->cr & CR_LVBIE)) {
                event = 1;
                level = 1;
            }
            if ((new_mask & SR_BCIS) && (r->cr & CR_IOCE)) {
                event = 1;
                level = 1;
            }
        }
    }

    r->sr = (uint16_t)new_sr;

    if (!event) {
        return;
    }

    /* xemu indexes masks[] with the channel; only PI/PO/MC have a status
     * bit there (SPDIF out never raises an event: it never transfers). */
    if (level) {
        if (index < 3)
            s->glob_sta |= masks[index];
        pci_irq_set(1);
    } else {
        if (index < 3)
            s->glob_sta &= ~masks[index];
        pci_irq_set(0);
    }
}

static void voice_set_active(AC97LinkState *s, int bm_index, int on)
{
    if (bm_index == PO_INDEX)
        s->po_active = on;
}

static void reset_bm_regs(AC97LinkState *s, AC97BusMasterRegs *r)
{
    r->bdbar = 0;
    r->civ = 0;
    r->lvi = 0;
    update_sr(s, r, SR_DCH);
    r->picb = 0;
    r->piv = 0;
    r->cr = r->cr & CR_DONT_CLEAR_MASK;
    r->bd_valid = 0;

    voice_set_active(s, (int)(r - s->bm_regs), 0);
    memset(s->silence, 0, sizeof(s->silence));
}

static void mixer_store(AC97LinkState *s, uint32_t i, uint16_t v)
{
    if (i + 2 > sizeof(s->mixer_data)) {
        return;
    }
    s->mixer_data[i + 0] = v & 0xff;
    s->mixer_data[i + 1] = v >> 8;
}

static uint16_t mixer_load(AC97LinkState *s, uint32_t i)
{
    uint16_t val = 0xffff;

    if (i + 2 <= sizeof(s->mixer_data)) {
        val = s->mixer_data[i + 0] | (s->mixer_data[i + 1] << 8);
    }
    return val;
}

static void open_voice(AC97LinkState *s, int index, int freq)
{
    if (freq > 0) {
        s->invalid_freq[index] = 0;
    } else {
        s->invalid_freq[index] = freq;
        if (index == PO_INDEX)
            s->po_active = 0;
    }
}

static void reset_voices(AC97LinkState *s, uint8_t active[LAST_INDEX])
{
    uint16_t freq = mixer_load(s, AC97_PCM_Front_DAC_Rate);
    open_voice(s, PO_INDEX, freq);
    s->po_active = active[PO_INDEX];
}

static void set_volume(AC97LinkState *s, int index, uint32_t val)
{
    /* Stored as xemu does; the volume itself belongs to the host voice. */
    switch (index) {
    case AC97_Master_Volume_Mute:
        val &= 0xbf3f;
        mixer_store(s, index, (uint16_t)val);
        break;
    case AC97_PCM_Out_Volume_Mute:
        val &= 0x9f1f;
        mixer_store(s, index, (uint16_t)val);
        break;
    case AC97_Record_Gain_Mute:
        val &= 0x8f0f;
        mixer_store(s, index, (uint16_t)val);
        break;
    }
}

static void record_select(AC97LinkState *s, uint32_t val)
{
    uint8_t rs = val & REC_MASK;
    uint8_t ls = (val >> 8) & REC_MASK;
    mixer_store(s, AC97_Record_Select, rs | (ls << 8));
}

static void mixer_reset(AC97LinkState *s)
{
    uint8_t active[LAST_INDEX];

    memset(s->mixer_data, 0, sizeof(s->mixer_data));
    memset(active, 0, sizeof(active));
    mixer_store(s, AC97_Reset, 0x0000);
    mixer_store(s, AC97_Headphone_Volume_Mute, 0x0000);
    mixer_store(s, AC97_Master_Volume_Mono_Mute, 0x0000);
    mixer_store(s, AC97_Master_Tone_RL, 0x0000);
    mixer_store(s, AC97_PC_BEEP_Volume_Mute, 0x0000);
    mixer_store(s, AC97_Phone_Volume_Mute, 0x0000);
    mixer_store(s, AC97_Mic_Volume_Mute, 0x0000);
    mixer_store(s, AC97_Line_In_Volume_Mute, 0x0000);
    mixer_store(s, AC97_CD_Volume_Mute, 0x0000);
    mixer_store(s, AC97_Video_Volume_Mute, 0x0000);
    mixer_store(s, AC97_Aux_Volume_Mute, 0x0000);
    mixer_store(s, AC97_Record_Gain_Mic_Mute, 0x0000);
    mixer_store(s, AC97_General_Purpose, 0x0000);
    mixer_store(s, AC97_3D_Control, 0x0000);
    mixer_store(s, AC97_Powerdown_Ctrl_Stat, 0x000f);

    /* Sigmatel 9700 (STAC9700) */
    mixer_store(s, AC97_Vendor_ID1, 0x8384);
    mixer_store(s, AC97_Vendor_ID2, 0x7600);

    mixer_store(s, AC97_Extended_Audio_ID, 0x0809);
    mixer_store(s, AC97_Extended_Audio_Ctrl_Stat, 0x0009);
    mixer_store(s, AC97_PCM_Front_DAC_Rate, 0xbb80);
    mixer_store(s, AC97_PCM_Surround_DAC_Rate, 0xbb80);
    mixer_store(s, AC97_PCM_LFE_DAC_Rate, 0xbb80);
    mixer_store(s, AC97_PCM_LR_ADC_Rate, 0xbb80);
    mixer_store(s, AC97_MIC_ADC_Rate, 0xbb80);

    record_select(s, 0);
    set_volume(s, AC97_Master_Volume_Mute, 0x8000);
    set_volume(s, AC97_PCM_Out_Volume_Mute, 0x8808);
    set_volume(s, AC97_Record_Gain_Mute, 0x8808);

    reset_voices(s, active);
}

/* Native audio mixer */
static uint32_t nam_readb(AC97LinkState *s, uint32_t addr)
{
    (void)addr;
    s->cas = 0;
    return ~0U;
}

static uint32_t nam_readw(AC97LinkState *s, uint32_t addr)
{
    s->cas = 0;
    return mixer_load(s, addr);
}

static uint32_t nam_readl(AC97LinkState *s, uint32_t addr)
{
    (void)addr;
    s->cas = 0;
    return ~0U;
}

static void nam_writeb(AC97LinkState *s, uint32_t addr, uint32_t val)
{
    (void)addr; (void)val;
    s->cas = 0;
}

static void nam_writew(AC97LinkState *s, uint32_t addr, uint32_t val)
{
    s->cas = 0;
    switch (addr) {
    case AC97_Reset:
        mixer_reset(s);
        break;
    case AC97_Powerdown_Ctrl_Stat:
        val &= ~0x800f;
        val |= mixer_load(s, addr) & 0xf;
        mixer_store(s, addr, (uint16_t)val);
        break;
    case AC97_PCM_Out_Volume_Mute:
    case AC97_Master_Volume_Mute:
    case AC97_Record_Gain_Mute:
        set_volume(s, addr, val);
        break;
    case AC97_Record_Select:
        record_select(s, val);
        break;
    case AC97_Vendor_ID1:
    case AC97_Vendor_ID2:
    case AC97_Extended_Audio_ID:
        break;
    case AC97_Extended_Audio_Ctrl_Stat:
        if (!(val & EACS_VRA)) {
            mixer_store(s, AC97_PCM_Front_DAC_Rate, 0xbb80);
            mixer_store(s, AC97_PCM_LR_ADC_Rate,    0xbb80);
            open_voice(s, PI_INDEX, 48000);
            open_voice(s, PO_INDEX, 48000);
        }
        if (!(val & EACS_VRM)) {
            mixer_store(s, AC97_MIC_ADC_Rate, 0xbb80);
            open_voice(s, MC_INDEX, 48000);
        }
        mixer_store(s, AC97_Extended_Audio_Ctrl_Stat, (uint16_t)val);
        break;
    case AC97_PCM_Front_DAC_Rate:
        if (mixer_load(s, AC97_Extended_Audio_Ctrl_Stat) & EACS_VRA) {
            mixer_store(s, addr, (uint16_t)val);
            open_voice(s, PO_INDEX, val);
        }
        break;
    case AC97_MIC_ADC_Rate:
        if (mixer_load(s, AC97_Extended_Audio_Ctrl_Stat) & EACS_VRM) {
            mixer_store(s, addr, (uint16_t)val);
            open_voice(s, MC_INDEX, val);
        }
        break;
    case AC97_PCM_LR_ADC_Rate:
        if (mixer_load(s, AC97_Extended_Audio_Ctrl_Stat) & EACS_VRA) {
            mixer_store(s, addr, (uint16_t)val);
            open_voice(s, PI_INDEX, val);
        }
        break;
    case AC97_Headphone_Volume_Mute:
    case AC97_Master_Volume_Mono_Mute:
    case AC97_Master_Tone_RL:
    case AC97_PC_BEEP_Volume_Mute:
    case AC97_Phone_Volume_Mute:
    case AC97_Mic_Volume_Mute:
    case AC97_Line_In_Volume_Mute:
    case AC97_CD_Volume_Mute:
    case AC97_Video_Volume_Mute:
    case AC97_Aux_Volume_Mute:
    case AC97_Record_Gain_Mic_Mute:
    case AC97_General_Purpose:
    case AC97_3D_Control:
    case AC97_Sigmatel_Analog:
    case AC97_Sigmatel_Dac2Invert:
        /* None of the features in these regs are emulated, so they are RO */
        break;
    default:
        /* xemu: mixer_store + assert(0) */
        aci_log_once("unhandled mixer writew", addr, val);
        mixer_store(s, addr, (uint16_t)val);
        break;
    }
}

static void nam_writel(AC97LinkState *s, uint32_t addr, uint32_t val)
{
    (void)addr; (void)val;
    s->cas = 0;
}

/* Native audio bus master */
static uint32_t nabm_readb(AC97LinkState *s, uint32_t addr)
{
    AC97BusMasterRegs *r = NULL;
    uint32_t val = ~0U;

    switch (addr) {
    case CAS:
        val = s->cas;
        s->cas = 1;
        break;
    case PI_CIV:
    case PO_CIV:
    case MC_CIV:
    case SO_CIV:
        r = &s->bm_regs[GET_BM(addr)];
        val = r->civ;
        break;
    case PI_LVI:
    case PO_LVI:
    case MC_LVI:
    case SO_LVI:
        r = &s->bm_regs[GET_BM(addr)];
        val = r->lvi;
        break;
    case PI_PIV:
    case PO_PIV:
    case MC_PIV:
    case SO_PIV:
        r = &s->bm_regs[GET_BM(addr)];
        val = r->piv;
        break;
    case PI_CR:
    case PO_CR:
    case MC_CR:
    case SO_CR:
        r = &s->bm_regs[GET_BM(addr)];
        val = r->cr;
        break;
    case PI_SR:
    case PO_SR:
    case MC_SR:
    case SO_SR:
        r = &s->bm_regs[GET_BM(addr)];
        val = r->sr & 0xff;
        break;
    default:
        /* xemu: assert(0) */
        aci_log_once("unhandled bus-master readb", addr, val);
        break;
    }
    return val;
}

static uint32_t nabm_readw(AC97LinkState *s, uint32_t addr)
{
    AC97BusMasterRegs *r = NULL;
    uint32_t val = ~0U;

    switch (addr) {
    case PI_SR:
    case PO_SR:
    case MC_SR:
    case SO_SR:
        r = &s->bm_regs[GET_BM(addr)];
        val = r->sr;
        break;
    case PI_PICB:
    case PO_PICB:
    case MC_PICB:
    case SO_PICB:
        r = &s->bm_regs[GET_BM(addr)];
        val = r->picb;
        break;
    default:
        val = nabm_readb(s, addr) | (nabm_readb(s, addr + 1) << 8);
        break;
    }
    return val;
}

static uint32_t nabm_readl(AC97LinkState *s, uint32_t addr)
{
    AC97BusMasterRegs *r = NULL;
    uint32_t val = ~0U;

    switch (addr) {
    case PI_BDBAR:
    case PO_BDBAR:
    case MC_BDBAR:
    case SO_BDBAR:
        r = &s->bm_regs[GET_BM(addr)];
        val = r->bdbar;
        break;
    case PI_CIV:
    case PO_CIV:
    case MC_CIV:
    case SO_CIV:
        r = &s->bm_regs[GET_BM(addr)];
        val = r->civ | (r->lvi << 8) | (r->sr << 16);
        break;
    case PI_PICB:
    case PO_PICB:
    case MC_PICB:
    case SO_PICB:
        r = &s->bm_regs[GET_BM(addr)];
        val = r->picb | (r->piv << 16) | ((uint32_t)r->cr << 24);
        break;
    case GLOB_CNT:
        val = s->glob_cnt;
        break;
    case GLOB_STA:
        val = s->glob_sta | GS_S0CR;
        break;
    default:
        val = nabm_readw(s, addr) | (nabm_readw(s, addr + 2) << 16);
        break;
    }
    return val;
}

static void nabm_writeb(AC97LinkState *s, uint32_t addr, uint32_t val)
{
    AC97BusMasterRegs *r = NULL;

    switch (addr) {
    case PI_LVI:
    case PO_LVI:
    case MC_LVI:
    case SO_LVI:
        r = &s->bm_regs[GET_BM(addr)];
        if ((r->cr & CR_RPBM) && (r->sr & SR_DCH)) {
            r->sr &= ~(SR_DCH | SR_CELV);
            r->civ = r->piv;
            r->piv = (r->piv + 1) % 32;
            fetch_bd(s, r);
        }
        r->lvi = val % 32;
        break;
    case PI_CR:
    case PO_CR:
    case MC_CR:
    case SO_CR:
        r = &s->bm_regs[GET_BM(addr)];
        if (val & CR_RR) {
            reset_bm_regs(s, r);
        } else {
            r->cr = val & CR_VALID_MASK;
            if (!(r->cr & CR_RPBM)) {
                voice_set_active(s, (int)(r - s->bm_regs), 0);
                r->sr |= SR_DCH;
            } else {
                r->civ = r->piv;
                r->piv = (r->piv + 1) % 32;
                fetch_bd(s, r);
                r->sr &= ~SR_DCH;
                voice_set_active(s, (int)(r - s->bm_regs), 1);
            }
        }
        break;
    case PI_SR:
    case PO_SR:
    case MC_SR:
    case SO_SR:
        r = &s->bm_regs[GET_BM(addr)];
        r->sr |= val & ~(SR_RO_MASK | SR_WCLEAR_MASK);
        update_sr(s, r, r->sr & ~(val & SR_WCLEAR_MASK));
        break;
    default:
        break;
    }
}

static void nabm_writew(AC97LinkState *s, uint32_t addr, uint32_t val)
{
    AC97BusMasterRegs *r = NULL;

    switch (addr) {
    case PI_SR:
    case PO_SR:
    case MC_SR:
    case SO_SR:
        r = &s->bm_regs[GET_BM(addr)];
        r->sr |= val & ~(SR_RO_MASK | SR_WCLEAR_MASK);
        update_sr(s, r, r->sr & ~(val & SR_WCLEAR_MASK));
        break;
    default:
        nabm_writeb(s, addr, val & 0xff);
        nabm_writeb(s, addr + 1, (val >> 8) & 0xff);
        break;
    }
}

static void nabm_writel(AC97LinkState *s, uint32_t addr, uint32_t val)
{
    AC97BusMasterRegs *r = NULL;

    switch (addr) {
    case PI_BDBAR:
    case PO_BDBAR:
    case MC_BDBAR:
    case SO_BDBAR:
        r = &s->bm_regs[GET_BM(addr)];
        r->bdbar = val & ~3u;
        break;
    case GLOB_CNT:
        /* TODO (xemu): Handle WR or CR being set (warm/cold reset requests) */
        if (!(val & (GC_WR | GC_CR))) {
            s->glob_cnt = val & GC_VALID_MASK;
        }
        break;
    case GLOB_STA:
        s->glob_sta &= ~(val & GS_WCLEAR_MASK);
        s->glob_sta |= (val & ~(GS_WCLEAR_MASK | GS_RO_MASK)) & GS_VALID_MASK;
        break;
    default:
        nabm_writew(s, addr, val & 0xffff);
        nabm_writew(s, addr + 2, (val >> 16) & 0xffff);
        break;
    }
}

/* write_audio with the host voice replaced by a sink that takes everything
 * offered (see the file comment). */
static int write_audio(AC97LinkState *s, AC97BusMasterRegs *r,
                       int max, int *stop)
{
    uint32_t addr = r->bd.addr;
    uint32_t temp = (uint32_t)r->picb << 1;
    uint32_t written;

    if ((uint32_t)max < temp)
        temp = (uint32_t)max;
    if (!temp) {
        *stop = 1;
        return 0;
    }
    written = temp;
    if (written >= 4)
        phys_read(addr + written - 4, &s->last_samp, 4);
    else
        s->last_samp = 0;
    r->bd.addr = addr + written;
    return (int)written;
}

static void write_bup(AC97LinkState *s, int elapsed)
{
    if (!(s->bup_flag & BUP_SET)) {
        s->bup_flag |= BUP_SET;
    }
    (void)elapsed;   /* silence / last sample repeated into the sink */
}

static void transfer_audio(AC97LinkState *s, int index, int elapsed)
{
    AC97BusMasterRegs *r = &s->bm_regs[index];
    int stop = 0;

    if (s->invalid_freq[index]) {
        return;
    }

    if (r->sr & SR_DCH) {
        if (r->cr & CR_RPBM) {
            switch (index) {
            case PO_INDEX:
                write_bup(s, elapsed);
                break;
            }
        }
        return;
    }

    while ((elapsed >> 1) && !stop) {
        int temp;

        if (!r->bd_valid) {
            fetch_bd(s, r);
        }

        if (!r->picb) {
            if (r->civ == r->lvi) {
                r->sr |= SR_DCH; /* CELV? */
                s->bup_flag = 0;
                break;
            }
            r->sr &= ~SR_CELV;
            r->civ = r->piv;
            r->piv = (r->piv + 1) % 32;
            fetch_bd(s, r);
            return;
        }

        switch (index) {
        case PO_INDEX:
            temp = write_audio(s, r, elapsed, &stop);
            elapsed -= temp;
            r->picb -= (uint16_t)(temp >> 1);
            break;
        default:
            return;   /* XBOX: no PCM-in / mic voices */
        }

        if (!r->picb) {
            uint32_t new_sr = r->sr & ~SR_CELV;

            if (r->bd.ctl_len & BD_IOC) {
                new_sr |= SR_BCIS;
            }

            if (r->civ == r->lvi) {
                new_sr |= SR_LVBCI | SR_DCH | SR_CELV;
                stop = 1;
                s->bup_flag = (r->bd.ctl_len & BD_BUP) ? BUP_LAST : 0;
            } else {
                r->civ = r->piv;
                r->piv = (r->piv + 1) % 32;
                fetch_bd(s, r);
            }

            update_sr(s, r, new_sr);
        }
    }
}

static void ac97_on_reset(AC97LinkState *s)
{
    reset_bm_regs(s, &s->bm_regs[0]);
    reset_bm_regs(s, &s->bm_regs[1]);
    reset_bm_regs(s, &s->bm_regs[2]);
    mixer_reset(s);
}

/* ---- aci.c: the MMIO page ---- */
static uint64_t aci_read(void *opaque, uint32_t off, unsigned size)
{
    AC97LinkState *s = opaque;
    uint64_t val = 0;   /* unassigned space reads 0 */

    EnterCriticalSection(&s_lock);
    if (off < 0x100) {
        switch (size) {
        case 1: val = nam_readb(s, off); break;
        case 2: val = nam_readw(s, off); break;
        case 4: val = nam_readl(s, off); break;
        }
    } else if (off - ACI_NABM_BASE < ACI_NABM_SIZE) {
        uint32_t a = off - ACI_NABM_BASE;
        switch (size) {
        case 1: val = nabm_readb(s, a); break;
        case 2: val = nabm_readw(s, a); break;
        case 4: val = nabm_readl(s, a); break;
        }
    }
    LeaveCriticalSection(&s_lock);
    if (size < 8)
        val &= (1ULL << (size * 8)) - 1;
    return val;
}

static void aci_write(void *opaque, uint32_t off, uint64_t v, unsigned size)
{
    AC97LinkState *s = opaque;
    uint32_t val = (uint32_t)v;

    EnterCriticalSection(&s_lock);
    if (off < 0x100) {
        switch (size) {
        case 1: nam_writeb(s, off, val); break;
        case 2: nam_writew(s, off, val); break;
        case 4: nam_writel(s, off, val); break;
        }
    } else if (off - ACI_NABM_BASE < ACI_NABM_SIZE) {
        uint32_t a = off - ACI_NABM_BASE;
        switch (size) {
        case 1: nabm_writeb(s, a, val); break;
        case 2: nabm_writew(s, a, val); break;
        case 4: nabm_writel(s, a, val); break;
        }
    }
    LeaveCriticalSection(&s_lock);
}

static LONG CALLBACK aci_mmio_veh(PEXCEPTION_POINTERS ep)
{
    PEXCEPTION_RECORD er = ep->ExceptionRecord;
    uintptr_t fault;

    if (er->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || er->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    fault = (uintptr_t)er->ExceptionInformation[1];
    if (fault < s_mmio_lo || fault >= s_mmio_hi)
        return EXCEPTION_CONTINUE_SEARCH;
    if (mmio_decode_and_run(ep->ContextRecord, (uint32_t)(fault - s_mmio_lo),
                            aci_read, aci_write, &s_ac97)) {
        /* A device-register access is an instruction boundary where a
         * pending interrupt can be taken (see kernel_bridge.c). */
        xbox_irq_deliver_from_mmio();
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    {
        static volatile LONG undecoded;
        if (InterlockedIncrement(&undecoded) <= 20) {
            const uint8_t *ip = (const uint8_t *)XBOX_CONTEXT_PC(ep->ContextRecord);
            fprintf(stderr, "[ACI] undecoded %s at ACI+0x%03llX, rip=%p "
                    "bytes %02X %02X %02X %02X %02X %02X\n",
                    er->ExceptionInformation[0] == 1 ? "write" : "read",
                    (unsigned long long)(fault - s_mmio_lo), (void *)ip,
                    ip[0], ip[1], ip[2], ip[3], ip[4], ip[5]);
            fflush(stderr);
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

/* The audio backend's pull: PCM out consumes 48 kHz x 4 bytes in real time
 * (the codec runs at 48 kHz; with VRA the rate register would say
 * otherwise, but the Xbox codec is fixed-rate). */
static DWORD WINAPI aci_pcm_out_thread(LPVOID arg)
{
    LARGE_INTEGER freq, last, now;
    double carry = 0.0;
    (void)arg;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&last);
    for (;;) {
        int bytes;
        Sleep(5);
        QueryPerformanceCounter(&now);
        carry += (double)(now.QuadPart - last.QuadPart) / (double)freq.QuadPart
                 * 48000.0 * 4.0;
        last = now;
        if (carry > 48000.0 * 4.0 / 10.0)   /* never catch up more than 100 ms */
            carry = 48000.0 * 4.0 / 10.0;
        bytes = (int)carry & ~3;
        if (!bytes)
            continue;
        carry -= bytes;
        EnterCriticalSection(&s_lock);
        if (s_ac97.po_active)   /* the backend only pulls active voices */
            transfer_audio(&s_ac97, PO_INDEX, bytes);
        LeaveCriticalSection(&s_lock);
    }
}

/* mmio_page: host address of guest VA 0xFEC00000 (one committed page);
 * phys: host address of Xbox physical address 0. */
BOOL mcpx_aci_attach(uint8_t *mmio_page, uint8_t *phys)
{
    DWORD old_protect;
    HANDLE th;

    if (InterlockedExchange(&s_attached, 1))
        return TRUE;
    InitializeCriticalSection(&s_lock);
    s_phys = phys;
    ac97_on_reset(&s_ac97);
    s_mmio_lo = (uintptr_t)mmio_page;
    s_mmio_hi = s_mmio_lo + ACI_MMIO_SIZE;
    AddVectoredExceptionHandler(1, aci_mmio_veh);
    if (!VirtualProtect(mmio_page, ACI_MMIO_SIZE, PAGE_NOACCESS, &old_protect)) {
        fprintf(stderr, "  ACI: VirtualProtect failed (%lu)\n", GetLastError());
        return FALSE;
    }
    th = CreateThread(NULL, 0, aci_pcm_out_thread, NULL, 0, NULL);
    if (th)
        CloseHandle(th);
    fprintf(stderr, "  ACI: AC'97 controller emulated at 0xFEC00000 "
            "(xemu ac97, IRQ %d, PCM out paced at 48 kHz, samples discarded)\n",
            ACI_IRQ_LINE);
    return TRUE;
}
