// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 meridian-woa contributors
// Fixture preview of the real Conn renderer; this is not a firmware boot test.
#include "conn_core.h"
#include "png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int self_test(void)
{
    ConnEntryList list = {0};
    list.count = 2;
    if (conn_obsidian_hit_index(&list, 0, 200, 260) != 0) return 1;
    if (conn_obsidian_hit_index(&list, 0, 200, 375) != 1) return 2;
    if (conn_obsidian_hit_index(&list, 0, 200, 350) != -1) return 3; /* gap */
    if (conn_obsidian_hit_index(&list, 0, 160, 260) != -1) return 4;
    if (conn_obsidian_hit_index(&list, 0, 200, 500) != -1) return 5;
    list.count = 24;
    if (conn_obsidian_hit_index(&list, 23, 200, 210) != 19) return 6;
    if (conn_obsidian_hit_index(&list, 23, 200, 510) != 23) return 7;
    list.count = 0;
    if (conn_obsidian_hit_index(&list, 0, 200, 260) != -1) return 8;
    puts("PASS: Obsidian pointer bounds, row gaps, scrolling, empty list");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc > 1 && strcmp(argv[1], "--self-test") == 0) return self_test();
    const char *path = argc > 1 ? argv[1] : "obsidian.png";
    const char *mode = argc > 2 ? argv[2] : "windows";
    UINTN target_w = argc > 3 ? (UINTN)strtoul(argv[3], NULL, 10) : conn_text_width();
    UINTN target_h = argc > 4 ? (UINTN)strtoul(argv[4], NULL, 10) : conn_text_height();
    if (target_w < 320 || target_w > 8192 || target_h < 200 || target_h > 8192) return 2;
    ConnEntryList list = {0};
    ConnModel model = {0};
    strcpy(list.entry[0].name, "Manual Stanza: Windows 11");
    strcpy(list.entry[0].sub, "Windows Boot Manager");
    strcpy(list.entry[0].loader, "bootmgfw.efi");
    strcpy(list.entry[1].name, "Manual Stanza: Omarchy Linux");
    strcpy(list.entry[1].sub, "Arch Linux / Limine");
    strcpy(list.entry[1].loader, "limine_aa64.efi");
    list.count = 2;
    model.ui.autoboot = TRUE;
    model.ui.countdown_s = 8;
    if (strcmp(mode, "omarchy") == 0) model.ui.selected = 1;
    if (strcmp(mode, "failed") == 0) model.ui.status = CONN_UI_LAUNCH_FAILED;
    if (strcmp(mode, "empty") == 0) list.count = 0;
    if (strcmp(mode, "saver") == 0) model.ui.saver_active = TRUE;
    if (strcmp(mode, "crowded") == 0) {
        list.count = 24;
        for (UINTN i = 2; i < list.count; i++) {
            snprintf(list.entry[i].name, sizeof(list.entry[i].name), "Firmware tool %u", (unsigned)i);
            strcpy(list.entry[i].loader, "tool.efi");
        }
        model.ui.selected = 23;
    }
    if (strcmp(mode, "tools") == 0) {
        list.count = 5;
        strcpy(list.entry[2].name, "Restart");
        strcpy(list.entry[3].name, "Shut down");
        strcpy(list.entry[4].name, "Firmware setup");
    }
    /* "native" (argv[5]) renders at the target size, as firmware now does for
     * Obsidian; otherwise the design canvas is upscaled like the retro themes. */
    int native = argc > 5 && strcmp(argv[5], "native") == 0;
    ConnSurface surface;
    surface.w = native ? target_w : conn_text_width();
    surface.h = native ? target_h : conn_text_height();
    surface.px = calloc(surface.w * surface.h, sizeof(*surface.px));
    unsigned char *out = calloc(target_w * target_h, 4);
    if (surface.px == NULL || out == NULL) return 3;
    conn_render_to_surface(&surface, &list, &model, &CONN_TXT_OBSIDIAN);
    if (native) {
        for (UINTN i = 0; i < target_w * target_h; i++) {
            out[i * 4 + 0] = surface.px[i].Red;
            out[i * 4 + 1] = surface.px[i].Green;
            out[i * 4 + 2] = surface.px[i].Blue;
            out[i * 4 + 3] = 255;
        }
        int result = conn_write_png(path, out, (int)target_w, (int)target_h);
        free(surface.px); free(out);
        if (!result) printf("Native Obsidian render: %s (%ux%u, %s)\n", path,
                            (unsigned)target_w, (unsigned)target_h, mode);
        return result;
    }

    /* Same aspect-fit and bilinear framebuffer presentation as firmware GOP. */
    UINTN dw = target_w, dh = target_h;
    if (surface.w * target_h <= surface.h * target_w) dw = surface.w * target_h / surface.h;
    else dh = surface.h * target_w / surface.w;
    UINTN dx = (target_w - dw) / 2, dy = (target_h - dh) / 2;
    UINT32 sx_step = (UINT32)(((UINT64)(surface.w - 1) << 16) / (dw > 1 ? dw - 1 : 1));
    UINT32 sy_step = (UINT32)(((UINT64)(surface.h - 1) << 16) / (dh > 1 ? dh - 1 : 1));
    for (UINTN i = 0; i < target_w * target_h; i++) out[i * 4 + 3] = 255;
    for (UINTN y = 0; y < dh; y++) {
        UINT32 sy = (UINT32)(y * sy_step);
        UINTN iy = sy >> 16, iy1 = iy + 1 < surface.h ? iy + 1 : iy;
        UINT32 fy = sy & 65535;
        for (UINTN x = 0; x < dw; x++) {
            UINT32 sx = (UINT32)(x * sx_step);
            UINTN ix = sx >> 16, ix1 = ix + 1 < surface.w ? ix + 1 : ix;
            UINT32 fx = sx & 65535;
            EFI_GRAPHICS_OUTPUT_BLT_PIXEL *a = surface.px + iy * surface.w + ix;
            EFI_GRAPHICS_OUTPUT_BLT_PIXEL *b = surface.px + iy * surface.w + ix1;
            EFI_GRAPHICS_OUTPUT_BLT_PIXEL *c = surface.px + iy1 * surface.w + ix;
            EFI_GRAPHICS_OUTPUT_BLT_PIXEL *d = surface.px + iy1 * surface.w + ix1;
            UINT8 ac[3] = {a->Red, a->Green, a->Blue}, bc[3] = {b->Red, b->Green, b->Blue};
            UINT8 cc[3] = {c->Red, c->Green, c->Blue}, dc[3] = {d->Red, d->Green, d->Blue};
            for (UINTN k = 0; k < 3; k++) {
                INT32 top = ac[k] + (((INT32)bc[k] - ac[k]) * (INT32)fx >> 16);
                INT32 bot = cc[k] + (((INT32)dc[k] - cc[k]) * (INT32)fx >> 16);
                out[((y + dy) * target_w + x + dx) * 4 + k] = (UINT8)(top + ((bot - top) * (INT32)fy >> 16));
            }
        }
    }
    int result = conn_write_png(path, out, (int)target_w, (int)target_h);
    free(surface.px); free(out);
    if (!result) printf("Actual Conn renderer fixture: %s (%ux%u, %s)\n", path,
                        (unsigned)target_w, (unsigned)target_h, mode);
    return result;
}
