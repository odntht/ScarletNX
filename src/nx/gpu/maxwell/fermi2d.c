/* SwitchRecomp - Copyright 2026 MZ Labs
   Licensed under the Apache License 2.0. See LICENSE.apache and NOTICE;
   any copy or derived work must keep the credit to SwitchRecomp by MZ Labs.
   This fork's changes are under GPL-3.0-or-later, the license of the fork as a whole (LICENSE). */
/* the 2D engine (Fermi 2D): blits between surfaces, on the renderer or in guest memory */
#include "nx/gpu/maxwell/engines_int.h"
/* ============================================================ Fermi 2D (blits) */
static void surf2d(u32* r, int b, Surf2D* s) {
    s->format = r[b];
    s->linear = r[b + 1];
    s->bh = (r[b + 2] >> 4) & 15;
    s->depth = r[b + 3];
    s->layer = r[b + 4];
    s->pitch = r[b + 5];
    s->width = r[b + 6];
    s->height = r[b + 7];
    s->addr = ((u64)r[b + 8] << 32) | r[b + 9];
}
/* where the slice of a block linear surface starts: a 3D surface (depth > 1) has its slices inside the blocks (z, with
   the block depth at bits 8-11 of the block size), an array layer is a whole 2D surface on (as dma.c does) */
