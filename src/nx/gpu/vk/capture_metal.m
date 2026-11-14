/* SPDX-License-Identifier: GPL-3.0-or-later
   Copyright 2026 the ScarletNX contributors */
/* Metal frame captures on iOS (capture_metal.h). MTLCaptureManager writes the bundle only with MTL_CAPTURE_ENABLED=1
   set before the first Metal device (vk_renderdoc_load sets it). The renderer's thread lives for the whole game, so each call
   drains its own autorelease pool */
#include "nx/gpu/vk/capture_metal.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>

static char g_capture_path[1024]; /* the running capture's bundle */

/* an error's text, or what to say when it gives none */
static void error_text(NSError* e, const char* none, char* why, size_t n) {
    const char* t = e.localizedDescription.UTF8String;
    snprintf(why, n, "%s", t && *t ? t : none);
}

int vk_metal_capture_start(const char* path, char* why, size_t n) {
    @autoreleasepool {
        static id<MTLDevice> metal; /* the system's default device is the one MoltenVK uses */
        static dispatch_once_t once;
        MTLCaptureManager* m = [MTLCaptureManager sharedCaptureManager];
        if (strlen(path) >= sizeof g_capture_path) {
            snprintf(why, n, "path too long");
            return 0;
        }
        snprintf(g_capture_path, sizeof g_capture_path, "%s", path);
        dispatch_once(&once, ^{
            metal = MTLCreateSystemDefaultDevice();
        });
        if (![m supportsDestination:MTLCaptureDestinationGPUTraceDocument]) {
            snprintf(why, n, "no capture to a file (Metal made a device before MTL_CAPTURE_ENABLED was set)");
            return 0;
        }
        MTLCaptureDescriptor* d = [[MTLCaptureDescriptor alloc] init];
        d.captureObject = metal;
        d.destination = MTLCaptureDestinationGPUTraceDocument;
        d.outputURL = [NSURL fileURLWithPath:@(g_capture_path)];
        NSError* e = nil;
        if (![m startCaptureWithDescriptor:d error:&e]) {
            error_text(e, "no error given", why, n);
            return 0;
        }
        return 1;
    }
}

int vk_metal_capture_end(uint64_t* bytes, char* why, size_t n) {
    @autoreleasepool {
        MTLCaptureManager* m = [MTLCaptureManager sharedCaptureManager];
        NSFileManager* fm = [NSFileManager defaultManager];
        NSString* path = @(g_capture_path);
        BOOL folder = NO;
        uint64_t sum = 0;
        if (!m.isCapturing) {
            snprintf(why, n, "no capture runs");
            return 0;
        }
        [m stopCapture];
        if (![fm fileExistsAtPath:path isDirectory:&folder] || !folder) {
            snprintf(why, n, "the bundle is not there");
            return 0;
        }
        NSDirectoryEnumerator* files =
            [fm enumeratorAtURL:[NSURL fileURLWithPath:path]
                includingPropertiesForKeys:@[ NSURLFileSizeKey ]
                                   options:0
                              errorHandler:^BOOL(NSURL* url, NSError* error) {
                                (void)url;
                                (void)error;
                                return YES; /* a file it can't read is left out of the sum */
                              }];
        for (NSURL* f in files) {
            NSNumber* size = nil;
            if ([f getResourceValue:&size forKey:NSURLFileSizeKey error:nil] && size) sum += size.unsignedLongLongValue;
        }
        *bytes = sum;
        return 1;
    }
}
