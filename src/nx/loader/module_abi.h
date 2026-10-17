/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
#ifndef SNX_NX_MODULE_ABI_H
#define SNX_NX_MODULE_ABI_H
#include "common/cpu/aarch64/cpu.h"
#include "common/cpu/arm/arm.h"
/* the module descriptor: what snx_module() in the game module's module.c (written by src/nx/loader/module.c) hands
   the runtime's loader. abi stays the first field so a module from another version is recognized before anything
   else is read */
typedef struct SnxModInfo {
    const char* name;
    u64 base, size;
    u8 build_id[32];
    u32 text_off, text_size, ro_off, ro_size, data_off, data_size, bss_size;
    u32 is_nro;
} SnxModInfo;
typedef struct SnxModule {
    u32 abi;
    u32 nmods;
    const SnxModInfo* mods;
    u32 nentries;
    const u64* entry_addr;
    const GuestFn* entry_fn;
    GuestFn interp;
    u64 code_lo, code_hi;
    const char* title;
    u64 title_id;
    u32 main_stack, main_prio, main_core;
} SnxModule;

/* The game module of a 32-bit program (the NPDM's is64 = 0): what snx_module_arm() hands the runtime, which a 64-bit
   one's lacks. Its code is AArch32 blocks (cpu/arm/llvm.h), each void(SnxArmCPU*) running from its entry to the next
   control transfer, and the runtime's dispatcher runs them one after another (nx/runtime/arm.c). It is a separate
   symbol with its own version, so 64-bit modules and SNX_ABI are unchanged by it. abi stays the first field */
#define SNX_ARM_MODULE_ABI (0x41333200u | SNX_ARM_ABI) /* "A32", then the SnxArmCPU layout's version */
typedef struct SnxArmModule {
    u32 abi;
    u32 nmods;
    const SnxModInfo* mods;
    u32 nentries;
    const u32* entry_addr; /* sorted: each block's guest address, with bit 0 set for a Thumb block */
    const SnxArmBlock* entry_fn;
    const char* title;
    u64 title_id;
    u32 main_stack, main_prio, main_core;
    u32 aspace; /* the NPDM's address space type */
} SnxArmModule;

#endif