static u64 surf2d_layer(const u32* r, int b, const Surf2D* s, u32 bpp, u32* z, u32* bd) {
    *z = 0;
    *bd = 0;
    if (s->linear) return 0;
    if (s->depth > 1) {
        *z = s->layer;
        *bd = (r[b + 2] >> 8) & 15;
        return 0;
    }
    u64 gh = 8ull << s->bh;
    return (((u64)s->width * bpp + 63) & ~63ull) * ((s->height + gh - 1) / gh * gh) * s->layer;
}
void fermi2d_method(GpuChannel* ch, u32 m, u32 v) {
    m &= 0x3FF;
    ch->r2d[m] = v;
    if (m == 0x237) { /* src_y0 write kicks the blit */
        u32* r = ch->r2d;
        Surf2D dst, src;
        surf2d(r, 0x80, &dst);
        surf2d(r, 0x8C, &src);
        i32 dx0 = (i32)r[0x22C], dy0 = (i32)r[0x22D], dw = (i32)r[0x22E], dh = (i32)r[0x22F];
        /* 32.32 fixed point, frac reg comes before int reg (DU_DX_FRAC, DU_DX_INT...) */
        i64 dudx = (i64)(((u64)r[0x231] << 32) | r[0x230]);
        i64 dvdy = (i64)(((u64)r[0x233] << 32) | r[0x232]);
        i64 sx0 = (i64)(((u64)r[0x235] << 32) | r[0x234]);
        i64 sy0 = (i64)(((u64)r[0x237] << 32) | r[0x236]);
        double sx = (double)sx0 / 4294967296.0, sy = (double)sy0 / 4294967296.0, sdx = (double)dudx / 4294967296.0,
               sdy = (double)dvdy / 4294967296.0;
        {
            /* Ryujinx subtracts the half texel for the CORNER origin, this for CENTER: the log tells which the game
               uses */
            static u32 seen[4];
            static int nseen;
            int k = 0;
            while (k < nseen && seen[k] != r[0x223]) k++;
            if (k == nseen && nseen < 4) {
                seen[nseen++] = r[0x223];
                LOG("gpu: 2D blit with sample mode %x: %dx%d at %d,%d from %.3f,%.3f step %.3f,%.3f\n", r[0x223], dw,
                    dh, dx0, dy0, sx, sy, sdx, sdy);
            }
        }
        /* CENTER origin (sample_mode bit0 == 0): coords address texel centers */
        if (!(r[0x223] & 1)) {
            sx -= sdx * 0.5;
            sy -= sdy * 0.5;
            if (sx < 0) sx = 0;
            if (sy < 0) sy = 0;
        }
        if (r3d_blit(&src, &dst, dx0, dy0, dw, dh, sx, sy, sdx, sdy)) return;
        /* in guest memory: a row of the source at a time, scaled into a row of the destination, clipped to it */
        u32 bpp = gpu_color_bpp(dst.format);
        if (gpu_color_bpp(src.format) != bpp || !src.width || !src.height) {
            WARN_LIMIT(4, "gpu: 2D blit from format %x (%ux%u) to %x not done: converting isn't done in memory\n",
                       src.format, src.width, src.height, dst.format);
            return;
        }
        if (src.format != dst.format) {
            LOG_LIMIT(4, "gpu: 2D blit from format %x to %x copies the bytes as they are\n", src.format, dst.format);
        }
        i32 x0 = dx0<0 ? -dx0 : 0, x1 = (i64)dx0 + dw>(i64) dst.width ? (i32)((i64)dst.width - dx0) : dw;
        if (x1 <= x0 || dh <= 0) return;
        u32 sz, sbd, dz, dbd, swb = src.width * bpp, dwb = dst.width * bpp;
        u64 sl = surf2d_layer(r, 0x8C, &src, bpp, &sz, &sbd), dl = surf2d_layer(r, 0x80, &dst, bpp, &dz, &dbd);
        u8* srow = malloc(swb);
        u8* drow = malloc((size_t)(x1 - x0) * bpp);
        if (!srow || !drow) {
            WARN("gpu: no memory for a 2D blit of %dx%d\n", dw, dh);
            free(srow);
            free(drow);
            return;
        }
        if (g_renderer_up) {
            g_renderer->rt_flush(src.addr);
            g_renderer->rt_flush(dst.addr);
        }
        /* bl_offset(x, y, z) is bl_offset(x, 0, 0) + bl_offset(0, y, z): a row's 16 byte runs are a table of column
           offsets made once and a base per row. A block linear surface is read and written through one host pointer
           when it is contiguous (as DMA does), else run by run, and the renderer's hooks run once over the span the
           blit reads and the span it writes, as I2M does (gpu.c), not for each run: they look through every render
           target. Each destination pixel's source column is the same on every row */
        u32 xb0 = (u32)(dx0 + x0) * bpp, xb1 = (u32)(dx0 + x1) * bpp, nsrun = (swb + 15) / 16, ndrun = 0;
        for (u32 xb = xb0; xb < xb1; xb = (xb | 15) + 1) ndrun++;
        u32* sxs = malloc((size_t)(x1 - x0) * sizeof *sxs);
        u64* scol = malloc((size_t)nsrun * sizeof *scol);
        u64* dcol = malloc((size_t)(ndrun ? ndrun : 1) * sizeof *dcol);
        if (!sxs || !scol || !dcol) {
            WARN("gpu: no memory for a 2D blit of %dx%d\n", dw, dh);
            free(sxs);
            free(scol);
            free(dcol);
            free(srow);
            free(drow);
            return;
        }
        for (i32 x = x0; x < x1; x++) {
            double fx = sx + sdx * x;
            sxs[x - x0] = (fx < 0 ? 0 : fx >= src.width ? src.width - 1 : (u32)fx) * bpp;
        }
        if (!src.linear)
            for (u32 k = 0; k < nsrun; k++) scol[k] = bl_offset(k * 16, 0, 0, swb, src.height, src.bh, sbd);
        if (!dst.linear) {
            u32 k = 0;
            for (u32 xb = xb0; xb < xb1; xb = (xb | 15) + 1)
                dcol[k++] = bl_offset(xb, 0, 0, dwb, dst.height, dst.bh, dbd);
        }
        u64 sspan = 0, dspan = 0, shi = 0, dhi = 0;
        u8* spp = src.linear ? NULL : bl_span_ptr(src.addr + sl, swb, src.height, src.bh, sz, sbd, &sspan);
        u8* dpp = dst.linear ? NULL : bl_span_ptr(dst.addr + dl, dwb, dst.height, dst.bh, dz, dbd, &dspan);
        for (int pass = 0; pass < 2; pass++) {
            if (pass) {
                if (!src.linear && !spp) gmem_need(src.addr + sl, shi);
                if (!dst.linear) gmem_before_write(dst.addr + dl, dhi);
            }
            for (i32 y = 0; y < dh; y++) {
                i64 ty = (i64)dy0 + y;
                if (ty < 0 || ty >= (i64)dst.height) continue;
                double fy = sy + sdy * y;
                u32 syy = fy < 0 ? 0 : fy >= src.height ? src.height - 1 : (u32)fy;
                u64 sbase = src.linear ? 0 : bl_offset(0, syy, sz, swb, src.height, src.bh, sbd);
                u64 dbase = dst.linear ? 0 : bl_offset(0, (u32)ty, dz, dwb, dst.height, dst.bh, dbd);
                if (!pass) {
                    /* the last run of a row is its highest byte, and the last row's the blit's (block linear) */
                    if (!src.linear && sbase + scol[nsrun - 1] + (swb - (nsrun - 1) * 16) > shi)
                        shi = sbase + scol[nsrun - 1] + (swb - (nsrun - 1) * 16);
                    if (!dst.linear && ndrun) {
                        u32 last = ((xb1 - 1) & ~15u) > xb0 ? (xb1 - 1) & ~15u : xb0;
                        if (dbase + dcol[ndrun - 1] + (xb1 - last) > dhi) dhi = dbase + dcol[ndrun - 1] + (xb1 - last);
                    }
                    continue;
                }
                if (src.linear)
                    gmem_read(src.addr + (u64)syy * src.pitch, srow, swb);
                else
                    for (u32 k = 0; k < nsrun; k++) {
                        u32 n = swb - k * 16 < 16 ? swb - k * 16 : 16;
                        u64 o = sbase + scol[k];
                        /* a whole run is one 16-byte move rather than a call: the game's blits come every frame */
                        if (spp && o + n <= sspan && n == 16)
                            memcpy(srow + k * 16, spp + o, 16);
                        else if (spp && o + n <= sspan)
                            memcpy(srow + k * 16, spp + o, n);
                        else
                            gmem_read_raw(src.addr + sl + o, srow + k * 16, n);
                    }
                if (bpp == 4)
                    for (i32 x = 0; x < x1 - x0; x++) memcpy(drow + (u64)x * 4, srow + sxs[x], 4);
                else
                    for (i32 x = 0; x < x1 - x0; x++) memcpy(drow + (u64)x * bpp, srow + sxs[x], bpp);
                if (dst.linear)
                    gmem_write(dst.addr + (u64)ty * dst.pitch + xb0, drow, xb1 - xb0);
                else {
                    u32 k = 0;
                    for (u32 xb = xb0; xb < xb1; k++) {
                        u32 n = 16 - (xb & 15);
                        if (n > xb1 - xb) n = xb1 - xb;
                        u64 o = dbase + dcol[k];
                        if (dpp && o + n <= dspan && n == 16)
                            memcpy(dpp + o, drow + (xb - xb0), 16);
                        else if (dpp && o + n <= dspan)
                            memcpy(dpp + o, drow + (xb - xb0), n);
                        else
                            gmem_write_raw(dst.addr + dl + o, drow + (xb - xb0), n);
                        xb += n;
                    }
                }
            }
        }
        if (!dst.linear) gpu_mem_written(dst.addr + dl, dhi);
        free(sxs);
        free(scol);
        free(dcol);
        free(srow);
        free(drow);
        if (g_renderer_up) g_renderer->rt_reload(dst.addr);
    }
}
