/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the guest address space (mem.c) */
#ifndef SNX_MEM_H
#define SNX_MEM_H
#include "common/cpu/aarch64/cpu.h"
#include <string.h>

/* the host reservation: every guest address of either process width is an offset below it */
#define AS_SIZE 0x2000000000ull
/* The regions of the process's address space. A 64-bit process has the 39-bit layout (the default, so it holds before
   mem_layout too); a 32-bit one has the console's 32-bit layout below 4 GiB (mem_layout) */
typedef struct {
    u64 base, end; /* the address space: [base, end) */
    u64 heap, heap_size, alias, alias_size, stack, stack_size;
    u64 kern, kern_end; /* TLS, kernel stacks, our own shmem */
} MemLayout;
extern MemLayout g_layout;
#define AS_BASE        (g_layout.base)
#define AS_END         (g_layout.end)
#define HEAP_BASE      (g_layout.heap)
#define HEAP_SIZE      (g_layout.heap_size)
#define ALIAS_BASE     (g_layout.alias)
#define ALIAS_SIZE     (g_layout.alias_size)
#define STACK_BASE     (g_layout.stack)
#define STACK_SIZE     (g_layout.stack_size)
#define KERN_BASE      (g_layout.kern)
#define KERN_END       (g_layout.kern_end)
/* the code base of a 32-bit process, where Loader puts its first module */
#define AS32_CODE_BASE 0x200000ull
/* the layout of a 32-bit process (the NPDM's address space type: 0 with an alias region, 2 without one), before
   anything is mapped */
void mem_layout32(unsigned aspace);

enum {
    MS_FREE = 0x00000000,
    MS_IO = 0x00182001,
    MS_STATIC = 0x00042002,
    MS_CODE = 0x04DC7E03,
    MS_CODEDATA = 0x07FEBD04,
    MS_NORMAL = 0x077EBD05,
    MS_SHARED = 0x04402006,
    MS_ALIASCODE = 0x04DD7E08,
    MS_ALIASCODEDATA = 0x07FFBD09,
    MS_IPC = 0x045C3C0A,
    MS_STACK = 0x045C3C0B,
    MS_TLS = 0x0400000C,
    MS_TRANSFERED = 0x055C3C0D,
    MS_SHAREDTRANSFERED = 0x045C380E,
    MS_SHAREDCODE = 0x0440380F,
    MS_INACCESSIBLE = 0x00000010,
    MS_KERNEL = 0x00000013,
    MS_GENCODE = 0x04402214,
    MS_CODEOUT = 0x04402015
};
enum { PERM_NONE = 0, PERM_R = 1, PERM_W = 2, PERM_RW = 3, PERM_X = 4, PERM_RX = 5, PERM_DONTCARE = 0x10000000 };

extern u8* g_mem;
typedef struct {
    u64 addr, size;
    u32 state, perm, attr, ipc, dev;
} MemBlock;
void mem_init(void);
int mem_map(u64 addr, u64 size, u32 state, u32 perm); /* 0 ok, -1 not free, -2 no host memory */
/* Heap and physical-memory changes share the memory lock and a combined byte budget. */
enum { MEM_BADSTATE = -1, MEM_NOMEM = -2, MEM_LIMIT = -3 };
int mem_map_physical(u64 addr, u64 size, u64 budget);
int mem_unmap_physical(u64 addr, u64 size);
int mem_unmap(u64 addr, u64 size);
/* ldr:ro's contiguous code view of two heap ranges. Like MapMemory, backed by copies: sources stay locked
   until unload copies back. These views do not charge the process's already-accounted heap twice. */
typedef struct {
    u64 base, source, size, bss, bss_size;
} MemCodeMap;
int mem_map_code(MemCodeMap* m); /* selects a free base; MEM_LIMIT if no code address space remains */
int mem_unmap_code(const MemCodeMap* m);
void mem_query(u64 addr, MemBlock* out);
int mem_set_state(u64 addr, u64 size, u32 state, u32 perm);
int mem_set_attr(u64 addr, u64 size, u32 mask, u32 attr);
u64 mem_find_free(u64 lo, u64 hi, u64 size, u64 align);
u64 kmem_alloc(u64 size, u32 state, u32 perm); /* reserved for the kernel */
int mem_heap_set(u64 size, u64 budget);        /* 0 or MEM_*; failed changes leave the heap intact */
extern u64 g_heap_size, g_phys_mapped;         /* atomic snapshots; mutations hold the memory lock */
extern u64 g_heap_max;                         /* heap_max_mb's cap, 0 = none */
extern u64 g_code_memory, g_main_stack_size;   /* charged once, before starting guest threads */
#define GP(a) (g_mem + (u64)(a))
static inline u32 gr32(u64 a) {
    u32 v;
    memcpy(&v, GP(a), 4);
    return v;
}
static inline u64 gr64(u64 a) {
    u64 v;
    memcpy(&v, GP(a), 8);
    return v;
}
static inline void gw32(u64 a, u32 v) { memcpy(GP(a), &v, 4); }
static inline void gw64(u64 a, u64 v) { memcpy(GP(a), &v, 8); }

#endif
