/* Local stand-in for a QEMU/xemu header (see src/gpu/PROVENANCE.md). */
#pragma once
#include <stdint.h>
typedef struct X86CPU { struct { uint32_t eip; } env; } X86CPU;
#define X86_CPU(obj) ((X86CPU *)(obj))
/* No emulated CPU: the recompiled title runs natively. */
#define current_cpu ((void *)0)

/* x86 4 KiB pages; sign-extended like QEMU's target_long mask. */
#define TARGET_PAGE_BITS 12
#define TARGET_PAGE_SIZE (1 << TARGET_PAGE_BITS)
#define TARGET_PAGE_MASK ((int64_t)-1 << TARGET_PAGE_BITS)
#define TARGET_PAGE_ALIGN(addr) (((addr) + TARGET_PAGE_SIZE - 1) & TARGET_PAGE_MASK)
