/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Metal frame captures on iOS, which renderdoc.c takes in RenderDoc's place (docs/hosts/apple.md): a capture of the
   system's Metal device, the one MoltenVK draws with, into a .gputrace bundle that Xcode opens. Only the C types, so
   the Objective-C file needn't take the runtime's C23 headers. The renderer's thread calls both, under vk_lock */
#ifndef SNX_GPU_VK_CAPTURE_METAL_H
#define SNX_GPU_VK_CAPTURE_METAL_H
#include <stddef.h>
#include <stdint.h>

/* start a capture into the bundle path: 1 when it runs, 0 with the reason in why */
int vk_metal_capture_start(const char* path, char* why, size_t n);
/* end it: 1 when the bundle is there, its files' bytes in bytes, 0 with the reason in why */
int vk_metal_capture_end(uint64_t* bytes, char* why, size_t n);

#endif
